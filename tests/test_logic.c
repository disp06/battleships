/*
 * Host-side tests for game_logic.c and ai.c (no Vita SDK needed):
 *   cc -O2 -Iinclude tests/test_logic.c src/game_logic.c src/ai.c -o test_logic && ./test_logic
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_logic.h"
#include "ai.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void validate_fleet(const Board *b)
{
    int i, j, a, c;
    CHECK(b->ships_placed == FLEET_SIZE, "placed %d", b->ships_placed);
    for (i = 0; i < FLEET_SIZE; i++)
        for (j = i + 1; j < FLEET_SIZE; j++)
            for (a = 0; a < b->ships[i].len; a++)
                for (c = 0; c < b->ships[j].len; c++) {
                    int dx = ship_cell_x(&b->ships[i], a) - ship_cell_x(&b->ships[j], c);
                    int dy = ship_cell_y(&b->ships[i], a) - ship_cell_y(&b->ships[j], c);
                    CHECK(abs(dx) > 1 || abs(dy) > 1, "ships %d/%d touch", i, j);
                }
}

int main(void)
{
    Rng r;
    Board b;
    int g, games = 5000, total_shots = 0, worst = 0, best = 1000;
    rng_seed(&r, 12345);

    /* Placement rules */
    board_clear(&b);
    CHECK(board_place(&b, 0, 0, 0, 1), "place 4 at A1");
    CHECK(!board_can_place(&b, 4, 1, 1, 1), "diagonal touch must be illegal");
    CHECK(!board_can_place(&b, 0, 1, 1, 1), "vertical touch must be illegal");
    CHECK(board_can_place(&b, 5, 0, 1, 1), "gap of one is legal");
    CHECK(!board_can_place(&b, 8, 5, 3, 1), "out of bounds");
    CHECK(!board_can_place(&b, 5, 8, 3, 0), "out of bounds vertical");
    board_unplace(&b, 0);
    CHECK(b.ships_placed == 0 && board_can_place(&b, 0, 1, 1, 1), "unplace frees cells");

    /* Sinking marks buffer */
    board_clear(&b);
    board_place(&b, 6, 5, 5, 1); /* sub */
    {
        Ship s; Radar rd; ShotResult res = board_receive_shot(&b, 5, 5, &s);
        CHECK(res == RES_SUNK, "sub sunk");
        radar_clear(&rd);
        CHECK(radar_apply(&rd, 5, 5, res, &s) == 8, "8 buffer cells auto-marked");
        CHECK(board_receive_shot(&b, 5, 5, NULL) == RES_INVALID, "repeat shot invalid");
    }

    /* Randomizer + AI full games */
    for (g = 0; g < games; g++) {
        Ai ai; int shots = 0;
        CHECK(board_randomize(&b, &r), "randomize");
        validate_fleet(&b);
        ai_init(&ai, (uint32_t)(g * 7919 + 1));
        while (!board_all_sunk(&b) && shots < 200) {
            int x, y; Ship s; ShotResult res;
            ai_choose(&ai, &x, &y);
            CHECK(radar_can_fire(&ai.radar, x, y), "AI chose fired cell %d,%d", x, y);
            res = board_receive_shot(&b, x, y, &s);
            CHECK(res != RES_INVALID, "AI invalid shot game %d", g);
            if (res == RES_INVALID) break;
            /* AI must never fire into guaranteed water */
            CHECK(!ai.water[y][x], "AI fired into known water %d,%d", x, y);
            ai_notify(&ai, x, y, res, res == RES_SUNK ? &s : NULL);
            shots++;
        }
        CHECK(board_all_sunk(&b), "game %d not finished in %d shots", g, shots);
        CHECK(radar_all_sunk(&ai.radar), "radar sunk count");
        total_shots += shots;
        if (shots > worst) worst = shots;
        if (shots < best) best = shots;
        if (fails > 20) break;
    }
    printf("AI: %d games, avg %.1f shots (best %d, worst %d) to sink 20 ship cells\n",
           g, (double)total_shots / g, best, worst);

    /* Baseline: random shooter for comparison */
    {
        int tot = 0;
        for (g = 0; g < 500; g++) {
            int order[100], i, shots = 0;
            board_randomize(&b, &r);
            for (i = 0; i < 100; i++) order[i] = i;
            for (i = 99; i > 0; i--) { int j = rng_range(&r, i + 1), t = order[i]; order[i] = order[j]; order[j] = t; }
            for (i = 0; i < 100 && !board_all_sunk(&b); i++) {
                board_receive_shot(&b, order[i] % 10, order[i] / 10, NULL); shots++;
            }
            tot += shots;
        }
        printf("Random shooter baseline: avg %.1f shots\n", tot / 500.0);
    }

    printf(fails ? "%d FAILURES\n" : "ALL TESTS PASSED\n", fails);
    return fails != 0;
}
