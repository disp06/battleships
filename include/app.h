/*
 * app.h - Shared application state (owned by main.c, read by render.c).
 */
#ifndef BS_APP_H
#define BS_APP_H

#include "game_logic.h"
#include "ai.h"

#define SCREEN_W 960
#define SCREEN_H 544

typedef enum {
    ST_TITLE = 0,
    ST_MODE_SELECT,
    ST_LOBBY_HOST,
    ST_LOBBY_JOIN,
    ST_SETUP,
    ST_BATTLE,
    ST_GAMEOVER
} AppState;

typedef enum { MODE_SINGLE = 0, MODE_ADHOC = 1 } GameMode;

typedef enum {
    TURN_ME = 0,
    TURN_ENEMY
} Turn;

/* Setup-screen touch buttons */
typedef enum { BTN_NONE = -1, BTN_ROTATE = 0, BTN_RANDOM, BTN_UNDO, BTN_READY, BTN_COUNT } SetupButton;

#define MENU_ITEMS 4      /* Single, Host, Join, Exit */
#define GAMEOVER_ITEMS 2  /* Rematch, Main menu       */

typedef struct {
    AppState state;
    GameMode mode;
    float    time;                 /* seconds since boot          */
    float    state_time;           /* seconds in current state    */

    /* screen transition (alpha fade through black) */
    int      trans_active;
    float    trans_t;              /* 0..1 fade-out then 1..0 fade-in */
    int      trans_dir;            /* +1 out, -1 in               */
    AppState trans_next;

    char     nick[32];
    char     peer_nick[32];

    /* menus */
    int      menu_sel;
    int      join_sel;

    /* setup */
    Board    me;
    int      cur_x, cur_y, cur_horiz;
    int      place_stack[FLEET_SIZE];
    int      place_n;
    int      ready_sent;
    int      peer_ready;

    /* battle */
    Radar    radar;                /* my view of the enemy        */
    Board    enemy;                /* single-player: AI's board   */
    Ai       ai;
    Turn     turn;
    int      aim_x, aim_y;
    int      shot_pending;         /* my shell in flight          */
    int      shot_x, shot_y;
    float    shot_timer;
    int      result_ready;         /* adhoc: RESULT arrived       */
    ShotResult pending_res;
    Ship     pending_sunk;
    float    ai_timer;             /* AI think / incoming delay   */
    int      ai_stage;             /* 0 thinking, 1 incoming      */
    int      ai_x, ai_y;
    int      last_in_x, last_in_y; /* last enemy shot (marker)    */
    float    sonar_timer;

    /* stats */
    int      my_shots, my_hits;
    int      en_shots, en_hits;

    float    end_timer;            /* delay before game-over panel */
    int      match_over;           /* result decided (set by finish_game)  */
    int      deferred_fire;        /* adhoc: FIRE that arrived mid-transition */
    int      deferred_x, deferred_y;

    /* game over */
    int      won;
    int      over_sel;
    int      rematch_me, rematch_peer;
    int      peer_left;            /* adhoc: opponent quit after the result was decided */

    /* banner ("CRUISER SUNK!") */
    char     banner[64];
    float    banner_t;
    unsigned banner_color;

    /* modal alert */
    int      alert_active;
    int      alert_confirm;        /* 1 = yes/no question (surrender) */
    char     alert_title[48];
    char     alert_msg[128];
} App;

#endif
