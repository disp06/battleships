/*
 * main.c - Main loop & state machine:
 *   Title -> Mode Select -> (Ad-Hoc Lobby) -> Setup -> Battle -> Game Over
 *
 * Battleship for PS Vita (PCH-1000 OLED, 960x544) - VitaSDK / vita2d.
 */
#include <stdio.h>
#include <string.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/power.h>
#include <psp2/sysmodule.h>

#include "app.h"
#include "game_logic.h"
#include "ai.h"
#include "audio.h"
#include "render.h"
#include "adhoc_net.h"

#define TRANS_SPEED   (1.0f / 0.18f)   /* fade-out / fade-in duration */
#define AI_THINK_S    0.6f             /* simulated thinking delay    */
#define SHELL_FLIGHT  0.45f
#define SONAR_EVERY   2.6f

static App   g;
static Rng   g_rng;
static int   g_running = 1;

static const char *SHIP_NAME[MAX_SHIP_LEN + 1] = { "", "SUBMARINE", "DESTROYER", "CRUISER", "BATTLESHIP" };

/* ================================================================ helpers */
static void go_state(AppState next)
{
    if (g.trans_active) return;
    g.trans_active = 1;
    g.trans_dir = 1;
    g.trans_next = next;
}

static void set_banner(const char *txt, unsigned color)
{
    strncpy(g.banner, txt, sizeof(g.banner) - 1);
    g.banner[sizeof(g.banner) - 1] = 0;
    g.banner_color = color;
    g.banner_t = 2.0f;
}

static void alert(const char *title, const char *msg)
{
    g.alert_active = 1;
    g.alert_confirm = 0;
    strncpy(g.alert_title, title, sizeof(g.alert_title) - 1);
    strncpy(g.alert_msg, msg, sizeof(g.alert_msg) - 1);
    audio_play(SFX_ERROR);
}

static void back_to_menu_with_alert(const char *title, const char *msg)
{
    net_leave();
    alert(title, msg);
    g.trans_active = 0;
    g.trans_t = 0;
    g.state = ST_MODE_SELECT;
    g.state_time = 0;
    fx_reset();
}

static void begin_new_match(void)
{
    g.peer_left = 0;
    g.match_over = 0;
    /* NOTE: peer_ready is cleared on lobby connect / game-over entry, never
     * here - the peer may already have sent READY for the new round. */
    g.ready_sent = 0;
    g.rematch_me = g.rematch_peer = 0;
    g.deferred_fire = 0;
}

static void on_enter(AppState s)
{
    g.state_time = 0;
    switch (s) {
    case ST_SETUP:
        board_clear(&g.me);
        g.cur_x = g.cur_y = 0;
        g.cur_horiz = 1;
        g.place_n = 0;
        g.ready_sent = 0;
        fx_reset();
        break;
    case ST_BATTLE:
        radar_clear(&g.radar);
        fx_reset();
        g.aim_x = g.aim_y = 4;
        g.shot_pending = 0;
        g.result_ready = 0;
        g.last_in_x = g.last_in_y = -1;
        g.my_shots = g.my_hits = g.en_shots = g.en_hits = 0;
        g.ai_stage = 0;
        g.ai_timer = AI_THINK_S;
        g.sonar_timer = 0;
        g.end_timer = 0;
        g.match_over = 0;
        g.banner_t = 0;
        if (g.mode == MODE_SINGLE) {
            g.turn = TURN_ME;
        } else {
            g.turn = net_is_host() ? TURN_ME : TURN_ENEMY;
        }
        set_banner(g.turn == TURN_ME ? "YOU FIRE FIRST" : "ENEMY FIRES FIRST", g.turn == TURN_ME ? COL_AMBER : COL_CORAL);
        break;
    case ST_GAMEOVER:
        g.over_sel = 0;
        g.peer_ready = 0;
        audio_play(g.won ? SFX_VICTORY : SFX_DEFEAT);
        break;
    case ST_LOBBY_JOIN:
        g.join_sel = 0;
        break;
    default: break;
    }
}

static void update_transition(float dt)
{
    if (!g.trans_active) return;
    if (g.trans_dir > 0) {
        g.trans_t += dt * TRANS_SPEED;
        if (g.trans_t >= 1.0f) {
            g.trans_t = 1.0f;
            g.state = g.trans_next;
            on_enter(g.state);
            g.trans_dir = -1;
        }
    } else {
        g.trans_t -= dt * TRANS_SPEED;
        if (g.trans_t <= 0.0f) { g.trans_t = 0.0f; g.trans_active = 0; }
    }
}

static int move_cursor(const Input *in, int *x, int *y, int maxx, int maxy)
{
    int ox = *x, oy = *y;
    if (in->nav & BTN_LEFT_)  (*x)--;
    if (in->nav & BTN_RIGHT_) (*x)++;
    if (in->nav & BTN_UP_)    (*y)--;
    if (in->nav & BTN_DOWN_)  (*y)++;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x > maxx) *x = maxx;
    if (*y > maxy) *y = maxy;
    if (*x != ox || *y != oy) { audio_play_vol(SFX_TICK, 0.6f); return 1; }
    return 0;
}

static int menu_nav(const Input *in, int *sel, int count)
{
    int old = *sel;
    if (in->nav & BTN_UP_)   *sel = (*sel + count - 1) % count;
    if (in->nav & BTN_DOWN_) *sel = (*sel + 1) % count;
    if (*sel != old) { audio_play_vol(SFX_TICK, 0.6f); return 1; }
    return 0;
}

/* ================================================================ setup */
static int current_ship(void)
{
    int i;
    for (i = 0; i < FLEET_SIZE; i++) if (!g.me.ships[i].placed) return i;
    return -1;
}

static void clamp_setup_cursor(void)
{
    int idx = current_ship(), len = idx >= 0 ? g.me.ships[idx].len : 1;
    int maxx = g.cur_horiz ? GRID_N - len : GRID_N - 1;
    int maxy = g.cur_horiz ? GRID_N - 1 : GRID_N - len;
    if (g.cur_x > maxx) g.cur_x = maxx;
    if (g.cur_y > maxy) g.cur_y = maxy;
    if (g.cur_x < 0) g.cur_x = 0;
    if (g.cur_y < 0) g.cur_y = 0;
}

static void setup_place(void)
{
    int idx = current_ship();
    if (idx < 0) return;
    if (board_place(&g.me, idx, g.cur_x, g.cur_y, g.cur_horiz)) {
        g.place_stack[g.place_n++] = idx;
        audio_play(SFX_CONFIRM);
        fx_splash(SIDE_ME, g.cur_x, g.cur_y);
        clamp_setup_cursor();
    } else {
        audio_play(SFX_ERROR);
        fx_shake(3.0f);
    }
}

static void setup_rotate(void)
{
    if (current_ship() < 0) return;
    g.cur_horiz = !g.cur_horiz;
    clamp_setup_cursor();
    audio_play_vol(SFX_TICK, 0.8f);
}

static void setup_random(void)
{
    int i;
    board_randomize(&g.me, &g_rng);
    g.place_n = 0;
    for (i = 0; i < FLEET_SIZE; i++) g.place_stack[g.place_n++] = i;
    audio_play(SFX_CONFIRM);
}

static void setup_undo(void)
{
    if (g.place_n <= 0) return;
    {
        int idx = g.place_stack[--g.place_n];
        Ship s = g.me.ships[idx];
        board_unplace(&g.me, idx);
        g.cur_x = s.x; g.cur_y = s.y; g.cur_horiz = s.horiz;
        /* undo can leave a smaller ship as "current" if randomised; keep order */
        clamp_setup_cursor();
    }
    audio_play_vol(SFX_TICK, 1.0f);
}

static void setup_ready(void)
{
    if (g.me.ships_placed < FLEET_SIZE || g.ready_sent) return;
    audio_play(SFX_CONFIRM);
    if (g.mode == MODE_SINGLE) {
        ai_init(&g.ai, rng_next(&g_rng));
        board_randomize(&g.enemy, &g_rng);
        go_state(ST_BATTLE);
    } else {
        Packet p;
        memset(&p, 0, sizeof(p));
        p.type = PKT_READY;
        net_send(&p);
        g.ready_sent = 1;
    }
}

static void update_setup(const Input *in)
{
    int idx = current_ship();
    if (g.ready_sent) {
        if (g.mode == MODE_ADHOC && g.peer_ready) go_state(ST_BATTLE);
        if (in->pressed & SCE_CTRL_CIRCLE) {
            g.alert_active = 1; g.alert_confirm = 1;
            strcpy(g.alert_title, "LEAVE GAME?");
            strcpy(g.alert_msg, "Disconnect from your opponent and return to the menu.");
        }
        return;
    }

    if (idx >= 0) {
        int len = g.me.ships[idx].len;
        move_cursor(in, &g.cur_x, &g.cur_y, g.cur_horiz ? GRID_N - len : GRID_N - 1,
                    g.cur_horiz ? GRID_N - 1 : GRID_N - len);
    }

    if (in->pressed & SCE_CTRL_CROSS) {
        if (idx >= 0) setup_place(); else setup_ready();
    }
    if (in->pressed & SCE_CTRL_START) setup_ready();
    if (in->pressed & (SCE_CTRL_TRIANGLE | SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER)) setup_rotate();
    if (in->pressed & SCE_CTRL_SQUARE) setup_random();
    if (in->pressed & SCE_CTRL_CIRCLE) {
        if (g.place_n > 0) setup_undo();
        else if (g.mode == MODE_ADHOC) {
            g.alert_active = 1; g.alert_confirm = 1;
            strcpy(g.alert_title, "LEAVE GAME?");
            strcpy(g.alert_msg, "Disconnect from your opponent and return to the menu.");
        } else go_state(ST_MODE_SELECT);
    }

    if (in->tap) {
        int cx, cy, b;
        if (hit_grid(SIDE_ME, in->tx, in->ty, &cx, &cy)) {
            if (idx >= 0) {
                g.cur_x = cx; g.cur_y = cy;
                if (board_can_place(&g.me, cx, cy, g.me.ships[idx].len, g.cur_horiz)) setup_place();
                else { clamp_setup_cursor(); audio_play_vol(SFX_TICK, 0.6f); }
            }
        } else if ((b = hit_setup_button(in->tx, in->ty)) != BTN_NONE) {
            switch (b) {
            case BTN_ROTATE: setup_rotate(); break;
            case BTN_RANDOM: setup_random(); break;
            case BTN_UNDO:   setup_undo(); break;
            case BTN_READY:  setup_ready(); break;
            }
        }
    }
}

/* ================================================================ battle */
static void finish_game(int won)
{
    g.won = won;
    g.match_over = 1;
    g.end_timer = 1.6f;
}

static void apply_my_result(void)
{
    ShotResult res = g.pending_res;
    int x = g.shot_x, y = g.shot_y;
    g.shot_pending = 0;
    g.result_ready = 0;

    radar_apply(&g.radar, x, y, res, res == RES_SUNK ? &g.pending_sunk : NULL);
    if (res == RES_MISS) {
        audio_play(SFX_SPLASH);
        fx_splash(SIDE_ENEMY, x, y);
        g.turn = TURN_ENEMY;
        g.ai_stage = 0;
        g.ai_timer = AI_THINK_S;
        g.sonar_timer = 0.3f;
    } else {
        g.my_hits++;
        if (res == RES_SUNK) {
            char b[64];
            snprintf(b, sizeof(b), "ENEMY %s SUNK!", SHIP_NAME[g.pending_sunk.len]);
            audio_play(SFX_SUNK);
            fx_shockwave(SIDE_ENEMY, x, y, 1);
            fx_sunk_ship(SIDE_ENEMY, &g.pending_sunk);
            set_banner(b, COL_CORAL);
        } else {
            audio_play(SFX_HIT);
            fx_shockwave(SIDE_ENEMY, x, y, 0);
        }
        if (radar_all_sunk(&g.radar)) finish_game(1);
    }
}

static void fire_at(int x, int y)
{
    if (g.turn != TURN_ME || g.shot_pending || g.end_timer > 0) return;
    if (!radar_can_fire(&g.radar, x, y)) { audio_play(SFX_ERROR); return; }
    g.aim_x = x; g.aim_y = y;
    g.shot_pending = 1;
    g.shot_x = x; g.shot_y = y;
    g.shot_timer = SHELL_FLIGHT;
    g.result_ready = 0;
    g.my_shots++;
    audio_play(SFX_FIRE);
    fx_shake(2.0f);

    if (g.mode == MODE_SINGLE) {
        g.pending_res = board_receive_shot(&g.enemy, x, y, &g.pending_sunk);
        g.result_ready = 1;
    } else {
        Packet p;
        memset(&p, 0, sizeof(p));
        p.type = PKT_FIRE; p.a = (uint8_t)x; p.b = (uint8_t)y;
        net_send(&p);
    }
}

/* incoming shot resolved on my board */
static void apply_incoming(int x, int y, ShotResult res, const Ship *sunk)
{
    g.last_in_x = x; g.last_in_y = y;
    g.en_shots++;
    if (res == RES_MISS || res == RES_INVALID) {
        audio_play_vol(SFX_SPLASH, 0.8f);
        fx_splash(SIDE_ME, x, y);
        g.turn = TURN_ME;
    } else {
        g.en_hits++;
        if (res == RES_SUNK) {
            char b[64];
            snprintf(b, sizeof(b), "YOUR %s WAS SUNK!", SHIP_NAME[sunk->len]);
            audio_play(SFX_SUNK);
            fx_shockwave(SIDE_ME, x, y, 1);
            fx_sunk_ship(SIDE_ME, sunk);
            set_banner(b, COL_AMBER);
        } else {
            audio_play(SFX_HIT);
            fx_shockwave(SIDE_ME, x, y, 0);
        }
        if (board_all_sunk(&g.me)) finish_game(0);
        g.ai_stage = 0;
        g.ai_timer = AI_THINK_S;
    }
}

static void handle_remote_fire(int x, int y)
{
    Ship s;
    ShotResult res;
    Packet p;
    memset(&s, 0, sizeof(s));
    res = board_receive_shot(&g.me, x, y, &s);
    memset(&p, 0, sizeof(p));
    p.type = PKT_RESULT;
    p.a = (uint8_t)x; p.b = (uint8_t)y;
    p.c = (uint8_t)(res == RES_INVALID ? RES_MISS : res);
    if (res == RES_SUNK) { p.d = (uint8_t)s.x; p.e = (uint8_t)s.y; p.f = s.len; p.g = s.horiz; }
    net_send(&p);
    apply_incoming(x, y, res, &s);
}

static void update_battle(const Input *in, float dt)
{
    if (g.banner_t > 0) g.banner_t -= dt;

    if (g.end_timer > 0) {
        g.end_timer -= dt;
        if (g.end_timer <= 0) go_state(ST_GAMEOVER);
        return;
    }

    /* shell in flight */
    if (g.shot_pending) {
        g.shot_timer -= dt;
        if (g.shot_timer <= 0 && g.result_ready) apply_my_result();
    }

    if (g.turn == TURN_ENEMY) {
        g.sonar_timer -= dt;
        if (g.sonar_timer <= 0) { audio_play_vol(SFX_SONAR, 0.7f); g.sonar_timer = SONAR_EVERY; }

        if (g.mode == MODE_SINGLE) {
            g.ai_timer -= dt;
            if (g.ai_stage == 0 && g.ai_timer <= 0) {
                ai_choose(&g.ai, &g.ai_x, &g.ai_y);
                audio_play_vol(SFX_FIRE, 0.75f);
                g.ai_stage = 1;
                g.ai_timer = SHELL_FLIGHT;
            } else if (g.ai_stage == 1 && g.ai_timer <= 0) {
                Ship s;
                ShotResult res = board_receive_shot(&g.me, g.ai_x, g.ai_y, &s);
                ai_notify(&g.ai, g.ai_x, g.ai_y, res, res == RES_SUNK ? &s : NULL);
                apply_incoming(g.ai_x, g.ai_y, res, &s);
                g.ai_stage = 0;
            }
        }
    } else if (!g.shot_pending) {
        move_cursor(in, &g.aim_x, &g.aim_y, GRID_N - 1, GRID_N - 1);
        if (in->pressed & SCE_CTRL_CROSS) fire_at(g.aim_x, g.aim_y);
        if (in->tap) {
            int cx, cy;
            if (hit_grid(SIDE_ENEMY, in->tx, in->ty, &cx, &cy)) fire_at(cx, cy);
        }
    }

    if (in->pressed & SCE_CTRL_CIRCLE) {
        g.alert_active = 1; g.alert_confirm = 1;
        strcpy(g.alert_title, "SURRENDER?");
        strcpy(g.alert_msg, g.mode == MODE_ADHOC ? "You will leave the match and disconnect."
                                                 : "Abandon this battle and return to the menu.");
    }
}

/* ================================================================ net */
/* The opponent may quit right after answering our winning shot, so RESULT
 * and QUIT can arrive in the same frame.  Apply the result first; if it
 * decided the game, keep the result screen instead of bailing out. */
static int settle_final_shot(void)
{
    if (g.state == ST_BATTLE && g.shot_pending && g.result_ready) {
        apply_my_result();
        if (g.match_over) {
            g.peer_left = 1;
            net_leave();
            return 1;
        }
    }
    return 0;
}

static char g_lobby_status[96];
static const char *g_names[NET_MAX_HOSTS];

static void update_network(void)
{
    Packet p;
    NetState ns;
    if (g.mode != MODE_ADHOC) return;
    if (g.state != ST_LOBBY_HOST && g.state != ST_LOBBY_JOIN && g.state != ST_SETUP &&
        g.state != ST_BATTLE && g.state != ST_GAMEOVER) return;

    net_update();

    /* Opponent leaving after the match is already decided is not an error:
     * keep the result on screen and just disable REMATCH. */
    if (g.state == ST_GAMEOVER || (g.state == ST_BATTLE && g.match_over)) {
        int left = 0;
        while (net_recv(&p)) {
            if (p.type == PKT_QUIT) left = 1;
            else if (p.type == PKT_REMATCH) g.rematch_peer = 1;
            else if (p.type == PKT_READY) g.peer_ready = 1;
        }
        ns = net_state();
        if (left || ns == NET_DISCONNECTED || ns == NET_ERROR) {
            if (!g.peer_left) {
                g.peer_left = 1;
                net_leave();
                audio_play_vol(SFX_ERROR, 0.6f);
            }
            return;
        }
        if (g.peer_left) return;
    }
    if (g.peer_left) return;

    ns = net_state();
    if ((g.state == ST_LOBBY_HOST || g.state == ST_LOBBY_JOIN) && ns == NET_CONNECTED && !g.trans_active) {
        strncpy(g.peer_nick, net_peer_nick(), sizeof(g.peer_nick) - 1);
        g.peer_ready = 0;
        audio_play(SFX_CONFIRM);
        begin_new_match();
        go_state(ST_SETUP);
    }

    while (net_recv(&p)) {
        switch (p.type) {
        case PKT_HELLO:
            if (p.a != NET_PROTO_VERSION) {
                back_to_menu_with_alert("VERSION MISMATCH", "Both consoles need the same Battleship version.");
                return;
            }
            break;
        case PKT_READY:
            g.peer_ready = 1;
            break;
        case PKT_FIRE:
            if (p.a >= GRID_N || p.b >= GRID_N) break;
            if (g.state == ST_BATTLE && !g.trans_active && g.turn == TURN_ENEMY && g.end_timer <= 0) {
                handle_remote_fire(p.a, p.b);
            } else if (g.state != ST_GAMEOVER) {
                /* arrived while we were still fading into battle */
                g.deferred_fire = 1; g.deferred_x = p.a; g.deferred_y = p.b;
            }
            break;
        case PKT_RESULT:
            if (g.shot_pending && p.a == g.shot_x && p.b == g.shot_y) {
                g.pending_res = (ShotResult)(p.c <= RES_SUNK ? p.c : RES_MISS);
                memset(&g.pending_sunk, 0, sizeof(g.pending_sunk));
                if (g.pending_res == RES_SUNK) {
                    g.pending_sunk.x = (int8_t)p.d; g.pending_sunk.y = (int8_t)p.e;
                    g.pending_sunk.len = p.f; g.pending_sunk.horiz = p.g; g.pending_sunk.placed = 1;
                    g.pending_sunk.hits = p.f;
                }
                g.result_ready = 1;
            }
            break;
        case PKT_REMATCH:
            g.rematch_peer = 1;
            break;
        case PKT_QUIT:
            if (settle_final_shot()) return;
            back_to_menu_with_alert("OPPONENT LEFT", "Your opponent has left the battle.");
            return;
        default: break;
        }
    }

    /* checked after draining packets, so a clean PKT_QUIT wins over the
     * socket-level disconnect that follows it */
    ns = net_state();
    if (ns == NET_ERROR) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s", net_last_error()[0] ? net_last_error() : "Ad-Hoc error");
        back_to_menu_with_alert("AD-HOC ERROR", msg);
        return;
    }
    if (ns == NET_DISCONNECTED) {
        if (settle_final_shot()) return;
        back_to_menu_with_alert("CONNECTION LOST", net_last_error()[0] ? net_last_error() : "The other Vita stopped responding.");
        return;
    }

    if (g.deferred_fire && g.state == ST_BATTLE && !g.trans_active && g.turn == TURN_ENEMY) {
        g.deferred_fire = 0;
        handle_remote_fire(g.deferred_x, g.deferred_y);
    }

    if (g.state == ST_GAMEOVER && g.rematch_me && g.rematch_peer && !g.trans_active) {
        begin_new_match();
        go_state(ST_SETUP);
    }
}

static void build_lobby_info(void)
{
    int i, n = 0;
    NetState ns = net_state();
    switch (ns) {
    case NET_HOSTING:   snprintf(g_lobby_status, sizeof(g_lobby_status), "Broadcasting - waiting for a challenger..."); break;
    case NET_SCANNING:  snprintf(g_lobby_status, sizeof(g_lobby_status), "Listening on Ad-Hoc channel"); break;
    case NET_JOINING:   snprintf(g_lobby_status, sizeof(g_lobby_status), "Requesting to join %s...", net_peer_nick()); break;
    case NET_LINKING:   snprintf(g_lobby_status, sizeof(g_lobby_status), "Linking with %s...", net_peer_nick()); break;
    case NET_CONNECTED: snprintf(g_lobby_status, sizeof(g_lobby_status), "Connected to %s", net_peer_nick()); break;
    default:            snprintf(g_lobby_status, sizeof(g_lobby_status), "Starting Ad-Hoc..."); break;
    }
    if (g.state == ST_LOBBY_JOIN && (ns == NET_SCANNING || ns == NET_JOINING)) {
        n = net_host_count();
        if (n > LOBBY_MAX_SHOWN) n = LOBBY_MAX_SHOWN;
        for (i = 0; i < n; i++) {
            const NetHost *h = net_host_get(i);
            g_names[i] = h ? h->nick : "?";
        }
    }
    render_set_lobby(g_lobby_status, ns == NET_SCANNING ? n : 0, g_names);
}

/* ================================================================ screens */
static void update_title(const Input *in)
{
    if ((in->pressed & (SCE_CTRL_CROSS | SCE_CTRL_START)) || in->tap) {
        audio_play(SFX_CONFIRM);
        go_state(ST_MODE_SELECT);
    }
}

static void choose_mode(int sel)
{
    audio_play(SFX_CONFIRM);
    switch (sel) {
    case 0:
        g.mode = MODE_SINGLE;
        strcpy(g.peer_nick, "CPU");
        begin_new_match();
        go_state(ST_SETUP);
        break;
    case 1:
        g.mode = MODE_ADHOC;
        if (net_host(g.nick) < 0) { alert("AD-HOC UNAVAILABLE", net_last_error()); net_leave(); return; }
        go_state(ST_LOBBY_HOST);
        break;
    case 2:
        g.mode = MODE_ADHOC;
        if (net_scan(g.nick) < 0) { alert("AD-HOC UNAVAILABLE", net_last_error()); net_leave(); return; }
        go_state(ST_LOBBY_JOIN);
        break;
    case 3:
        g_running = 0;
        break;
    }
}

static void update_mode_select(const Input *in)
{
    int hit;
    menu_nav(in, &g.menu_sel, MENU_ITEMS);
    if (in->pressed & SCE_CTRL_CROSS) choose_mode(g.menu_sel);
    if (in->pressed & SCE_CTRL_CIRCLE) go_state(ST_TITLE);
    if (in->tap && (hit = hit_menu(MENU_ITEMS, in->tx, in->ty)) >= 0) {
        g.menu_sel = hit;
        choose_mode(hit);
    }
}

static void update_lobby(const Input *in, float dt)
{
    g.sonar_timer -= dt;
    if (g.sonar_timer <= 0) { audio_play_vol(SFX_SONAR, 0.6f); g.sonar_timer = 2.8f; }

    if (in->pressed & SCE_CTRL_CIRCLE) {
        net_leave();
        go_state(ST_MODE_SELECT);
        return;
    }
    if (g.state == ST_LOBBY_JOIN && net_state() == NET_SCANNING) {
        int n = net_host_count(), hit;
        if (n > LOBBY_MAX_SHOWN) n = LOBBY_MAX_SHOWN;
        if (n > 0) {
            if (g.join_sel >= n) g.join_sel = n - 1;
            menu_nav(in, &g.join_sel, n);
            if (in->pressed & SCE_CTRL_CROSS) { audio_play(SFX_CONFIRM); net_join(g.join_sel); }
            if (in->tap && (hit = hit_join_list(n, in->tx, in->ty)) >= 0) {
                g.join_sel = hit; audio_play(SFX_CONFIRM); net_join(hit);
            }
        }
    }
}

static void update_gameover(const Input *in)
{
    int hit, choose = -1;
    if (in->nav & (BTN_LEFT_ | BTN_UP_))    { if (g.over_sel != 0) audio_play_vol(SFX_TICK, 0.6f); g.over_sel = 0; }
    if (in->nav & (BTN_RIGHT_ | BTN_DOWN_)) { if (g.over_sel != 1) audio_play_vol(SFX_TICK, 0.6f); g.over_sel = 1; }
    if (in->pressed & SCE_CTRL_CROSS) choose = g.over_sel;
    if (in->pressed & SCE_CTRL_CIRCLE) choose = 1;
    if (in->tap && (hit = hit_gameover(in->tx, in->ty)) >= 0) { g.over_sel = hit; choose = hit; }

    if (choose == 0) {
        if (g.mode == MODE_SINGLE) {
            audio_play(SFX_CONFIRM);
            begin_new_match();
            go_state(ST_SETUP);
        } else if (g.peer_left) {
            audio_play(SFX_ERROR);
        } else if (!g.rematch_me) {
            Packet p;
            memset(&p, 0, sizeof(p));
            p.type = PKT_REMATCH;
            net_send(&p);
            g.rematch_me = 1;
            audio_play(SFX_CONFIRM);
        }
    } else if (choose == 1) {
        audio_play(SFX_CONFIRM);
        if (g.mode == MODE_ADHOC) net_leave();
        g.peer_left = 0;
        go_state(ST_MODE_SELECT);
    }
}

static void update_alert(const Input *in)
{
    if (g.alert_confirm) {
        if (in->pressed & SCE_CTRL_CROSS || in->tap) {
            g.alert_active = 0;
            g.alert_confirm = 0;
            audio_play(SFX_CONFIRM);
            if (g.mode == MODE_ADHOC) net_leave();
            go_state(ST_MODE_SELECT);
        } else if (in->pressed & SCE_CTRL_CIRCLE) {
            g.alert_active = 0;
            g.alert_confirm = 0;
            audio_play_vol(SFX_TICK, 0.8f);
        }
    } else if ((in->pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) || in->tap) {
        g.alert_active = 0;
        audio_play(SFX_CONFIRM);
    }
}

/* ================================================================ main */
static void read_nickname(void)
{
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    strcpy(g.nick, "CAPTAIN");
    if (sceAppUtilInit(&init, &boot) >= 0) {
        char name[SCE_SYSTEM_PARAM_USERNAME_MAXSIZE];
        memset(name, 0, sizeof(name));
        if (sceAppUtilSystemParamGetString(SCE_SYSTEM_PARAM_ID_USERNAME, (SceChar8 *)name, sizeof(name)) >= 0 && name[0]) {
            strncpy(g.nick, name, sizeof(g.nick) - 1);
            g.nick[sizeof(g.nick) - 1] = 0;
        }
    }
}

int main(int argc, char *argv[])
{
    Input in;
    SceUInt64 last;
    (void)argc; (void)argv;

    memset(&g, 0, sizeof(g));
    scePowerSetArmClockFrequency(444);
    scePowerSetGpuClockFrequency(222);

    read_nickname();
    rng_seed(&g_rng, (uint32_t)sceKernelGetProcessTimeWide() ^ 0xA5A5F00Du);
    if (render_init() < 0) sceKernelExitProcess(0);
    audio_init();               /* game still runs silently if this fails */
    input_init();

    g.state = ST_TITLE;
    g.mode = MODE_SINGLE;
    on_enter(ST_TITLE);
    last = sceKernelGetProcessTimeWide();

    while (g_running) {
        SceUInt64 now = sceKernelGetProcessTimeWide();
        float dt = (float)(now - last) / 1000000.0f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;          /* resume from sleep etc. */
        g.time += dt;
        g.state_time += dt;

        input_poll(&in);
        update_transition(dt);
        update_network();

        if (g.alert_active) {
            update_alert(&in);
        } else if (!g.trans_active) {
            switch (g.state) {
            case ST_TITLE:       update_title(&in); break;
            case ST_MODE_SELECT: update_mode_select(&in); break;
            case ST_LOBBY_HOST:
            case ST_LOBBY_JOIN:  update_lobby(&in, dt); break;
            case ST_SETUP:       update_setup(&in); break;
            case ST_BATTLE:      update_battle(&in, dt); break;
            case ST_GAMEOVER:    update_gameover(&in); break;
            }
        } else if (g.state == ST_BATTLE) {
            Input none;
            memset(&none, 0, sizeof(none));
            update_battle(&none, dt);    /* keep timers/AI ticking under fades */
        }

        if (g.state == ST_LOBBY_HOST || g.state == ST_LOBBY_JOIN) build_lobby_info();
        render_frame(&g, dt);
    }

    net_shutdown();
    audio_shutdown();
    render_shutdown();
    sceKernelExitProcess(0);
    return 0;
}
