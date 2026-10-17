/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (makkuro) */
/* Host test of src/usbhost_audio.c (the USB host's microphone stream, SMK-37 Pro): the PCM unpack (24 bit stereo,
 * 16 bit mono, sign, the top 16 bits), malformed formats refused; the resampler: a 1 kHz sine at 48 kHz in 1 ms
 * packets against the 44,117.6 Hz I2S rate comes out at 1 kHz with its level, no underrun after the prime, the ring's
 * fill held near its target, also with the device's clock 0.5 % fast and slow; packets stopping: a fade, no step,
 * idle afterwards and the output untouched; the soft limiter (never past full scale, linear below the knee); the
 * meters (peak, RMS) and the dB scale. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define UH_POOL
#include "../firmware/src/usbhost_audio.c"

static int fails;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                 \
            fails++;                                                            \
        }                                                                       \
    } while (0)

#define I2S_HZ (24000000.0 / 544.0)

/* one 1 ms packet of a sine (amplitude a of 24-bit full scale), 24-bit stereo LE, R = -L */
static uint32_t make_pkt(uint8_t *p, uint32_t frames, double *ph, double hz, double a)
{
    uint32_t i;
    for (i = 0; i < frames; i++) {
        int32_t v = (int32_t)lrint(sin(*ph) * a * 8388607.0);
        int32_t u = -v;
        p[6 * i] = (uint8_t)v;
        p[6 * i + 1] = (uint8_t)(v >> 8);
        p[6 * i + 2] = (uint8_t)(v >> 16);
        p[6 * i + 3] = (uint8_t)u;
        p[6 * i + 4] = (uint8_t)(u >> 8);
        p[6 * i + 5] = (uint8_t)(u >> 16);
        *ph += 2.0 * M_PI * hz / 48000.0;
    }
    return frames * 6u;
}

/* run the stream for secs: the device sends 48 frames every 1 ms * clk (its clock), the audio ISR takes 32-frame
 * blocks at the I2S rate; returns the output's zero crossings (left) and fills its peak and the fill's range */
static uint32_t run(double secs, double clk, double a, int32_t *peak, uint32_t *fmin, uint32_t *fmax, uint32_t *uruns)
{
    static int32_t out[64];
    uint8_t pkt[48 * 6];
    double ph = 0.0, t_pkt = 0.0, t = 0.0;
    uint32_t zc = 0, i, blocks = (uint32_t)(secs * I2S_HZ / 32.0), b, u0 = uhs.underruns;
    int32_t last = 0;
    *peak = 0;
    *fmin = 0xFFFFFFFFu;
    *fmax = 0;
    for (b = 0; b < blocks; b++) {
        t += 32.0 / I2S_HZ;
        while (t_pkt < t) {                                       /* the packets due by now */
            uhs_push(pkt, make_pkt(pkt, 48, &ph, 1000.0, a));
            t_pkt += 0.001 * clk;
        }
        memset(out, 0, sizeof out);
        uhs_mix(out, 32);
        if (b > (uint32_t)(0.1 * I2S_HZ / 32.0)) {               /* after the prime and the fade in */
            uint32_t f = uhs.w - uhs.r;
            if (f < *fmin)
                *fmin = f;
            if (f > *fmax)
                *fmax = f;
            for (i = 0; i < 32u; i++) {
                if ((last < 0) != (out[2 * i] < 0))
                    zc++;
                last = out[2 * i];
                if (out[2 * i] > *peak)
                    *peak = out[2 * i];
                if (out[2 * i] + out[2 * i + 1] > 2 || out[2 * i] + out[2 * i + 1] < -2)
                    *peak = -1;                                   /* R must stay -L */
            }
        }
    }
    *uruns = uhs.underruns - u0;
    return zc;
}

int main(void)
{
    uint8_t p[12];
    int32_t out[64], peak;
    uint32_t i, fmin, fmax, ur, zc, pk[2], rms[2];

    /* the unpack */
    uhs_start(48000, 2, 3);
    p[0] = 0x56; p[1] = 0x34; p[2] = 0x12;                       /* L 0x123456 -> 0x1234 */
    p[3] = 0x00; p[4] = 0x00; p[5] = 0x80;                       /* R -8388608 -> -32768 */
    p[6] = 0xFF; p[7] = 0xFF; p[8] = 0xFF;                       /* L -1 -> -1 (the arithmetic shift) */
    p[9] = 0xFF; p[10] = 0xFF; p[11] = 0x7F;                     /* R max -> 32767 */
    CHECK(uhs_push(p, 12) == 2);
    CHECK(uhs_ring[0] == 0x1234 && uhs_ring[1] == -32768 && uhs_ring[2] == -1 && uhs_ring[3] == 32767);
    CHECK(uhs_push(p, 11) == 1);                                  /* a cut frame is left out */
    uhs_start(48000, 1, 2);
    i = uhs.w;
    p[0] = 0x34; p[1] = 0x12; p[2] = 0xFE; p[3] = 0xFF;          /* 16 bit mono: 0x1234, -2 */
    CHECK(uhs_push(p, 4) == 2);
    CHECK(uhs_ring[(i & (UHS_N - 1u)) * 2u] == 0x1234 && uhs_ring[(i & (UHS_N - 1u)) * 2u + 1u] == 0x1234);
    CHECK(uhs_ring[((i + 1u) & (UHS_N - 1u)) * 2u + 1u] == -2);
    uhs_start(48000, 2, 4);
    CHECK(uhs_push(p, 8) == 0);                                   /* 32-bit samples: not handled, refused */
    uhs_stop();
    CHECK(uhs_push(p, 4) == 0);                                   /* stopped: nothing taken */
    CHECK(uhs_step_for(48000) == 71303u && uhs_step_for(44100) == 65509u);

    /* the stream: 1 kHz in, 1 kHz out at the level, no underrun, the fill near its target */
    for (i = 0; i < 2000; i++) {                                  /* (idle out: the ring empties, the fade ends) */
        memset(out, 0, sizeof out);
        uhs_mix(out, 32);
    }
    CHECK(!uhs.active);
    uhs_start(48000, 2, 3);
    zc = run(2.0, 1.0, 0.5, &peak, &fmin, &fmax, &ur);
    printf("usbhost_audio: nominal: %u crossings / 1.9 s, peak %d, fill %u..%u, underruns %u, step %u\n",
           zc, peak, fmin, fmax, ur, uhs.step);
    CHECK(ur == 0 && uhs.primes == 1);
    CHECK(zc > 3760 && zc < 3840);                                /* 1000 Hz over ~1.9 s: ~3800 crossings */
    CHECK(peak > 16000 && peak < 16500);                          /* 0.5 FS of 24 bit -> 0.5 of Q15 (16383) */
    CHECK(fmin > 60u && fmax < UHS_TARGET + 150u);
    CHECK(uhs.lrdiff == uhs.pkts);                                /* (R = -L: every packet differs) */
    {   /* empty packets: silence frames keep the fill, no underrun, no new prime */
        uint32_t f0 = uhs.w - uhs.r, p0 = uhs.primes, u0 = uhs.underruns, k2;
        for (k2 = 0; k2 < 50u; k2++) {
            uhs_push_silence(48);
            memset(out, 0, sizeof out);
            uhs_mix(out, 32);
            uhs_mix(out + 0, 12);
        }
        CHECK(uhs.silent == 2400u && uhs.underruns == u0 && uhs.primes == p0);
        CHECK(uhs.w - uhs.r > f0 / 4u);
        CHECK(out[0] == 0 && out[1] == 0);                         /* (silence, after the old frames played) */
    }
    for (i = 0; i < 2; i++) {
        double clk = i ? 0.995 : 1.005;                           /* the device 0.5 % fast, then slow */
        uint32_t o = uhs.overruns;
        zc = run(4.0, clk, 0.5, &peak, &fmin, &fmax, &ur);
        printf("usbhost_audio: clock x%.3f: fill %u..%u, underruns %u, overruns %u, step %u\n",
               1.0 / clk, fmin, fmax, ur, uhs.overruns - o, uhs.step);
        CHECK(ur == 0 && uhs.overruns == o && peak > 16000);
        CHECK(fmin > 40u && fmax < UHS_N - 200u);
    }

    /* the meters: peak and RMS of the last packets (a 0.5 FS sine: peak 16383, RMS ~11585) */
    uhs_meter_take(pk, rms);
    {
        uint8_t q[48 * 6];
        double ph = 0.0;
        uhs_push(q, make_pkt(q, 48, &ph, 1000.0, 0.5));
    }
    uhs_meter_take(pk, rms);
    CHECK(pk[0] > 16300 && pk[0] < 16400 && pk[1] > 16300 && pk[1] < 16400);
    CHECK(rms[0] > 11400 && rms[0] < 11800);
    CHECK(uhs_db(32767) == 0 && uhs_db(16384) == -6 && uhs_db(0) == -90 && uhs_db(1) <= -88);
    CHECK(uhs_db(3277) <= -18 && uhs_db(3277) >= -22);

    /* the packets stop: the ring runs dry, a fade from the last frame (no step), then idle and untouched */
    {
        int32_t prev = 0, maxstep = 0, k, dry = 0;
        uint32_t u0 = uhs.underruns;
        uhs_stop();
        for (k = 0; k < 400; k++) {                               /* (the ring's 10 ms play out first) */
            memset(out, 0, sizeof out);
            uhs_mix(out, 32);
            for (i = 0; i < 32u; i++) {
                int32_t d = out[2 * i] - prev;
                if (dry && (d > maxstep || -d > maxstep))
                    maxstep = d > 0 ? d : -d;
                prev = out[2 * i];
            }
            dry |= !uhs.play;
        }
        printf("usbhost_audio: stopped: largest step in the fade %d, underruns +%u\n", maxstep, uhs.underruns - u0);
        CHECK(!uhs.active && prev == 0);
        CHECK(maxstep <= 80);                                     /* the fade: the last frame / 256 a sample */
        for (i = 0; i < 64u; i++)
            out[i] = (int32_t)i * 37 - 1000;
        uhs_mix(out, 32);
        for (i = 0; i < 64u; i++)
            CHECK(out[i] == (int32_t)i * 37 - 1000);
    }

    /* the limiter */
    CHECK(uhs_limit(20000) == 20000 && uhs_limit(-24576) == -24576);
    CHECK(uhs_limit(30000) > 24576 && uhs_limit(30000) < 30000);
    CHECK(uhs_limit(1000000) <= 32767 && uhs_limit(-1000000) >= -32767);
    CHECK(uhs_limit(40000) > uhs_limit(30000));                   /* monotonic */

    if (fails) {
        printf("usbhost_audio_test: %d FAILED\n", fails);
        return 1;
    }
    printf("usbhost_audio_test: ok\n");
    return 0;
}
