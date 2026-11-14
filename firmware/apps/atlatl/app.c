/* SPDX-License-Identifier: GPL-3.0-only */
/* Atl-Atl: The Atltuner — the computer's audio, over USB, tuned to the key held on the FM-1, out of its
 * speaker/headphones. One app; no picker, no OPTIONS, no module list. The USB sink is sys/usb.c
 * (FW_USB_PLAY in, FELUCCA_UAC out; build.txt). The tuning is at_dsp.c, reached through atlatl_dsp.h. */
#include "atlatl_dsp.h"

#if !FW_USB_PLAY || !FELUCCA_UAC
#error "atlatl: build.txt defines FW_USB_PLAY=1 and FELUCCA_UAC=1"
#endif

#define CFG_MAGIC 0x41544E31u              /* "ATN1" */
typedef struct { uint32_t magic, ver; int32_t speed, mix, correct, oct, latch, fold; } cfg_t;

static int32_t p_speed = 20, p_mix = 100, p_correct = 100, p_oct, p_latch, p_fold = 1;
static uint32_t held, last_key;
static int target;
static int32_t gain_target, gain;
static int16_t usb_in[2u * FW_BLOCK];

static int32_t master_gain(uint32_t m)
{
    return m < 4u ? 0 : (int32_t)(m * m >> 5);
}

static void str_cat(char *d, const char *s, unsigned max)
{
    unsigned n = str_len(d);
    if (n < max) str_cpy(d + n, s, max - n);
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

static void note_name(char *p, int midi)
{
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int n = midi / 12 - 1, k = midi % 12;
    const char *s = N[k < 0 ? k + 12 : k];
    *p++ = *s++;
    if (*s) *p++ = *s++;
    if (n < 0) { *p++ = '-'; n = -n; }
    while (n >= 10) { *p++ = (char)('0' + n / 10); n %= 10; }
    *p++ = (char)('0' + n);
    *p = 0;
}

static int key_note(uint32_t k) { return FW_KEY_BASE_NOTE + (int)k + 12 * p_oct; }

static void params_changed(void)
{
    atlatl_params(p_speed, p_mix, p_correct);
    atlatl_fold(p_fold);
    atlatl_target(target);
}

static void set_target(int midi)
{
    target = midi;
    atlatl_target(target);
}

static void octave(int o)
{
    if (o < -2) o = -2;
    if (o > 2) o = 2;
    if (o == p_oct) return;
    p_oct = o;
    if (held) {
        int k;
        for (k = 26; k >= 0; k--)
            if (held >> k & 1u) { last_key = (uint32_t)k; break; }
        set_target(k >= 0 ? key_note((uint32_t)k) : 0);
    }
}

static void save_cfg(void)
{
    cfg_t c = {CFG_MAGIC, 1, p_speed, p_mix, p_correct, p_oct, p_latch, p_fold};
    fw_store_save(0, &c, sizeof c);
}

static void load_cfg(void)
{
    cfg_t c;
    if (fw_store_load(0, &c, sizeof c) == 0 && c.magic == CFG_MAGIC && c.ver == 1) {
        p_speed = c.speed; p_mix = c.mix; p_correct = c.correct;
        p_oct = c.oct; p_latch = c.latch; p_fold = c.fold ? 1 : 0;
    }
    if (p_speed < 0) p_speed = 0;
    if (p_speed > 100) p_speed = 100;
    if (p_mix < 0) p_mix = 0;
    if (p_mix > 100) p_mix = 100;
    if (p_correct < 0) p_correct = 0;
    if (p_correct > 100) p_correct = 100;
    if (p_oct < -2) p_oct = -2;
    if (p_oct > 2) p_oct = 2;
}

static void draw_pitch(int note, int cents, int voiced)
{
    char b[24], n[8];
    if (voiced) {
        note_name(n, note);
        b[0] = 0;
        str_cat(b, n, sizeof b);
        str_cat(b, cents >= 0 ? " +" : " ", sizeof b);
        { char c[8]; put_u(c, cents); str_cat(b, c, sizeof b); }
        str_cat(b, "c", sizeof b);
        draw_text_box(10, 82, 220, &FONT_S, b, C_HI, 1);
    } else {
        draw_text_box(10, 82, 220, &FONT_S, "waiting for a voice", C_DIM, 1);
    }
}

static void draw_row(int y, const char *name, int32_t v, const char *suffix)
{
    char b[24];
    draw_text_box(10, y, 120, &FONT_S, name, C_GRAY, 0);
    put_u(b, v);
    if (suffix) str_cat(b, suffix, sizeof b);
    draw_text_box(150, y, 80, &FONT_S, b, C_HI, 1);
}

static void draw_all(int note, int cents, int voiced, int play)
{
    char n[8];
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(10, 4, 160, &FONT_S, "atlatl", C_HI, 0);
    draw_text_box(176, 4, 56, &FONT_S, play == 2 ? "USB" : play ? "WAIT" : "NO USB",
                  play == 2 ? C_ACCENT : C_GRAY, 1);
    draw_text_box(10, 22, 220, &FONT_S, "THE ATLTUNER", C_GRAY, 0);
    if (target) {
        note_name(n, target);
        draw_text_box(10, 42, 220, &FONT_L, n, C_ACCENT, 1);
    } else {
        draw_text_box(10, 42, 220, &FONT_L, "---", C_DIM, 1);
    }
    draw_pitch(note, cents, voiced);
    draw_row(106, "SPEED", p_speed, 0);
    draw_row(124, "MIX", p_mix, "%");
    draw_row(142, "CORRECT", p_correct, "%");
    draw_row(160, "OCT", p_oct, 0);
    draw_text_box(10, 184, 220, &FONT_S, p_latch ? "FX: LATCH" : "FX: HOLD", C_GRAY, 0);
    draw_text_box(10, 202, 220, &FONT_S, p_fold ? "AUTO OCTAVE" : "FIXED OCTAVE", C_GRAY, 0);
    draw_text_box(10, 220, 220, &FONT_S, "EDIT: FOLD   SAVE: KEEP", C_GRAY, 0);
}

/* Only the lines whose value changed are redrawn. The full 240 x 240 fill happens once, at start. */
static void draw_ui(int note, int cents, int voiced, int play, uint32_t now)
{
    static int init, s_target, s_note, s_cents, s_voiced, s_play;
    static int s_speed, s_mix, s_correct, s_oct, s_latch, s_fold;
    static uint32_t pitch_ms;
    char n[8];
    if (!init) {
        draw_all(note, cents, voiced, play);
        s_target = target; s_note = note; s_cents = cents; s_voiced = voiced; s_play = play;
        s_speed = p_speed; s_mix = p_mix; s_correct = p_correct; s_oct = p_oct;
        s_latch = p_latch; s_fold = p_fold; pitch_ms = now; init = 1;
        return;
    }
    if (play != s_play) {
        draw_text_box(176, 4, 56, &FONT_S, play == 2 ? "USB" : play ? "WAIT" : "NO USB",
                      play == 2 ? C_ACCENT : C_GRAY, 1);
        s_play = play;
    }
    if (target != s_target) {
        if (target) { note_name(n, target); draw_text_box(10, 42, 220, &FONT_L, n, C_ACCENT, 1); }
        else draw_text_box(10, 42, 220, &FONT_L, "---", C_DIM, 1);
        s_target = target;
    }
    if (p_speed != s_speed) { draw_row(106, "SPEED", p_speed, 0); s_speed = p_speed; }
    if (p_mix != s_mix) { draw_row(124, "MIX", p_mix, "%"); s_mix = p_mix; }
    if (p_correct != s_correct) { draw_row(142, "CORRECT", p_correct, "%"); s_correct = p_correct; }
    if (p_oct != s_oct) { draw_row(160, "OCT", p_oct, 0); s_oct = p_oct; }
    if (p_latch != s_latch) {
        draw_text_box(10, 184, 220, &FONT_S, p_latch ? "FX: LATCH" : "FX: HOLD", C_GRAY, 0);
        s_latch = p_latch;
    }
    if (p_fold != s_fold) {
        draw_text_box(10, 202, 220, &FONT_S, p_fold ? "AUTO OCTAVE" : "FIXED OCTAVE", C_GRAY, 0);
        s_fold = p_fold;
    }
    if ((note != s_note || cents != s_cents || voiced != s_voiced) && now - pitch_ms >= 100u) {
        draw_pitch(note, cents, voiced);
        s_note = note; s_cents = cents; s_voiced = voiced; pitch_ms = now;
    }
}

static void app_init(void)
{
    load_cfg();
    usb_play_enable(1);
    atlatl_init(FW_FS);
    atlatl_fold(p_fold);
    atlatl_params(p_speed, p_mix, p_correct);
    atlatl_target(0);
}

static void app_frame(const fw_input_t *in)
{
    uint32_t down = in->keys_pressed, up = in->keys_released;
    int k, play = usb_play_state();
    fw_midi_t m;

    gain_target = master_gain(in->master);

    while (fw_midi_read(&m)) {
        uint32_t st = m.status & 0xF0u;
        if (st == 0x90u && m.d2) {
            set_target(m.d1);
        } else if ((st == 0x80u || st == 0x90u) && (target == m.d1) && !p_latch) {
            set_target(0);
        } else if (st == 0xB0u) {
            switch (m.d1) {
            case 74: p_speed = (int32_t)m.d2 * 100 / 127; params_changed(); break;
            case 71: p_mix = (int32_t)m.d2 * 100 / 127; params_changed(); break;
            case 73: p_correct = (int32_t)m.d2 * 100 / 127; params_changed(); break;
            case 72: octave((int)((uint32_t)m.d2 * 5u / 127u) - 2); break;
            default: break;
            }
        }
    }

    for (k = 0; k < 27; k++) {
        if (down >> k & 1u) {
            held |= 1u << k;
            last_key = (uint32_t)k;
            fw_led_note_key((uint32_t)k, 1);
            set_target(key_note((uint32_t)k));
        }
        if (up >> k & 1u) {
            held &= ~(1u << k);
            fw_led_note_key((uint32_t)k, 0);
            if (!p_latch && (uint32_t)k == last_key) {
                int j;
                for (j = 26; j >= 0; j--)
                    if (held >> j & 1u) { last_key = (uint32_t)j; break; }
                set_target(j >= 0 ? key_note((uint32_t)j) : 0);
            }
        }
    }

    if (in->knob[FW_KNOB_1]) { p_speed += in->knob[FW_KNOB_1] * 5; if (p_speed < 0) p_speed = 0; if (p_speed > 100) p_speed = 100; params_changed(); }
    if (in->knob[FW_KNOB_2]) { p_mix += in->knob[FW_KNOB_2] * 5; if (p_mix < 0) p_mix = 0; if (p_mix > 100) p_mix = 100; params_changed(); }
    if (in->knob[FW_KNOB_3]) { p_correct += in->knob[FW_KNOB_3] * 5; if (p_correct < 0) p_correct = 0; if (p_correct > 100) p_correct = 100; params_changed(); }
    if (in->knob[FW_KNOB_4]) octave(p_oct + (in->knob[FW_KNOB_4] > 0 ? 1 : in->knob[FW_KNOB_4] < 0 ? -1 : 0));

    if (in->pressed & FW_BIT(FW_BTN_OCT_DOWN)) octave(p_oct - 1);
    if (in->pressed & FW_BIT(FW_BTN_OCT_UP)) octave(p_oct + 1);
    if (in->pressed & FW_BIT(FW_BTN_FX)) p_latch = !p_latch;
    if (in->pressed & FW_BIT(FW_BTN_EDIT)) { p_fold = !p_fold; params_changed(); }
    if (in->pressed & FW_BIT(FW_BTN_SAVE)) save_cfg();

    fw_led_button(FW_BTN_PLAY, play == 2);
    fw_led_button(FW_BTN_REC, usb_rec_state() == 2);
    fw_led_button(FW_BTN_FX, p_latch);
    fw_led_button(FW_BTN_EDIT, p_fold);

    {
        int f0, ratio, voiced, clarity, note = 0, cents = 0;
        atlatl_status(&f0, &ratio, &voiced, &clarity);
        if (voiced)
            note = atlatl_note(&cents);
        draw_ui(note, cents, voiced, play, in->ms);
    }
}

static void app_audio(int32_t *lr, uint32_t frames)
{
    uint32_t i;
    int32_t g1 = gain_target, step = (g1 - gain) / (int32_t)frames, g = gain;

    if (!(usb_play_begin() && usb_play_read(usb_in, frames)))
        memset(usb_in, 0, frames * 2u * sizeof usb_in[0]);
    atlatl_process(usb_in, lr, frames);
    for (i = 0; i < frames; i++) {
        g += step;
        lr[2u * i] = lr[2u * i] * g >> 15;
        lr[2u * i + 1u] = lr[2u * i + 1u] * g >> 15;
    }
    gain = g1;
}
