/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37's pictures: drawn into gfx.c's canvas a strip at a time (the screen's rows sv_y0 .. sv_y0 + strip height;
 * every coordinate here is the screen's). No hardware, no float: the host test renders them (tests/sp_screen.c).
 *   the display   the Speak & Spell's: a dark glass, a line of 20 sixteen-segment ("starburst") characters (two lines
 *                 here, the sentence wrapped at its words), cyan-green like a vacuum fluorescent display: the word
 *                 spoken brightest (near white) with a glow around its segments, underlined, and always on the first line (the sentence
 *                 scrolls), the words before it dimmer, the ones to come bright; the unlit segments a faint ghost, as on the real glass; a full stop or a comma is the
 *                 decimal point of the character before it
 *   the mouth     the speaking voice's filter (its K1..K10 now) as a spectrum, 24 bars
 *   the knob block, the header, the popups, the boot (sp_ui.c puts them together) */
/* the gfx.c palette Speak37 runs: NIGHT (black, not the GREY one: GREY blends text on its red channel alone, so a
 * colour without red, the cyan of ROBOT, came out black) */
#define SP_PALETTE 8u
#define SV_BG RGB(4, 6, 8)
#define SV_GLASS RGB(2, 10, 10)
#define SV_RIM RGB(30, 44, 46)
#define SV_GHOST RGB(5, 16, 15)              /* an unlit segment: a faint trace, as on the real glass */
#define SV_DIM RGB(60, 150, 132)             /* the words said */
#define SV_MID RGB(110, 225, 200)            /* the words to come: clearly readable */
#define SV_ON RGB(235, 255, 250)             /* the word spoken: near white, underlined */
#define SV_GLOW RGB(26, 110, 94)
#define SV_GRID RGB(34, 44, 48)
#define SV_TAG RGB(90, 104, 110)              /* a knob's tag in its cell (K5, 1) */
#define SV_LAB RGB(255, 128, 48)             /* labels: the Speak & Spell's orange */
#define SV_TEXT RGB(225, 235, 235)
#define SV_HOT RGB(255, 200, 120)
#define SV_RED RGB(255, 60, 40)

/* sin(2 pi i / 64), Q14 (the boot's wave, the spectrum) */
static const int16_t SV_SIN64[64] = {
    0, 1606, 3196, 4756, 6270, 7723, 9102, 10394, 11585, 12665, 13623, 14449, 15137, 15679, 16069, 16305, 16384,
    16305, 16069, 15679, 15137, 14449, 13623, 12665, 11585, 10394, 9102, 7723, 6270, 4756, 3196, 1606, 0, -1606,
    -3196, -4756, -6270, -7723, -9102, -10394, -11585, -12665, -13623, -14449, -15137, -15679, -16069, -16305,
    -16384, -16305, -16069, -15679, -15137, -14449, -13623, -12665, -11585, -10394, -9102, -7723, -6270, -4756,
    -3196, -1606};
#include "vo_font.h"                       /* AF_T: the header's face (build.py) */

static int32_t sv_y0, sv_h;                /* the strip being drawn: screen rows sv_y0 .. sv_y0 + sv_h */

static void sv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    if (y + h <= sv_y0 || y >= sv_y0 + sv_h)
        return;
    cv_rect(x, y - sv_y0, w, h, c);
}
static void sv_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    if ((y0 < sv_y0 && y1 < sv_y0) || (y0 >= sv_y0 + sv_h && y1 >= sv_y0 + sv_h))
        return;
    cv_line(x0, y0 - sv_y0, x1, y1 - sv_y0, c);
}
static void sv_frame(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    sv_rect(x, y, w, 1, c);
    sv_rect(x, y + h - 1, w, 1, c);
    sv_rect(x, y, 1, h, c);
    sv_rect(x + w - 1, y, 1, h, c);
}
static int32_t sv_text(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t c)
{
    if (y + (int32_t)f->h <= sv_y0 || y >= sv_y0 + sv_h)
        return x + text_w(f, s);
    return cv_text(x, y - sv_y0, f, s, c);
}
static int32_t sv_text_on(int32_t x, int32_t y, const aafont_t *f, const char *s, uint16_t c, uint16_t bg)
{
    if (y + (int32_t)f->h <= sv_y0 || y >= sv_y0 + sv_h)
        return x + text_w(f, s);
    return cv_text_on(x, y - sv_y0, f, s, c, bg);
}
static void sv_begin(int32_t y0, int32_t h)
{
    sv_y0 = y0;
    sv_h = h;
    cv_begin(240, (uint32_t)h, SV_BG);
}
/* a rounded box (radius 3): the glass, the popups */
static void sv_rbox(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t fill, uint16_t rim)
{
    sv_rect(x + 3, y, w - 6, h, fill);
    sv_rect(x, y + 3, w, h - 6, fill);
    sv_rect(x + 1, y + 1, w - 2, h - 2, fill);
    sv_rect(x + 3, y, w - 6, 1, rim);
    sv_rect(x + 3, y + h - 1, w - 6, 1, rim);
    sv_rect(x, y + 3, 1, h - 6, rim);
    sv_rect(x + w - 1, y + 3, 1, h - 6, rim);
    sv_rect(x + 1, y + 1, 2, 1, rim);
    sv_rect(x + 1, y + 2, 1, 1, rim);
    sv_rect(x + w - 3, y + 1, 2, 1, rim);
    sv_rect(x + w - 2, y + 2, 1, 1, rim);
    sv_rect(x + 1, y + h - 2, 2, 1, rim);
    sv_rect(x + 1, y + h - 3, 1, 1, rim);
    sv_rect(x + w - 3, y + h - 2, 2, 1, rim);
    sv_rect(x + w - 2, y + h - 3, 1, 1, rim);
}
/* a bar: v of 127 */
static void sv_bar(int32_t x, int32_t y, int32_t w, int32_t h, int32_t v, uint16_t col)
{
    v = v < 0 ? 0 : v > 127 ? 127 : v;
    sv_frame(x, y, w, h, SV_GRID);
    sv_rect(x + 1, y + 1, (w - 2) * v / 127, h - 2, col);
}

/* ------------------------------------------------------ sixteen segments --- */
/* the segments: A1 A2 the top halves, B C the right side (upper, lower), D2 D1 the bottom halves, E F the left side
 * (lower, upper), G1 G2 the middle halves, H the diagonal from the top-left corner to the centre, I the upper
 * centre stroke, J the top-right diagonal, K the diagonal from the centre to the bottom-left corner, L the lower
 * centre stroke, M the bottom-right diagonal */
enum { S_A1 = 1, S_A2 = 2, S_B = 4, S_C = 8, S_D2 = 16, S_D1 = 32, S_E = 64, S_F = 128, S_G1 = 256, S_G2 = 512,
       S_H = 1024, S_I = 2048, S_J = 4096, S_K = 8192, S_L = 16384, S_M = 32768 };
#define S_A (S_A1 | S_A2)
#define S_D (S_D1 | S_D2)
#define S_G (S_G1 | S_G2)
static uint16_t sv_glyph(char ch)
{
    switch (ch) {
    case '0': return S_A | S_B | S_C | S_D | S_E | S_F | S_J | S_K;
    case '1': return S_B | S_C | S_J;
    case '2': return S_A | S_B | S_G | S_E | S_D;
    case '3': return S_A | S_B | S_C | S_D | S_G2;
    case '4': return S_F | S_G | S_B | S_C;
    case '5': return S_A | S_F | S_G | S_C | S_D;
    case '6': return S_A | S_F | S_E | S_D | S_C | S_G;
    case '7': return S_A | S_B | S_C;
    case '8': return S_A | S_B | S_C | S_D | S_E | S_F | S_G;
    case '9': return S_A | S_B | S_C | S_D | S_F | S_G;
    case 'A': return S_A | S_B | S_C | S_E | S_F | S_G;
    case 'B': return S_A | S_B | S_C | S_D | S_E | S_F | S_G2;
    case 'C': return S_A | S_F | S_E | S_D;
    case 'D': return S_A | S_B | S_C | S_D | S_I | S_L;
    case 'E': return S_A | S_F | S_E | S_D | S_G;
    case 'F': return S_A | S_F | S_E | S_G;
    case 'G': return S_A | S_F | S_E | S_D | S_C | S_G2;
    case 'H': return S_F | S_E | S_B | S_C | S_G;
    case 'I': return S_A | S_I | S_L | S_D;
    case 'J': return S_B | S_C | S_D | S_E;
    case 'K': return S_F | S_E | S_G1 | S_J | S_M;
    case 'L': return S_F | S_E | S_D;
    case 'M': return S_F | S_E | S_B | S_C | S_H | S_J;
    case 'N': return S_F | S_E | S_B | S_C | S_H | S_M;
    case 'O': return S_A | S_B | S_C | S_D | S_E | S_F;
    case 'P': return S_A | S_B | S_F | S_E | S_G;
    case 'Q': return S_A | S_B | S_C | S_D | S_E | S_F | S_M;
    case 'R': return S_A | S_B | S_F | S_E | S_G | S_M;
    case 'S': return S_A | S_F | S_G | S_C | S_D;
    case 'T': return S_A | S_I | S_L;
    case 'U': return S_F | S_E | S_D | S_C | S_B;
    case 'V': return S_F | S_E | S_K | S_J;
    case 'W': return S_F | S_E | S_B | S_C | S_K | S_M;
    case 'X': return S_H | S_J | S_K | S_M;
    case 'Y': return S_H | S_J | S_L;
    case 'Z': return S_A | S_J | S_K | S_D;
    case '\'': return S_J;
    case '-': return S_G;
    case '?': return S_A | S_B | S_G2 | S_L;
    case '!': return S_I | S_L;
    case ':': return S_I | S_L;
    case '+': return S_G | S_I | S_L;
    case '/': return S_J | S_K;
    default: return 0;
    }
}

/* one segment of the character at (x, y) (its box 10 x 24 px: strokes 2 px, the centre strokes on x 4..5), grown by
 * g px (the glow) */
static void sv_seg(int32_t x, int32_t y, uint32_t s, int32_t g, uint16_t c)
{
    switch (s) {
    case S_A1: sv_rect(x + 1 - g, y - g, 4 + 2 * g, 2 + 2 * g, c); return;
    case S_A2: sv_rect(x + 5 - g, y - g, 4 + 2 * g, 2 + 2 * g, c); return;
    case S_B: sv_rect(x + 8 - g, y + 2 - g, 2 + 2 * g, 9 + 2 * g, c); return;
    case S_C: sv_rect(x + 8 - g, y + 13 - g, 2 + 2 * g, 9 + 2 * g, c); return;
    case S_D2: sv_rect(x + 5 - g, y + 22 - g, 4 + 2 * g, 2 + 2 * g, c); return;
    case S_D1: sv_rect(x + 1 - g, y + 22 - g, 4 + 2 * g, 2 + 2 * g, c); return;
    case S_E: sv_rect(x - g, y + 13 - g, 2 + 2 * g, 9 + 2 * g, c); return;
    case S_F: sv_rect(x - g, y + 2 - g, 2 + 2 * g, 9 + 2 * g, c); return;
    case S_G1: sv_rect(x + 1 - g, y + 11 - g, 4 + 2 * g, 2 + 2 * g, c); return;
    case S_G2: sv_rect(x + 5 - g, y + 11 - g, 4 + 2 * g, 2 + 2 * g, c); return;
    case S_I: sv_rect(x + 4 - g, y + 3 - g, 2 + 2 * g, 7 + 2 * g, c); return;
    case S_L: sv_rect(x + 4 - g, y + 14 - g, 2 + 2 * g, 7 + 2 * g, c); return;
    default: {                                             /* the diagonals: two lines side by side (the glow: four) */
        int32_t x0, y0, x1, y1, k;
        if (s == S_H) { x0 = x + 2; y0 = y + 3; x1 = x + 3; y1 = y + 10; }
        else if (s == S_J) { x0 = x + 7; y0 = y + 3; x1 = x + 6; y1 = y + 10; }
        else if (s == S_K) { x0 = x + 3; y0 = y + 14; x1 = x + 2; y1 = y + 21; }
        else { x0 = x + 6; y0 = y + 14; x1 = x + 7; y1 = y + 21; }
        if (s == S_J || s == S_K)
            for (k = -g; k <= g; k++)
                sv_line(x0 + k, y0, x1 + k, y1, c), sv_line(x0 + k - 1, y0, x1 + k - 1, y1, c);
        else
            for (k = -g; k <= g; k++)
                sv_line(x0 + k, y0, x1 + k, y1, c), sv_line(x0 + k + 1, y0, x1 + k + 1, y1, c);
    }
    }
}

/* a character: its lit segments in c (a glow under them when glow), the others a ghost; dp: the decimal point */
static void sv_char(int32_t x, int32_t y, char ch, uint32_t dp, uint16_t c, uint32_t glow)
{
    uint32_t m = sv_glyph(ch), s;
    for (s = 1; s <= 32768u; s <<= 1)                      /* the ghost of every segment */
        if (!(m & s))
            sv_seg(x, y, s, 0, SV_GHOST);
    sv_rect(x + 10, y + 22, 1, 2, dp ? c : SV_GHOST);
    if (glow) {
        for (s = 1; s <= 32768u; s <<= 1)
            if (m & s)
                sv_seg(x, y, s, 1, SV_GLOW);
        if (dp)
            sv_rect(x + 9, y + 21, 3, 4, SV_GLOW);
    }
    for (s = 1; s <= 32768u; s <<= 1)
        if (m & s)
            sv_seg(x, y, s, 0, c);
    if (dp)
        sv_rect(x + 10, y + 22, 1, 2, c);
}

/* the display: 2 lines of SV_COLS characters at (x, y), the words of s (between spaces) wrapped at their words; the
 * word cur (-1 none) brightest, the ones before dim; the two lines shown are those around cur */
#define SV_COLS 20
#define SV_CW 11                           /* a character's pitch */
#define SV_LH 30                           /* a line's */
typedef struct { uint8_t line, col, len, dp; } sv_word_t;
static uint32_t sv_layout(const char *s, sv_word_t *w, uint32_t maxw)
{
    uint32_t n = 0, line = 0, col = 0;
    while (*s && n < maxw) {
        uint32_t len = 0, cells = 0;
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        while (s[len] && s[len] != ' ')
            len++;
        {
            uint32_t i;
            for (i = 0; i < len; i++)
                cells += s[i] != '.' && s[i] != ',';
        }
        if (cells > SV_COLS)
            cells = SV_COLS;
        if (col && col + cells > SV_COLS) {
            line++;
            col = 0;
        }
        w[n].line = (uint8_t)line;
        w[n].col = (uint8_t)col;
        w[n].len = (uint8_t)len;
        w[n].dp = 0;
        n++;
        col += cells + 1u;
        s += len;
    }
    return n;
}
static void sv_display(int32_t x, int32_t y, const char *s, int32_t cur, uint32_t t)
{
    sv_word_t w[40];
    const char *p = s;
    uint32_t n = sv_layout(s, w, 40), i, top = 0, k;
    char cells[2][SV_COLS];
    uint8_t dps[2][SV_COLS], lvl[2][SV_COLS];
    for (i = 0; i < 2u; i++)
        for (k = 0; k < SV_COLS; k++)
            cells[i][k] = ' ', dps[i][k] = 0, lvl[i][k] = 1;
    if (cur >= 0 && (uint32_t)cur < n)                     /* the lines shown: the word spoken always on the first */
        top = w[cur].line;
    for (i = 0; i < n; i++) {
        uint32_t len, j, c;
        while (*p == ' ')
            p++;
        len = w[i].len;
        if (w[i].line >= top && w[i].line < top + 2u) {
            uint32_t L = w[i].line - top;
            for (j = 0, c = w[i].col; j < len && c <= SV_COLS; j++) {
                if (p[j] == '.' || p[j] == ',') {          /* the decimal point of the character before */
                    if (c)
                        dps[L][c - 1u] = 1;
                    continue;
                }
                if (c < SV_COLS) {
                    cells[L][c] = p[j];
                    lvl[L][c] = cur < 0 ? 1u : (int32_t)i < cur ? 0u : (int32_t)i == cur ? 2u : 1u;
                }
                c++;
            }
        }
        p += len;
    }
    for (i = 0; i < 2u; i++)
        for (k = 0; k < SV_COLS; k++) {
            uint32_t l = lvl[i][k];
            int32_t cx = x + (int32_t)k * SV_CW, cy = y + (int32_t)i * SV_LH;
            if (l == 2u && cells[i][k] != ' ') {           /* the word spoken: a bright bar under it, glowing */
                sv_rect(cx - 1, cy + 25, SV_CW + 1, 4, SV_GLOW);
                sv_rect(cx, cy + 26, SV_CW - 1, 2, SV_ON);
            }
            sv_char(cx, cy, cells[i][k], dps[i][k], l == 2u ? SV_ON : l == 1u ? SV_MID : SV_DIM, l == 2u);
        }
    (void)t;
}

/* ------------------------------------------------------------ the mouth --- */
/* the filter of reflection coefficients k (Q15) as 24 bars (100 Hz .. 3.8 kHz at 8 kHz), heights 0..h; e: the energy
 * (0: the bars fall) */
static void sv_mouth(int32_t x, int32_t y, int32_t w, int32_t h, const int32_t *k, int32_t e, uint8_t *hold)
{
    int32_t a[11], b[11], i, j, n = 24;
    a[0] = 1 << 14;                                        /* the step-up: K -> A(z), Q14 */
    for (i = 1; i <= 10; i++)
        a[i] = 0;
    for (i = 0; i < 10; i++) {
        for (j = 1; j <= i; j++)
            b[j] = a[j] + (int32_t)((int64_t)k[i] * a[i + 1 - j] >> 15);
        for (j = 1; j <= i; j++)
            a[j] = b[j];
        a[i + 1] = k[i] >> 1;
    }
    for (i = 0; i < n; i++) {
        /* |A(e^jw)|^2 at w = 2 pi f / 8000, f log spaced; the bar: -log of it (the spectrum's height) */
        int32_t fi = 2 + i * 2 + i * i / 12, re = 0, im = 0, lv, bh;      /* (in 1/64 of 4 kHz: 0.1 .. 3.8 kHz) */
        uint64_t mag, m;
        for (j = 0; j <= 10; j++) {
            uint32_t ph = (uint32_t)(fi * j) & 127u;       /* w j in 1/128 of a turn */
            int32_t c = SV_SIN64[((ph >> 1) + 16u) & 63u], s = SV_SIN64[(ph >> 1) & 63u];
            re += a[j] * c >> 14;
            im -= a[j] * s >> 14;
        }
        mag = (uint64_t)((int64_t)re * re + (int64_t)im * im) >> 8;   /* |A|^2 x 2^20 */
        for (lv = 0, m = 1; m < mag && lv < 40; lv++, m <<= 1)
            ;
        bh = e ? (34 - lv) * h / 26 : 0;                   /* 1 / |A|: a low |A| is a formant */
        bh = bh < 0 ? 0 : bh > h ? h : bh;
        if (e && e < 400)
            bh = bh * e / 400;
        if (bh >= hold[i])
            hold[i] = (uint8_t)bh;
        else
            hold[i] = (uint8_t)(hold[i] > 1 ? hold[i] - 2 : 0);
        bh = hold[i];
        for (j = 0; j < bh; j += 3)                        /* a VFD bar graph: dashes */
            sv_rect(x + i * w / n, y + h - 2 - j, w / n - 2, 2, j + 3 >= bh ? SV_ON : SV_MID);
        if (!bh)
            sv_rect(x + i * w / n, y + h - 2, w / n - 2, 2, SV_GHOST);
    }
}
