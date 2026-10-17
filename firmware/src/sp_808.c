/* SPDX-License-Identifier: MIT
 * Copyright (c) 2020 Electrosmith, Corp, Emilie Gillet; 2026 M-VAVE; 2026 M. Geisler (genkigenki/makkuro).
 * See LICENSES/MIT-fm1_open-drum808.txt. */
/* RHYTHM's 808: FM1 Open's 808 drum voices (third_party/fm1_open/drum808.c, M-VAVE's DaisySP / Plaits adaptation)
 * ported to fixed point for a CPU without an FPU in use (Felucca's platform: no float anywhere). The same voices,
 * sample for sample: the kick's pulse into a swept resonant band-pass with its body and the 28 Hz high-pass, the
 * snare's shells and its noise, the toms' and congas' sines, the rim and the clave, the clap's three pulses and tail,
 * the cowbell's two squares and the hats' and cymbal's six metallic squares; the constants are drum808.c's own
 * (tools/gen_808.c runs it and writes sp_808_tab.h). Kept from drum808.c: one voice a sound (here: a lane), the
 * kit's defaults (no DRIVE, no sends), the closed hat choking the open one, a voice ending when it has been quiet
 * 20 ms (after its first 50 ms). Not kept: the parameters' editing, the pans (sp_rhythm.c pans the lanes).
 *
 * Numbers: the signals Q24 (1.0 = 1 << 24, drum808.c's float 1.0), the envelopes and gains Q30, the filters'
 * coefficients Q28 (sp_808_tab.h). Products in 64 bits; a 32-bit divide a sample in the soft clips (the snare's
 * shell, the metals' swing, the kick's diode while negative). tests/sp_808_test.c compares every sound with
 * drum808.c's, sample for sample. */
#include "sp_808_tab.h"

#define S8_ONE (1 << 24)
#define S8_Q30 (1 << 30)
#define S8_Q28 (1 << 28)

typedef struct { int32_t low, band; } s8_svf_t;
typedef struct {
    uint8_t on, s;
    uint32_t age, quiet, pc, fmc, clap, rnd, ph[6];
    int32_t env, lvl, choke, chd;          /* Q30 */
    int32_t pulse, plp, retrig, last, peak; /* Q24; the clap's pulse Q30 */
    int32_t fmlp, acc;                     /* Q30: the kick's FM gate, smoothed; the hit's velocity */
    int32_t k[3];                          /* per sound, from its constants at the hit (see s8_hit) */
    int32_t c0[3];                         /* the kick's swept filter (g, r + g, h; Q28) */
    s8_svf_t f[4];
} s8_voice_t;

static inline int32_t s8_m30(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 30); }
static inline int32_t s8_m28(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 28); }
static inline int32_t s8_m24(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 24); }
static inline int32_t s8_d30(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b + (1 << 29)) >> 30); }
static inline int32_t s8_abs(int32_t x) { return x < 0 ? -x : x; }

/* the trapezoidal SVF (stmlib / Plaits, as dsp_math.h's): mode 0 low, 1 band, 2 high */
static inline int32_t s8_svf(s8_svf_t *s, const int32_t *c, int32_t x, uint32_t mode)
{
    int32_t hp = s8_m28(x - s8_m28(c[1], s->band) - s->low, c[2]);
    int32_t gh = s8_m28(c[0], hp), bp = gh + s->band, gb, lp;
    s->band = bp + gh;
    gb = s8_m28(c[0], bp);
    lp = gb + s->low;
    s->low = lp + gb;
    return mode == 0 ? lp : mode == 1 ? bp : hp;
}

/* dsp_noise: xorshift32, -1 .. 1 (Q24) */
static inline int32_t s8_noise(uint32_t *st)
{
    uint32_t n = *st;
    n ^= n << 13;
    n ^= n >> 17;
    n ^= n << 5;
    *st = n;
    return ((int32_t)(n >> 8) - 0x800000) * 2;
}

/* num / den (Q24 both, den >= 1.0): the quotient Q24 (den to 8 fraction bits: a 32-bit divide) */
static inline int32_t s8_div(int32_t num, int32_t den)
{
    return (num / (den >> 16)) * 256;
}

/* the sine of a phase (Q24): Felucca's table (dsp.c), as dsp_sin of drum808.c's phase */
static inline int32_t s8_sin(uint32_t ph) { return sine_i(ph) * 512; }

/* a hit of sound s at velocity vel (Q15) on voice v: drum808_trigger (a new voice, its noise kept) */
static void s8_hit(s8_voice_t *v, uint32_t s, int32_t vel)
{
    const sp8_const_t *C = &SP8[s];
    uint32_t rnd = v->rnd;
    int32_t t;
    memset(v, 0, sizeof *v);
    v->rnd = rnd ? rnd : 0x716fbc31u + s * 0x7193u;
    v->s = (uint8_t)s;
    v->on = 1;
    v->env = v->lvl = v->choke = v->chd = S8_Q30;
    v->acc = vel << 15;                                    /* (Q15 -> Q30) */
    v->pc = 44;
    v->fmc = 265;
    switch (s) {
    case S8_BD:                                            /* .08 (tone + .25): the pulse into the tone filter */
        v->k[0] = s8_m30(85899346, C->tone + 268435456);  /* (Q30) */
        v->k[1] = 56908317 + s8_m30(85899346, v->acc);    /* .053 + .08 accent: the pitch's drop */
        v->c0[0] = C->svf[0][0];
        v->c0[1] = C->svf[0][1];
        v->c0[2] = C->svf[0][2];
        break;
    case S8_SD:
        t = S8_Q30 - C->attack;                            /* shell = softclip((1.5 + 4.5 (1 - t)^2) a + (.15 + 2 t) b), */
        v->k[0] = 402653184 + (s8_m30(s8_m30(t, t), 1207959552));        /* (Q28) */
        v->k[1] = 40265318 + (C->attack >> 1);            /* t: the snare's TONE (its attack slot); Q28: 2 t */
        v->k[2] = s8_m30(s8_m30(C->pgain, S8_Q30 - C->snappy), 1449551462);   /* 5.4 pgain (1 - snappy) (Q22) */
        break;
    default:
        break;
    }
}

/* drum808.c's percussion(): one sample of voice v (Q24, before its gain) */
static inline int32_t s8_sample(s8_voice_t *v)
{
    const sp8_const_t *C = &SP8[v->s];
    int32_t r = 0;
    switch (v->s) {
    case S8_BD: {
        int32_t p, x, bp, body, m;
        if (v->pc) {
            --v->pc;
            v->pulse = (v->pc ? 10 : 9) * S8_ONE;
        } else
            v->pulse = s8_m30(v->pulse, 952002388);        /* x .886621315 */
        p = v->pulse;
        v->plp += s8_m30(p - v->plp, 243478872);           /* x .22675737 */
        x = p - v->plp + s8_m30(p, 47244640);              /* + .044 p, into the diode */
        if (x < 0)
            x = s8_div(s8_m30(x, 1503238554), S8_ONE - 2 * x);   /* 1.4 x / (1 + 2 |x|) */
        {
            int32_t fm = v->fmc ? S8_Q30 : 0;
            if (v->fmc && !--v->fmc)
                v->retrig = -13421773;                     /* -.8 */
            if (!v->fmc)
                v->retrig = s8_m30(v->retrig, 1073254866); /* x .999546485 */
            v->fmlp += s8_m30(fm - v->fmlp, 243478872);
        }
        if (!(v->age & 3)) {                               /* the swept band-pass: f (.958 + k env + 2 attack fm) */
            int32_t g, d;
            m = 1028644667 + s8_m30(v->k[1], v->env) + 2 * s8_m30(v->fmlp, C->attack);
            g = s8_m30(C->g0, m) >> 2;                     /* tan (pi f / fs), Q28 (f is low: tan is linear there) */
            d = s8_m28(C->r0, g) + s8_m28(g, g);
            v->c0[0] = g;
            v->c0[1] = C->r0 + g;
            v->c0[2] = S8_Q28 - d + s8_m28(d, d);          /* 1 / (1 + r g + g^2) */
        }
        bp = s8_svf(&v->f[0], v->c0, s8_m24(x - s8_m30(v->retrig, 214748365), C->pgain), 1);   /* (the diode's pulse) */
        v->last += s8_m30(bp + s8_m30(v->k[0], x) - v->last, C->tone_c);
        body = s8_m30(s8_m30(1481763717, 536870912 + (v->lvl >> 1)), s8_svf(&v->f[1], C->svf[1], s8_abs(v->last), 1));
        r = s8_svf(&v->f[2], C->svf[2], v->last + body, 2);
        break;
    }
    case S8_SD: {
        int32_t p, a, b, sh, x2;
        if (v->pc) {
            --v->pc;
            v->pulse = (v->pc ? 10 : 9) * S8_ONE;
        } else
            v->pulse = s8_m30(v->pulse, 830262952);        /* x .77324263 */
        p = v->pulse;
        v->plp += s8_m30(p - v->plp, 805306368);           /* x .75 */
        a = s8_svf(&v->f[0], C->svf[0], p - v->plp + s8_m30(p, 6442451), 1);
        b = s8_svf(&v->f[1], C->svf[1], s8_m30(p, 27917287), 1);
        sh = s8_m28(v->k[0], a) + s8_m28(v->k[1], b);      /* softclip: x (27 + x^2) / (27 + 9 x^2), |x| <= 3 */
        sh = sh > 3 * S8_ONE ? 3 * S8_ONE : sh < -3 * S8_ONE ? -3 * S8_ONE : sh;
        x2 = s8_m24(sh, sh);
        sh = s8_div(s8_m24(sh, 27 * S8_ONE + x2), 27 * S8_ONE + 9 * x2);
        r = (int32_t)(((int64_t)v->k[2] * sh) >> 22)
            + s8_m30(s8_m30(C->snappy, 429496730), s8_svf(&v->f[2], C->svf[2], s8_m30(s8_noise(&v->rnd), v->env), 2));
        break;
    }
    case S8_MT:
    case S8_MC: {
        int32_t w;
        uint32_t inc = C->inc[0];
        inc += (uint32_t)(((uint64_t)inc * (uint32_t)s8_m30(v->s == S8_MT ? 37580964 : 8589935, v->lvl)) >> 30);
        v->ph[0] += inc;
        w = s8_sin(v->ph[0]);
        r = s8_m30(w, v->env);
        if (v->s == S8_MT) {                               /* the tom's overtones and its noise */
            int32_t sq = s8_m24(w, w), l2, l3, fade, t;
            fade = v->age >= 96u ? S8_Q30 : (int32_t)(v->age * (uint32_t)(S8_Q30 / 96));
            t = s8_m30(s8_m30(S8_ONE - 2 * sq, 236223201), 697932186 + s8_m30(375809638, v->lvl))
                + s8_m30(s8_m30(s8_m24(w, 3 * S8_ONE - 4 * sq), 150323855), v->lvl);
            r += s8_m30(s8_m30(t, v->env), fade);
            l2 = s8_m30(v->lvl, v->lvl);
            l3 = s8_m30(l2, v->lvl);
            r += s8_m30(s8_m30(s8_svf(&v->f[2], C->svf[2], s8_noise(&v->rnd), 0), 53687091), s8_m30(l3, l3));
        }
        break;
    }
    case S8_RS:
    case S8_CL:
        v->ph[0] += C->inc[0];
        v->ph[1] += C->inc[1];
        r = s8_sin(v->ph[0]);
        if (v->s == S8_RS)
            r += s8_m30(s8_sin(v->ph[1]), 858993459) + s8_m30(s8_noise(&v->rnd), 171798692);   /* .8, .16 noise */
        r = s8_m30(r, v->env);
        break;
    case S8_CP: {
        int32_t e;
        if (v->age < 1323u) {
            if (!v->clap)
                v->pulse = S8_Q30;
            if (++v->clap == 441u)
                v->clap = 0;
            v->pulse = s8_m30(v->pulse, 1063004406);       /* x .990 */
        } else if (v->age == 1323u) {
            v->env = S8_Q30;
            v->pulse = 0;
        }
        e = (v->age >= 1323u ? v->env : 0) + v->pulse + s8_m30(v->lvl, 39728447);
        r = s8_m30(s8_svf(&v->f[3], C->svf[3], s8_noise(&v->rnd), 1), e);
        break;
    }
    case S8_CB: {
        int32_t sq, l3;
        v->ph[0] += C->inc[0];
        v->ph[1] += C->inc[1];
        sq = ((int32_t)(v->ph[0] >> 31) + (int32_t)(v->ph[1] >> 31) - 1) * S8_ONE;
        l3 = s8_m30(s8_m30(v->lvl, v->lvl), v->lvl);
        r = s8_m30(s8_m30(s8_svf(&v->f[0], C->svf[0], sq, 1), v->env), 257698038 + s8_m30(816043786, l3));
        break;
    }
    default: {                                             /* CH, OH, CY: six squares, metallic */
        uint32_t j, total = 0;
        int32_t col, sw, e, u;
        int64_t uu;
        for (j = 0; j < 6u; j++) {
            v->ph[j] += C->inc[j];
            total += v->ph[j] >> 31;
        }
        col = s8_svf(&v->f[2], C->svf[2], (int32_t)total * 5536481 - S8_ONE, 1);   /* .33 total - 1 */
        uu = (int64_t)v->age * C->tail;
        u = uu >= S8_Q30 ? S8_Q30 : (int32_t)uu;
        if (v->s == S8_OH) {
            int32_t u3 = s8_m30(s8_m30(u, u), u);
            e = S8_Q30 - u3;
            e = s8_m30(e, e);
            if (v->age < 400u)
                e = s8_m30(e, (int32_t)(v->age * (uint32_t)(S8_Q30 / 400)));
        } else if (v->s == S8_CH) {
            int32_t u2 = s8_m30(u, u);
            e = s8_m30(S8_Q30 - s8_m30(u2, u2), v->env);
        } else
            e = s8_m30(v->env, 1288490189) + s8_m30(S8_Q30 - u, 246960620);   /* 1.2 env + .23 (1 - u) */
        sw = col > 0 ? col * 10 : s8_m30(col, 107374182);
        sw = s8_div(sw, S8_ONE + s8_abs(sw));
        r = s8_svf(&v->f[0], C->svf[0], s8_svf(&v->f[3], C->svf[3], s8_m30(sw + S8_ONE, e), 2), 2);
        break;
    }
    }
    v->age++;
    v->env = s8_d30(v->env, C->decay);
    v->lvl = s8_d30(v->lvl, C->tdecay);
    v->choke = s8_d30(v->choke, v->chd);
    return s8_m30(r, v->choke);
}

/* a block of voice v into y (Q15, at the sound's own level), 0 when it has ended (not inlined: the audio ISR keeps
 * sp_render inlined, tests/sp_budget.py counts this one on its own) */
__attribute__((noinline)) static uint32_t s8_run(s8_voice_t *v, int32_t *y, uint32_t n)
{
    const int32_t g = SP8[v->s].gain;
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t x, a;
        if (!v->on) {
            y[i] = 0;
            continue;
        }
        x = s8_sample(v);
        a = s8_abs(x);
        if (a > v->peak)
            v->peak = a;
        if (a < (v->peak >> 10 > 168 ? v->peak >> 10 : 168))   /* (drum808.c: quiet below 1e-5 or peak / 1000) */
            v->quiet++;
        else
            v->quiet = 0;
        if ((v->quiet > 882u && v->age > 2205u) || v->age > 44100u * 16u)
            v->on = 0;
        y[i] = s8_m24(x, g) >> 9;                          /* (Q24 -> Q15) */
    }
    return v->on;
}

/* the closed hat on the open one: its choke (CH>OH, drum808.c's default) */
static void s8_choke(s8_voice_t *v) { v->chd = 1009317315; }   /* x .94 a sample */
