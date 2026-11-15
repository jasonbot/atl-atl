/* SPDX-License-Identifier: GPL-3.0-only */
/* Atl-Atl: The Atltuner — USB audio in, key-targeted tuning, a three-second one-shot/loop sampler, tempo from
 * SELECT or USB-MIDI clock, mask on ARP. One app; no picker, no OPTIONS, no module list.
 * The USB sink is sys/usb.c (FW_USB_PLAY in, FELUCCA_UAC out; build.txt). The tuner is at_dsp.c, reached through
 * atlatl_dsp.h. */
#include "atlatl_dsp.h"

#if !FW_USB_PLAY || !FELUCCA_UAC
#error "atlatl: build.txt defines FW_USB_PLAY=1 and FELUCCA_UAC=1"
#endif

#define CFG_MAGIC 0x41544E31u              /* "ATN1" */
#define CFG_VER 2u
typedef struct {
    uint32_t magic, ver;
    int32_t speed, mix, correct, oct, latch, fold, bpm, loop, mask;
} cfg_t;

#define SAMPLE_MAX (3u * FW_FS)            /* three seconds, mono, at the codec's rate */
#define SAMPLE_DIVS 7u
#define TARGET_NONE (-1)
#define SRC_NONE 0
#define SRC_KEY 1
#define SRC_MIDI 2
static const uint8_t DIV[SAMPLE_DIVS] = {1, 2, 3, 4, 8, 9, 16};   /* whole .. sixteenth; 1/3 and 1/9 as asked */

static int32_t p_speed = 20, p_mix = 100, p_correct = 100, p_oct, p_bpm = 120;
static volatile int32_t p_latch, p_mask, p_loop;
static int32_t p_fold = 1;
static volatile uint32_t held;
static uint32_t last_key, midi_held[4];
static volatile int target = TARGET_NONE;
static int target_src;
static volatile uint8_t midi_any;
static volatile uint8_t rec, playing, rec_done;
static volatile uint32_t s_w, s_len, s_r, loop_pos;
static volatile uint32_t loop_period, loop_play;
static uint32_t loop_div = 4;
static int clock_active;
static uint32_t clock_ms, clock_start_ms, clock_count, clock_bpm = 120;
static uint8_t beat_on;
static uint32_t beat_next_ms, beat_clocks;
static int32_t gain_target, gain;
static int16_t usb_in[2u * FW_BLOCK];
FW_POOL static int16_t sample[SAMPLE_MAX];

static int32_t master_gain(uint32_t m) { return m < 4u ? 0 : (int32_t)(m * m >> 5); }

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
static uint32_t absdiff(uint32_t a, uint32_t b) { return a > b ? a - b : b - a; }
static int tempo_now(void)
{
    int bpm = clock_active ? (int)clock_bpm : (int)p_bpm;
    return bpm < 30 ? 30 : bpm > 300 ? 300 : bpm;
}

/* The loop's interval is the note length nearest the sample's own length. A whole note is 240/BPM seconds;
 * the sample is clipped to the interval, or followed by silence, so every loop lands on the grid. */
static void tempo_recompute(void)
{
    uint32_t whole = (240u * FW_FS) / (uint32_t)tempo_now();
    uint32_t best = whole, best_err = absdiff(whole, s_len), best_div = 1;
    uint32_t i;
    for (i = 1; i < SAMPLE_DIVS; i++) {
        uint32_t per = whole / DIV[i];
        uint32_t err = absdiff(per, s_len);
        if (err < best_err || (err == best_err && DIV[i] > best_div)) {
            best = per;
            best_err = err;
            best_div = DIV[i];
        }
    }
    loop_period = best;
    loop_div = best_div;
    loop_play = best < s_len ? best : s_len;
}

static void clock_tick(uint32_t ms)
{
    uint32_t span;
    if (!clock_active) {
        clock_active = 1;
        clock_start_ms = ms;
        clock_count = 0;
        beat_clocks = 0;
        beat_on = 0;
    }
    if (!clock_count)
        clock_start_ms = ms;
    clock_count++;
    if (++beat_clocks >= 24u) {
        beat_clocks = 0;
        beat_on = !beat_on;
    }
    clock_ms = ms;
    span = ms - clock_start_ms;
    if (span >= 500u) {
        uint32_t bpm = (clock_count * 60000u) / (span * 24u);   /* 24 clocks a quarter note */
        if (bpm >= 30u && bpm <= 300u)
            clock_bpm = (clock_bpm * 3u + bpm) / 4u;
        clock_count = 0;
        clock_start_ms = ms;
    }
    tempo_recompute();
}

static void params_changed(void)
{
    atlatl_params(p_speed, p_mix, p_correct);
    atlatl_fold(p_fold);
    atlatl_target(target);
}

static void set_target(int midi) { target = midi; atlatl_target(midi); }

static int midi_held_test(uint32_t n) { return (midi_held[n >> 5] >> (n & 31u)) & 1u; }

static void midi_held_set(uint32_t n, int on)
{
    uint32_t *w = &midi_held[n >> 5], bit = 1u << (n & 31u);
    if (on) *w |= bit; else *w &= ~bit;
    midi_any = (midi_held[0] | midi_held[1] | midi_held[2] | midi_held[3]) != 0;
}

/* Physical keys first, then MIDI; a released target falls back to whatever is still held. */
static void target_from_held(void)
{
    int j;
    for (j = 26; j >= 0; j--)
        if (held >> j & 1u) {
            last_key = (uint32_t)j;
            set_target(key_note((uint32_t)j));
            target_src = SRC_KEY;
            return;
        }
    for (j = 127; j >= 0; j--)
        if (midi_held_test((uint32_t)j)) {
            set_target(j);
            target_src = SRC_MIDI;
            return;
        }
    set_target(TARGET_NONE);
    target_src = SRC_NONE;
}

static void octave(int o)
{
    if (o < -2) o = -2;
    if (o > 2) o = 2;
    if (o == p_oct) return;
    p_oct = o;
    if (target_src == SRC_KEY && held)
        target_from_held();
}

/* A held REC can start before the input and run after it; the captured sample is the part between the first and
 * last nonzero frames. */
static void sample_trim(void)
{
    uint32_t first = 0, last = s_len, i;
    while (first < last && sample[first] == 0) first++;
    while (last > first && sample[last - 1u] == 0) last--;
    for (i = first; i < last; i++)
        sample[i - first] = sample[i];
    s_len = last - first;
}

/* Auto-stop leaves s_len at zero so PLAY cannot expose the untrimmed buffer while the main loop finishes it. */
static void sample_finish(void)
{
    s_len = s_w;
    sample_trim();
    rec_done = 0;
    tempo_recompute();
}

static void record_start(void)
{
    fm1_irq_off();
    rec = 1;
    rec_done = 0;
    s_w = 0;
    s_len = 0;
    playing = 0;
    s_r = 0;
    loop_pos = 0;
    fm1_irq_on();
    tempo_recompute();
}

static void record_stop(void)
{
    int was_rec;
    fm1_irq_off();
    was_rec = rec;
    rec = 0;
    fm1_irq_on();
    if (was_rec)
        sample_finish();
}

static void play_toggle(void)
{
    if (rec_done)
        sample_finish();
    fm1_irq_off();
    if (playing) {
        playing = 0;
    } else if (s_len) {
        s_r = 0;
        loop_pos = 0;
        playing = 1;
    }
    fm1_irq_on();
    tempo_recompute();
}

static void save_cfg(void)
{
    cfg_t c = {CFG_MAGIC, CFG_VER, p_speed, p_mix, p_correct, p_oct, p_latch, p_fold, p_bpm, p_loop, p_mask};
    fw_store_save(0, &c, sizeof c);
}

static void load_cfg(void)
{
    cfg_t c;
    if (fw_store_load(0, &c, sizeof c) == 0 && c.magic == CFG_MAGIC && c.ver == CFG_VER) {
        p_speed = c.speed; p_mix = c.mix; p_correct = c.correct;
        p_oct = c.oct; p_latch = c.latch; p_fold = c.fold ? 1 : 0;
        p_bpm = c.bpm; p_loop = c.loop; p_mask = c.mask;
    }
    if (p_speed < 0) p_speed = 0;
    if (p_speed > 100) p_speed = 100;
    if (p_mix < 0) p_mix = 0;
    if (p_mix > 100) p_mix = 100;
    if (p_correct < 0) p_correct = 0;
    if (p_correct > 100) p_correct = 100;
    if (p_oct < -2) p_oct = -2;
    if (p_oct > 2) p_oct = 2;
    if (p_bpm < 30) p_bpm = 30;
    if (p_bpm > 300) p_bpm = 300;
    p_loop = p_loop ? 1 : 0;
    p_mask = p_mask ? 1 : 0;
}

/* ---- screen ---- */
/* The UI background is assets/ui.svg, rasterized to app/ui.png at 240 x 240. The SVG's named rectangles are the
 * live text slots; the four bottom boxes keep their labels and get their values below. */
#define UI_NOTE_X 9u
#define UI_NOTE_Y 66u
#define UI_NOTE_W 95u
#define UI_PITCH_Y 103u
#define UI_BPM_X 174u
#define UI_BPM_Y 60u
#define UI_BPM_W 64u
#define UI_SAMPLE_X 174u
#define UI_SAMPLE_Y 97u
#define UI_SAMPLE_W 64u
#define UI_KNOB_Y 209u
#define UI_MODES_X 0u
#define UI_MODES_Y 138u
#define UI_MODES_W 140u
#define UI_MODES_H 36u
#define UI_REC_X 160u
#define UI_REC_Y 135u
#define UI_REC_W 80u
#define UI_REC_H 35u
#define UI_LATCH_X 199u
#define UI_LATCH_Y 0u
#define UI_LATCH_W 41u
#define UI_LATCH_H 45u
#define UI_OFF_ALPHA 13u                 /* 5% opacity: 95% transparent when a mode is off */

static void draw_note(int note, int cents, int voiced)
{
    char n[8], b[24];
    if (target >= 0)
        note_name(n, target);
    else
        str_cpy(n, "---", sizeof n);
    ui_text(UI_NOTE_X, UI_NOTE_Y, UI_NOTE_W, 36u, &FONT_L, n, target >= 0 ? C_HI : C_DIM, 1);
    if (voiced) {
        note_name(n, note);
        b[0] = 0;
        str_cat(b, n, sizeof b);
        str_cat(b, cents >= 0 ? " +" : " ", sizeof b);
        { char c[8]; put_u(c, cents); str_cat(b, c, sizeof b); }
        str_cat(b, "c", sizeof b);
    } else {
        str_cpy(b, "waiting", sizeof b);
    }
    ui_text(UI_NOTE_X, UI_PITCH_Y, UI_NOTE_W, 18u, &FONT_S, b, voiced ? C_GREEN : C_DIM, 1);
}

static void draw_bpm_mark(int tempo, int beat)
{
    ui_fill(108u, UI_BPM_Y, 66u, 34u, C_BLACK);
    ui_mask(FW_UI_BPM_X, FW_UI_BPM_Y, FW_UI_BPM_W, FW_UI_BPM_H, FW_UI_BPM_MASK,
            (!tempo || beat) ? C_WHITE : C_BPM);
}

static void draw_bpm(int bpm, int clk)
{
    char b[16];
    put_u(b, bpm);
    if (clk) str_cat(b, " CLK", sizeof b);
    ui_text(UI_BPM_X, UI_BPM_Y, UI_BPM_W, 34u, &FONT_S, b, C_BPM, 0);
}

static void draw_sample(uint32_t frames, int recording)
{
    char a[16], b[8];
    if (!frames && !recording) {
        ui_text(UI_SAMPLE_X, UI_SAMPLE_Y, UI_SAMPLE_W, 34u, &FONT_S, "EMPTY", C_LINK, 0);
        return;
    }
    {   uint32_t t = (frames * 10u + FW_FS / 2u) / FW_FS;
        a[0] = 0;
        { char c[8]; put_u(c, (int32_t)t / 10); str_cat(a, c, sizeof a); }
        str_cat(a, ".", sizeof a);
        { char c[8]; put_u(c, (int32_t)t % 10); str_cat(a, c, sizeof a); }
        str_cat(a, "s", sizeof a);
    }
    if (recording) {
        ui_text(UI_SAMPLE_X, UI_SAMPLE_Y, UI_SAMPLE_W, 34u, &FONT_S, a, C_LINK, 0);
    } else {
        b[0] = 0;
        str_cat(b, "1/", sizeof b);
        { char c[8]; put_u(c, (int32_t)loop_div); str_cat(b, c, sizeof b); }
        ui_text(UI_SAMPLE_X, UI_SAMPLE_Y, UI_SAMPLE_W, 17u, &FONT_S, a, C_LINK, 0);
        ui_text(UI_SAMPLE_X, UI_SAMPLE_Y + 17u, UI_SAMPLE_W, 17u, &FONT_S, b, C_LINK, 0);
    }
}

static void draw_knobs(void)
{
    char b[8];
    put_u(b, p_speed);
    ui_text(7u, UI_KNOB_Y, 50u, 16u, &FONT_S, b, C_BLACK, 1);
    put_u(b, p_mix);
    str_cat(b, "%", sizeof b);
    ui_text(67u, UI_KNOB_Y, 50u, 16u, &FONT_S, b, C_BLACK, 1);
    put_u(b, p_correct);
    str_cat(b, "%", sizeof b);
    ui_text(126u, UI_KNOB_Y, 50u, 16u, &FONT_S, b, C_BLACK, 1);
    put_u(b, p_oct);
    ui_text(186u, UI_KNOB_Y, 50u, 16u, &FONT_S, b, C_BLACK, 1);
}

static void draw_modes(int loop, int mask)
{
    ui_fill(UI_MODES_X, UI_MODES_Y, UI_MODES_W, UI_MODES_H, C_BLACK);
    ui_icon(FW_UI_REPEAT_X, FW_UI_REPEAT_Y - 4u, FW_UI_REPEAT_W, FW_UI_REPEAT_H,
            FW_UI_REPEAT, loop ? 255u : UI_OFF_ALPHA);
    ui_icon(FW_UI_ONCE_X, FW_UI_ONCE_Y - 4u, FW_UI_ONCE_W, FW_UI_ONCE_H,
            FW_UI_ONCE, loop ? UI_OFF_ALPHA : 255u);
    ui_icon(FW_UI_MASK_X, FW_UI_MASK_Y - 4u, FW_UI_MASK_W, FW_UI_MASK_H,
            FW_UI_MASK, mask ? 255u : UI_OFF_ALPHA);
}

static void draw_rec(int recording)
{
    ui_fill(UI_REC_X, UI_REC_Y, UI_REC_W, UI_REC_H, C_BLACK);
    if (recording)
        ui_icon(FW_UI_REC_X, FW_UI_REC_Y, FW_UI_REC_W, FW_UI_REC_H, FW_UI_REC, 255u);
}

static void draw_latch(int latch)
{
    ui_fill(UI_LATCH_X, UI_LATCH_Y, UI_LATCH_W, UI_LATCH_H, C_BLACK);
    ui_icon(FW_UI_LATCH_X, FW_UI_LATCH_Y, FW_UI_LATCH_W, FW_UI_LATCH_H,
            FW_UI_LATCH, latch ? 255u : UI_OFF_ALPHA);
}

static void draw_link(int play)
{
    ui_fill(128u, 100u, 44u, 28u, C_BLACK);
    ui_icon(FW_UI_LINK_X, FW_UI_LINK_Y, FW_UI_LINK_W, FW_UI_LINK_H,
            FW_UI_LINK, play == 2 ? 255u : play ? 60u : UI_OFF_ALPHA);
}

static void draw_all(int note, int cents, int voiced, int play)
{
    ui_draw_bg();
    draw_note(note, cents, voiced);
    draw_knobs();
    draw_bpm_mark(clock_active || playing, beat_on);
    draw_bpm(tempo_now(), clock_active);
    draw_sample(rec ? s_w : s_len, rec);
    draw_modes(p_loop, p_mask);
    draw_rec(rec);
    draw_latch(p_latch);
    draw_link(play);
}

/* Only changed slots are redrawn. The full 240 x 240 background happens once, at start. */
static void draw_ui(int note, int cents, int voiced, int play, uint32_t now)
{
    static int init, s_target, s_note, s_cents, s_voiced;
    static int s_speed, s_mix, s_correct, s_oct, s_bpm, s_clk;
    static int s_loop, s_mask, s_rec, s_latch, s_play, s_tempo, s_beat;
    static uint32_t s_frames, s_div, pitch_ms, sample_ms;
    uint32_t frames = rec ? s_w : s_len;
    if (!init) {
        draw_all(note, cents, voiced, play);
        s_target = target; s_note = note; s_cents = cents; s_voiced = voiced;
        s_speed = p_speed; s_mix = p_mix; s_correct = p_correct; s_oct = p_oct;
        s_bpm = tempo_now(); s_clk = clock_active;
        s_tempo = clock_active || playing; s_beat = beat_on;
        s_loop = p_loop; s_mask = p_mask; s_rec = rec; s_latch = p_latch;
        s_play = play; s_frames = frames; s_div = loop_div;
        pitch_ms = sample_ms = now; init = 1;
        return;
    }
    if ((target != s_target || note != s_note || cents != s_cents || voiced != s_voiced) &&
        now - pitch_ms >= 100u) {
        draw_note(note, cents, voiced);
        s_target = target; s_note = note; s_cents = cents; s_voiced = voiced; pitch_ms = now;
    }
    if (p_speed != s_speed || p_mix != s_mix || p_correct != s_correct || p_oct != s_oct) {
        draw_knobs();
        s_speed = p_speed; s_mix = p_mix; s_correct = p_correct; s_oct = p_oct;
    }
    if (tempo_now() != s_bpm || clock_active != s_clk) {
        draw_bpm(tempo_now(), clock_active);
        s_bpm = tempo_now(); s_clk = clock_active;
    }
    if ((clock_active || playing) != s_tempo || beat_on != s_beat) {
        draw_bpm_mark(clock_active || playing, beat_on);
        s_tempo = clock_active || playing; s_beat = beat_on;
    }
    if (rec != s_rec) {
        draw_rec(rec);
        draw_sample(frames, rec);
        s_rec = rec; s_frames = frames; s_div = loop_div; sample_ms = now;
    } else if (frames != s_frames || loop_div != s_div) {
        if (!rec || now - sample_ms >= 100u) {
            draw_sample(frames, rec);
            s_frames = frames; s_div = loop_div; sample_ms = now;
        }
    }
    if (p_loop != s_loop || p_mask != s_mask) {
        draw_modes(p_loop, p_mask);
        s_loop = p_loop; s_mask = p_mask;
    }
    if (p_latch != s_latch) {
        draw_latch(p_latch);
        s_latch = p_latch;
    }
    if (play != s_play) {
        draw_link(play);
        s_play = play;
    }
}

static void app_init(void)
{
    load_cfg();
    usb_play_enable(1);
    atlatl_init(FW_FS);
    atlatl_fold(p_fold);
    atlatl_params(p_speed, p_mix, p_correct);
    atlatl_target(TARGET_NONE);
    tempo_recompute();
}

static void app_frame(const fw_input_t *in)
{
    uint32_t down = in->keys_pressed, up = in->keys_released;
    int k, play = usb_play_state();
    fw_midi_t m;

    gain_target = master_gain(in->master);

    while (fw_midi_read(&m)) {
        uint32_t st;
        if (m.status == 0xF8u) { clock_tick(in->ms); continue; }
        if (m.status == 0xFAu || m.status == 0xFBu) {
            clock_active = 1;
            clock_start_ms = in->ms;
            clock_count = 0;
            clock_ms = in->ms;
            beat_clocks = 0;
            beat_on = 0;
            beat_next_ms = 0;
            tempo_recompute();
            continue;
        }
        if (m.status == 0xFCu) {
            clock_active = 0;
            beat_clocks = 0;
            beat_on = 0;
            beat_next_ms = 0;
            tempo_recompute();
            continue;
        }
        st = m.status & 0xF0u;
        if (st == 0x90u && m.d2 && m.d1 < 128u) {
            midi_held_set(m.d1, 1);
            set_target(m.d1);
            target_src = SRC_MIDI;
        } else if ((st == 0x80u || st == 0x90u) && m.d1 < 128u) {
            midi_held_set(m.d1, 0);
            if (!p_latch && target_src == SRC_MIDI && target == (int)m.d1)
                target_from_held();
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
    if (clock_active && in->ms - clock_ms > 1500u) {
        clock_active = 0;
        beat_clocks = 0;
        beat_on = 0;
        beat_next_ms = 0;
        tempo_recompute();
    }

    for (k = 0; k < 27; k++) {
        if (down >> k & 1u) {
            held |= 1u << k;
            last_key = (uint32_t)k;
            fw_led_note_key((uint32_t)k, 1);
            set_target(key_note((uint32_t)k));
            target_src = SRC_KEY;
        }
        if (up >> k & 1u) {
            held &= ~(1u << k);
            fw_led_note_key((uint32_t)k, 0);
            if (!p_latch && target_src == SRC_KEY && (uint32_t)k == last_key)
                target_from_held();
        }
    }

    {
        int32_t d = in->knob[FW_KNOB_SELECT];
        if (d) {
            p_bpm += d * 2;
            if (p_bpm < 30) p_bpm = 30;
            if (p_bpm > 300) p_bpm = 300;
            tempo_recompute();
        }
    }
    if (in->knob[FW_KNOB_1]) { p_speed += in->knob[FW_KNOB_1] * 5; if (p_speed < 0) p_speed = 0; if (p_speed > 100) p_speed = 100; params_changed(); }
    if (in->knob[FW_KNOB_2]) { p_mix += in->knob[FW_KNOB_2] * 5; if (p_mix < 0) p_mix = 0; if (p_mix > 100) p_mix = 100; params_changed(); }
    if (in->knob[FW_KNOB_3]) { p_correct += in->knob[FW_KNOB_3] * 5; if (p_correct < 0) p_correct = 0; if (p_correct > 100) p_correct = 100; params_changed(); }
    if (in->knob[FW_KNOB_4]) octave(p_oct + (in->knob[FW_KNOB_4] > 0 ? 1 : in->knob[FW_KNOB_4] < 0 ? -1 : 0));

    if (rec_done)
        sample_finish();

    if (in->pressed & FW_BIT(FW_BTN_OCT_DOWN)) octave(p_oct - 1);
    if (in->pressed & FW_BIT(FW_BTN_OCT_UP)) octave(p_oct + 1);
    if (in->pressed & FW_BIT(FW_BTN_REC)) record_start();
    if (in->released & FW_BIT(FW_BTN_REC)) record_stop();
    if (in->pressed & FW_BIT(FW_BTN_PLAY)) play_toggle();
    if (in->pressed & FW_BIT(FW_BTN_SEQ)) {
        p_loop = !p_loop;
        playing = 0;
        loop_pos = 0;
        tempo_recompute();
    }
    if (in->pressed & FW_BIT(FW_BTN_ARP)) p_mask = !p_mask;
    if (in->pressed & FW_BIT(FW_BTN_FX)) p_latch = !p_latch;
    if (in->pressed & FW_BIT(FW_BTN_EDIT)) { p_fold = !p_fold; params_changed(); }
    if (in->pressed & FW_BIT(FW_BTN_SAVE)) save_cfg();

    fw_led_button(FW_BTN_PLAY, playing);
    fw_led_button(FW_BTN_REC, rec);
    fw_led_button(FW_BTN_SEQ, p_loop);
    fw_led_button(FW_BTN_ARP, p_mask);
    fw_led_button(FW_BTN_FX, p_latch);
    fw_led_button(FW_BTN_EDIT, p_fold);

    {
        uint32_t now = in->ms;
        if (playing && !clock_active) {
            uint32_t interval = 60000u / (uint32_t)tempo_now();
            if (!beat_next_ms) {
                beat_on = 0;
                beat_next_ms = now + interval;
            } else if ((int32_t)(now - beat_next_ms) >= 0) {
                beat_on = !beat_on;
                beat_next_ms += interval;
                if ((int32_t)(now - beat_next_ms) >= 0)
                    beat_next_ms = now + interval;
            }
        } else if (!clock_active) {
            beat_next_ms = 0;
            beat_on = 0;
        } else {
            beat_next_ms = 0;
        }
    }

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
    int gate = !p_mask || p_latch || target >= 0 || held || midi_any;

    if (!(usb_play_begin() && usb_play_read(usb_in, frames)))
        memset(usb_in, 0, frames * 2u * sizeof usb_in[0]);

    if (rec) {
        for (i = 0; i < frames; i++) {
            int32_t l = usb_in[2u * i], r = usb_in[2u * i + 1u];
            int32_t m = (l < 0 ? -l : l) >= (r < 0 ? -r : r) ? l : r;   /* mono: the louder channel */
            if (s_w < SAMPLE_MAX)
                sample[s_w++] = (int16_t)m;
            if (s_w >= SAMPLE_MAX) { rec = 0; rec_done = 1; break; }
        }
    }

    atlatl_process(usb_in, lr, frames);

    for (i = 0; i < frames; i++) {
        int32_t v = 0, l, r;
        if (playing) {
            if (p_loop) {
                if (!loop_period) {
                    playing = 0;
                } else {
                    if (loop_pos >= loop_period) loop_pos = 0;
                    if (loop_pos < loop_play) v = sample[loop_pos];
                    loop_pos++;
                }
            } else if (s_r < s_len) {
                v = sample[s_r++];
            } else {
                playing = 0;
            }
        }
        if (!gate) {
            l = r = 0;
        } else {
            l = lr[2u * i] + v;
            r = lr[2u * i + 1u] + v;
        }
        g += step;
        l = (l * g) >> 15;
        r = (r * g) >> 15;
        lr[2u * i] = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
        lr[2u * i + 1u] = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
    }
    gain = g1;
}
