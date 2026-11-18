/*
 * render.h - OLED-styled vita2d UI, animations, touch & button input.
 */
#ifndef BS_RENDER_H
#define BS_RENDER_H

#include <stdint.h>
#include "app.h"

/* ---------------------------------------------------------- palette */
/* vita2d colours are ABGR (RGBA8 macro). */
#define C_RGBA(r, g, b, a) ((((uint32_t)(a) & 0xFF) << 24) | (((uint32_t)(b) & 0xFF) << 16) | \
                            (((uint32_t)(g) & 0xFF) << 8) | ((uint32_t)(r) & 0xFF))
#define COL_BLACK   C_RGBA(0x00, 0x00, 0x00, 0xFF)
#define COL_CYAN    C_RGBA(0x00, 0xF0, 0xFF, 0xFF)
#define COL_CORAL   C_RGBA(0xFF, 0x3B, 0x5C, 0xFF)
#define COL_AMBER   C_RGBA(0xFF, 0x9F, 0x1C, 0xFF)
#define COL_SLATE   C_RGBA(0x1A, 0x26, 0x39, 0xFF)
#define COL_GREEN   C_RGBA(0x39, 0xFF, 0x88, 0xFF)
#define COL_WHITE   C_RGBA(0xE8, 0xF4, 0xFF, 0xFF)
#define COL_DIM     C_RGBA(0x5A, 0x6E, 0x88, 0xFF)

static inline uint32_t col_alpha(uint32_t c, float a)
{
    uint32_t aa;
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    aa = (uint32_t)(((c >> 24) & 0xFF) * a);
    return (c & 0x00FFFFFFu) | (aa << 24);
}

/* ---------------------------------------------------------- layout */
#define GRID_CELL   36
#define GRID_PX     (GRID_CELL * GRID_N)
#define GRID_TOP    126
#define GRID_L_X    66
#define GRID_R_X    534

typedef enum { SIDE_ME = 0, SIDE_ENEMY = 1 } GridSide;

/* ---------------------------------------------------------- input */
#define BTN_UP_     (1u << 0)
#define BTN_DOWN_   (1u << 1)
#define BTN_LEFT_   (1u << 2)
#define BTN_RIGHT_  (1u << 3)

typedef struct {
    uint32_t held;       /* SceCtrl buttons currently down            */
    uint32_t pressed;    /* SceCtrl buttons that went down this frame */
    uint32_t nav;        /* BTN_*_ directions with auto-repeat (dpad + stick) */
    int      tap;        /* front touch went down this frame          */
    int      tx, ty;     /* tap position in screen pixels             */
    int      touching;
} Input;

void input_init(void);
void input_poll(Input *in);

/* ---------------------------------------------------------- render */
int  render_init(void);
void render_shutdown(void);
void render_frame(const App *app, float dt);
#define LOBBY_MAX_SHOWN 5
/* lobby info supplied by main (keeps render independent of the net layer) */
void render_set_lobby(const char *status, int nhosts, const char *const *names);

/* effects (positions in grid cells) */
void fx_shockwave(GridSide side, int cx, int cy, int big);
void fx_splash(GridSide side, int cx, int cy);
void fx_shake(float amount);
void fx_sunk_ship(GridSide side, const Ship *s);
void fx_reset(void);

/* hit-testing for touch */
int  hit_grid(GridSide side, int px, int py, int *cx, int *cy);
int  hit_menu(int count, int px, int py);          /* main menu items           */
int  hit_join_list(int count, int px, int py);     /* lobby host list           */
int  hit_setup_button(int px, int py);             /* SetupButton               */
int  hit_gameover(int px, int py);                 /* 0 rematch, 1 menu         */

#endif
