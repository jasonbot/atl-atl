/* SPDX-License-Identifier: GPL-3.0-only */
/* The FPU side of the tuner. app.c is built without the FPU, so every bridge here takes or returns ints. */
#include "at_dsp.h"
#include "atlatl_dsp.h"

#define ATL_BLOCK 256u
static at_state dsp;
static int32_t in32[2u * ATL_BLOCK];

void atlatl_init(uint32_t sr) { at_init(&dsp, sr); }
void atlatl_target(int midi) { at_set_target(&dsp, midi); }

void atlatl_params(int speed, int mix, int correct)
{
    at_set_params(&dsp, (float)speed / 100.0f, (float)mix / 100.0f, (float)correct / 100.0f);
}

void atlatl_fold(int fold) { at_set_fold(&dsp, fold); }

void atlatl_process(const int16_t *in, int32_t *out, uint32_t n)
{
    uint32_t i;
    if (n > ATL_BLOCK)
        n = ATL_BLOCK;
    for (i = 0; i < n; i++) {
        int32_t l = in[2u * i], r = in[2u * i + 1u];
        int32_t m = (l < 0 ? -l : l) >= (r < 0 ? -r : r) ? l : r;   /* mono analysis: the louder channel */
        in32[2u * i] = in32[2u * i + 1u] = m;
    }
    at_process(&dsp, in32, out, n);
}

void atlatl_status(int *f0_x100, int *ratio_x100, int *voiced, int *clarity)
{
    *f0_x100 = (int)(dsp.f0 * 100.0f);
    *ratio_x100 = (int)(dsp.ratio * 100.0f);
    *voiced = dsp.voiced;
    *clarity = (int)(dsp.clarity * 100.0f);
}

int atlatl_note_hz100(int midi)
{
    return (int)(at_note_freq(&dsp, midi) * 100.0f + 0.5f);
}

int atlatl_note(int *cents)
{
    float c;
    int n = at_note_of(&dsp, dsp.f0, &c);
    *cents = (int)c;
    return n;
}
