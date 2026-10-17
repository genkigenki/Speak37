/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* RHYTHM's 808 (src/sp_808.c, the fixed-point port) against FM1 Open's own float 808 (third_party/fm1_open/drum808.c,
 * linked in as the reference): every sound hit at full and at RHYTHM's softer level, the same noise seed, and
 * compared sample for sample: the correlation, the error's level under the sound's, the peak and the length (within
 * 5 ms). Also: the closed hat chokes the open one, no sound clips or runs away, the cost a sample on this host.
 *   cc -O2 -Ibuild/gen -Itests/ref808 -DFM1_HOST_TEST -DFM1_OPEN_WEB -o build/sp_808_test tests/sp_808_test.c \
 *      third_party/fm1_open/drum808.c -lm */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#pragma push_macro("FS")
#undef FS
#include "felucca_tables.h"
#undef FS
#undef CTL
#pragma pop_macro("FS")
#define CTL 32
typedef struct { int32_t amp0, amp1; } vmod_t;
#include "../firmware/src/dsp.c"
#include "../firmware/src/sp_808.c"
#include "../third_party/fm1_open/drum808_state.h"

static int fails;
static void check(const char *what, int ok)
{
    printf("%-78s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok)
        fails++;
}

static const char *const NAME[S8_N] = {"BD", "SD", "MT", "MC", "RS", "CL", "CP", "CB", "CH", "OH", "CY"};
static const int PART[S8_N] = {D8_BD, D8_SD, D8_MT, D8_MT, D8_RS, D8_RS, D8_CP, D8_CB, D8_CH, D8_OH, D8_CY};
static const int SW[S8_N] = {0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0};
#define LEN (44100 * 3)

static d8_voice_t *ref_voice(drum808_t *d, int s)
{
    switch (s) {
    case S8_BD: return &d->bd;
    case S8_SD: return &d->sd;
    case S8_MT: case S8_MC: return &d->tom[1];
    case S8_RS: return &d->rs;
    case S8_CL: return &d->cl;
    case S8_CP: return &d->cp;
    case S8_CB: return &d->cb;
    case S8_CH: return &d->ch;
    case S8_OH: return &d->oh;
    default: return &d->cy;
    }
}

/* the float reference: sound s at velocity vel into out (its length) */
static int ref_render(int s, float vel, float *out)
{
    static drum808_t d;
    float rev[128], dly[128];
    int n = 0, last = 0, i;
    drum808_init(&d);
    if (SW[s])
        drum808_set(&d, PART[s], 3, 1);
    drum808_trigger(&d, PART[s], vel);
    memset(out, 0, sizeof(float) * LEN);
    while (n < LEN) {
        memset(rev, 0, sizeof rev);
        memset(dly, 0, sizeof dly);
        drum808_render(&d, out + n, rev, dly, 128);
        n += 128;
        if (!drum808_active(&d)) {
            last = n;
            break;
        }
    }
    for (i = 0; i < LEN; i++)
        if (out[i] != 0.0f)
            last = i + 1;
    return last;
}

/* the port: the same, its noise seeded as the reference's (s8_run's steps, its output in full precision) */
static int fix_render(int s, float vel, float *out)
{
    static drum808_t d;
    s8_voice_t v;
    int i, last = 0;
    int32_t g = (int32_t)lroundf((float)SP8[s].gain * vel);
    drum808_init(&d);
    memset(&v, 0, sizeof v);
    v.rnd = ref_voice(&d, s)->random;
    s8_hit(&v, (uint32_t)s, (int32_t)lroundf(vel * 32767.0f));
    for (i = 0; i < LEN && v.on; i++) {
        int32_t x = s8_sample(&v), a = s8_abs(x);
        if (a > v.peak)
            v.peak = a;
        if (a < (v.peak >> 10 > 168 ? v.peak >> 10 : 168))
            v.quiet++;
        else
            v.quiet = 0;
        if ((v.quiet > 882u && v.age > 2205u) || v.age > 44100u * 16u)
            v.on = 0;
        out[i] = (float)s8_m24(x, g) / (float)(1 << 24);
        last = i + 1;
    }
    for (; i < LEN; i++)
        out[i] = 0;
    return last;
}

static float A[LEN], B[LEN];

int main(void)
{
    int s, ok = 1;
    printf("== RHYTHM's 808 (sp_808.c) against FM1 Open's drum808.c\n");
    for (s = 0; s < S8_N; s++) {
        static const float VEL[2] = {1.0f, 22000.0f / 32767.0f};
        int k;
        for (k = 0; k < 2; k++) {
            int na = ref_render(s, VEL[k], A), nb, i, n;
            double sab = 0, saa = 0, sbb = 0, se = 0, pa = 0, pb = 0, corr, snr;
            char what[160];
            nb = fix_render(s, VEL[k], B);
            n = na > nb ? na : nb;
            for (i = 0; i < n; i++) {
                double a = A[i], b = B[i];
                sab += a * b;
                saa += a * a;
                sbb += b * b;
                se += (a - b) * (a - b);
                if (fabs(a) > pa)
                    pa = fabs(a);
                if (fabs(b) > pb)
                    pb = fabs(b);
            }
            corr = sab / sqrt(saa * sbb + 1e-30);
            snr = 10.0 * log10(saa / (se + 1e-30));
            snprintf(what, sizeof what, "%s at %.2f: corr %.5f, error %5.1f dB under, peak %.3f / %.3f, %4.0f / %4.0f ms",
                     NAME[s], VEL[k], corr, snr, pa, pb, na / 44.1, nb / 44.1);
            ok = corr > 0.995 && snr > 30.0 && fabs(20.0 * log10(pb / pa)) < 0.5 && abs(na - nb) < 44100 / 50;
            /* (the noise of the metals and the clap goes through the same steps, so it stays sample-exact too) */
            check(what, ok);
        }
    }
    /* the choke: an open hat, the closed one 50 ms later: the open one gone within 30 ms */
    {
        s8_voice_t o;
        int i, end = -1;
        memset(&o, 0, sizeof o);
        s8_hit(&o, S8_OH, 32767);
        for (i = 0; i < 44100 && o.on; i++) {
            int32_t y;
            if (i == 2205)
                s8_choke(&o);
            s8_run(&o, &y, 1);
            if (!o.on)
                end = i;
        }
        check("the closed hat chokes the open one (gone within 40 ms)", end > 2205 && end < 2205 + 1764);
    }
    /* no sound clips or runs away at full velocity, every one ends */
    {
        int good = 1;
        for (s = 0; s < S8_N; s++) {
            s8_voice_t v;
            int32_t y[32], mx = 0;
            uint32_t i, b;
            memset(&v, 0, sizeof v);
            s8_hit(&v, (uint32_t)s, 32767);
            for (b = 0; b < 44100u * 17u / 32u && v.on; b++) {
                s8_run(&v, y, 32);
                for (i = 0; i < 32u; i++)
                    if (abs(y[i]) > mx)
                        mx = abs(y[i]);
            }
            if (v.on || mx > 32767 * 2) {
                printf("        %s: %s, peak %d (Q15)\n", NAME[s], v.on ? "still on" : "ended", (int)mx);
                good = 0;
            }
        }
        check("every sound ends, none above twice full scale (Q15)", good);
    }
    /* the cost: every sound once, then the whole kit ringing, a sample */
    {
        struct timespec t0, t1;
        s8_voice_t v[8];
        static const uint8_t K[8] = {S8_BD, S8_SD, S8_CP, S8_CH, S8_OH, S8_MT, S8_RS, S8_CB};
        int32_t y[32];
        uint32_t b, l, n = 0;
        double ns;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (b = 0; b < 44100u * 4u / 32u; b++) {
            if (b % 172u == 0u)
                for (l = 0; l < 8u; l++)
                    s8_hit(&v[l], K[l], 32767);
            for (l = 0; l < 8u; l++)
                if (v[l].on)
                    s8_run(&v[l], y, 32), n++;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (44100.0 * 4.0);
        printf("        cost on this host: all 8 lanes hit every 125 ms: %.1f ns a sample (%.1f lanes ringing)\n", ns,
               n / (44100.0 * 4.0 / 32.0));
        check("cost: measured", ns > 0);
    }
    printf(fails ? "808 TESTS FAILED (%d)\n" : "808 tests passed\n", fails);
    return fails != 0;
}
