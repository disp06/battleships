/*
 * Desktop helper: render every procedural sound effect to a .wav file so
 * the synth can be auditioned/tuned without a Vita.
 *   cc -O2 -DAUDIO_HOST_DUMP -Iinclude tests/dump_sfx.c src/audio.c -lm -o dump_sfx
 *   ./dump_sfx out_dir
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "audio.h"

void audio_host_build(void);
const int16_t *audio_host_get(int id, int *len);

static const char *NAMES[SFX_COUNT] = {
    "tick", "confirm", "error", "sonar", "fire", "splash", "hit", "sunk", "victory", "defeat"
};

static void put32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc(v >> 8, f); }

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    int i;
    audio_host_build();
    for (i = 0; i < SFX_COUNT; i++) {
        char path[512];
        int len, k, peak = 0;
        const int16_t *d = audio_host_get(i, &len);
        FILE *f;
        snprintf(path, sizeof(path), "%s/sfx_%s.wav", dir, NAMES[i]);
        f = fopen(path, "wb");
        if (!f) { perror(path); return 1; }
        fwrite("RIFF", 1, 4, f); put32(f, 36 + len * 2); fwrite("WAVEfmt ", 1, 8, f);
        put32(f, 16); put16(f, 1); put16(f, 1); put32(f, 48000); put32(f, 96000); put16(f, 2); put16(f, 16);
        fwrite("data", 1, 4, f); put32(f, len * 2);
        for (k = 0; k < len; k++) { put16(f, (uint16_t)d[k]); if (abs(d[k]) > peak) peak = abs(d[k]); }
        fclose(f);
        printf("%-8s %6.0f ms  peak %5d (%.1f dBFS)\n", NAMES[i], len / 48.0, peak,
               peak ? 20.0 * __builtin_log10(peak / 32768.0) : -99.0);
    }
    return 0;
}
