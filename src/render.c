/*
 * render.c - OLED-styled vita2d UI, animations ("juice"), and the
 * touch + button input layer.
 *
 * Pure black background, neon cyan for the player / UI, coral for hits,
 * amber for warnings & turn banner, slate-blue grid lines.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <vita2d.h>
#include "render.h"
#include "audio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static vita2d_pgf *g_font;

/* =================================================================
 *                              INPUT
 * ================================================================= */
static uint32_t g_prev_buttons;
static int      g_prev_touch;
static int      g_rep_dir;     /* direction mask being repeated */
static int      g_rep_timer;

#define REP_DELAY 14
#define REP_RATE  5
#define STICK_DEAD 50

void input_init(void)
{
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    g_prev_buttons = 0;
    g_prev_touch = 0;
}

void input_poll(Input *in)
{
    SceCtrlData pad;
    SceTouchData touch;
    uint32_t dir = 0;
    int lx, ly;

    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    in->held = pad.buttons;
    in->pressed = pad.buttons & ~g_prev_buttons;
    g_prev_buttons = pad.buttons;

    /* digital + analog directions, merged, with typematic repeat */
    lx = (int)pad.lx - 128;
    ly = (int)pad.ly - 128;
    if ((pad.buttons & SCE_CTRL_UP)    || ly < -STICK_DEAD) dir |= BTN_UP_;
    if ((pad.buttons & SCE_CTRL_DOWN)  || ly >  STICK_DEAD) dir |= BTN_DOWN_;
    if ((pad.buttons & SCE_CTRL_LEFT)  || lx < -STICK_DEAD) dir |= BTN_LEFT_;
    if ((pad.buttons & SCE_CTRL_RIGHT) || lx >  STICK_DEAD) dir |= BTN_RIGHT_;

    in->nav = 0;
    if (dir != (uint32_t)g_rep_dir) {
        in->nav = dir & ~(uint32_t)g_rep_dir;   /* newly pressed directions fire at once */
        g_rep_dir = (int)dir;
        g_rep_timer = REP_DELAY;
    } else if (dir) {
        /* deeper stick deflection repeats faster ("smooth" cursor) */
        int deflect = (abs(lx) > abs(ly) ? abs(lx) : abs(ly));
        int rate = deflect > 115 ? 3 : REP_RATE;
        if (--g_rep_timer <= 0) { in->nav = dir; g_rep_timer = rate; }
    }

    memset(&touch, 0, sizeof(touch));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    in->touching = touch.reportNum > 0;
    in->tap = in->touching && !g_prev_touch;
    if (in->touching) {
        /* front panel reports 1920x1088 -> 960x544 */
        in->tx = touch.report[0].x / 2;
        in->ty = touch.report[0].y / 2;
    }
    g_prev_touch = in->touching;
}

/* =================================================================
 *                            EFFECTS
 * ================================================================= */
#define MAX_RINGS 24
#define MAX_PARTS 96

typedef struct { float x, y, t, life, maxr; uint32_t col; int active; } Ring;
typedef struct { float x, y, vx, vy, t, life; uint32_t col; int active; } Part;

static Ring  g_rings[MAX_RINGS];
static Part  g_parts[MAX_PARTS];
static float g_shake;
static float g_off_x, g_off_y;           /* current shake offset */
static uint32_t g_fx_rng = 0xC0FFEEu;

static float frand(void)
{
    g_fx_rng ^= g_fx_rng << 13; g_fx_rng ^= g_fx_rng >> 17; g_fx_rng ^= g_fx_rng << 5;
    return (float)(g_fx_rng & 0xFFFF) / 65535.0f;
}

static float grid_x(GridSide s) { return (float)(s == SIDE_ME ? GRID_L_X : GRID_R_X); }
static float cell_cx(GridSide s, int cx) { return grid_x(s) + cx * GRID_CELL + GRID_CELL * 0.5f; }
static float cell_cy(int cy) { return (float)GRID_TOP + cy * GRID_CELL + GRID_CELL * 0.5f; }

static void add_ring(float x, float y, float maxr, float life, uint32_t col)
{
    int i;
    for (i = 0; i < MAX_RINGS; i++)
        if (!g_rings[i].active) {
            g_rings[i].x = x; g_rings[i].y = y; g_rings[i].t = 0;
            g_rings[i].life = life; g_rings[i].maxr = maxr; g_rings[i].col = col;
            g_rings[i].active = 1;
            return;
        }
}

static void add_parts(float x, float y, int n, float speed, uint32_t col)
{
    int i;
    for (i = 0; i < MAX_PARTS && n > 0; i++)
        if (!g_parts[i].active) {
            float a = frand() * 2.0f * (float)M_PI, v = speed * (0.35f + frand() * 0.65f);
            g_parts[i].x = x; g_parts[i].y = y;
            g_parts[i].vx = cosf(a) * v; g_parts[i].vy = sinf(a) * v;
            g_parts[i].t = 0; g_parts[i].life = 0.35f + frand() * 0.45f;
            g_parts[i].col = col; g_parts[i].active = 1;
            n--;
        }
}

void fx_reset(void)
{
    memset(g_rings, 0, sizeof(g_rings));
    memset(g_parts, 0, sizeof(g_parts));
    g_shake = 0;
}

void fx_shake(float amount) { if (amount > g_shake) g_shake = amount; }

void fx_shockwave(GridSide side, int cx, int cy, int big)
{
    float x = cell_cx(side, cx), y = cell_cy(cy);
    add_ring(x, y, big ? 120.0f : 60.0f, big ? 0.9f : 0.55f, COL_CORAL);
    add_ring(x, y, big ? 80.0f : 34.0f, big ? 0.6f : 0.35f, COL_AMBER);
    add_parts(x, y, big ? 40 : 18, big ? 260.0f : 170.0f, COL_CORAL);
    add_parts(x, y, big ? 16 : 8, big ? 200.0f : 120.0f, COL_AMBER);
    fx_shake(big ? 9.0f : 4.0f);
}

void fx_splash(GridSide side, int cx, int cy)
{
    float x = cell_cx(side, cx), y = cell_cy(cy);
    add_ring(x, y, 30.0f, 0.5f, COL_CYAN);
    add_ring(x, y, 18.0f, 0.35f, COL_WHITE);
    add_parts(x, y, 8, 90.0f, col_alpha(COL_CYAN, 0.8f));
}

void fx_sunk_ship(GridSide side, const Ship *s)
{
    int i;
    for (i = 0; i < s->len; i++) {
        float x = cell_cx(side, ship_cell_x(s, i)), y = cell_cy(ship_cell_y(s, i));
        add_ring(x, y, 70.0f + 15.0f * i, 0.8f + 0.1f * i, COL_CORAL);
        add_parts(x, y, 10, 220.0f, COL_AMBER);
    }
    fx_shake(12.0f);
}

static void fx_update(float dt)
{
    int i;
    for (i = 0; i < MAX_RINGS; i++)
        if (g_rings[i].active && (g_rings[i].t += dt) >= g_rings[i].life) g_rings[i].active = 0;
    for (i = 0; i < MAX_PARTS; i++)
        if (g_parts[i].active) {
            Part *p = &g_parts[i];
            p->t += dt;
            p->x += p->vx * dt; p->y += p->vy * dt;
            p->vx *= 1.0f - 3.0f * dt; p->vy *= 1.0f - 3.0f * dt;
            if (p->t >= p->life) p->active = 0;
        }
    if (g_shake > 0.05f) {
        g_off_x = (frand() * 2.0f - 1.0f) * g_shake;
        g_off_y = (frand() * 2.0f - 1.0f) * g_shake;
        g_shake *= expf(-dt * 9.0f);
    } else {
        g_shake = 0; g_off_x = g_off_y = 0;
    }
}

/* =================================================================
 *                        DRAWING PRIMITIVES
 * ================================================================= */
static void rect(float x, float y, float w, float h, uint32_t c) { vita2d_draw_rectangle(x, y, w, h, c); }

static void frame_rect(float x, float y, float w, float h, float t, uint32_t c)
{
    rect(x, y, w, t, c);
    rect(x, y + h - t, w, t, c);
    rect(x, y + t, t, h - 2 * t, c);
    rect(x + w - t, y + t, t, h - 2 * t, c);
}

static void glow_rect(float x, float y, float w, float h, uint32_t c, float strength)
{
    int i;
    for (i = 3; i >= 1; i--)
        frame_rect(x - i * 2, y - i * 2, w + i * 4, h + i * 4, 2, col_alpha(c, strength * 0.12f * (4 - i)));
    frame_rect(x, y, w, h, 2, c);
}

static void circle_outline(float x, float y, float r, uint32_t c, int seg)
{
    int i;
    float px = x + r, py = y;
    for (i = 1; i <= seg; i++) {
        float a = (float)i / seg * 2.0f * (float)M_PI;
        float nx = x + cosf(a) * r, ny = y + sinf(a) * r;
        vita2d_draw_line(px, py, nx, ny, c);
        px = nx; py = ny;
    }
}

static void thick_line(float x0, float y0, float x1, float y1, float w, uint32_t c)
{
    vita2d_color_vertex *v = (vita2d_color_vertex *)vita2d_pool_memalign(6 * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy), nx, ny;
    if (!v || l < 0.001f) return;
    nx = -dy / l * w * 0.5f; ny = dx / l * w * 0.5f;
    v[0].x = x0 + nx; v[0].y = y0 + ny;
    v[1].x = x0 - nx; v[1].y = y0 - ny;
    v[2].x = x1 + nx; v[2].y = y1 + ny;
    v[3].x = x1 + nx; v[3].y = y1 + ny;
    v[4].x = x0 - nx; v[4].y = y0 - ny;
    v[5].x = x1 - nx; v[5].y = y1 - ny;
    { int i; for (i = 0; i < 6; i++) { v[i].z = 0.5f; v[i].color = c; } }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, 6);
}

static void ring_thick(float x, float y, float r, float w, uint32_t c)
{
    const int seg = 40;
    int i;
    vita2d_color_vertex *v = (vita2d_color_vertex *)vita2d_pool_memalign(seg * 6 * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    if (!v) return;
    for (i = 0; i < seg; i++) {
        float a0 = (float)i / seg * 2.0f * (float)M_PI, a1 = (float)(i + 1) / seg * 2.0f * (float)M_PI;
        float ri = r - w * 0.5f, ro = r + w * 0.5f;
        vita2d_color_vertex *q = &v[i * 6];
        if (ri < 0) ri = 0;
        q[0].x = x + cosf(a0) * ri; q[0].y = y + sinf(a0) * ri;
        q[1].x = x + cosf(a0) * ro; q[1].y = y + sinf(a0) * ro;
        q[2].x = x + cosf(a1) * ro; q[2].y = y + sinf(a1) * ro;
        q[3] = q[0]; q[4] = q[2];
        q[5].x = x + cosf(a1) * ri; q[5].y = y + sinf(a1) * ri;
        { int k; for (k = 0; k < 6; k++) { q[k].z = 0.5f; q[k].color = c; } }
    }
    vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, seg * 6);
}

static int text_w(float scale, const char *s) { return vita2d_pgf_text_width(g_font, scale, s); }

static void text(float x, float y, uint32_t c, float scale, const char *s)
{
    vita2d_pgf_draw_text(g_font, (int)x, (int)y, c, scale, s);
}

static void text_c(float cx, float y, uint32_t c, float scale, const char *s)
{
    text(cx - text_w(scale, s) * 0.5f, y, c, scale, s);
}

/* letter-spaced text for headings */
static void text_spaced_c(float cx, float y, uint32_t c, float scale, float spacing, const char *s)
{
    char ch[2] = { 0, 0 };
    float total = 0, x;
    const char *p;
    for (p = s; *p; p++) { ch[0] = *p; total += text_w(scale, ch) + spacing; }
    total -= spacing;
    x = cx - total * 0.5f;
    for (p = s; *p; p++) {
        ch[0] = *p;
        text(x, y, c, scale, ch);
        x += text_w(scale, ch) + spacing;
    }
}

static void text_glow_c(float cx, float y, uint32_t c, float scale, float spacing, const char *s)
{
    static const int o[8][2] = { {-2,0},{2,0},{0,-2},{0,2},{-1,-1},{1,1},{-1,1},{1,-1} };
    int i;
    for (i = 0; i < 8; i++) text_spaced_c(cx + o[i][0], y + o[i][1], col_alpha(c, 0.18f), scale, spacing, s);
    text_spaced_c(cx, y, c, scale, spacing, s);
}

/* PlayStation face-button glyphs drawn with primitives */
typedef enum { G_CROSS, G_CIRCLE, G_TRIANGLE, G_SQUARE, G_LR, G_START } Glyph;
static float glyph(float x, float y, Glyph g)
{
    float r = 8.0f, cx = x + r, cy = y - 6.0f;
    circle_outline(cx, cy, r + 1.5f, COL_DIM, 20);
    switch (g) {
    case G_CROSS:
        thick_line(cx - 4, cy - 4, cx + 4, cy + 4, 2, C_RGBA(0x7C, 0xB2, 0xE8, 0xFF));
        thick_line(cx - 4, cy + 4, cx + 4, cy - 4, 2, C_RGBA(0x7C, 0xB2, 0xE8, 0xFF));
        break;
    case G_CIRCLE:   circle_outline(cx, cy, 4.5f, C_RGBA(0xFF, 0x66, 0x66, 0xFF), 16); break;
    case G_TRIANGLE:
        vita2d_draw_line(cx, cy - 5, cx - 5, cy + 4, C_RGBA(0x40, 0xE0, 0xA0, 0xFF));
        vita2d_draw_line(cx - 5, cy + 4, cx + 5, cy + 4, C_RGBA(0x40, 0xE0, 0xA0, 0xFF));
        vita2d_draw_line(cx + 5, cy + 4, cx, cy - 5, C_RGBA(0x40, 0xE0, 0xA0, 0xFF));
        break;
    case G_SQUARE:   frame_rect(cx - 4, cy - 4, 9, 9, 1, C_RGBA(0xF0, 0x80, 0xD0, 0xFF)); break;
    case G_LR:       text(cx - 7, cy + 5, COL_WHITE, 0.55f, "LR"); break;
    case G_START:    rect(cx - 5, cy - 2, 10, 4, COL_WHITE); break;
    }
    return x + 2 * r + 8;
}

static float hint(float x, float y, Glyph g, const char *label)
{
    x = glyph(x, y, g);
    text(x, y, COL_DIM, 0.72f, label);
    return x + text_w(0.72f, label) + 22;
}

/* =================================================================
 *                         GRID RENDERING
 * ================================================================= */
static void draw_grid_frame(GridSide side, uint32_t accent, const char *title)
{
    float gx = grid_x(side) + g_off_x, gy = GRID_TOP + g_off_y;
    int i;
    char lbl[4];
    /* backing tint so the grid reads on pure black without killing OLED blacks */
    rect(gx, gy, GRID_PX, GRID_PX, C_RGBA(0x05, 0x0A, 0x12, 0xFF));
    for (i = 0; i <= GRID_N; i++) {
        uint32_t c = (i == 0 || i == GRID_N) ? col_alpha(accent, 0.55f) : COL_SLATE;
        rect(gx + i * GRID_CELL, gy, 1, GRID_PX + 1, c);
        rect(gx, gy + i * GRID_CELL, GRID_PX + 1, 1, c);
    }
    for (i = 0; i < GRID_N; i++) {
        lbl[0] = (char)('A' + i); lbl[1] = 0;
        text_c(gx + i * GRID_CELL + GRID_CELL / 2, gy - 6, COL_DIM, 0.7f, lbl);
        snprintf(lbl, sizeof(lbl), "%d", i + 1);
        text(gx - 8 - text_w(0.7f, lbl), gy + i * GRID_CELL + 25, COL_DIM, 0.7f, lbl);
    }
    text_spaced_c(gx + GRID_PX / 2, gy - 30, accent, 0.9f, 3.0f, title);
}

static void draw_ship_body(GridSide side, const Ship *s, uint32_t fill, uint32_t edge)
{
    float gx = grid_x(side) + g_off_x, gy = GRID_TOP + g_off_y;
    float x = gx + s->x * GRID_CELL + 4, y = gy + s->y * GRID_CELL + 4;
    float w = (s->horiz ? s->len : 1) * GRID_CELL - 7;
    float h = (s->horiz ? 1 : s->len) * GRID_CELL - 7;
    rect(x, y, w, h, fill);
    frame_rect(x, y, w, h, 2, edge);
    /* "bow" notch + deck line for a bit of naval character */
    if (s->len > 1) {
        if (s->horiz) rect(x + 6, y + h / 2 - 1, w - 12, 2, col_alpha(edge, 0.45f));
        else          rect(x + w / 2 - 1, y + 6, 2, h - 12, col_alpha(edge, 0.45f));
    } else {
        rect(x + w / 2 - 3, y + h / 2 - 3, 6, 6, col_alpha(edge, 0.6f));
    }
}

static void draw_cell_miss(GridSide side, int cx, int cy, int faint)
{
    float x = cell_cx(side, cx) + g_off_x, y = cell_cy(cy) + g_off_y;
    if (faint) vita2d_draw_fill_circle(x, y, 2.0f, C_RGBA(0x3A, 0x4E, 0x6C, 0xFF));
    else {
        vita2d_draw_fill_circle(x, y, 4.0f, C_RGBA(0x6A, 0x86, 0xA8, 0xFF));
        circle_outline(x, y, 8.0f, C_RGBA(0x2A, 0x3C, 0x58, 0xFF), 16);
    }
}

static void draw_cell_hit(GridSide side, int cx, int cy, float t)
{
    float x = cell_cx(side, cx) + g_off_x, y = cell_cy(cy) + g_off_y;
    float pulse = 0.6f + 0.4f * sinf(t * 6.0f + cx + cy);
    rect(x - 15, y - 15, 30, 30, col_alpha(COL_CORAL, 0.18f * pulse));
    thick_line(x - 9, y - 9, x + 9, y + 9, 4, COL_CORAL);
    thick_line(x - 9, y + 9, x + 9, y - 9, 4, COL_CORAL);
}

static void draw_cursor(GridSide side, int cx, int cy, int len, int horiz, uint32_t c, float t)
{
    float gx = grid_x(side) + g_off_x, gy = GRID_TOP + g_off_y;
    float x = gx + cx * GRID_CELL, y = gy + cy * GRID_CELL;
    float w = (horiz ? len : 1) * GRID_CELL, h = (horiz ? 1 : len) * GRID_CELL;
    float k = 9.0f, pad = 2.0f + 2.0f * sinf(t * 7.0f);
    uint32_t cc = col_alpha(c, 0.85f + 0.15f * sinf(t * 7.0f));
    x -= pad; y -= pad; w += pad * 2; h += pad * 2;
    rect(x, y, k, 3, cc); rect(x, y, 3, k, cc);
    rect(x + w - k, y, k, 3, cc); rect(x + w - 3, y, 3, k, cc);
    rect(x, y + h - 3, k, 3, cc); rect(x, y + h - k, 3, k, cc);
    rect(x + w - k, y + h - 3, k, 3, cc); rect(x + w - 3, y + h - k, 3, k, cc);
}

static void draw_radar_sweep(float t)
{
    float cx = GRID_R_X + GRID_PX * 0.5f + g_off_x, cy = GRID_TOP + GRID_PX * 0.5f + g_off_y;
    float R = GRID_PX * 0.72f;
    float ang = fmodf(t * 1.1f, 2.0f * (float)M_PI);
    const int seg = 24;
    const float trail = 0.9f;
    int i;
    vita2d_color_vertex *v;

    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(GRID_R_X + 1 + (int)g_off_x, GRID_TOP + 1 + (int)g_off_y,
                              GRID_R_X + GRID_PX + (int)g_off_x, GRID_TOP + GRID_PX + (int)g_off_y);

    /* range rings */
    for (i = 1; i <= 3; i++) circle_outline(cx, cy, GRID_PX * 0.17f * i, C_RGBA(0x0E, 0x2C, 0x36, 0xFF), 48);

    /* trailing wedge, alpha falls off away from the sweep line */
    v = (vita2d_color_vertex *)vita2d_pool_memalign(seg * 3 * sizeof(vita2d_color_vertex), sizeof(vita2d_color_vertex));
    if (v) {
        for (i = 0; i < seg; i++) {
            float a0 = ang - trail * (float)i / seg, a1 = ang - trail * (float)(i + 1) / seg;
            float al0 = 0.22f * (1.0f - (float)i / seg), al1 = 0.22f * (1.0f - (float)(i + 1) / seg);
            vita2d_color_vertex *q = &v[i * 3];
            q[0].x = cx; q[0].y = cy; q[0].color = col_alpha(COL_CYAN, al0);
            q[1].x = cx + cosf(a0) * R; q[1].y = cy + sinf(a0) * R; q[1].color = col_alpha(COL_CYAN, al0);
            q[2].x = cx + cosf(a1) * R; q[2].y = cy + sinf(a1) * R; q[2].color = col_alpha(COL_CYAN, al1);
            q[0].z = q[1].z = q[2].z = 0.5f;
        }
        vita2d_draw_array(SCE_GXM_PRIMITIVE_TRIANGLES, v, seg * 3);
    }
    thick_line(cx, cy, cx + cosf(ang) * R, cy + sinf(ang) * R, 2, col_alpha(COL_CYAN, 0.65f));
    vita2d_disable_clipping();
}

static void draw_my_board(const App *a, int show_ghost)
{
    int x, y, i;
    const Board *b = &a->me;
    draw_grid_frame(SIDE_ME, COL_CYAN, "YOUR FLEET");
    for (i = 0; i < FLEET_SIZE; i++) {
        const Ship *s = &b->ships[i];
        if (!s->placed) continue;
        if (s->hits >= s->len) draw_ship_body(SIDE_ME, s, col_alpha(COL_CORAL, 0.20f), col_alpha(COL_CORAL, 0.7f));
        else                   draw_ship_body(SIDE_ME, s, col_alpha(COL_CYAN, 0.16f), COL_CYAN);
    }
    for (y = 0; y < GRID_N; y++)
        for (x = 0; x < GRID_N; x++) {
            if (b->shot[y][x] == SHOT_HIT) draw_cell_hit(SIDE_ME, x, y, a->time);
            else if (b->shot[y][x] == SHOT_MISS) draw_cell_miss(SIDE_ME, x, y, 0);
        }
    if (a->state == ST_BATTLE && a->last_in_x >= 0)
        draw_cursor(SIDE_ME, a->last_in_x, a->last_in_y, 1, 1, col_alpha(COL_AMBER, 0.6f), a->time);

    if (show_ghost) {
        int idx = -1;
        for (i = 0; i < FLEET_SIZE; i++) if (!b->ships[i].placed) { idx = i; break; }
        if (idx >= 0) {
            Ship g;
            int ok;
            g.x = (int8_t)a->cur_x; g.y = (int8_t)a->cur_y; g.len = b->ships[idx].len; g.horiz = (uint8_t)a->cur_horiz;
            ok = board_can_place(b, a->cur_x, a->cur_y, g.len, g.horiz);
            /* buffer zone preview */
            if (ok) {
                for (i = 0; i < g.len; i++) {
                    int dx, dy;
                    for (dy = -1; dy <= 1; dy++)
                        for (dx = -1; dx <= 1; dx++) {
                            int nx = ship_cell_x(&g, i) + dx, ny = ship_cell_y(&g, i) + dy;
                            if (in_grid(nx, ny))
                                rect(GRID_L_X + nx * GRID_CELL + 1, GRID_TOP + ny * GRID_CELL + 1,
                                     GRID_CELL - 1, GRID_CELL - 1, col_alpha(COL_GREEN, 0.05f));
                        }
                }
            }
            draw_ship_body(SIDE_ME, &g, col_alpha(ok ? COL_GREEN : COL_CORAL, 0.30f), ok ? COL_GREEN : COL_CORAL);
            draw_cursor(SIDE_ME, a->cur_x, a->cur_y, g.len, g.horiz, ok ? COL_GREEN : COL_CORAL, a->time);
        }
    }
}

static void draw_radar(const App *a, int show_cursor)
{
    int x, y, i;
    draw_grid_frame(SIDE_ENEMY, COL_CORAL, "ENEMY RADAR");
    draw_radar_sweep(a->time);
    for (i = 0; i < a->radar.sunk_count; i++)
        draw_ship_body(SIDE_ENEMY, &a->radar.sunk_ships[i], col_alpha(COL_CORAL, 0.25f), COL_CORAL);
    for (y = 0; y < GRID_N; y++)
        for (x = 0; x < GRID_N; x++) {
            switch (a->radar.cell[y][x]) {
            case RADAR_MISS: draw_cell_miss(SIDE_ENEMY, x, y, 0); break;
            case RADAR_AUTO: draw_cell_miss(SIDE_ENEMY, x, y, 1); break;
            case RADAR_HIT:  draw_cell_hit(SIDE_ENEMY, x, y, a->time); break;
            case RADAR_SUNK: draw_cell_hit(SIDE_ENEMY, x, y, 0); break;
            default: break;
            }
        }
    if (a->shot_pending) {
        /* incoming-shell marker */
        float px = cell_cx(SIDE_ENEMY, a->shot_x) + g_off_x, py = cell_cy(a->shot_y) + g_off_y;
        float k = 1.0f - a->shot_timer / 0.45f;
        if (k < 0) k = 0;
        circle_outline(px, py, 4.0f + 14.0f * (1.0f - k), COL_AMBER, 24);
    }
    if (show_cursor) {
        int can = radar_can_fire(&a->radar, a->aim_x, a->aim_y);
        draw_cursor(SIDE_ENEMY, a->aim_x, a->aim_y, 1, 1, can ? COL_AMBER : COL_DIM, a->time);
        if (can) {
            float px = cell_cx(SIDE_ENEMY, a->aim_x) + g_off_x, py = cell_cy(a->aim_y) + g_off_y;
            vita2d_draw_line(px - 6, py, px + 6, py, COL_AMBER);
            vita2d_draw_line(px, py - 6, px, py + 6, COL_AMBER);
        }
    }
}

static void draw_fx(void)
{
    int i;
    for (i = 0; i < MAX_RINGS; i++)
        if (g_rings[i].active) {
            Ring *r = &g_rings[i];
            float k = r->t / r->life, e = 1.0f - (1.0f - k) * (1.0f - k);
            ring_thick(r->x + g_off_x, r->y + g_off_y, 4 + r->maxr * e, 3.0f * (1.0f - k) + 1.0f, col_alpha(r->col, 1.0f - k));
        }
    for (i = 0; i < MAX_PARTS; i++)
        if (g_parts[i].active) {
            Part *p = &g_parts[i];
            float k = 1.0f - p->t / p->life;
            rect(p->x + g_off_x - 1.5f, p->y + g_off_y - 1.5f, 3, 3, col_alpha(p->col, k));
        }
}

/* =================================================================
 *                               HUD
 * ================================================================= */
static void draw_silhouettes(float x, float y, const uint8_t *sunk_by_len, uint32_t col, int right_align)
{
    /* ships drawn biggest -> smallest; sunk ones are hollow & coral */
    const int cw = 8, gap = 2, sgap = 9;
    int i, total = 0, sunk_left[MAX_SHIP_LEN + 1];
    float cx;
    for (i = 0; i <= MAX_SHIP_LEN; i++) sunk_left[i] = sunk_by_len[i];
    for (i = 0; i < FLEET_SIZE; i++) total += FLEET_LENGTHS[i] * (cw + gap) - gap + sgap;
    total -= sgap;
    cx = right_align ? x - total : x;
    for (i = 0; i < FLEET_SIZE; i++) {
        int len = FLEET_LENGTHS[i];
        float w = (float)(len * (cw + gap) - gap);
        int dead = 0;
        if (sunk_left[len] > 0) { dead = 1; sunk_left[len]--; }
        if (dead) frame_rect(cx, y, w, cw, 1, col_alpha(COL_CORAL, 0.6f));
        else {
            rect(cx, y, w, cw, col_alpha(col, 0.85f));
        }
        cx += w + sgap;
    }
}

static void draw_hud(const App *a, const char *mode_label, const char *banner, uint32_t banner_col)
{
    uint8_t my_sunk[MAX_SHIP_LEN + 1] = {0};
    int i;
    char opp[48];

    for (i = 0; i < FLEET_SIZE; i++)
        if (a->me.ships[i].placed && a->me.ships[i].hits >= a->me.ships[i].len) my_sunk[a->me.ships[i].len]++;

    rect(0, 0, SCREEN_W, 1, col_alpha(COL_CYAN, 0.25f));
    rect(0, 78, SCREEN_W, 1, COL_SLATE);
    text(20, 26, COL_DIM, 0.68f, mode_label);
    snprintf(opp, sizeof(opp), "VS %s", a->mode == MODE_SINGLE ? "CPU" : a->peer_nick);
    text(SCREEN_W - 20 - text_w(0.68f, opp), 26, COL_DIM, 0.68f, opp);
    text(20, 50, COL_CYAN, 0.8f, a->nick);

    if (banner && banner[0]) {
        int w = text_w(0.95f, banner) + 50;
        float x = SCREEN_W / 2 - w / 2;
        float pulse = 0.75f + 0.25f * sinf(a->time * 4.0f);
        rect(x, 12, (float)w, 40, col_alpha(banner_col, 0.10f * pulse));
        glow_rect(x, 12, (float)w, 40, col_alpha(banner_col, pulse), 1.0f);
        text_c(SCREEN_W / 2, 41, banner_col, 0.95f, banner);
    }

    draw_silhouettes(GRID_L_X, 62, my_sunk, COL_CYAN, 0);
    draw_silhouettes(GRID_R_X + GRID_PX, 62, a->radar.sunk_by_len, COL_CORAL, 1);
}

/* =================================================================
 *                           LAYOUT RECTS
 * ================================================================= */
#define MENU_X   270
#define MENU_W   420
#define MENU_Y   190
#define MENU_H   54
#define MENU_GAP 66

int hit_menu(int count, int px, int py)
{
    int i;
    for (i = 0; i < count; i++) {
        int y = MENU_Y + i * MENU_GAP;
        if (px >= MENU_X && px < MENU_X + MENU_W && py >= y && py < y + MENU_H) return i;
    }
    return -1;
}

#define JOIN_Y 170
int hit_join_list(int count, int px, int py)
{
    int i;
    for (i = 0; i < count; i++) {
        int y = JOIN_Y + i * 58;
        if (px >= MENU_X && px < MENU_X + MENU_W && py >= y && py < y + 48) return i;
    }
    return -1;
}

#define SBTN_X 548
#define SBTN_Y 390
#define SBTN_W 160
#define SBTN_H 42
static void setup_btn_rect(int b, int *x, int *y)
{
    *x = SBTN_X + (b % 2) * (SBTN_W + 12);
    *y = SBTN_Y + (b / 2) * (SBTN_H + 12);
}

int hit_setup_button(int px, int py)
{
    int b;
    for (b = 0; b < BTN_COUNT; b++) {
        int x, y;
        setup_btn_rect(b, &x, &y);
        if (px >= x && px < x + SBTN_W && py >= y && py < y + SBTN_H) return b;
    }
    return BTN_NONE;
}

#define GO_Y 382
int hit_gameover(int px, int py)
{
    int i;
    for (i = 0; i < GAMEOVER_ITEMS; i++) {
        int x = 300 + i * 190;
        if (px >= x && px < x + 170 && py >= GO_Y && py < GO_Y + 46) return i;
    }
    return -1;
}

int hit_grid(GridSide side, int px, int py, int *cx, int *cy)
{
    int gx = (int)grid_x(side);
    if (px < gx || py < GRID_TOP || px >= gx + GRID_PX || py >= GRID_TOP + GRID_PX) return 0;
    *cx = (px - gx) / GRID_CELL;
    *cy = (py - GRID_TOP) / GRID_CELL;
    return 1;
}

static void button(float x, float y, float w, float h, const char *label, int selected, int enabled, uint32_t accent, float t)
{
    uint32_t c = enabled ? accent : COL_DIM;
    if (selected && enabled) {
        float p = 0.75f + 0.25f * sinf(t * 5.0f);
        rect(x, y, w, h, col_alpha(c, 0.14f));
        glow_rect(x, y, w, h, col_alpha(c, p), 1.0f);
        rect(x + 8, y + h / 2 - 3, 6, 6, c);
    } else {
        frame_rect(x, y, w, h, 1, col_alpha(c, enabled ? 0.55f : 0.3f));
    }
    text_c(x + w / 2, y + h / 2 + 8, selected && enabled ? COL_WHITE : c, 0.9f, label);
}

/* =================================================================
 *                             SCREENS
 * ================================================================= */
static void draw_backdrop_sonar(float cx, float cy, float t, uint32_t c)
{
    int i;
    for (i = 0; i < 4; i++) {
        float k = fmodf(t * 0.35f + i * 0.25f, 1.0f);
        circle_outline(cx, cy, 30 + k * 300, col_alpha(c, 0.25f * (1.0f - k)), 64);
    }
}

static void screen_title(const App *a)
{
    float t = a->time;
    draw_backdrop_sonar(SCREEN_W / 2, 250, t, COL_CYAN);
    text_glow_c(SCREEN_W / 2, 230, COL_CYAN, 2.6f, 10.0f, "BATTLESHIP");
    text_spaced_c(SCREEN_W / 2, 272, COL_DIM, 0.9f, 6.0f, "TACTICAL NAVAL COMBAT");
    rect(SCREEN_W / 2 - 160, 290, 320, 1, col_alpha(COL_CYAN, 0.4f));
    if (fmodf(t, 1.2f) < 0.8f) {
        float x = SCREEN_W / 2 - 110;
        x = glyph(x, 380, G_CROSS);
        text(x, 380, COL_AMBER, 1.0f, "PRESS TO START");
    }
    text_c(SCREEN_W / 2, 520, col_alpha(COL_DIM, 0.6f), 0.6f, "PS VITA HOMEBREW  -  SINGLE PLAYER & AD-HOC");
}

static void screen_mode(const App *a)
{
    static const char *items[MENU_ITEMS] = { "SINGLE PLAYER  vs CPU", "HOST AD-HOC GAME", "JOIN AD-HOC GAME", "EXIT" };
    int i;
    float x;
    text_glow_c(SCREEN_W / 2, 110, COL_CYAN, 1.5f, 6.0f, "BATTLESHIP");
    text_c(SCREEN_W / 2, 150, COL_DIM, 0.8f, "SELECT MISSION");
    for (i = 0; i < MENU_ITEMS; i++)
        button(MENU_X, MENU_Y + i * MENU_GAP, MENU_W, MENU_H, items[i], a->menu_sel == i, 1,
               i == MENU_ITEMS - 1 ? COL_CORAL : COL_CYAN, a->time);
    x = 340;
    x = hint(x, 525, G_CROSS, "Select");
    hint(x, 525, G_CIRCLE, "Back");
}

static const char *net_status_text(void);

static void screen_lobby_host(const App *a)
{
    char line[96];
    draw_backdrop_sonar(SCREEN_W / 2, 280, a->time, COL_AMBER);
    text_glow_c(SCREEN_W / 2, 110, COL_AMBER, 1.3f, 5.0f, "AD-HOC ROOM OPEN");
    snprintf(line, sizeof(line), "Host: %s", a->nick);
    text_c(SCREEN_W / 2, 160, COL_CYAN, 1.0f, line);
    text_c(SCREEN_W / 2, 290, COL_WHITE, 1.0f, net_status_text());
    text_c(SCREEN_W / 2, 330, COL_DIM, 0.75f, "The other Vita should pick JOIN AD-HOC GAME");
    hint(SCREEN_W / 2 - 50, 525, G_CIRCLE, "Cancel");
}

static void screen_lobby_join(const App *a, int nhosts, const char *const *names)
{
    int i;
    float x;
    draw_backdrop_sonar(SCREEN_W / 2, 300, a->time, COL_CYAN);
    text_glow_c(SCREEN_W / 2, 100, COL_CYAN, 1.3f, 5.0f, "NEARBY COMMANDERS");
    if (nhosts == 0) {
        int dots = (int)(a->time * 2) % 4;
        char s[32] = "SCANNING";
        for (i = 0; i < dots; i++) strcat(s, ".");
        text_c(SCREEN_W / 2, 280, COL_WHITE, 1.0f, s);
        text_c(SCREEN_W / 2, 320, COL_DIM, 0.75f, "Waiting for a Vita running HOST AD-HOC GAME");
    } else {
        for (i = 0; i < nhosts; i++)
            button(MENU_X, JOIN_Y + i * 58, MENU_W, 48, names[i], a->join_sel == i, 1, COL_CYAN, a->time);
    }
    text_c(SCREEN_W / 2, 480, COL_AMBER, 0.8f, net_status_text());
    x = 360;
    if (nhosts) x = hint(x, 525, G_CROSS, "Join");
    hint(x, 525, G_CIRCLE, "Back");
}

static void screen_setup(const App *a)
{
    static const char *names[MAX_SHIP_LEN + 1] = { "", "SUBMARINE", "DESTROYER", "CRUISER", "BATTLESHIP" };
    static const char *blabels[BTN_COUNT] = { "ROTATE", "RANDOM", "UNDO", "READY" };
    int i, all = a->me.ships_placed >= FLEET_SIZE, cur = -1;
    float x;
    char line[64];

    draw_hud(a, a->mode == MODE_SINGLE ? "MODE: SINGLE PLAYER" : "MODE: AD-HOC",
             a->ready_sent ? "WAITING FOR OPPONENT" : "DEPLOY YOUR FLEET", a->ready_sent ? COL_AMBER : COL_CYAN);
    draw_my_board(a, !all && !a->ready_sent);

    for (i = 0; i < FLEET_SIZE; i++) if (!a->me.ships[i].placed) { cur = i; break; }

    /* right panel: roster */
    text_spaced_c(GRID_R_X + GRID_PX / 2, GRID_TOP - 30, COL_CYAN, 0.9f, 3.0f, "FLEET ROSTER");
    for (i = 0; i < FLEET_SIZE; i++) {
        const Ship *s = &a->me.ships[i];
        float y = GRID_TOP + 6 + i * 24;
        float bx = GRID_R_X + 14;
        int k;
        uint32_t c = s->placed ? COL_DIM : (i == cur ? COL_GREEN : COL_WHITE);
        for (k = 0; k < s->len; k++) {
            if (s->placed) frame_rect(bx + k * 14, y + 4, 12, 12, 1, col_alpha(c, 0.7f));
            else rect(bx + k * 14, y + 4, 12, 12, col_alpha(c, i == cur ? 1.0f : 0.6f));
        }
        text(bx + 70, y + 16, c, 0.7f, names[s->len]);
        if (s->placed) text(bx + 220, y + 16, COL_DIM, 0.65f, "DEPLOYED");
        else if (i == cur) text(bx + 220, y + 16, COL_GREEN, 0.65f, a->cur_horiz ? "< HORIZONTAL >" : "^ VERTICAL v");
    }

    for (i = 0; i < BTN_COUNT; i++) {
        int bx, by, en = 1;
        if (i == BTN_READY) en = all && !a->ready_sent;
        if (i == BTN_ROTATE) en = !all && !a->ready_sent;
        if (i == BTN_UNDO) en = a->place_n > 0 && !a->ready_sent;
        if (i == BTN_RANDOM) en = !a->ready_sent;
        setup_btn_rect(i, &bx, &by);
        button((float)bx, (float)by, SBTN_W, SBTN_H, blabels[i], i == BTN_READY && en, en,
               i == BTN_READY ? COL_GREEN : COL_CYAN, a->time);
    }

    if (a->ready_sent) {
        snprintf(line, sizeof(line), a->peer_ready ? "Starting..." : "Waiting for %s...", a->peer_nick);
        text_c(GRID_R_X + GRID_PX / 2, 506, COL_AMBER, 0.75f, line);
    } else if (a->mode == MODE_ADHOC && a->peer_ready) {
        text_c(GRID_R_X + GRID_PX / 2, 506, COL_GREEN, 0.75f, "Opponent is ready!");
    }

    x = 40;
    if (all) {
        x = hint(x, 528, G_CROSS, "Ready");
    } else {
        x = hint(x, 528, G_CROSS, "Place");
        x = hint(x, 528, G_TRIANGLE, "Rotate");
    }
    x = hint(x, 528, G_SQUARE, "Randomize");
    x = hint(x, 528, G_CIRCLE, a->place_n ? "Undo" : "Back");
    text(x, 528, col_alpha(COL_DIM, 0.7f), 0.65f, "Ships may not touch - not even diagonally");
}

static void screen_battle(const App *a)
{
    const char *banner;
    uint32_t bc;
    float x;
    if (a->state == ST_GAMEOVER) { banner = a->won ? "VICTORY" : "DEFEAT"; bc = a->won ? COL_CYAN : COL_CORAL; }
    else if (a->turn == TURN_ME) { banner = "YOUR TURN  -  FIRE!"; bc = COL_AMBER; }
    else { banner = a->mode == MODE_SINGLE ? "ENEMY TURN  -  CPU TARGETING" : "ENEMY TURN"; bc = COL_CORAL; }

    draw_hud(a, a->mode == MODE_SINGLE ? "MODE: SINGLE PLAYER" : "MODE: AD-HOC", banner, bc);
    draw_my_board(a, 0);
    draw_radar(a, a->state == ST_BATTLE && a->turn == TURN_ME && !a->shot_pending);
    draw_fx();

    if (a->banner_t > 0 && a->banner[0]) {
        float k = a->banner_t > 1.6f ? (2.0f - a->banner_t) / 0.4f : (a->banner_t < 0.4f ? a->banner_t / 0.4f : 1.0f);
        rect(0, 262, SCREEN_W, 52, col_alpha(COL_BLACK, 0.75f * k));
        rect(0, 262, SCREEN_W, 1, col_alpha(a->banner_color, k));
        rect(0, 313, SCREEN_W, 1, col_alpha(a->banner_color, k));
        text_glow_c(SCREEN_W / 2, 298, col_alpha(a->banner_color, k), 1.2f, 4.0f, a->banner);
    }

    if (a->state == ST_BATTLE) {
        x = 40;
        x = hint(x, 528, G_CROSS, "Fire");
        x = hint(x, 528, G_CIRCLE, "Surrender");
        text(x, 528, col_alpha(COL_DIM, 0.7f), 0.65f, "Tap the radar to fire. A hit grants another shot.");
    }
}

static void screen_gameover(const App *a)
{
    static const char *items[GAMEOVER_ITEMS] = { "REMATCH", "MAIN MENU" };
    char line[96];
    int i;
    uint32_t c = a->won ? COL_CYAN : COL_CORAL;
    float acc = a->my_shots ? 100.0f * a->my_hits / a->my_shots : 0;

    screen_battle(a);
    rect(0, 0, SCREEN_W, SCREEN_H, col_alpha(COL_BLACK, 0.72f));
    rect(240, 150, 480, 300, COL_BLACK);
    glow_rect(240, 150, 480, 300, c, 1.0f);
    text_glow_c(SCREEN_W / 2, 225, c, 2.0f, 8.0f, a->won ? "VICTORY" : "DEFEAT");
    text_c(SCREEN_W / 2, 262, COL_DIM, 0.8f, a->won ? "Enemy fleet destroyed" : "Your fleet has been sunk");
    snprintf(line, sizeof(line), "Shots %d   Hits %d   Accuracy %.0f%%", a->my_shots, a->my_hits, acc);
    text_c(SCREEN_W / 2, 310, COL_WHITE, 0.8f, line);
    if (a->mode == MODE_ADHOC && a->peer_left) {
        text_c(SCREEN_W / 2, 350, COL_CORAL, 0.75f, "Opponent has left - rematch unavailable");
    } else if (a->mode == MODE_ADHOC) {
        if (a->rematch_me && !a->rematch_peer) text_c(SCREEN_W / 2, 350, COL_AMBER, 0.75f, "Waiting for opponent to accept rematch...");
        else if (a->rematch_peer) text_c(SCREEN_W / 2, 350, COL_GREEN, 0.75f, "Opponent wants a rematch!");
    }
    for (i = 0; i < GAMEOVER_ITEMS; i++)
        button(300 + i * 190, GO_Y, 170, 46, items[i], a->over_sel == i, !(i == 0 && (a->rematch_me || a->peer_left)), i == 0 ? COL_GREEN : COL_CYAN, a->time);
}

static void draw_alert(const App *a)
{
    float x;
    rect(0, 0, SCREEN_W, SCREEN_H, col_alpha(COL_BLACK, 0.8f));
    rect(200, 180, 560, 190, COL_BLACK);
    glow_rect(200, 180, 560, 190, COL_AMBER, 1.0f);
    text_glow_c(SCREEN_W / 2, 230, COL_AMBER, 1.2f, 4.0f, a->alert_title);
    text_c(SCREEN_W / 2, 278, COL_WHITE, 0.8f, a->alert_msg);
    if (a->alert_confirm) {
        x = SCREEN_W / 2 - 90;
        x = hint(x, 340, G_CROSS, "Yes");
        hint(x, 340, G_CIRCLE, "No");
    } else {
        x = SCREEN_W / 2 - 30;
        hint(x, 340, G_CROSS, "OK");
    }
}

/* lobby text comes from main through these hooks (keeps render.c
 * independent of adhoc_net.c) */
static const char *g_net_status = "";
static int g_nhosts = 0;
static const char *g_host_names[LOBBY_MAX_SHOWN];
void render_set_lobby(const char *status, int nhosts, const char *const *names)
{
    int i;
    g_net_status = status ? status : "";
    g_nhosts = nhosts > LOBBY_MAX_SHOWN ? LOBBY_MAX_SHOWN : nhosts;
    for (i = 0; i < g_nhosts; i++) g_host_names[i] = names[i];
}
static const char *net_status_text(void) { return g_net_status; }

/* =================================================================
 *                              FRAME
 * ================================================================= */
int render_init(void)
{
    if (vita2d_init() < 0) return -1;
    vita2d_set_clear_color(COL_BLACK);
    vita2d_set_vblank_wait(1);                 /* lock to 60 FPS */
    g_font = vita2d_load_default_pgf();
    if (!g_font) return -2;
    fx_reset();
    return 0;
}

void render_shutdown(void)
{
    vita2d_wait_rendering_done();
    if (g_font) vita2d_free_pgf(g_font);
    vita2d_fini();
}

void render_frame(const App *a, float dt)
{
    fx_update(dt);
    vita2d_start_drawing();
    vita2d_clear_screen();

    switch (a->state) {
    case ST_TITLE:       screen_title(a); break;
    case ST_MODE_SELECT: screen_mode(a); break;
    case ST_LOBBY_HOST:  screen_lobby_host(a); break;
    case ST_LOBBY_JOIN:  screen_lobby_join(a, g_nhosts, g_host_names); break;
    case ST_SETUP:       screen_setup(a); draw_fx(); break;
    case ST_BATTLE:      screen_battle(a); break;
    case ST_GAMEOVER:    screen_gameover(a); break;
    }

    if (a->alert_active) draw_alert(a);

    if (a->trans_active || a->trans_t > 0) {
        float k = a->trans_t;
        rect(0, 0, SCREEN_W, SCREEN_H, col_alpha(COL_BLACK, k));
    }

    vita2d_end_drawing();
    if (a->sys_dialog) vita2d_common_dialog_update();   /* system NetCheck dialog */
    vita2d_swap_buffers();
}
