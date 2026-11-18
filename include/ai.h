/*
 * ai.h - "Hunt & Target" single-player opponent.
 */
#ifndef BS_AI_H
#define BS_AI_H

#include "game_logic.h"

typedef enum { AI_MODE_HUNT = 0, AI_MODE_TARGET = 1 } AiMode;

typedef struct {
    Radar   radar;                    /* what the AI knows about the player */
    uint8_t water[GRID_N][GRID_N];    /* cells inferred/known empty         */
    uint8_t parity_seed;
    AiMode  mode;
    Rng     rng;
} Ai;

void ai_init(Ai *ai, uint32_t seed);
/* Pick the next cell to fire at. Always returns a legal, un-fired cell. */
void ai_choose(Ai *ai, int *out_x, int *out_y);
/* Feed back the result of the AI's last shot. */
void ai_notify(Ai *ai, int x, int y, ShotResult res, const Ship *sunk);

#endif
