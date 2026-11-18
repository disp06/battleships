/*
 * Host simulation driver: compiles the REAL main.c (renamed) together with
 * stubbed Vita APIs (sim_stubs.c) and a bot that drives the UI through the
 * same button / touch paths a player uses.
 *
 * Roles (env BS_ROLE): single | host | join
 * Env BS_GAMES: number of games to play (default 3)
 */
#define main game_main
#include "../../src/main.c"
#undef main

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

/* hooks consumed by sim_stubs.c */
extern void (*sim_input_hook)(SceCtrlData *pad, int *touch, int *tx, int *ty);
extern uint64_t sim_now_us(void);

static const char *role;
static int games_target = 3, games_done = 0, wins = 0;
static int frame;
static uint64_t last_progress;
static int last_shots_total = -1;
static AppState last_state = (AppState)-1;
static int exit_requested = 0;
static int saw_opponent_left = 0;
static int gameover_counted = 0;

static void dump_fleet(const char *tag, const Ship *ships, int n)
{
    int i;
    printf("%s %s:", role, tag);
    for (i = 0; i < n; i++) printf(" [%d,%d,%d,%d]", ships[i].x, ships[i].y, ships[i].len, ships[i].horiz);
    printf("\n");
}

static void bot(SceCtrlData *pad, int *touch, int *tx, int *ty)
{
    int press = (frame++ % 4) == 0;          /* tap buttons every 4th frame */
    uint64_t now = sim_now_us();
    int shots_total = g.my_shots + g.en_shots;

    pad->buttons = 0; pad->lx = pad->ly = 128;
    *touch = 0;
    if (g.state != ST_GAMEOVER) gameover_counted = 0;
    if (getenv("BS_DIE_AFTER_SHOTS") && g.state == ST_BATTLE && g.my_shots >= atoi(getenv("BS_DIE_AFTER_SHOTS"))) {
        printf("%s simulating console power-off mid-battle\n", role); fflush(stdout); _exit(9);   /* no QUIT sent */
    }
    if (saw_opponent_left) exit_requested = 1;

    if (g.state != last_state || shots_total != last_shots_total) {
        last_state = g.state; last_shots_total = shots_total; last_progress = now;
    }
    if (now - last_progress > 60ULL * 1000000ULL) {
        printf("%s DEADLOCK in state %d turn %d pending %d peer_ready %d ready_sent %d net %d\n",
               role, g.state, g.turn, g.shot_pending, g.peer_ready, g.ready_sent, net_state());
        fflush(stdout);
        _exit(3);
    }

    if (g.alert_active) {
        static char last_alert[200];
        char cur[200];
        if (!strcmp(g.alert_title, "OPPONENT LEFT")) saw_opponent_left = 1;
        snprintf(cur, sizeof(cur), "%s - %s", g.alert_title, g.alert_msg);
        if (strcmp(cur, last_alert)) {
            printf("%s ALERT: %s  [state=%d turn=%d pending=%d result_ready=%d end=%.2f my_hits=%d en_hits=%d radar_sunk=%d]\n",
                   role, cur, g.state, g.turn, g.shot_pending, g.result_ready, g.end_timer, g.my_hits, g.en_hits, g.radar.sunk_count);
            strcpy(last_alert, cur);
        }
        if (press) pad->buttons = g.alert_confirm ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
        return;
    }
    if (g.trans_active || !press) return;

    switch (g.state) {
    case ST_TITLE: pad->buttons = SCE_CTRL_CROSS; break;
    case ST_MODE_SELECT: {
        int want = (exit_requested || games_done >= games_target) ? 3 : (!strcmp(role, "single") ? 0 : !strcmp(role, "host") ? 1 : 2);
        if (g.menu_sel != want) pad->buttons = SCE_CTRL_DOWN;
        else pad->buttons = SCE_CTRL_CROSS;
        break;
    }
    case ST_LOBBY_JOIN:
        if (net_host_count() > 0 && net_state() == NET_SCANNING) pad->buttons = SCE_CTRL_CROSS;
        break;
    case ST_SETUP:
        if (g.ready_sent) break;
        if (g.me.ships_placed == 0 && (frame / 4) % 3 == 0) {
            /* exercise manual placement via touch for the battleship */
            *touch = 1; *tx = GRID_L_X + 2 * GRID_CELL + 5; *ty = GRID_TOP + 3 * GRID_CELL + 5;
        } else if (g.me.ships_placed < FLEET_SIZE) pad->buttons = SCE_CTRL_SQUARE;
        else pad->buttons = SCE_CTRL_CROSS;
        break;
    case ST_BATTLE:
        if (g.turn == TURN_ME && !g.shot_pending && g.end_timer <= 0) {
            /* simple human-like bot: target around hits, else random - fires via TOUCH */
            int x, y, tries = 0;
            do { x = rand() % GRID_N; y = rand() % GRID_N; } while (!radar_can_fire(&g.radar, x, y) && ++tries < 500);
            {
                int yy, xx;
                for (yy = 0; yy < GRID_N; yy++) for (xx = 0; xx < GRID_N; xx++)
                    if (g.radar.cell[yy][xx] == RADAR_HIT) {
                        static const int d[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
                        int k;
                        for (k = 0; k < 4; k++)
                            if (radar_can_fire(&g.radar, xx + d[k][0], yy + d[k][1])) { x = xx + d[k][0]; y = yy + d[k][1]; }
                    }
            }
            *touch = 1;
            *tx = GRID_R_X + x * GRID_CELL + GRID_CELL / 2;
            *ty = GRID_TOP + y * GRID_CELL + GRID_CELL / 2;
        }
        break;
    case ST_GAMEOVER: {
        if (!gameover_counted) {
            gameover_counted = 1;
            games_done++;
            if (g.won) wins++;
            printf("%s GAME %d: %s  my_shots=%d my_hits=%d en_shots=%d en_hits=%d\n", role, games_done,
                   g.won ? "WON" : "LOST", g.my_shots, g.my_hits, g.en_shots, g.en_hits);
            dump_fleet("my_fleet", g.me.ships, FLEET_SIZE);
            dump_fleet("radar_sunk", g.radar.sunk_ships, g.radar.sunk_count);
            if (g.peer_left) printf("%s note: opponent left before game-over panel\n", role);
            fflush(stdout);
        }
        if (games_done >= games_target) {
            /* host / single leave via MAIN MENU; joiner waits to be told */
            if (strcmp(role, "join") != 0 || g.peer_left) { exit_requested = 1; pad->buttons = SCE_CTRL_CROSS; g.over_sel = 1; }
        } else if (!g.rematch_me) {
            g.over_sel = 0; pad->buttons = SCE_CTRL_CROSS;
        }
        break;
    }
    default: break;
    }
}

int main(void)
{
    const char *gs = getenv("BS_GAMES");
    role = getenv("BS_ROLE") ? getenv("BS_ROLE") : "single";
    if (gs) games_target = atoi(gs);
    srand(!strcmp(role, "host") ? 11 : 22);
    sim_input_hook = bot;
    last_progress = sim_now_us();
    game_main(0, NULL);
    printf("%s SUMMARY games=%d wins=%d\n", role, games_done, wins);
    return games_done >= games_target ? 0 : 1;
}
