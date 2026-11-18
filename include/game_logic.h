/*
 * game_logic.h - Board state, ship placement validation and combat rules.
 * Pure C99, no Vita dependencies (unit-testable on a desktop host).
 */
#ifndef BS_GAME_LOGIC_H
#define BS_GAME_LOGIC_H

#include <stdint.h>

#define GRID_N       10
#define FLEET_SIZE   10
#define MAX_SHIP_LEN 4

/* Fleet: 1x4, 2x3, 3x2, 4x1 - placed largest first. */
extern const uint8_t FLEET_LENGTHS[FLEET_SIZE];

typedef struct {
    int8_t  x, y;      /* origin (top-left cell)               */
    uint8_t len;       /* 1..4                                 */
    uint8_t horiz;     /* 1 = extends +x, 0 = extends +y       */
    uint8_t hits;      /* number of cells hit                  */
    uint8_t placed;    /* 1 once on the board                  */
} Ship;

/* Own board: what is where + what the enemy shot. */
typedef enum {
    SHOT_NONE = 0,
    SHOT_MISS = 1,
    SHOT_HIT  = 2
} ShotMark;

typedef struct {
    int8_t  ship_at[GRID_N][GRID_N];   /* [y][x]: ship index or -1 */
    uint8_t shot[GRID_N][GRID_N];      /* ShotMark                  */
    Ship    ships[FLEET_SIZE];
    uint8_t ships_placed;
    uint8_t ships_sunk;
} Board;

/* Radar = what we know about the enemy board. */
typedef enum {
    RADAR_UNKNOWN = 0,
    RADAR_MISS    = 1,   /* real miss                         */
    RADAR_HIT     = 2,   /* hit, ship not yet sunk            */
    RADAR_SUNK    = 3,   /* cell of a sunk ship               */
    RADAR_AUTO    = 4    /* auto-marked buffer water (sunk)   */
} RadarMark;

typedef struct {
    uint8_t cell[GRID_N][GRID_N];      /* RadarMark */
    Ship    sunk_ships[FLEET_SIZE];    /* revealed sunk ships     */
    uint8_t sunk_count;
    uint8_t sunk_by_len[MAX_SHIP_LEN + 1];
} Radar;

typedef enum {
    RES_INVALID = -1,   /* out of range / already shot */
    RES_MISS    = 0,
    RES_HIT     = 1,
    RES_SUNK    = 2
} ShotResult;

/* Simple deterministic RNG so the logic is testable and platform-neutral. */
typedef struct { uint32_t s; } Rng;
void     rng_seed(Rng *r, uint32_t seed);
uint32_t rng_next(Rng *r);
int      rng_range(Rng *r, int n);          /* 0..n-1 */

/* ---- Board / placement ---- */
void board_clear(Board *b);
int  board_can_place(const Board *b, int x, int y, int len, int horiz);
int  board_place(Board *b, int ship_idx, int x, int y, int horiz);
void board_unplace(Board *b, int ship_idx);
int  board_randomize(Board *b, Rng *r);      /* places full fleet, 1 on success */
int  board_cell_free_of_buffer(const Board *b, int x, int y); /* no ship within 1 */

/* Defender side: resolve an incoming shot. On RES_SUNK, *sunk_out gets the ship. */
ShotResult board_receive_shot(Board *b, int x, int y, Ship *sunk_out);
int  board_all_sunk(const Board *b);

/* After a sink on the own board, mark surrounding water as miss (cosmetic). */
void board_mark_sunk_buffer(Board *b, const Ship *s);

/* ---- Radar (attacker view) ---- */
void radar_clear(Radar *r);
int  radar_can_fire(const Radar *r, int x, int y);
/* Apply a result. For RES_SUNK pass the revealed ship. Returns # cells auto-marked. */
int  radar_apply(Radar *r, int x, int y, ShotResult res, const Ship *sunk);
int  radar_all_sunk(const Radar *r);

/* Helpers */
static inline int in_grid(int x, int y) { return x >= 0 && y >= 0 && x < GRID_N && y < GRID_N; }
static inline int ship_cell_x(const Ship *s, int i) { return s->x + (s->horiz ? i : 0); }
static inline int ship_cell_y(const Ship *s, int i) { return s->y + (s->horiz ? 0 : i); }

#endif
