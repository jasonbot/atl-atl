/* SPDX-License-Identifier: GPL-3.0-only */
/* What app.c reaches of the FPU DSP: integers and pointers only. */
#ifndef ATLATL_DSP_H
#define ATLATL_DSP_H
#include <stdint.h>

void atlatl_init(uint32_t sr);
void atlatl_target(int midi);                  /* -1: none */
void atlatl_params(int speed, int mix, int correct);   /* 0..100 */
void atlatl_fold(int fold);
void atlatl_process(const int16_t *in, int32_t *out, uint32_t n);   /* Q15 stereo */
void atlatl_status(int *f0_x100, int *ratio_x100, int *voiced, int *clarity);
int atlatl_note(int *cents);                   /* nearest MIDI note to the last f0 */
int atlatl_note_hz100(int midi);               /* equal-tempered note frequency, Hz * 100 */
#endif
