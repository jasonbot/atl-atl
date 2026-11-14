/* SPDX-License-Identifier: GPL-3.0-only */
/* The autotune core: YIN pitch detection on a 4x-decimated signal, then a pitch-synchronous
 * granular shifter (after Tom Baran's Autotalent / Jeremy Salwen's TalentedHack pitch_shifter.c,
 * GPL-3.0). Mono analysis; the wet signal is duplicated to both outputs.
 *
 *   at_init(&s, sr)                   zeros, the Hann window
 *   at_set_target(&s, midi)           0: none (pass through)
 *   at_set_params(&s, speed, mix, correct)   0..1 each
 *   at_process(&s, in, out, n)        n stereo Q15 frames, in and out may be the same buffer
 *
 * No host calls, no allocation: every buffer is in at_state (about 40 KiB). */
#pragma once
#include <stdint.h>

#define AT_DEC 4
#define AT_WIN 512
#define AT_MINLAG 10
#define AT_MAXLAG 220
#define AT_DN 1024
#define AT_N 2048

typedef struct at_state {
    uint32_t sr;                 /* the codec rate: 48000 or 44100 */
    /* detector */
    float dring[AT_DN];
    uint32_t dw;
    float diff[AT_MAXLAG + 1];
    float x[AT_WIN];
    float win[AT_N];
    float nf[128];
    float f0, clarity, rms, dc;
    int voiced;
    uint32_t dec_phase, blocks;
    float dec_acc;
    /* shifter */
    float ibuf[AT_N], frag[AT_N], obuf[AT_N];
    uint32_t iw, obr;
    float phasein, phaseout, phincfact;
    int fragsize;
    float pin, pout, ratio;
    /* parameters */
    float speed, mix, correct;
    int target;
    int fold;                                  /* 1: fold the target to the octave nearest the singer */
} at_state;

void at_init(at_state *s, uint32_t sr);
void at_reset(at_state *s);
void at_set_target(at_state *s, int midi);
void at_set_params(at_state *s, float speed, float mix, float correct);
void at_set_fold(at_state *s, int fold);
void at_process(at_state *s, const int32_t *in, int32_t *out, uint32_t n);
float at_note_freq(const at_state *s, int midi);
int at_note_of(const at_state *s, float f, float *cents);
