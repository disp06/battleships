/*
 * game_logic.c - 10x10 board state, placement validation (strict 1-cell
 * buffer, diagonals included) and combat rules.
 */
#include <string.h>
#include "game_logic.h"

const uint8_t FLEET_LENGTHS[FLEET_SIZE] = { 4, 3, 3, 2, 2, 2, 1, 1, 1, 1 };

/* ------------------------------------------------------------------ RNG */
void rng_seed(Rng *r, uint32_t seed) { r->s = seed ? seed : 0x9E3779B9u; }

uint32_t rng_next(Rng *r)
{
    /* xorshift32 */
    uint32_t x = r->s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->s = x;
    return x;
}

int rng_range(Rng *r, int n) { return n > 0 ? (int)(rng_next(r) % (uint32_t)n) : 0; }

/* ---------------------------------------------------------------- Board */
void board_clear(Board *b)
{
    int i;
    memset(b, 0, sizeof(*b));
    memset(b->ship_at, -1, sizeof(b->ship_at));
    for (i = 0; i < FLEET_SIZE; i++) {
        b->ships[i].len = FLEET_LENGTHS[i];
        b->ships[i].horiz = 1;
        b->ships[i].x = b->ships[i].y = -1;
    }
}

int board_cell_free_of_buffer(const Board *b, int x, int y)
{
    int dx, dy;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (in_grid(nx, ny) && b->ship_at[ny][nx] >= 0)
                return 0;
        }
    return 1;
}

int board_can_place(const Board *b, int x, int y, int len, int horiz)
{
    int i;
    if (len < 1 || len > MAX_SHIP_LEN) return 0;
    for (i = 0; i < len; i++) {
        int cx = x + (horiz ? i : 0);
        int cy = y + (horiz ? 0 : i);
        if (!in_grid(cx, cy)) return 0;
        if (!board_cell_free_of_buffer(b, cx, cy)) return 0;
    }
    return 1;
}

int board_place(Board *b, int idx, int x, int y, int horiz)
{
    Ship *s;
    int i;
    if (idx < 0 || idx >= FLEET_SIZE) return 0;
    s = &b->ships[idx];
    if (s->placed) return 0;
    if (!board_can_place(b, x, y, s->len, horiz)) return 0;
    s->x = (int8_t)x; s->y = (int8_t)y; s->horiz = (uint8_t)(horiz ? 1 : 0);
    s->hits = 0; s->placed = 1;
    for (i = 0; i < s->len; i++)
        b->ship_at[ship_cell_y(s, i)][ship_cell_x(s, i)] = (int8_t)idx;
    b->ships_placed++;
    return 1;
}

void board_unplace(Board *b, int idx)
{
    Ship *s;
    int i;
    if (idx < 0 || idx >= FLEET_SIZE) return;
    s = &b->ships[idx];
    if (!s->placed) return;
    for (i = 0; i < s->len; i++)
        b->ship_at[ship_cell_y(s, i)][ship_cell_x(s, i)] = -1;
    s->placed = 0;
    s->x = s->y = -1;
    b->ships_placed--;
}

int board_randomize(Board *b, Rng *r)
{
    int attempt;
    for (attempt = 0; attempt < 200; attempt++) {
        int i, ok = 1;
        board_clear(b);
        for (i = 0; i < FLEET_SIZE && ok; i++) {
            /* Collect every legal spot for this ship, then pick one.  This
             * never fails for the standard fleet unless an earlier layout
             * boxed us in, in which case we restart. */
            static int16_t cand[GRID_N * GRID_N * 2];
            int n = 0, x, y, h;
            for (h = 0; h < 2; h++)
                for (y = 0; y < GRID_N; y++)
                    for (x = 0; x < GRID_N; x++)
                        if (board_can_place(b, x, y, FLEET_LENGTHS[i], h))
                            cand[n++] = (int16_t)(h * 100 + y * GRID_N + x);
            if (n == 0) { ok = 0; break; }
            {
                int c = cand[rng_range(r, n)];
                h = c / 100; c %= 100;
                board_place(b, i, c % GRID_N, c / GRID_N, h);
            }
        }
        if (ok) return 1;
    }
    return 0;
}

void board_mark_sunk_buffer(Board *b, const Ship *s)
{
    int i, dx, dy;
    for (i = 0; i < s->len; i++) {
        int cx = ship_cell_x(s, i), cy = ship_cell_y(s, i);
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++) {
                int nx = cx + dx, ny = cy + dy;
                if (in_grid(nx, ny) && b->ship_at[ny][nx] < 0 && b->shot[ny][nx] == SHOT_NONE)
                    b->shot[ny][nx] = SHOT_MISS;
            }
    }
}

ShotResult board_receive_shot(Board *b, int x, int y, Ship *sunk_out)
{
    int idx;
    Ship *s;
    if (!in_grid(x, y)) return RES_INVALID;
    if (b->shot[y][x] != SHOT_NONE) return RES_INVALID;
    idx = b->ship_at[y][x];
    if (idx < 0) {
        b->shot[y][x] = SHOT_MISS;
        return RES_MISS;
    }
    b->shot[y][x] = SHOT_HIT;
    s = &b->ships[idx];
    s->hits++;
    if (s->hits >= s->len) {
        b->ships_sunk++;
        board_mark_sunk_buffer(b, s);
        if (sunk_out) *sunk_out = *s;
        return RES_SUNK;
    }
    return RES_HIT;
}

int board_all_sunk(const Board *b) { return b->ships_sunk >= FLEET_SIZE; }

/* ---------------------------------------------------------------- Radar */
void radar_clear(Radar *r) { memset(r, 0, sizeof(*r)); }

int radar_can_fire(const Radar *r, int x, int y)
{
    return in_grid(x, y) && r->cell[y][x] == RADAR_UNKNOWN;
}

int radar_apply(Radar *r, int x, int y, ShotResult res, const Ship *sunk)
{
    int i, dx, dy, marked = 0;
    if (!in_grid(x, y)) return 0;
    switch (res) {
    case RES_MISS: r->cell[y][x] = RADAR_MISS; break;
    case RES_HIT:  r->cell[y][x] = RADAR_HIT;  break;
    case RES_SUNK:
        if (!sunk || sunk->len < 1 || sunk->len > MAX_SHIP_LEN) {
            r->cell[y][x] = RADAR_HIT;
            break;
        }
        for (i = 0; i < sunk->len; i++) {
            int cx = ship_cell_x(sunk, i), cy = ship_cell_y(sunk, i);
            if (in_grid(cx, cy)) r->cell[cy][cx] = RADAR_SUNK;
        }
        for (i = 0; i < sunk->len; i++) {
            int cx = ship_cell_x(sunk, i), cy = ship_cell_y(sunk, i);
            for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++) {
                    int nx = cx + dx, ny = cy + dy;
                    if (in_grid(nx, ny) && r->cell[ny][nx] == RADAR_UNKNOWN) {
                        r->cell[ny][nx] = RADAR_AUTO;
                        marked++;
                    }
                }
        }
        if (r->sunk_count < FLEET_SIZE) {
            r->sunk_ships[r->sunk_count++] = *sunk;
            r->sunk_by_len[sunk->len]++;
        }
        break;
    default: break;
    }
    return marked;
}

int radar_all_sunk(const Radar *r) { return r->sunk_count >= FLEET_SIZE; }
