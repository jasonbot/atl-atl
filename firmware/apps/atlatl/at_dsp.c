/* SPDX-License-Identifier: GPL-3.0-only */
/* The autotune core. The shifter follows TalentedHack's pitch_shifter.c (Jeremy Salwen, after
 * Tom Baran's Autotalent; GPL-3.0): phasein takes a fragment of N samples from the input ring
 * N/2 back, phaseout overlap-adds it with a raised cosine at the ratio between the periods.
 * The detector is YIN on a 4x-decimated signal (no FFT, no allocation). */
#include "at_dsp.h"
#include <math.h>
#include <string.h>

#define AT_MASK (AT_N - 1u)
#define AT_DMASK (AT_DN - 1u)

float at_note_freq(const at_state *s, int midi)
{
    if (midi < 0) midi = 0;
    if (midi > 127) midi = 127;
    return s->nf[midi];
}

int at_note_of(const at_state *s, float f, float *cents)
{
    int i, best = 0;
    float d0 = 1e30f;
    for (i = 0; i < 128; i++) {
        float d = f - s->nf[i];
        if (d < 0) d = -d;
        if (d < d0) { d0 = d; best = i; }
    }
    if (cents) *cents = (f - s->nf[best]) / s->nf[best] * 1731.0f;
    return best;
}

void at_init(at_state *s, uint32_t sr)
{
    uint32_t i;
    memset(s, 0, sizeof *s);
    s->target = -1;                                    /* no target: pass through */
    s->sr = sr;
    s->nf[0] = 8.1757989156f;                          /* C-1; a semitone is 2^(1/12) */
    for (i = 1; i < 128; i++)
        s->nf[i] = s->nf[i - 1] * 1.0594630943593f;
    for (i = 0; i < AT_N; i++)
        s->win[i] = 0.5f - 0.5f * cosf(6.28318530718f * (float)i / (float)AT_N);
    s->pin = (float)sr / 220.0f;                      /* a sane period before the first detection */
    s->pout = 441.0f;
    s->phincfact = 1.0f;
    s->ratio = 1.0f;
    s->speed = 0.35f;
    s->mix = 1.0f;
    s->correct = 1.0f;
    s->fold = 1;
}

void at_reset(at_state *s)
{
    float speed = s->speed, mix = s->mix, correct = s->correct;
    int target = s->target, fold = s->fold;
    uint32_t sr = s->sr;
    at_init(s, sr);
    s->speed = speed;
    s->mix = mix;
    s->correct = correct;
    s->target = target;
    s->fold = fold;
}

void at_set_target(at_state *s, int midi) { s->target = midi >= 0 && midi <= 127 ? midi : -1; }

void at_set_fold(at_state *s, int fold) { s->fold = fold != 0; }

void at_set_params(at_state *s, float speed, float mix, float correct)
{
    s->speed = speed < 0 ? 0 : speed > 1 ? 1 : speed;
    s->mix = mix < 0 ? 0 : mix > 1 ? 1 : mix;
    s->correct = correct < 0 ? 0 : correct > 1 ? 1 : correct;
}

/* ---- detector: YIN over the last AT_WIN decimated samples, lags AT_MINLAG..AT_MAXLAG ---- */
static void at_detect(at_state *s)
{
    uint32_t i, tau, base = (s->dw - AT_WIN) & AT_DMASK, best = 0;
    float sum = 0, energy = 0, dmin;

    for (i = 0; i < AT_WIN; i++)
        s->x[i] = s->dring[(base + i) & AT_DMASK];
    for (i = 0; i < AT_WIN; i++)
        energy += s->x[i] * s->x[i];
    s->rms = energy / (float)AT_WIN;                   /* mean square: no sqrt in the audio path */
    s->diff[0] = 1e30f;
    for (tau = 1; tau <= AT_MAXLAG; tau++) {
        float dsum = 0;
        for (i = 0; i + tau < AT_WIN; i++) {
            float e = s->x[i] - s->x[i + tau];
            dsum += e * e;
        }
        sum += dsum;
        s->diff[tau] = dsum * (float)tau / (sum + 1e-12f);   /* YIN's cumulative mean */
    }
    for (tau = AT_MINLAG; tau <= AT_MAXLAG; tau++) {   /* the first local dip below the threshold */
        if (s->diff[tau] < 0.15f && s->diff[tau] <= s->diff[tau - 1u] &&
            (tau == AT_MAXLAG || s->diff[tau] <= s->diff[tau + 1u])) {
            best = tau;
            break;
        }
    }
    if (!best) {                                       /* no dip: the global minimum, if any */
        dmin = 1e30f;
        for (tau = AT_MINLAG; tau <= AT_MAXLAG; tau++)
            if (s->diff[tau] < dmin) {
                dmin = s->diff[tau];
                best = tau;
            }
    }
    if (best >= AT_MINLAG) {
        float y0 = s->diff[best - 1u], y1 = s->diff[best];
        float y2 = s->diff[best + 1u <= AT_MAXLAG ? best + 1u : best];
        float den = y0 - 2.0f * y1 + y2, delta = 0;
        if (den > 1e-9f || den < -1e-9f)
            delta = 0.5f * (y0 - y2) / den;
        if (delta > 0.5f) delta = 0.5f;
        if (delta < -0.5f) delta = -0.5f;
        s->f0 = ((float)s->sr / (float)AT_DEC) / ((float)best + delta);
        s->clarity = 1.0f - y1;
        s->voiced = s->clarity > 0.5f && s->rms > 4.0e-6f && s->f0 > 55.0f && s->f0 < 1200.0f;
    } else {
        s->voiced = 0;
        s->clarity = 0;
    }
}

/* ---- shifter: the last AT_N input samples are in ibuf, iw is the next write slot ---- */
static void at_interp(at_state *s, int bound)
{
    int i;
    if (bound < 2)
        return;
    if (bound > (int)AT_N / 2)
        bound = (int)AT_N / 2 - 1;
    for (i = -bound / 2; i < bound / 2; i++) {
        int wi = (int)((float)AT_N / 2.0f + (float)i * (float)AT_N / (float)bound);
        float tf, indd, d, v0, v1, v2, v3, val;
        int ind1, idx;
        if (wi < 0) wi = 0;
        if (wi >= (int)AT_N) wi = (int)AT_N - 1;
        tf = s->win[wi];
        indd = s->phincfact * (float)i;
        ind1 = (int)indd;
        d = indd - (float)ind1;
        v0 = s->frag[(uint32_t)(ind1 - 1) & AT_MASK];
        v1 = s->frag[(uint32_t)ind1 & AT_MASK];
        v2 = s->frag[(uint32_t)(ind1 + 1) & AT_MASK];
        v3 = s->frag[(uint32_t)(ind1 + 2) & AT_MASK];
        val = -v0 * d * (d - 1.0f) * (d - 2.0f) * (1.0f / 6.0f) +
              v1 * (d + 1.0f) * (d - 1.0f) * (d - 2.0f) * 0.5f -
              v2 * (d + 1.0f) * d * (d - 2.0f) * 0.5f +
              v3 * (d + 1.0f) * d * (d - 1.0f) * (1.0f / 6.0f);
        idx = (int)((uint32_t)(i + (int)s->obr + 3 * (int)AT_N / 2) & AT_MASK);
        s->obuf[idx] += val * tf;
    }
}

void at_process(at_state *s, const int32_t *in, int32_t *out, uint32_t n)
{
    uint32_t i;
    float mix_eff = s->mix;
    float ratio_t = 1.0f;

    if ((s->blocks & 1u) == 0u)
        at_detect(s);
    s->blocks++;
    if (s->target >= 0 && s->voiced) {
        float ft = at_note_freq(s, s->target);
        if (s->fold) {
            while (ft > s->f0 * 1.41421356f) ft *= 0.5f;   /* the octave nearest the singer */
            while (ft < s->f0 * 0.70710678f) ft *= 2.0f;
        }
        ratio_t = ft / s->f0;
        if (ratio_t < 0.25f) ratio_t = 0.25f;
        if (ratio_t > 4.0f) ratio_t = 4.0f;
    } else {
        mix_eff = 0;                                   /* unvoiced or no key: the delayed dry signal */
    }
    ratio_t = 1.0f + (ratio_t - 1.0f) * s->correct;
    {
        float coeff = 1.0f / (1.0f + s->speed * 16.0f);   /* 0: instant; 1: ~200 ms */
        s->ratio += (ratio_t - s->ratio) * coeff;
        if (s->ratio < 0.25f) s->ratio = 0.25f;
        if (s->ratio > 4.0f) s->ratio = 4.0f;
    }
    if (s->voiced) {
        float pin = (float)s->sr / s->f0;
        s->pin += (pin - s->pin) * 0.5f;
        if (s->pin < 32.0f) s->pin = 32.0f;
        if (s->pin > (float)AT_N / 2) s->pin = (float)AT_N / 2;
    }
    s->pout = s->pin / s->ratio;
    if (s->pout < 32.0f) s->pout = 32.0f;
    if (s->pout > (float)AT_N) s->pout = (float)AT_N;
    s->phincfact = s->pin / s->pout;

    for (i = 0; i < n; i++) {
        float x = 0.5f * ((float)in[2u * i] + (float)in[2u * i + 1u]) * (1.0f / 32768.0f);
        float dry, wet, y;
        uint32_t fb, k;
        s->dc += (x - s->dc) * 0.002f;
        x -= s->dc;
        s->ibuf[s->iw] = x;
        s->iw = (s->iw + 1u) & AT_MASK;
        s->dec_acc += x;
        if (++s->dec_phase == AT_DEC) {
            s->dec_phase = 0;
            s->dring[s->dw++ & AT_DMASK] = s->dec_acc * (1.0f / AT_DEC);
            s->dec_acc = 0;
        }
        s->phasein += 1.0f / s->pin;
        if (s->phasein >= 1.0f) {
            s->phasein -= 1.0f;
            fb = (s->iw - AT_N / 2u) & AT_MASK;
            for (k = 0; k < AT_N; k++)
                s->frag[k] = s->ibuf[(fb + k) & AT_MASK];
        }
        s->phaseout += 1.0f / s->pout;
        if (s->phaseout >= 1.0f) {
            int bound;
            s->phaseout -= 1.0f;
            s->fragsize *= 2;
            if (s->fragsize > (int)AT_N)
                s->fragsize = (int)AT_N;
            bound = (int)((float)s->fragsize / s->phincfact);
            at_interp(s, bound);
            s->fragsize = 0;
        }
        s->fragsize++;
        wet = s->obuf[s->obr];
        s->obuf[s->obr] = 0;
        s->obr = (s->obr + 1u) & AT_MASK;
        dry = s->ibuf[(s->iw - AT_N / 2u) & AT_MASK];
        y = mix_eff * wet + (1.0f - mix_eff) * dry;
        if (y > 1.0f) y = 1.0f;
        if (y < -1.0f) y = -1.0f;
        out[2u * i] = out[2u * i + 1u] = (int32_t)(y * 32767.0f);
    }
}
