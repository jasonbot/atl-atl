/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Modified for GATOR by npi3pak, 2026, from ORBIT 0.6.0 (dspaudio/orbit) */
/* Small-canvas renderer (no full framebuffer). Draw text/lines into
 * an off-screen strip, then blit it in one DMA transfer. Pixels are stored
 * byte-swapped (the panel takes RGB565 big-endian). */
typedef struct {               /* proportional, see tools/gen_font.py */
    uint8_t h;
    uint8_t pad;               /* bitmap starts this many pixels left of the pen */
    uint8_t first, last;
    uint8_t sh;                /* 1: the bitmaps are another font's, drawn at 2x (L = S at 2x, no copy) */
    const uint8_t *adv;        /* advance per glyph */
    const uint8_t *bw;         /* bitmap width per glyph (starts FONT_PAD left of the pen) */
    const uint16_t *off;       /* byte offset of each glyph */
    const uint8_t *data;
} felucca_font_t;
#include "felucca_font.h"
#include "fw_ui.h"

#define CV_MAX (240u * 64u)       /* text rows and the graph strip; keeps the pool for the 3 s sample */
static uint16_t cv_px[CV_MAX] FW_POOL;
static uint32_t cv_w, cv_h;
static int32_t cv_oy;            /* y offset for graph drawing */

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BLACK 0x0000u
#define C_WHITE 0xFFFFu
#define C_GRAY RGB(130, 130, 140)
#define C_DIM RGB(70, 70, 80)
#define C_HI RGB(230, 230, 240)
#define C_ACCENT RGB(40, 124, 255)
#define C_GREEN RGB(171, 200, 55)
#define C_LIME RGB(205, 222, 135)
#define C_RED RGB(255, 0, 0)
#define C_YELLOW RGB(255, 204, 0)
#define C_LATCH RGB(255, 238, 170)
#define C_BPM RGB(212, 255, 42)
#define C_LINK RGB(170, 212, 0)

static inline uint16_t swap16(uint32_t c) { return (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8)); }

static void cv_begin(uint32_t w, uint32_t h, uint16_t bg)
{
    uint32_t i, n;
    if (w * h > CV_MAX)
        h = CV_MAX / w;
    lcd_sync();                     /* the last blit may still read cv_px */
    cv_w = w;
    cv_h = h;
    n = w * h;
    for (i = 0; i < n; i++)
        cv_px[i] = swap16(bg);
}

static void cv_blit(uint32_t x, uint32_t y) { lcd_blit(x, y, cv_w, cv_h, cv_px); }

/* canvas rows r0 .. cv_h-1 only, to screen row y + r0 */
static void cv_blit_from(uint32_t x, uint32_t y, uint32_t r0)
{
    if (r0 < cv_h)
        lcd_blit(x, y + r0, cv_w, cv_h - r0, cv_px + r0 * cv_w);
}

static inline void cv_pset(int32_t x, int32_t y, uint16_t c)
{
    y += cv_oy;
    if ((uint32_t)x < cv_w && (uint32_t)y < cv_h)
        cv_px[(uint32_t)y * cv_w + (uint32_t)x] = swap16(c);
}

static void cv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t x1 = x + w, y1 = y + h + cv_oy, i;
    uint16_t sc = swap16(c);
    y += cv_oy;                             /* clipped once, then filled row by row */
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > (int32_t)cv_w)
        x1 = (int32_t)cv_w;
    if (y1 > (int32_t)cv_h)
        y1 = (int32_t)cv_h;
    for (; y < y1; y++)
        for (i = x; i < x1; i++)
            cv_px[(uint32_t)y * cv_w + (uint32_t)i] = sc;
}

static void cv_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy, guard = 2000;
    while (guard--) {                       /* bounded: a line is never longer than 480 px */
        int32_t e2 = 2 * err;               /* both tests use the same error value */
        cv_pset(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* glyph index of a character: lower case folds to upper case when the font
 * has none, anything missing (and C1 controls) draws as '?' */
static uint32_t glyph(const felucca_font_t *f, uint32_t ch)
{
    if (ch >= 'a' && ch <= 'z' && f->last < 'a')
        ch -= 32u;
    if (ch < f->first || ch > f->last || (ch >= 127u && ch < 160u))
        ch = '?';
    return ch - f->first;
}

/* text, alpha-blended onto black with colour c; returns the end x */
static int32_t cv_text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    uint16_t ramp[16];
    uint32_t r = c >> 11, g = (c >> 5) & 63u, b = c & 31u, a;
    for (a = 0; a < 16u; a++)
        ramp[a] = (uint16_t)(((r * a / 15u) << 11) | ((g * a / 15u) << 5) | (b * a / 15u));
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w, bpr;
        const uint8_t *gd;
        w = f->bw[gi];
        bpr = ((w >> f->sh) + 1u) / 2u;
        gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t sx = gx >> f->sh, v = gd[(gy >> f->sh) * bpr + sx / 2u];
                v = (sx & 1u) ? (v & 15u) : (v >> 4);
                if (v)
                    cv_pset(x - f->pad + (int32_t)gx, y + (int32_t)gy, ramp[v]);
            }
        x += f->adv[gi];
    }
    return x;
}

static int32_t text_w(const felucca_font_t *f, const char *s)
{
    int32_t w = 0;
    for (; *s; s++)
        w += f->adv[glyph(f, (uint8_t)*s)];
    return w;
}

/* one-shot: text in a box, cleared to black, blitted */
/* the boot logo (fw_logo.h, tools/gen_logo.py), centred, in canvas strips: the LCD DMA reads RAM only */
static void draw_logo(void)
{
#if FW_LOGO
    const uint32_t strip = CV_MAX / FW_LOGO_W;
    uint32_t y, i;
    for (y = 0; y < FW_LOGO_H; y += strip) {
        const uint32_t h = FW_LOGO_H - y < strip ? FW_LOGO_H - y : strip;
        const uint8_t *src = &FW_LOGO_PX[y * FW_LOGO_W];
        cv_begin(FW_LOGO_W, h, C_BLACK);
        for (i = 0; i < FW_LOGO_W * h; i++)
            cv_px[i] = swap16(FW_LOGO_PAL[src[i]]);
        cv_blit((240u - FW_LOGO_W) / 2u, (240u - FW_LOGO_H) / 2u + y);
    }
    lcd_sync();
#endif
}

static void draw_text_box(uint32_t x, uint32_t y, uint32_t w, const felucca_font_t *f, const char *s,
                          uint16_t c, int align)
{
    int32_t tw = text_w(f, s), tx = 0;
    cv_begin(w, f->h, C_BLACK);
    if (align == 1)
        tx = ((int32_t)w - tw) / 2;
    else if (align == 2)
        tx = (int32_t)w - tw;
    cv_text(tx, 0, f, s, c);
    cv_blit(x, y);
    lcd_sync();                     /* one-shots (boot, crash, UBOOT, update) finish here */
}



#if FW_UI
/* The UI raster is a 256-colour background. Live values are drawn over the matching pixels, so the gradients and
 * the box fills survive. */
static void ui_pixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t r, c;
    cv_begin(w, h, C_BLACK);
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++)
            cv_px[r * w + c] = swap16(FW_UI_PAL[FW_UI_PX[(y + r) * FW_UI_W + x + c]]);
}

static void ui_draw_bg(void)
{
    const uint32_t strip = CV_MAX / FW_UI_W;
    uint32_t y, i;
    for (y = 0; y < FW_UI_H; y += strip) {
        const uint32_t h = FW_UI_H - y < strip ? FW_UI_H - y : strip;
        cv_begin(FW_UI_W, h, C_BLACK);
        for (i = 0; i < FW_UI_W * h; i++)
            cv_px[i] = swap16(FW_UI_PAL[FW_UI_PX[y * FW_UI_W + i]]);
        cv_blit(0, y);
    }
    lcd_sync();
}

static void ui_text(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const felucca_font_t *f,
                    const char *s, uint16_t c, int align)
{
    int32_t tw = text_w(f, s), tx = 0;
    ui_pixels(x, y, w, h);
    if (align == 1)
        tx = ((int32_t)w - tw) / 2;
    else if (align == 2)
        tx = (int32_t)w - tw;
    cv_text(tx, ((int32_t)h - (int32_t)f->h) / 2, f, s, c);
    cv_blit(x, y);
    lcd_sync();
}

static void ui_text_bottom(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const felucca_font_t *f,
                           const char *s, uint16_t c)
{
    ui_pixels(x, y, w, h);
    cv_text(0, (int32_t)h - (int32_t)f->h, f, s, c);
    cv_blit(x, y);
    lcd_sync();
}

static void ui_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    cv_begin(w, h, c);
    cv_blit(x, y);
    lcd_sync();
}

static uint16_t ui_fade(uint16_t c, uint8_t a)
{
    uint32_t r = (c >> 11) * a / 255u;
    uint32_t g = ((c >> 5) & 63u) * a / 255u;
    uint32_t b = (c & 31u) * a / 255u;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* A live icon cropped from the SVG raster: alpha 0..255, black where the icon is empty. */
static void ui_icon(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *icon, uint8_t alpha)
{
    uint32_t i;
    cv_begin(w, h, C_BLACK);
    for (i = 0; i < w * h; i++)
        if (icon[i])
            cv_px[i] = swap16(ui_fade(icon[i], alpha));
    cv_blit(x, y);
    lcd_sync();
}

/* The same, but the shape is an alpha mask and the caller picks its colour. */
static void ui_mask(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t *mask, uint16_t c)
{
    uint32_t i;
    cv_begin(w, h, C_BLACK);
    for (i = 0; i < w * h; i++)
        if (mask[i])
            cv_px[i] = swap16(ui_fade(c, mask[i]));
    cv_blit(x, y);
    lcd_sync();
}
#endif
