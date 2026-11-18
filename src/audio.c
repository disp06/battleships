/*
 * audio.c - Procedural PCM sound synthesizer + PS Vita audio thread.
 *
 * Every effect is generated into an int16 mono buffer (48 kHz) at startup;
 * no .wav/.ogg assets are shipped. A dedicated thread mixes up to
 * MAX_VOICES concurrent voices and feeds sceAudioOutOutput(), which blocks
 * only that thread, so the 60 FPS render loop is never stalled.
 *
 * Build with -DAUDIO_HOST_DUMP on a desktop to compile only the synth part
 * (used by tests/dump_sfx.c to write the effects to .wav for auditioning).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "audio.h"

#ifndef AUDIO_HOST_DUMP
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#endif

#define SR          48000
#define GRAIN       256
#define MAX_VOICES  12
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { int16_t *data; int len; } Sample;
static Sample g_sfx[SFX_COUNT];

/* ===================================================================
 *                            SYNTHESIS
 * =================================================================== */
static uint32_t g_noise = 0x1234567u;
static float noise(void)
{
    g_noise ^= g_noise << 13; g_noise ^= g_noise >> 17; g_noise ^= g_noise << 5;
    return ((float)(g_noise & 0xFFFF) / 32767.5f) - 1.0f;
}

static float *fbuf(int n) { return (float *)calloc((size_t)n, sizeof(float)); }

/* soft-clip and convert to int16 with peak normalisation to `peak` */
static void finish(SfxId id, float *f, int n, float peak)
{
    int i;
    float m = 0.0001f;
    int16_t *out = (int16_t *)malloc((size_t)n * sizeof(int16_t));
    for (i = 0; i < n; i++) if (fabsf(f[i]) > m) m = fabsf(f[i]);
    for (i = 0; i < n; i++) {
        float v = f[i] / m * peak;
        v = tanhf(v * 1.2f) / tanhf(1.2f);       /* gentle saturation */
        out[i] = (int16_t)(v * 32000.0f);
    }
    /* 2 ms fade-out guard against clicks */
    for (i = 0; i < 96 && i < n; i++) out[n - 1 - i] = (int16_t)(out[n - 1 - i] * (i / 96.0f));
    free(f);
    g_sfx[id].data = out;
    g_sfx[id].len = n;
}

/* one-pole low-pass, cutoff in Hz */
typedef struct { float y; } Lp;
static float lp(Lp *s, float x, float hz)
{
    float a = 1.0f - expf(-2.0f * (float)M_PI * hz / SR);
    s->y += a * (x - s->y);
    return s->y;
}

static void synth_tick(void)
{
    int n = SR * 30 / 1000, i;
    float *f = fbuf(n), ph = 0;
    for (i = 0; i < n; i++) {
        float t = (float)i / SR;
        float env = expf(-t * 180.0f);
        ph += 2.0f * (float)M_PI * (3200.0f - 1400.0f * t * 30.0f) / SR;
        f[i] = sinf(ph) * env + 0.25f * noise() * expf(-t * 900.0f);
    }
    finish(SFX_TICK, f, n, 0.22f);
}

static void synth_confirm(void)
{
    int n = SR * 140 / 1000, i;
    float *f = fbuf(n), ph = 0;
    for (i = 0; i < n; i++) {
        float t = (float)i / SR;
        float hz = t < 0.06f ? 1320.0f : 1980.0f;
        float env = expf(-fmodf(t, 0.06f) * 40.0f) * (t < 0.06f ? 0.8f : 1.0f);
        ph += 2.0f * (float)M_PI * hz / SR;
        f[i] = (sinf(ph) + 0.3f * sinf(ph * 2.0f)) * env;
    }
    finish(SFX_CONFIRM, f, n, 0.40f);
}

static void synth_error(void)
{
    int n = SR * 180 / 1000, i;
    float *f = fbuf(n), ph = 0;
    Lp l = {0};
    for (i = 0; i < n; i++) {
        float t = (float)i / SR;
        float env = (t < 0.08f || (t > 0.1f && t < 0.17f)) ? 1.0f : 0.0f;
        float sq;
        ph += 2.0f * (float)M_PI * 180.0f / SR;
        sq = sinf(ph) > 0 ? 1.0f : -1.0f;
        f[i] = lp(&l, sq, 1800.0f) * env;
    }
    finish(SFX_ERROR, f, n, 0.30f);
}

static void synth_sonar(void)
{
    /* "pinnng" + two faint echoes, slight downward pitch glide */
    int n = SR * 2, i, e;
    float *f = fbuf(n);
    const float delays[3] = { 0.0f, 0.42f, 0.84f };
    const float amps[3]   = { 1.0f, 0.32f, 0.11f };
    for (e = 0; e < 3; e++) {
        int off = (int)(delays[e] * SR);
        float ph = 0;
        for (i = 0; off + i < n; i++) {
            float t = (float)i / SR;
            float hz = 1180.0f - 40.0f * t;
            float att = t < 0.004f ? t / 0.004f : 1.0f;
            float env = att * expf(-t * 4.2f);
            ph += 2.0f * (float)M_PI * hz / SR;
            f[off + i] += amps[e] * env * (sinf(ph) + 0.18f * sinf(ph * 2.01f) + 0.06f * sinf(ph * 3.0f));
        }
    }
    finish(SFX_SONAR, f, n, 0.30f);
}

static void synth_fire(void)
{
    int n = SR * 450 / 1000, i;
    float *f = fbuf(n), ph = 0;
    Lp l = {0};
    for (i = 0; i < n; i++) {
        float t = (float)i / SR;
        float boom, crack;
        ph += 2.0f * (float)M_PI * (110.0f * expf(-t * 9.0f) + 38.0f) / SR;
        boom  = sinf(ph) * expf(-t * 9.0f);
        crack = lp(&l, noise(), 5000.0f * expf(-t * 12.0f) + 300.0f) * expf(-t * 16.0f);
        f[i] = 0.9f * boom + 1.1f * crack;
    }
    finish(SFX_FIRE, f, n, 0.80f);
}

/* two cascaded one-poles = 12 dB/oct, enough to make noise sound "dark" */
typedef struct { Lp a, b; } Lp2;
static float lp2(Lp2 *s, float x, float hz) { return lp(&s->b, lp(&s->a, x, hz), hz); }

static void synth_splash(void)
{
    /* filtered white-noise burst: bright "sploosh" closing to a low wash */
    int n = SR * 700 / 1000, i;
    float *f = fbuf(n);
    Lp2 a = {{0}, {0}};
    Lp2 fz = {{0}, {0}};
    for (i = 0; i < n; i++) {
        float t = (float)i / SR;
        float att = t < 0.012f ? t / 0.012f : 1.0f;
        float env = att * expf(-t * 6.0f);
        float cut = 1800.0f * expf(-t * 4.0f) + 220.0f;
        float nz = noise();
        float lo = lp2(&a, nz, cut);
        float fizz = lp2(&fz, nz, 3500.0f) - lo;            /* spray band */
        f[i] = env * (lo + 0.12f * fizz * expf(-t * 12.0f));
    }
    finish(SFX_SPLASH, f, n, 0.55f);
}

static void synth_explosion(SfxId id, float dur, float base_hz, float decay, float rumble)
{
    /* bass thump (pitch-dropping sine) + dark noise body + short grit,
     * optional sub-rumble and a second, delayed boom for sinkings */
    int n = (int)(SR * dur), i;
    float *f = fbuf(n), ph = 0, ph2 = 0;
    Lp2 body = {{0}, {0}}, grit = {{0}, {0}}, rum = {{0}, {0}};
    for (i = 0; i < n; i++) {
        float t = (float)i / SR;
        float sweep = base_hz * (1.0f + 2.0f * expf(-t * 18.0f));
        float v;
        ph += 2.0f * (float)M_PI * sweep / SR;
        /* 2nd/3rd harmonics keep the thump audible on the Vita's small speakers */
        v  = 1.4f * (sinf(ph) + 0.35f * sinf(2.0f * ph) + 0.15f * sinf(3.0f * ph)) * expf(-t * decay);
        v += 1.3f * lp2(&body, noise(), 420.0f * expf(-t * 3.0f) + 70.0f) * expf(-t * decay * 0.5f);
        v += 0.35f * lp2(&grit, noise(), 2200.0f) * expf(-t * 30.0f);
        if (rumble > 0) {
            float t2 = t - 0.28f;
            v += rumble * 1.6f * lp2(&rum, noise(), 90.0f) * expf(-t * 1.3f);
            if (t2 > 0) {
                ph2 += 2.0f * (float)M_PI * (base_hz * 0.8f * (1.0f + 1.5f * expf(-t2 * 14.0f))) / SR;
                v += rumble * 1.1f * sinf(ph2) * expf(-t2 * 3.5f);
            }
        }
        f[i] = v;
    }
    finish(id, f, n, 0.95f);
}

static void synth_arp(SfxId id, const float *notes, int count, float step, float tail, int major)
{
    int n = (int)(SR * (step * count + tail)), i, k;
    float *f = fbuf(n);
    for (k = 0; k < count; k++) {
        int off = (int)(k * step * SR);
        float hz = notes[k], ph = 0;
        int last = (k == count - 1);
        float len = last ? tail + step : step * 2.2f;
        for (i = 0; off + i < n && i < (int)(len * SR); i++) {
            float t = (float)i / SR;
            float env = (t < 0.005f ? t / 0.005f : 1.0f) * expf(-t * (last ? 2.6f : 7.0f));
            float tri;
            ph += hz / SR;
            ph -= floorf(ph);
            tri = 4.0f * fabsf(ph - 0.5f) - 1.0f;                  /* triangle  */
            f[off + i] += env * (tri * 0.7f + 0.3f * sinf(2.0f * (float)M_PI * ph * 2.0f) * (major ? 1.0f : 0.6f));
        }
    }
    finish(id, f, n, 0.55f);
}

static void synth_all(void)
{
    static const float win[]  = { 523.25f, 659.25f, 783.99f, 1046.5f, 1318.5f };  /* C E G C E */
    static const float lose[] = { 659.25f, 587.33f, 523.25f, 415.30f, 349.23f };  /* E D C Ab F */
    synth_tick();
    synth_confirm();
    synth_error();
    synth_sonar();
    synth_fire();
    synth_splash();
    synth_explosion(SFX_HIT,  0.9f, 55.0f, 7.0f, 0.0f);
    synth_explosion(SFX_SUNK, 2.2f, 38.0f, 3.2f, 0.9f);
    synth_arp(SFX_VICTORY, win, 5, 0.11f, 0.9f, 1);
    synth_arp(SFX_DEFEAT, lose, 5, 0.18f, 1.1f, 0);
}

#ifdef AUDIO_HOST_DUMP
/* Desktop helper: expose generated buffers. */
void audio_host_build(void) { synth_all(); }
const int16_t *audio_host_get(int id, int *len) { *len = g_sfx[id].len; return g_sfx[id].data; }
#else

/* ===================================================================
 *                         VITA OUTPUT THREAD
 * =================================================================== */
typedef struct { const Sample *s; int pos; int vol; } Voice;   /* vol Q8 */

#define REQ_RING 32
static volatile struct { uint8_t id; uint8_t vol; } g_req[REQ_RING];
static volatile unsigned g_req_w = 0, g_req_r = 0;   /* SPSC ring */

static Voice   g_voices[MAX_VOICES];
static int     g_port = -1;
static SceUID  g_thread = -1;
static volatile int g_run = 0;
static volatile int g_master = 256;                /* Q8 */
static int16_t g_out[2][GRAIN] __attribute__((aligned(64)));

static void voice_start(int id, int vol)
{
    int i, slot = -1, oldest = 0, maxpos = -1;
    for (i = 0; i < MAX_VOICES; i++) {
        if (!g_voices[i].s) { slot = i; break; }
        if (g_voices[i].pos > maxpos) { maxpos = g_voices[i].pos; oldest = i; }
    }
    if (slot < 0) slot = oldest;   /* steal the voice closest to its end */
    g_voices[slot].s = &g_sfx[id];
    g_voices[slot].pos = 0;
    g_voices[slot].vol = vol;
}

static int audio_thread(SceSize args, void *argp)
{
    int buf = 0;
    (void)args; (void)argp;
    while (g_run) {
        int i, v;
        int32_t acc[GRAIN];

        while (g_req_r != g_req_w) {
            unsigned r = g_req_r % REQ_RING;
            voice_start(g_req[r].id, g_req[r].vol);
            __sync_synchronize();
            g_req_r++;
        }

        memset(acc, 0, sizeof(acc));
        for (v = 0; v < MAX_VOICES; v++) {
            Voice *vo = &g_voices[v];
            int n;
            if (!vo->s) continue;
            n = vo->s->len - vo->pos;
            if (n > GRAIN) n = GRAIN;
            for (i = 0; i < n; i++)
                acc[i] += (vo->s->data[vo->pos + i] * vo->vol) >> 8;
            vo->pos += n;
            if (vo->pos >= vo->s->len) vo->s = NULL;
        }
        for (i = 0; i < GRAIN; i++) {
            int32_t s = (acc[i] * g_master) >> 8;
            /* soft knee limiter for stacked explosions */
            if (s > 24000)  s = 24000 + (s - 24000) / 4;
            if (s < -24000) s = -24000 + (s + 24000) / 4;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            g_out[buf][i] = (int16_t)s;
        }
        sceAudioOutOutput(g_port, g_out[buf]);   /* blocks this thread only */
        buf ^= 1;
    }
    return 0;
}

int audio_init(void)
{
    int vol[2] = { SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB };
    synth_all();
    g_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, SR, SCE_AUDIO_OUT_MODE_MONO);
    if (g_port < 0) return g_port;
    sceAudioOutSetVolume(g_port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    g_run = 1;
    /* Slightly higher priority than the default user main thread (0x10000100). */
    g_thread = sceKernelCreateThread("bs_audio", audio_thread, 0x10000100 - 10, 0x4000, 0, 0, NULL);
    if (g_thread < 0) { g_run = 0; return g_thread; }
    sceKernelStartThread(g_thread, 0, NULL);
    return 0;
}

void audio_shutdown(void)
{
    int i;
    if (g_thread >= 0) {
        g_run = 0;
        sceKernelWaitThreadEnd(g_thread, NULL, NULL);
        sceKernelDeleteThread(g_thread);
        g_thread = -1;
    }
    if (g_port >= 0) { sceAudioOutReleasePort(g_port); g_port = -1; }
    for (i = 0; i < SFX_COUNT; i++) { free(g_sfx[i].data); g_sfx[i].data = NULL; }
}

void audio_play_vol(SfxId id, float vol)
{
    unsigned w;
    if (id < 0 || id >= SFX_COUNT || !g_sfx[id].data || !g_run) return;
    if (g_req_w - g_req_r >= REQ_RING) return;          /* full: drop */
    if (vol < 0) vol = 0;
    if (vol > 1) vol = 1;
    w = g_req_w % REQ_RING;
    g_req[w].id = (uint8_t)id;
    g_req[w].vol = (uint8_t)(vol * 255.0f);
    __sync_synchronize();
    g_req_w++;
}

void audio_play(SfxId id) { audio_play_vol(id, 1.0f); }

void audio_set_master(float vol)
{
    if (vol < 0) vol = 0;
    if (vol > 1) vol = 1;
    g_master = (int)(vol * 256.0f);
}
#endif
