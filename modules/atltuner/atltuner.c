/* SPDX-License-Identifier: GPL-3.0-only */
/* ATLTUNER: the FM-1 as a USB audio sink and a key-targeted hard tuner.
 * Hold a key: the input from the computer is tuned to that note (auto-octave folded).
 * KNOB1 SPEED, KNOB2 MIX, KNOB3 CORRECT, KNOB4 OCT. FX latches the last target; SAVE stores. */
#include "hb_module.h"
#include "at_dsp.h"
#include <string.h>

#define C_BG HB_RGB(10, 10, 18)
#define C_FG HB_RGB(235, 235, 245)
#define C_DIM HB_RGB(130, 130, 150)
#define C_ACC HB_RGB(80, 220, 160)
#define C_WARN HB_RGB(240, 160, 80)

static const hb_host_t *H;
static at_state dsp;
static volatile float v_f0, v_ratio, v_clarity;
static volatile uint8_t v_voiced;
static uint32_t held, last_key;
static int target, oct, latch, fold = 1, dirty = 1;
static int32_t p_speed = 20, p_mix = 100, p_correct = 100, p_oct = 0;
static uint32_t drawn_hash, drawn_ms;

#define CFG_MAGIC 0x41544E31u              /* "ATN1" */
typedef struct { uint32_t magic, ver; int32_t speed, mix, correct, oct, latch, fold; } cfg_t;

static void note_name(char *p, int midi)
{
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int n = midi / 12 - 1, k = midi % 12;
    const char *s = N[k < 0 ? k + 12 : k];
    if (n < 0) { *p++ = '-'; n = -n; }
    while (n >= 10) { *p++ = (char)('0' + n / 10); n %= 10; }
    *p++ = (char)('0' + n);
    *p++ = *s++;
    if (*s) *p++ = *s++;
    *p = 0;
}

static char *put_u(char *p, int32_t v)
{
    char t[12];
    int n = 0;
    if (v < 0) { *p++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *p++ = t[--n];
    *p = 0;
    return p;
}

static void ui_refresh(void)
{
    at_set_params(&dsp, (float)p_speed / 100.0f, (float)p_mix / 100.0f, (float)p_correct / 100.0f);
    at_set_fold(&dsp, fold);
    at_set_target(&dsp, target);
    dirty = 1;
}

static int key_to_note(uint32_t k) { return 53 + (int)k + 12 * oct; }   /* key 0 = F3 */

static void set_oct(int o)
{
    if (o < -2) o = -2;
    if (o > 2) o = 2;
    if (o == oct) return;
    oct = o;
    p_oct = o;
    if (H && HB_HOST_HAS(H, key_base))
        H->key_base((uint32_t)(53 + 12 * oct));
    if (held) {
        target = key_to_note(last_key);
        ui_refresh();
    }
    dirty = 1;
}

static int init(const hb_host_t *h)
{
    cfg_t c;
    H = h;
    at_init(&dsp);
    p_speed = 20; p_mix = 100; p_correct = 100; p_oct = 0; latch = 0; oct = 0; fold = 1;
    if (H->load(&c, sizeof c) == 0 && c.magic == CFG_MAGIC && c.ver == 1) {
        p_speed = c.speed; p_mix = c.mix; p_correct = c.correct;
        p_oct = c.oct; latch = c.latch; fold = c.fold ? 1 : 0;
        if (p_speed < 0) p_speed = 0;
        if (p_speed > 100) p_speed = 100;
        if (p_mix < 0) p_mix = 0;
        if (p_mix > 100) p_mix = 100;
        if (p_correct < 0) p_correct = 0;
        if (p_correct > 100) p_correct = 100;
        if (p_oct < -2) p_oct = -2;
        if (p_oct > 2) p_oct = 2;
        oct = p_oct;
    }
    if (HB_HOST_HAS(H, key_base))
        H->key_base((uint32_t)(53 + 12 * oct));
    target = 0;
    ui_refresh();
    return 0;
}

static void release(void) { target = 0; at_set_target(&dsp, 0); }

static void note(uint32_t ch, uint32_t key, uint32_t vel)
{
    (void)ch;
    if (key > 127u) return;
    if (vel) {
        target = (int)key;
        if (latch) { /* keep it */ }
        ui_refresh();
    } else if (target == (int)key && !latch) {
        target = 0;
        ui_refresh();
    }
}

static void cc(uint32_t ch, uint32_t num, uint32_t val)
{
    (void)ch;
    int32_t v = (int32_t)(val > 127u ? 127u : val);
    switch (num) {
    case 74: p_speed = v * 100 / 127; break;       /* KNOBS CC: cutoff -> speed */
    case 71: p_mix = v * 100 / 127; break;         /* resonance -> mix */
    case 73: p_correct = v * 100 / 127; break;     /* attack -> correct */
    case 72: set_oct((int)(v * 4 / 127) - 2); return;   /* release -> octave */
    default: return;
    }
    ui_refresh();
}

static void ui(uint32_t ev, int32_t arg)
{
    uint32_t kind = HB_EV_KIND(ev), id = HB_EV_ID(ev);
    if (kind == HB_EV_KEY_DOWN) {
        if (id > 26u) return;
        held |= 1u << id;
        last_key = id;
        target = key_to_note(id);
        if (H->led) H->led(HB_LED_KEY(id), 1);
        ui_refresh();
    } else if (kind == HB_EV_KEY_UP) {
        if (id > 26u) return;
        held &= ~(1u << id);
        if (H->led) H->led(HB_LED_KEY(id), 0);
        if ((int)id == (int)last_key && !latch) {
            int k;
            for (k = 26; k >= 0; k--)
                if (held >> k & 1u) { last_key = (uint32_t)k; break; }
            target = k >= 0 ? key_to_note((uint32_t)k) : 0;
            ui_refresh();
        }
    } else if (kind == HB_EV_ENC) {
        int32_t d = arg;
        if (id == HB_ENC_KNOB1) { p_speed += d * 5; if (p_speed < 0) p_speed = 0; if (p_speed > 100) p_speed = 100; }
        else if (id == HB_ENC_KNOB2) { p_mix += d * 5; if (p_mix < 0) p_mix = 0; if (p_mix > 100) p_mix = 100; }
        else if (id == HB_ENC_KNOB3) { p_correct += d * 5; if (p_correct < 0) p_correct = 0; if (p_correct > 100) p_correct = 100; }
        else if (id == HB_ENC_KNOB4) set_oct(oct + (d > 0 ? 1 : d < 0 ? -1 : 0));
        else return;
        ui_refresh();
    } else if (kind == HB_EV_BTN_DOWN) {
        if (id == HB_BTN_OCT_DOWN) set_oct(oct - 1);
        else if (id == HB_BTN_OCT_UP) set_oct(oct + 1);
        else if (id == HB_BTN_FX) { latch = !latch; dirty = 1; }
        else if (id == HB_BTN_EDIT) { fold = !fold; ui_refresh(); }
        else if (id == HB_BTN_SAVE) {
            cfg_t c = {CFG_MAGIC, 1, p_speed, p_mix, p_correct, p_oct, latch, fold};
            H->save(&c, sizeof c);
            dirty = 1;
        }
    }
}

static void render(int32_t *out, uint32_t n) { (void)out; (void)n; }   /* HB_F_AUDIO_IN uses render_in */

static void render_in(const int32_t *in, int32_t *out, uint32_t n)
{
    at_process(&dsp, in, out, n);
    v_f0 = dsp.f0;
    v_ratio = dsp.ratio;
    v_clarity = dsp.clarity;
    v_voiced = (uint8_t)dsp.voiced;
}

static void draw_line(int y, const char *name, int32_t v, const char *suffix)
{
    char b[24];
    H->text(10, y, 120, 0, name, C_DIM, 0);
    put_u(b, v);
    if (suffix) strncat(b, suffix, sizeof b - strlen(b) - 1);
    H->text(150, y, 80, 0, b, C_FG, 1);
}

static void draw_pitch(int det, int cents)
{
    char b[24], n[8];
    if (v_voiced) {
        note_name(n, det);
        b[0] = 0;
        strncat(b, n, sizeof b - 1);
        strncat(b, cents >= 0 ? " +" : " ", sizeof b - strlen(b) - 1);
        { char c[8]; put_u(c, cents); strncat(b, c, sizeof b - strlen(b) - 1); }
        strncat(b, "c", sizeof b - strlen(b) - 1);
        H->text(10, 82, 220, 0, b, C_FG, 1);
    } else {
        H->text(10, 82, 220, 0, "waiting for a voice", C_DIM, 1);
    }
}

static void draw(uint32_t full)
{
    char n[8];
    uint32_t now = H->ms(), hash, usb = HB_HOST_HAS(H, usb_in_state) ? H->usb_in_state() : 0;
    int det = 0, cents = 0;
    if (v_voiced) {
        float c;
        det = at_note_of(&dsp, v_f0, &c);
        cents = (int)c;
    }
    hash = (uint32_t)target * 31u + (uint32_t)(v_f0 * 10.0f) + (uint32_t)(v_ratio * 100.0f) +
           (uint32_t)v_voiced * 7u + (uint32_t)p_speed * 11u + (uint32_t)p_mix * 13u +
           (uint32_t)p_correct * 17u + (uint32_t)latch * 19u + (uint32_t)oct * 23u +
           (uint32_t)fold * 29u + usb * 37u;
    if (!full && !dirty) {
        if (now - drawn_ms < 100u || hash == drawn_hash)
            return;
        drawn_hash = hash;                             /* only the pitch line changed */
        drawn_ms = now;
        H->fill(0, 74, 240, 30, C_BG);
        draw_pitch(det, cents);
        return;
    }
    drawn_hash = hash;
    drawn_ms = now;
    dirty = 0;
    H->fill(0, 0, 240, 240, C_BG);
    H->text(10, 4, 160, 0, "atlatl", C_FG, 0);
    H->text(180, 4, 56, 0, usb ? "USB" : "NO USB", usb ? C_ACC : C_WARN, 0);
    H->text(10, 22, 220, 0, "THE ATLTUNER", C_DIM, 0);
    if (target) {
        note_name(n, target);
        H->text(10, 42, 220, 1, n, C_ACC, 1);
    } else {
        H->text(10, 42, 220, 1, "---", C_DIM, 1);
    }
    draw_pitch(det, cents);
    draw_line(106, "SPEED", p_speed, 0);
    draw_line(124, "MIX", p_mix, "%");
    draw_line(142, "CORRECT", p_correct, "%");
    draw_line(160, "OCT", oct, 0);
    H->text(10, 184, 220, 0, latch ? "FX: LATCH" : "FX: HOLD", C_DIM, 0);
    H->text(10, 202, 220, 0, fold ? "AUTO OCTAVE" : "FIXED OCTAVE", C_DIM, 0);
    H->text(10, 220, 220, 0, "EDIT: FOLD   SAVE: KEEP", C_DIM, 0);
}

HB_DESC = {HB_ABI, "ATLTUNER", init, release, note, cc, render, ui, draw, 0, 0,
           HB_F_SCREEN | HB_F_PANEL | HB_F_AUDIO_IN, 0, 0, render_in};
