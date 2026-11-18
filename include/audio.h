/*
 * audio.h - Procedural PCM SFX synthesised at startup, mixed on a
 * dedicated sceAudioOut thread (non-blocking for the game loop).
 */
#ifndef BS_AUDIO_H
#define BS_AUDIO_H

typedef enum {
    SFX_TICK = 0,     /* cursor move                       */
    SFX_CONFIRM,      /* menu confirm / ship placed        */
    SFX_ERROR,        /* invalid action                    */
    SFX_SONAR,        /* periodic submarine ping           */
    SFX_FIRE,         /* cannon shot                       */
    SFX_SPLASH,       /* miss                              */
    SFX_HIT,          /* hit                               */
    SFX_SUNK,         /* ship sunk - deep rumble           */
    SFX_VICTORY,      /* arpeggio                          */
    SFX_DEFEAT,       /* descending arpeggio               */
    SFX_COUNT
} SfxId;

int  audio_init(void);          /* builds buffers + starts thread; 0 on success */
void audio_shutdown(void);
void audio_play(SfxId id);      /* thread-safe, never blocks */
void audio_play_vol(SfxId id, float vol);
void audio_set_master(float vol);

#endif
