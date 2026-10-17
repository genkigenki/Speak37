/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (makkuro) */
/* Host test of src/vocoder.c (the USB host mode's vocoder, SMK-37 Pro): a synthetic vowel modulator (a 120 Hz pulse
 * train through three formant resonators: "a" 700 / 1220 / 2600 Hz, "i" 270 / 2290 / 3010 Hz) and a held carrier
 * note at the I2S rate. The output's band energies must follow the modulator: "a" louder around 1.2 kHz than around
 * 2.3 kHz, "i" the other way round, by a clear margin; silence in, silence out (the envelopes release); its level
 * sane, no runaway, no clicks (sample steps bounded); the formant knob moves the spectrum up; the keys: velocity,
 * release, voices stolen without hanging notes; the dry mix passes the microphone; the cost per sample.
 * The bands' overlap (a sine swept through all analysis bands: the summed power's ripple), the voiced / unvoiced
 * detector (a hiss "s" unvoiced, vowels voiced), and synthetic words ("sa", "ti", "shu": formant trajectories, noise
 * bursts) with an intelligibility proxy: band by band, how the output's 5 ms envelopes follow the input's over time
 * (correlation), and the consonants' energy kept (the output's 4..9 kHz in the bursts against the vowels). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
static uint64_t t_keys;
static uint8_t t_vel[64];
#define VC_KEYS() t_keys
#define VC_VEL(k) t_vel[k]
#include "../firmware/src/vocoder.c"

static int fails;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                 \
            fails++;                                                            \
        }                                                                       \
    } while (0)

#define FS (24000000.0 / 544.0)
#define N 66176                             /* 1.5 s at the I2S rate */

typedef struct { double b0, b2, a1, a2, x1, x2, y1, y2; } bq;
static void bp_init(bq *f, double hz, double q)
{
    double w = 2 * M_PI * hz / FS, al = sin(w) / (2 * q), a0 = 1 + al;
    memset(f, 0, sizeof *f);
    f->b0 = al / a0;
    f->b2 = -al / a0;
    f->a1 = -2 * cos(w) / a0;
    f->a2 = (1 - al) / a0;
}
static double bp_run(bq *f, double x)
{
    double y = f->b0 * x + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1;
    f->x1 = x;
    f->y2 = f->y1;
    f->y1 = y;
    return y;
}

static int16_t mod[N], out[N];
static uint8_t uvb[N / 32];                 /* the voiced / unvoiced decision per block (1: unvoiced) */

/* a vowel: a 120 Hz pulse train through three resonators, scaled to peak ~0.3 FS */
static void vowel(const double *fm)
{
    bq r[3];
    double pk = 0, buf;
    int i, k;
    static double tmp[N];
    for (k = 0; k < 3; k++)
        bp_init(&r[k], fm[k], 8.0);
    for (i = 0; i < N; i++) {
        double x = fmod(i * 120.0 / FS, 1.0) < 120.0 / FS ? 1.0 : 0.0;
        buf = 0;
        for (k = 0; k < 3; k++)
            buf += bp_run(&r[k], x) / (k + 1);
        tmp[i] = buf;
        if (fabs(buf) > pk)
            pk = fabs(buf);
    }
    for (i = 0; i < N; i++)
        mod[i] = (int16_t)lrint(tmp[i] / pk * 0.3 * 32767);
}

/* run the vocoder over mod[]; returns the largest sample step after the first 0.1 s */
static int32_t render(void)
{
    int i;
    int32_t mx = 0, prev = 0;
    for (i = 0; i < N; i++) {
        int32_t y;
        if (i % 32 == 0) {
            vc_block();
            uvb[i / 32] = vc.uv > 16384;
        }
        y = vc_sample(mod[i]);
        y = y > 32767 ? 32767 : y < -32768 ? -32768 : y;
        out[i] = (int16_t)y;
        if (i > (int)(FS * 0.1) && abs(y - prev) > mx)
            mx = abs(y - prev);
        prev = y;
    }
    return mx;
}

/* the output's energy (RMS) around hz over the last second */
static double rms_in(const int16_t *x, double hz, double q)
{
    bq f;
    double s = 0;
    int i, n = 0;
    bp_init(&f, hz, q);
    for (i = 0; i < N; i++) {
        double y = bp_run(&f, x[i]);
        if (i >= N - (int)FS) {
            s += y * y;
            n++;
        }
    }
    return sqrt(s / n) + 1e-3;
}
static double band_rms(double hz) { return rms_in(out, hz, 12.0); }

/* the band profile in dB at the vocoder's own band centres: of the modulator (in) or the output */
static void profile(double *db, const int16_t *x)
{
    uint32_t b;
    for (b = 0; b < VC_NB; b++)
        db[b] = 20.0 * log10(rms_in(x, 50.0 * pow(2.0, vc_band_idx(b) / 12.0), 4.0));
}

/* how well the output's change between two vowels follows the modulator's: the correlation of the differences */
static double follow(const double *ma, const double *mi, const double *oa, const double *oi)
{
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, n = VC_NB;
    uint32_t b;
    for (b = 0; b < VC_NB; b++) {
        double x = ma[b] - mi[b], y = oa[b] - oi[b];
        sx += x;
        sy += y;
        sxx += x * x;
        syy += y * y;
        sxy += x * y;
    }
    return (sxy - sx * sy / n) / sqrt((sxx - sx * sx / n) * (syy - sy * sy / n));
}

/* the spectral centroid of the output (probes a third of an octave apart, 150 Hz .. 9.6 kHz) */
static double centroid(void)
{
    double f, e, se = 0, sf = 0;
    for (f = 150; f < 10000; f *= 1.26) {
        e = band_rms(f);
        se += e * e;
        sf += e * e * f;
    }
    return sf / se;
}

/* the bands' overlap: a sine at f through every band (the synthesis filters at the full rate; the analysis has the
 * same at its rates): the summed power, dB */
static double band_sum_db(double f)
{
    double sum = 0;
    uint32_t b;
    for (b = 0; b < VC_NB; b++) {
        int32_t st[2][2] = {{0, 0}, {0, 0}}, y;
        double e = 0;
        int i;
        for (i = 0; i < 20000; i++) {
            y = vc_bp(&vc.syn[vc.set][b], st[1], vc_bp(&vc.syn[vc.set][b], st[0],
                                                          (int32_t)lrint(8000.0 * sin(2 * M_PI * f * i / FS))));
            if (i >= 10000)
                e += (double)y * y;
        }
        sum += e / 10000;
    }
    return 10.0 * log10(sum / (8000.0 * 8000.0 / 2));
}

/* synthetic words into mod[] (N samples): a silence, then segments: 's' hiss (4..9 kHz), 'S' (sh: 2..6 kHz),
 * 't' a burst and aspiration, vowels by their formants gliding from the last; seg[]: 1 consonant, 2 vowel, per block */
static uint8_t seg[N / 32];
static void words(void)
{
    static const struct { char k; double ms, f1, f2, f3; } W[] = {
        {' ', 100, 0, 0, 0}, {'s', 150, 0, 0, 0}, {'v', 260, 700, 1220, 2600}, {' ', 120, 0, 0, 0},
        {'t', 25, 0, 0, 0}, {'h', 45, 0, 0, 0}, {'v', 260, 270, 2290, 3010}, {' ', 120, 0, 0, 0},
        {'S', 150, 0, 0, 0}, {'v', 260, 300, 870, 2240}, {' ', 200, 0, 0, 0}};
    double f[3] = {500, 1500, 2500}, y1[3] = {0}, y2[3] = {0};
    bq hs, hsh, ht;
    uint32_t x = 7, i = 0, w, k;
    bp_init(&hs, 6500, 1.2);
    bp_init(&hsh, 3500, 1.2);
    bp_init(&ht, 3000, 0.6);
    memset(mod, 0, sizeof mod);
    memset(seg, 0, sizeof seg);
    for (w = 0; w < sizeof W / sizeof W[0] && i < N; w++) {
        uint32_t n = (uint32_t)(W[w].ms * FS / 1000.0), j;
        for (j = 0; j < n && i < N; j++, i++) {
            double v = 0, nz;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            nz = (double)(int32_t)x / 2147483648.0;
            if (W[w].k == 's')
                v = bp_run(&hs, nz) * 9000;
            else if (W[w].k == 'S')
                v = bp_run(&hsh, nz) * 9000;
            else if (W[w].k == 't')
                v = bp_run(&ht, nz) * 16000;
            else if (W[w].k == 'h')
                v = bp_run(&ht, nz) * 4000;
            else if (W[w].k == 'v') {
                double pulse = fmod(i * 125.0 / FS, 1.0) < 125.0 / FS ? 1.0 : 0.0;
                for (k = 0; k < 3; k++) {             /* the formants glide over the first 60 ms */
                    double ft = W[w].f1, r, c;
                    ft = k == 0 ? W[w].f1 : k == 1 ? W[w].f2 : W[w].f3;
                    f[k] += (ft - f[k]) * (j < (uint32_t)(0.06 * FS) ? 0.0015 : 1.0);
                    r = exp(-M_PI * 90.0 / FS);
                    c = 2 * r * cos(2 * M_PI * f[k] / FS);
                    {
                        double yy = pulse + c * y1[k] - r * r * y2[k];
                        y2[k] = y1[k];
                        y1[k] = yy;
                        v += yy * (1 - r) * 30000.0 / (k + 1);
                    }
                }
            }
            mod[i] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
            seg[i / 32] = W[w].k == 'v' ? 2 : W[w].k == ' ' ? 0 : 1;
        }
    }
}

/* the 5 ms envelope of x around hz (two 2-pole band-passes, Q 4), frame by frame into e[] (dB) */
static void env_frames(const int16_t *x, double hz, double *e, int nf)
{
    bq a, b;
    int i, fr = (int)(0.005 * FS);
    bp_init(&a, hz, 3.0);
    bp_init(&b, hz, 3.0);
    for (i = 0; i < nf; i++) {
        double s2 = 0;
        int j;
        for (j = 0; j < fr; j++) {
            double y = bp_run(&b, bp_run(&a, x[i * fr + j]));
            s2 += y * y;
        }
        e[i] = 10.0 * log10(s2 / fr + 1.0);
    }
}

static double corr(const double *a, const double *b, int n)
{
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    int i;
    for (i = 0; i < n; i++) {
        sx += a[i];
        sy += b[i];
        sxx += a[i] * a[i];
        syy += b[i] * b[i];
        sxy += a[i] * b[i];
    }
    sxx -= sx * sx / n;
    syy -= sy * sy / n;
    return sxx > 0 && syy > 0 ? (sxy - sx * sy / n) / sqrt(sxx * syy) : 0;
}

static double rms_all(void)
{
    double s = 0;
    int i;
    for (i = N - (int)FS; i < N; i++)
        s += (double)out[i] * out[i];
    return sqrt(s / FS);
}

int main(void)
{
    static const double A[3] = {700, 1220, 2600}, I[3] = {270, 2290, 3010};
    double a12, a23, i12, i23, r, ma[VC_NB], mi[VC_NB], oa[VC_NB], oi[VC_NB];
    int32_t step;
    clock_t t0;
    uint32_t k, held;

    vc_init();
    CHECK(vc.p[VCP_MIX] == 127 && vc.p[VCP_SIBIL] > 0);   /* the defaults: the vocoder alone, the consonants on */
    vc.p[VCP_SIBIL] = 0;                                  /* (the band tests: the vocoder bands alone, no */
    vc.p[VCP_NORM] = 0;                                   /* normalising: a vowel held 1.5 s would be flattened) */
    vc_apply();
    {   /* the bands' overlap: the summed power of a sine swept 120 Hz .. 8 kHz through all the bands */
        double f, lo = 1e9, hi = -1e9;
        for (f = 120; f < 8000; f *= 1.06) {
            double d = band_sum_db(f);
            lo = d < lo ? d : lo;
            hi = d > hi ? d : hi;
        }
        printf("vocoder: %u bands, the summed power over 120 Hz .. 8 kHz: %.1f .. %.1f dB (ripple %.1f dB)\n", VC_NB,
               lo, hi, hi - lo);
        CHECK(hi - lo < 4.0);
    }
    t_keys = 1ull << 0 | 1ull << 7 | 1ull << 12;         /* C3 G3 C4 (MIDI 48 55 60), velocity 100: a chord, as */
    t_vel[0] = t_vel[7] = t_vel[12] = 100;                /* played (one low note leaves the lowest bands empty) */

    vowel(A);
    profile(ma, mod);
    t0 = clock();
    step = render();
    profile(oa, out);
    r = (double)(clock() - t0) / CLOCKS_PER_SEC;
    a12 = band_rms(1220);
    a23 = band_rms(2300);
    printf("vocoder: \"a\": rms %.0f, 1.2 kHz %.0f, 2.3 kHz %.0f, step %d, %.0f ns a sample (host)\n", rms_all(), a12,
           a23, step, r * 1e9 / N);
    CHECK(rms_all() > 300 && rms_all() < 20000);
    CHECK(step < 20000);

    vowel(I);
    profile(mi, mod);
    step = render();
    profile(oi, out);
    i12 = band_rms(1220);
    i23 = band_rms(2300);
    printf("vocoder: \"i\": rms %.0f, 1.2 kHz %.0f, 2.3 kHz %.0f, step %d\n", rms_all(), i12, i23, step);
    CHECK(rms_all() > 300 && rms_all() < 20000);
    /* the output follows the vowel: "a" leans to 1.2 kHz, "i" to 2.3 kHz; band by band, the output's change from "a"
     * to "i" follows the modulator's (correlation over the 16 bands) */
    CHECK((oa[7] - oa[11]) - (oi[7] - oi[11]) > 6.0);    /* (bands 7, 11: 755 Hz, 2.5 kHz) */
    r = follow(ma, mi, oa, oi);
    printf("vocoder: \"a\" -> \"i\": the output's band changes follow the modulator's, correlation %.2f\n", r);
    CHECK(r > 0.6);                                       /* (steady vowels; the words below: over time) */

    /* the formant knob up: the "i" spectrum moves up (its centroid) */
    {
        double c0 = centroid(), c1;
        vc.p[VCP_FMT] = 127;
        vc_apply();
        render();
        c1 = centroid();
        printf("vocoder: formant +12: centroid %.0f Hz -> %.0f Hz\n", c0, c1);
        CHECK(c1 > c0 * 1.25);                            /* (the carrier falls 6 dB an octave) */
        vc.p[VCP_FMT] = 64;
        vc_apply();
    }

    /* silence in: the envelopes release, the output dies away */
    memset(mod, 0, sizeof mod);
    render();
    printf("vocoder: silence in: rms %.1f\n", rms_all());
    CHECK(rms_all() < 2.0);

    /* the keys: release, velocity, more notes than voices, all released again */
    vowel(A);
    t_keys = 0;
    render();
    printf("vocoder: no key: rms %.2f\n", rms_all());
    CHECK(rms_all() < 2.0);                               /* no key: no carrier, nothing */
    for (k = 0; k < 10u; k++) {
        t_keys |= 1ull << (k * 3u);
        t_vel[k * 3u] = (uint8_t)(40 + k * 8);
        vc_block();
    }
    for (k = 0, held = 0; k < VC_NV; k++)
        held += vc.v[k].on;
    CHECK(held == VC_NV);
    t_keys = 0;
    render();
    for (k = 0, held = 0; k < VC_NV; k++)
        held += vc.v[k].on || vc.v[k].amp;
    CHECK(held == 0 && rms_all() < 2.0);

    /* the sibilance (defaults): a hiss ("s": noise around 7 kHz) with a note held is unvoiced, and comes through */
    {
        double h7;
        uint32_t x = 1, j, uv = 0;
        bq f;
        vc.p[VCP_SIBIL] = VCP_DEF[VCP_SIBIL];
        vc.p[VCP_NORM] = VCP_DEF[VCP_NORM];
        vc_apply();
        bp_init(&f, 7000, 1.0);
        for (j = 0; j < (uint32_t)N; j++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            mod[j] = (int16_t)lrint(bp_run(&f, (double)(int32_t)x / 2147483648.0 * 6000.0));
        }
        t_keys = 1;
        render();
        h7 = band_rms(7000);
        for (j = N / 64; j < N / 32; j++)
            uv += uvb[j];
        printf("vocoder: hiss: unvoiced %.0f %% of the blocks, the output at 7 kHz %.0f\n", 100.0 * uv / (N / 64), h7);
        CHECK(uv > (N / 64) * 8 / 10 && h7 > 300);
        vowel(A);
        render();
        for (j = N / 64, uv = 0; j < N / 32; j++)
            uv += uvb[j];
        printf("vocoder: vowel \"a\": unvoiced %.0f %% of the blocks\n", 100.0 * uv / (N / 64));
        CHECK(uv < (N / 64) / 10);
        t_keys = 0;
        render();
    }

    /* words ("sa", "ti", "shu"): the intelligibility proxy with the defaults and a chord held */
    {
        static double ei[N / 200], eo[N / 200];
        int nf = (int)(N / (0.005 * FS)), b2, i2;
        double cs = 0, cons_o = 0, vow_o = 0, cons_i = 0, vow_i = 0, ruv = 0;
        uint32_t nc = 0, nv = 0, uvc = 0, uvv = 0;
        words();
        t_keys = 1ull << 0 | 1ull << 7 | 1ull << 12;      /* C3 G3 C4 */
        t_vel[7] = t_vel[12] = 100;
        render();
        for (b2 = 0; b2 < (int)VC_NB; b2++) {
            double hz = 50.0 * pow(2.0, vc_band_idx((uint32_t)b2) / 12.0);
            env_frames(mod, hz, ei, nf);
            env_frames(out, hz, eo, nf);
            cs += corr(ei, eo, nf);
        }
        cs /= VC_NB;
        for (i2 = 0; i2 < N / 32; i2++) {                 /* the consonants against the vowels, 4..9 kHz */
            if (seg[i2] == 1) {
                nc++;
                uvc += uvb[i2];
            } else if (seg[i2] == 2) {
                nv++;
                uvv += uvb[i2];
            }
        }
        {
            bq f1, f2;
            bp_init(&f1, 6000, 0.8);
            bp_init(&f2, 6000, 0.8);
            for (i2 = 0; i2 < N; i2++) {
                double yi = bp_run(&f1, mod[i2]), yo = bp_run(&f2, out[i2]);
                if (seg[i2 / 32] == 1) {
                    cons_i += yi * yi;
                    cons_o += yo * yo;
                } else if (seg[i2 / 32] == 2) {
                    vow_i += yi * yi;
                    vow_o += yo * yo;
                }
            }
        }
        ruv = 10 * log10((cons_o / nc) / (vow_o / nv + 1)) - 10 * log10((cons_i / nc) / (vow_i / nv + 1));
        printf("vocoder: words: band envelopes in -> out, correlation %.2f; consonants 4..9 kHz over the vowels: "
               "%.1f dB in, %.1f dB out (%+.1f dB); unvoiced: consonants %.0f %%, vowels %.0f %%\n",
               cs, 10 * log10((cons_i / nc) / (vow_i / nv + 1)), 10 * log10((cons_o / nc) / (vow_o / nv + 1)), ruv,
               100.0 * uvc / nc, 100.0 * uvv / nv);
        CHECK(cs > 0.6);
        CHECK(ruv > -6.0);                                /* the consonants keep their weight against the vowels */
        CHECK(uvc > nc / 2 && uvv < nv / 10);
        t_keys = 0;
        render();
    }

    /* a track as the carrier (vc_track: the firmware's VOX TRACK): the words through the keys' synth rendered into a
     * buffer a block at a time, the analysis on the input side (vc_in, vc_sib_push), the same as vc_sample */
    {
        static double ei[N / 200], eo[N / 200];
        int nf = (int)(N / (0.005 * FS)), b2, i2, j2;
        double cs = 0;
        int32_t blk[32];
        words();
        t_keys = 1ull << 0 | 1ull << 7 | 1ull << 12;
        for (i2 = 0; i2 + 32 <= N; i2 += 32) {
            vc_block();
            for (j2 = 0; j2 < 32; j2++) {
                int32_t am;
                blk[j2] = vc_carrier(&am);
            }
            vc_track(blk, 32);                            /* (the track renders before the input's block) */
            for (j2 = 0; j2 < 32; j2++) {
                int32_t y = blk[j2] * vc.mix * 258 >> 15;
                vc_in(mod[i2 + j2]);
                vc_sib_push();
                out[i2 + j2] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
            }
        }
        for (b2 = 0; b2 < (int)VC_NB; b2++) {
            double hz = 50.0 * pow(2.0, vc_band_idx((uint32_t)b2) / 12.0);
            env_frames(mod, hz, ei, nf);
            env_frames(out, hz, eo, nf);
            cs += corr(ei, eo, nf);
        }
        cs /= VC_NB;
        printf("vocoder: a track as the carrier (vc_track): words, band envelopes in -> out, correlation %.2f\n", cs);
        CHECK(cs > 0.6);
        t_keys = 0;
        render();
    }

    /* the knobs: K1..K8 detents change the controls (two steps each, clamped), the text shows it */
    {
        char t0s[16], t1s[16];
        vc_ctl_text(VCP_FMT, t0s);
        CHECK(!strcmp(t0s, "FMT +0"));
        CHECK(vc_turn(VCP_FMT, 3) == 1 && vc.p[VCP_FMT] == 70);
        vc_ctl_text(VCP_FMT, t1s);
        CHECK(!strcmp(t1s, "FMT +1"));
        CHECK(vc_turn(VCP_MIX, 5) == 0 && vc.p[VCP_MIX] == 127);  /* (clamped) */
        CHECK(vc_turn(VCP_MIX, -10) == 0 && vc.p[VCP_MIX] == 107);
        vc_ctl_text(VCP_MIX, t1s);
        CHECK(!strcmp(t1s, "MIX 107"));
        CHECK(vc_turn(VCP_REL, -100) == 1 && vc.p[VCP_REL] == 0);
        vc_turn(VCP_WAVE, 64);
        vc_ctl_text(VCP_WAVE, t1s);
        CHECK(!strcmp(t1s, "WAVE SUP"));
        vc_ctl_text(VCP_GAIN, t1s);
        CHECK(!strcmp(t1s, "GAIN +0dB"));
        CHECK(vc_turn(8, 1) == 0 && vc_turn(VCP_DET, 0) == 0);    /* (no 9th knob; no turn) */
        vc_turn(VCP_GAIN, 6);
        vc_block();
        CHECK(vc.ig == 64 + 36 * 8);                              /* the gain takes effect at the next block */
        vc_turn(VCP_GAIN, -6);
    }

    /* the dry mix: the microphone, high-passed (a 1 kHz tone through, x2: the compressor's makeup below its threshold) */
    {
        int32_t pk = 0;
        vc.p[VCP_MIX] = 0;
        vc_apply();
        for (k = 0; k < (uint32_t)N; k++) {
            int32_t y;
            if (k % 32u == 0)
                vc_block();
            y = vc_sample((int32_t)lrint(1000.0 * sin(2 * M_PI * 1000.0 * k / FS)));
            if (k > (uint32_t)N / 2u && abs(y) > pk)
                pk = abs(y);
        }
        printf("vocoder: dry 1 kHz, amplitude 1000 -> %d\n", pk);
        CHECK(pk > 1800 && pk < 2200);
    }

    if (fails) {
        printf("vocoder_test: %d FAILED\n", fails);
        return 1;
    }
    printf("vocoder_test: ok\n");
    return 0;
}
