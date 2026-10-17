/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (makkuro) */
/* USB host audio (SMK-37 Pro, milestone 2), the part without registers (host tested: tests/usbhost_audio_test.c):
 *   uhs_push   one isochronous IN packet (PCM, 16 or 24 bit, mono or stereo, little endian) -> the ring
 *              (int16 stereo frames), the level meters; the producer: TIMER5 (usbhost.c uhs_service)
 *   uhs_mix    the ring resampled to the I2S rate (linear, the step steered by the ring's fill, as the USB
 *              audio input steers its packets by its ring's fill) and added to the master output with a soft
 *              limiter on the sum; the consumer: the audio ISR (audio.c audio_block)
 * Nothing stalls when the packets stop: the ring runs dry, the last frame fades out over 256 samples, and the
 * ring fills to its target again before the sound comes back. While no stream has started uhs_mix returns at
 * once: the output is bit for bit the synth's. */
#ifndef UH_POOL
#define UH_POOL __attribute__((section(".pool")))
#endif
#ifndef RING_PUBLISH
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#endif
#define UHS_N 2048u                        /* ring frames (~43 ms at 48 kHz) */
#define UHS_TARGET 168u                    /* the fill kept: 3.5 ms at 48 kHz (also the prime before playing) */
#ifndef UHS_VC
#define UHS_VC 0                           /* 1: vocoder.c is there (the firmware); uhs.vc runs the stream through it */
#endif
#define UHS_FADE 256u                      /* fade in / out, samples */
#define UHS_T_KNEE 24576                   /* the limiter: linear below 0.75 FS, then towards FS */

static int16_t uhs_ring[UHS_N * 2u] UH_POOL;

static struct {
    volatile uint32_t w, r;                /* frames written (producer), read (consumer) */
    volatile uint8_t active;               /* uhs_mix runs (set by uhs_start, cleared once faded out and stopped) */
    volatile uint8_t feeding;              /* the stream is on (uhs_start .. uhs_stop) */
    uint8_t ch, sb;                        /* the format: channels 1 / 2, bytes per sample 2 / 3 */
    uint8_t play;                          /* the consumer plays (primed); else it fades / waits */
    uint16_t g;                            /* fade gain 0..UHS_FADE */
    int16_t hold[2];                       /* the last frame played (faded out from) */
    uint32_t frac, step0;                  /* Q16: the phase between r and r + 1; the nominal step (rate / I2S rate) */
    uint32_t step;                         /* the step in use (the console) */
    uint32_t pkts, frames, overruns, underruns, primes, limited;
    uint32_t lrdiff, silent;               /* packets with L != R somewhere; frames of silence for empty packets */
    volatile uint8_t vc;                   /* the vocoder on, the keys' synth its carrier (the module's own mode) */
    volatile uint8_t vtrack;               /* the vocoder on, an instrument track its carrier: track + 1 (0: off): the
                                            * analysis here, the synthesis in the track's mix (vocoder.c vc_track) */
    volatile uint8_t head;                 /* host mode in the background: the header shows MIC / VOX (ui_draw.c) */
    volatile uint8_t page;                 /* the VOCODER page is open (main.c: it takes the knobs and the screen) */
    volatile uint16_t hpk;                 /* the input's peak since the header last looked */
    volatile uint32_t cyc_mix, cyc_svc;    /* timer ticks spent in uhs_mix (the vocoder) and in the iso service (the glue
                                            * counts them: audio.c, main.c) */
    uint16_t peak[2];                      /* level meters since the last uhs_meter_take */
    uint32_t mcnt;
    uint64_t sq[2];
} uhs;

/* the I2S frame rate in Hz * 2^16 / 1 is not needed: the step from the DAC's 544 TIMER4 ticks per frame at
 * 24 MHz (audio.c DAC_TICKS): step = rate / 44117.6 in Q16 */
static uint32_t uhs_step_for(uint32_t rate) { return (uint32_t)(((uint64_t)rate * 544u << 16) / 24000000u); }

static void uhs_start(uint32_t rate, uint32_t ch, uint32_t sb)
{
    uhs.feeding = 0;
    RING_PUBLISH();
    uhs.ch = (uint8_t)ch;
    uhs.sb = (uint8_t)sb;
    uhs.step0 = uhs.step = uhs_step_for(rate);
    uhs.pkts = uhs.frames = uhs.overruns = uhs.underruns = uhs.primes = uhs.limited = 0;
    uhs.lrdiff = uhs.silent = 0;
    uhs.r = uhs.w;                                       /* (the consumer only reads r while active) */
    uhs.frac = 0;
    uhs.play = 0;
    RING_PUBLISH();
    uhs.active = 1;
    uhs.feeding = 1;
}

static void uhs_stop(void) { uhs.feeding = 0; }        /* uhs_mix fades out and goes idle */

/* one packet into the ring; returns the frames taken (0: dropped, the ring full or the packet malformed) */
static uint32_t uhs_push(const uint8_t *p, uint32_t len)
{
    uint32_t fb = (uint32_t)uhs.ch * uhs.sb, n, i, w = uhs.w, k, lr = 0;
    if (!uhs.feeding || !fb || (uhs.sb != 2u && uhs.sb != 3u))
        return 0;
    n = len / fb;
    uhs.pkts++;
    if (w - uhs.r + n > UHS_N - 64u) {                  /* full: the consumer stalled; drop this one */
        uhs.overruns++;
        return 0;
    }
    for (i = 0; i < n; i++, p += fb) {
        int32_t v[2];
        for (k = 0; k < uhs.ch && k < 2u; k++) {
            const uint8_t *q = p + k * uhs.sb;
            v[k] = uhs.sb == 3u ? (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24) >> 16
                                : (int32_t)(int16_t)((uint32_t)q[0] | (uint32_t)q[1] << 8);
        }
        if (uhs.ch == 1u)
            v[1] = v[0];
        lr |= (uint32_t)(v[0] != v[1]);
        {
            uint32_t a0 = (uint32_t)(v[0] < 0 ? -v[0] : v[0]);
            if (a0 > uhs.hpk)
                uhs.hpk = (uint16_t)a0;
        }
        for (k = 0; k < 2u; k++) {
            uint32_t a = (uint32_t)(v[k] < 0 ? -v[k] : v[k]);
            uhs_ring[((w + i) & (UHS_N - 1u)) * 2u + k] = (int16_t)v[k];
            if (a > uhs.peak[k])
                uhs.peak[k] = (uint16_t)(a > 0xFFFFu ? 0xFFFFu : a);
            uhs.sq[k] += (uint64_t)(a * a);
        }
    }
    uhs.mcnt += n;
    uhs.frames += n;
    uhs.lrdiff += lr;
    RING_PUBLISH();                                      /* the frames before the index */
    uhs.w = w + n;
    return n;
}

/* an empty packet (a device may send them in silence): n frames of silence, so the ring keeps its fill and the sound
 * goes on without a fade or a new prime */
static void uhs_push_silence(uint32_t n)
{
    uint32_t w = uhs.w, i;
    if (!uhs.feeding || w - uhs.r + n > UHS_N - 64u)
        return;
    for (i = 0; i < n; i++)
        uhs_ring[((w + i) & (UHS_N - 1u)) * 2u] = uhs_ring[((w + i) & (UHS_N - 1u)) * 2u + 1u] = 0;
    uhs.silent += n;
    RING_PUBLISH();
    uhs.w = w + n;
}

/* soft limiter on the sum: linear to the knee, then towards full scale (never past it) */
static int32_t uhs_limit(int32_t x)
{
    const int32_t T = UHS_T_KNEE, R = 32767 - UHS_T_KNEE;
    if (x > T) {
        int32_t d = x - T;
        uhs.limited++;
        d = d > 0x3FFFF ? 0x3FFFF : d;                   /* (32-bit: d * R < 2^31) */
        return T + d * R / (d + R);
    }
    if (x < -T) {
        int32_t d = -x - T;
        uhs.limited++;
        d = d > 0x3FFFF ? 0x3FFFF : d;
        return -T - d * R / (d + R);
    }
    return x;
}

/* the audio ISR, per block: n stereo frames of the master output (Q15 in int32), the stream added */
static void uhs_mix(int32_t *out, uint32_t n)
{
    uint32_t i, w, r, frac = uhs.frac, step;
    int32_t e;
    if (!uhs.active)
        return;
#if UHS_VC
    if (uhs.vc || uhs.vtrack)
        vc_block();                                      /* the keys, the voices' pitch, the controls */
#endif
    w = uhs.w;
    RING_PUBLISH();
    r = uhs.r;
    if (!uhs.play && uhs.feeding && w - r >= UHS_TARGET) {
        uhs.play = 1;                                    /* primed: play (fading in) */
        uhs.primes++;
        frac = 0;
    }
    e = (int32_t)(w - r) - (int32_t)UHS_TARGET;          /* the fill's error steers the step, 1 % at most */
    if (e > 100)
        e = 100;
    if (e < -100)
        e = -100;
    step = uhs.step0 + (uint32_t)(e * (int32_t)uhs.step0 / 10000);
    uhs.step = step;
    for (i = 0; i < n; i++) {
        int32_t s[2], k;
        if (uhs.play && w - r < 2u) {                    /* ran dry: fade out the last frame, prime again */
            uhs.play = 0;
            uhs.underruns++;
        }
        if (uhs.play) {
            const int16_t *a = &uhs_ring[(r & (UHS_N - 1u)) * 2u], *b = &uhs_ring[((r + 1u) & (UHS_N - 1u)) * 2u];
            int32_t f = (int32_t)(frac >> 1);            /* Q15: (b - a) * f fits 31 bits */
            for (k = 0; k < 2; k++) {
                s[k] = a[k] + (((int32_t)b[k] - a[k]) * f >> 15);
                uhs.hold[k] = (int16_t)s[k];
            }
            if (uhs.g < UHS_FADE)
                uhs.g++;
            frac += step;
            r += frac >> 16;
            frac &= 0xFFFFu;
        } else {
            s[0] = uhs.hold[0];
            s[1] = uhs.hold[1];
            if (uhs.g)
                uhs.g--;
        }
#if UHS_VC
        if (uhs.vtrack) {                                /* the vocoder on a track: the analysis, the dry by MIX */
            int32_t m = vc_in((s[0] + s[1]) * (int32_t)uhs.g / (int32_t)(2u * UHS_FADE)), y;
            vc_sib_push();
            y = m * (127 - vc.mix) * 258 >> 15;
            out[2u * i] = uhs_limit(out[2u * i] + y);
            out[2u * i + 1u] = uhs_limit(out[2u * i + 1u] + y);
            continue;
        }
        if (uhs.vc) {                                    /* the vocoder: the microphone (mono) the modulator */
            int32_t y = vc_sample((s[0] + s[1]) * (int32_t)uhs.g / (int32_t)(2u * UHS_FADE));
            out[2u * i] = uhs_limit(out[2u * i] + y);
            out[2u * i + 1u] = uhs_limit(out[2u * i + 1u] + y);
            continue;
        }
#endif
        if (uhs.g) {
            out[2u * i] = uhs_limit(out[2u * i] + s[0] * (int32_t)uhs.g / (int32_t)UHS_FADE);
            out[2u * i + 1u] = uhs_limit(out[2u * i + 1u] + s[1] * (int32_t)uhs.g / (int32_t)UHS_FADE);
        }
    }
    uhs.frac = frac;
    RING_PUBLISH();
    uhs.r = r;
    if (!uhs.feeding && !uhs.play && !uhs.g)
        uhs.active = 0;                                  /* stopped and faded out: idle again */
}

/* the audio ISR, per block, for an instrument that is a vocoder of its own: n samples of the stream (mono: L + R
 * averaged, Q15 in int32, the fades applied) into m[], resampled as uhs_mix does; nothing added anywhere. Returns 1
 * while the stream plays (primed), 0 when there is none (m[] silence) */
static uint32_t uhs_read_mono(int32_t *m, uint32_t n)
{
    uint32_t i, w, r, frac = uhs.frac, step;
    int32_t e;
    if (!uhs.active) {
        for (i = 0; i < n; i++)
            m[i] = 0;
        return 0;
    }
    w = uhs.w;
    RING_PUBLISH();
    r = uhs.r;
    if (!uhs.play && uhs.feeding && w - r >= UHS_TARGET) {
        uhs.play = 1;
        uhs.primes++;
        frac = 0;
    }
    e = (int32_t)(w - r) - (int32_t)UHS_TARGET;
    e = e > 100 ? 100 : e < -100 ? -100 : e;
    step = uhs.step0 + (uint32_t)(e * (int32_t)uhs.step0 / 10000);
    uhs.step = step;
    for (i = 0; i < n; i++) {
        int32_t s[2], k;
        if (uhs.play && w - r < 2u) {
            uhs.play = 0;
            uhs.underruns++;
        }
        if (uhs.play) {
            const int16_t *a = &uhs_ring[(r & (UHS_N - 1u)) * 2u], *b = &uhs_ring[((r + 1u) & (UHS_N - 1u)) * 2u];
            int32_t f = (int32_t)(frac >> 1);
            for (k = 0; k < 2; k++) {
                s[k] = a[k] + (((int32_t)b[k] - a[k]) * f >> 15);
                uhs.hold[k] = (int16_t)s[k];
            }
            if (uhs.g < UHS_FADE)
                uhs.g++;
            frac += step;
            r += frac >> 16;
            frac &= 0xFFFFu;
        } else {
            s[0] = uhs.hold[0];
            s[1] = uhs.hold[1];
            if (uhs.g)
                uhs.g--;
        }
        m[i] = (s[0] + s[1]) * (int32_t)uhs.g / (int32_t)(2u * UHS_FADE);
    }
    uhs.frac = frac;
    RING_PUBLISH();
    uhs.r = r;
    if (!uhs.feeding && !uhs.play && !uhs.g)
        uhs.active = 0;
    return uhs.play;
}

/* the meters since the last call: peak and RMS per channel (0..32767); the caller masks interrupts around it */
static void uhs_meter_take(uint32_t *pk, uint32_t *rms)
{
    uint32_t k;
    for (k = 0; k < 2u; k++) {
        uint64_t m = uhs.mcnt ? uhs.sq[k] / uhs.mcnt : 0u;
        uint32_t x = 0, b;
        for (b = 1u << 15; b; b >>= 1)                   /* integer square root */
            if ((uint64_t)(x | b) * (x | b) <= m)
                x |= b;
        pk[k] = uhs.peak[k];
        rms[k] = x;
        uhs.peak[k] = 0;
        uhs.sq[k] = 0;
    }
    uhs.mcnt = 0;
}

/* dBFS of a Q15 level, -90..0 (for the meter: integer, 0.1 dB steps not needed) */
static int32_t uhs_db(uint32_t v)
{
    int32_t db = 0;
    if (!v)
        return -90;
    while (v < 32768u && db > -90) {                     /* 6.02 dB per halving, the rest by quarter steps */
        if (v * 2u <= 32768u) {
            v *= 2u;
            db -= 6;
        } else {
            uint32_t q = v + v / 4u;                     /* * 1.25: ~1.9 dB */
            if (q > 32768u)
                break;
            v = q;
            db -= 2;
        }
    }
    return db;
}
