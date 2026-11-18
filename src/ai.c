/*
 * ai.c - Hunt & Target AI.
 *
 * HUNT   : probability-density map of every legal placement of the ships
 *          still afloat, restricted to a checkerboard parity lattice whose
 *          spacing equals the shortest surviving ship (2 -> classic
 *          checkerboard).  Highest-density parity cell wins.
 * TARGET : after a hit, probe the 4 orthogonal neighbours (ranked by
 *          density); once a second hit lands the axis is locked and the AI
 *          walks both ends of the line until the ship sinks.
 *
 * Because ships may never touch (diagonals included), the four diagonal
 * neighbours of any hit are guaranteed water - the AI uses that knowledge,
 * as would a sharp human player.
 */
#include <string.h>
#include "ai.h"

static int cell_unknown(const Ai *ai, int x, int y)
{
    return in_grid(x, y) && ai->radar.cell[y][x] == RADAR_UNKNOWN && !ai->water[y][x];
}

static void remaining_counts(const Ai *ai, int cnt[MAX_SHIP_LEN + 1])
{
    int i;
    memset(cnt, 0, sizeof(int) * (MAX_SHIP_LEN + 1));
    for (i = 0; i < FLEET_SIZE; i++) cnt[FLEET_LENGTHS[i]]++;
    for (i = 1; i <= MAX_SHIP_LEN; i++) {
        cnt[i] -= ai->radar.sunk_by_len[i];
        if (cnt[i] < 0) cnt[i] = 0;
    }
}

/* Density over unknown cells. If must_cover >= 0, only placements covering
 * all current HIT cells (target mode) are counted, and hit cells count as
 * passable. */
static void density(const Ai *ai, int dens[GRID_N][GRID_N], int target_mode)
{
    int cnt[MAX_SHIP_LEN + 1];
    int len, x, y, h, i;
    int nhits = 0;
    memset(dens, 0, sizeof(int) * GRID_N * GRID_N);
    remaining_counts(ai, cnt);

    if (target_mode)
        for (y = 0; y < GRID_N; y++)
            for (x = 0; x < GRID_N; x++)
                if (ai->radar.cell[y][x] == RADAR_HIT) nhits++;

    for (len = 1; len <= MAX_SHIP_LEN; len++) {
        if (!cnt[len]) continue;
        for (h = 0; h < (len == 1 ? 1 : 2); h++)
            for (y = 0; y < GRID_N; y++)
                for (x = 0; x < GRID_N; x++) {
                    int ok = 1, covered = 0;
                    for (i = 0; i < len && ok; i++) {
                        int cx = x + (h ? i : 0), cy = y + (h ? 0 : i);
                        if (!in_grid(cx, cy)) { ok = 0; break; }
                        if (ai->radar.cell[cy][cx] == RADAR_HIT) {
                            if (!target_mode) ok = 0; else covered++;
                        } else if (!cell_unknown(ai, cx, cy)) ok = 0;
                    }
                    if (!ok) continue;
                    if (target_mode && covered != nhits) continue;
                    for (i = 0; i < len; i++) {
                        int cx = x + (h ? i : 0), cy = y + (h ? 0 : i);
                        if (cell_unknown(ai, cx, cy)) dens[cy][cx] += cnt[len];
                    }
                }
    }
}

void ai_init(Ai *ai, uint32_t seed)
{
    memset(ai, 0, sizeof(*ai));
    rng_seed(&ai->rng, seed);
    radar_clear(&ai->radar);
    ai->parity_seed = (uint8_t)rng_range(&ai->rng, 12);
    ai->mode = AI_MODE_HUNT;
}

static int pick_best(Ai *ai, int dens[GRID_N][GRID_N], const uint8_t allow[GRID_N][GRID_N],
                     int *ox, int *oy)
{
    int best = -1, nbest = 0, x, y;
    for (y = 0; y < GRID_N; y++)
        for (x = 0; x < GRID_N; x++) {
            if (!allow[y][x] || !cell_unknown(ai, x, y)) continue;
            if (dens[y][x] > best) { best = dens[y][x]; nbest = 1; }
            else if (dens[y][x] == best) nbest++;
        }
    if (best < 0) return 0;
    {
        int pick = rng_range(&ai->rng, nbest);
        for (y = 0; y < GRID_N; y++)
            for (x = 0; x < GRID_N; x++) {
                if (!allow[y][x] || !cell_unknown(ai, x, y) || dens[y][x] != best) continue;
                if (pick-- == 0) { *ox = x; *oy = y; return 1; }
            }
    }
    return 0;
}

void ai_choose(Ai *ai, int *ox, int *oy)
{
    int dens[GRID_N][GRID_N];
    uint8_t allow[GRID_N][GRID_N];
    int hx[GRID_N * GRID_N], hy[GRID_N * GRID_N], nh = 0;
    int x, y;

    for (y = 0; y < GRID_N; y++)
        for (x = 0; x < GRID_N; x++)
            if (ai->radar.cell[y][x] == RADAR_HIT) { hx[nh] = x; hy[nh] = y; nh++; }

    memset(allow, 0, sizeof(allow));

    if (nh > 0) {
        /* ---------------- TARGET MODE ---------------- */
        ai->mode = AI_MODE_TARGET;
        density(ai, dens, 1);
        if (nh >= 2) {
            int same_row = 1, same_col = 1, i;
            for (i = 1; i < nh; i++) {
                if (hy[i] != hy[0]) same_row = 0;
                if (hx[i] != hx[0]) same_col = 0;
            }
            if (same_row) {
                int minx = hx[0], maxx = hx[0];
                for (i = 1; i < nh; i++) { if (hx[i] < minx) minx = hx[i]; if (hx[i] > maxx) maxx = hx[i]; }
                if (cell_unknown(ai, minx - 1, hy[0])) allow[hy[0]][minx - 1] = 1;
                if (cell_unknown(ai, maxx + 1, hy[0])) allow[hy[0]][maxx + 1] = 1;
            } else if (same_col) {
                int miny = hy[0], maxy = hy[0];
                for (i = 1; i < nh; i++) { if (hy[i] < miny) miny = hy[i]; if (hy[i] > maxy) maxy = hy[i]; }
                if (cell_unknown(ai, hx[0], miny - 1)) allow[miny - 1][hx[0]] = 1;
                if (cell_unknown(ai, hx[0], maxy + 1)) allow[maxy + 1][hx[0]] = 1;
            }
        } else {
            static const int d[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
            int k;
            for (k = 0; k < 4; k++) {
                int nx = hx[0] + d[k][0], ny = hy[0] + d[k][1];
                if (cell_unknown(ai, nx, ny)) allow[ny][nx] = 1;
            }
        }
        if (pick_best(ai, dens, allow, ox, oy)) return;
        /* Fallthrough should not happen with consistent results. */
    }

    /* ---------------- HUNT MODE ---------------- */
    ai->mode = AI_MODE_HUNT;
    density(ai, dens, 0);
    {
        int cnt[MAX_SHIP_LEN + 1], minlen = MAX_SHIP_LEN, l, any = 0;
        remaining_counts(ai, cnt);
        for (l = MAX_SHIP_LEN; l >= 1; l--) if (cnt[l]) minlen = l;
        if (minlen >= 2) {
            int c = ai->parity_seed % minlen;
            for (y = 0; y < GRID_N; y++)
                for (x = 0; x < GRID_N; x++)
                    if ((x + y) % minlen == c && cell_unknown(ai, x, y) && dens[y][x] > 0) {
                        allow[y][x] = 1; any = 1;
                    }
        }
        if (!any) {
            for (y = 0; y < GRID_N; y++)
                for (x = 0; x < GRID_N; x++)
                    allow[y][x] = (uint8_t)cell_unknown(ai, x, y);
        }
    }
    if (pick_best(ai, dens, allow, ox, oy)) return;

    /* Last resort: any cell the radar still lets us fire at. */
    for (y = 0; y < GRID_N; y++)
        for (x = 0; x < GRID_N; x++)
            if (radar_can_fire(&ai->radar, x, y)) { *ox = x; *oy = y; return; }
    *ox = 0; *oy = 0;
}

void ai_notify(Ai *ai, int x, int y, ShotResult res, const Ship *sunk)
{
    radar_apply(&ai->radar, x, y, res, sunk);
    if (res == RES_MISS) {
        ai->water[y][x] = 1;
    } else if (res == RES_HIT || res == RES_SUNK) {
        static const int d[4][2] = { {1,1}, {1,-1}, {-1,1}, {-1,-1} };
        int k;
        for (k = 0; k < 4; k++) {
            int nx = x + d[k][0], ny = y + d[k][1];
            if (in_grid(nx, ny) && ai->radar.cell[ny][nx] == RADAR_UNKNOWN)
                ai->water[ny][nx] = 1;
        }
    }
    {
        int yy, xx, open = 0;
        for (yy = 0; yy < GRID_N; yy++)
            for (xx = 0; xx < GRID_N; xx++)
                if (ai->radar.cell[yy][xx] == RADAR_HIT) open = 1;
        ai->mode = open ? AI_MODE_TARGET : AI_MODE_HUNT;
    }
}
