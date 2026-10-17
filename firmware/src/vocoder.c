/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (makkuro) */
/* Channel vocoder (SMK-37 Pro, USB host mode): the microphone stream (usbhost_audio.c) is the modulator, a small
 * polyphonic synth played by the keys the carrier. Fixed point, no float. Self-contained: the keys come through
 * VC_KEYS() / VC_VEL(k) and RING_PUBLISH() orders stores, both from the includer (a glue file; the host test its own).
 *
 *   carrier    VC_NV voices: band-limited saw (polyBLEP), pulse, or a 2-saw supersaw (the default; detune), velocity,
 *              pitch bend, a linear attack / release; noise at the notes' level: a floor (SIBIL), and in place of the
 *              notes in the upper bands while the input is unvoiced
 *   bands      VC_NB (16) band-passes, log spaced 80 Hz .. 10 kHz (5.6 semitones apart), each two trapezoidal SVF
 *              stages (A. Simper; Q30 coefficients; Q 3 each: 4 poles), the same for analysis and synthesis
 *   multirate  the analysis runs where the bands need it: from ~3.2 kHz at the full rate, 1 .. 3 kHz at a quarter
 *              (after a 4-pole low-pass at 4.5 kHz), below 1 kHz at a sixteenth (another at 1.1 kHz): about 14 filter
 *              stages a sample instead of 32 (the envelopes are slow anyway)
 *   analysis   envelope followers (attack ~1.5 ms, release by knob 10..300 ms, ~26 ms by default); each band's long
 *              average (~0.7 s) gives it a normalising gain towards the bands' mean (+-12 dB, by NORM): the lows do
 *              not drown the highs
 *   voicing    zero crossings (~5 ms) and the energy above ~3.5 kHz against that below 1 kHz: unvoiced (S, F, SH, T)
 *              moves the upper bands' carrier to noise and lets the input above 5 kHz through (the consonants)
 *   synthesis  the carrier through the bands, shifted by the formant knob (+-12 semitones), each times its envelope,
 *              its normalising gain and its weight (+3 dB an octave: a saw's bands fall; +4 dB 1..3 kHz: FBOOST),
 *              summed; the dry microphone mixed in by MIX
 *   input      the microphone: input gain, a high-pass at ~80 Hz (two one-pole stages), a gentle compressor (3:1
 *              above -24 dBFS, 5 ms / 100 ms); its peak and clips counted (vocdump)
 * The controls are 0..127 values (vc.p[]; K1..K8 the first eight, vc_turn); vc_apply turns them into coefficients
 * outside the audio ISR (double buffered: the ISR switches set at its next block). Run per output sample at the I2S
 * rate (44,117.6 Hz), vc_block every 32 samples. */
#ifndef VC_NB
#define VC_NB 16u                          /* bands (20 costs ~25 % more: hardware test 7, 102 % CPU with 20) */
#endif
#define VC_NV 4u                           /* carrier voices */
#define VC_NG 102u                         /* VC_G: 50 Hz * 2^(i/12), i = 0..101 (17.1 kHz) */
#if VC_NB >= 20
#define VC_QK 10923                        /* damping k = 1/Q of each of a band's two SVF stages in Q15: Q 3 (4.4 semitones
                                            * apart; the pair: 4 poles, 12 dB an octave outside) */
#else
#define VC_QK 12616                        /* Q 2.6 (5.6 semitones apart: the bands' sum within ~3 dB) */
#endif

/* tan(pi f / fs) in Q24, f = 50 Hz * 2^(i/12), fs = 24 MHz / 544 */
static const int32_t VC_G[VC_NG] = {
    59735, 63287, 67050, 71037, 75262, 79737, 84478, 89502, 94824, 100463, 106437, 112766, 119472, 126576,
    134103, 142078, 150526, 159478, 168961, 179009, 189654, 200933, 212882, 225542, 238955, 253166, 268223,
    284175, 301077, 318984, 337957, 358059, 379357, 401923, 425833, 451166, 478008, 506448, 536583, 568514,
    602348, 638199, 676188, 716444, 759102, 804308, 852214, 902985, 956792, 1013820, 1074265, 1138335, 1206250,
    1278247, 1354576, 1435505, 1521319, 1612321, 1708838, 1811216, 1919828, 2035072, 2157376, 2287199, 2425036,
    2571421, 2726929, 2892184, 3067863, 3254701, 3453504, 3665149, 3890601, 4130925, 4387297, 4661024, 4953567,
    5266560, 5601850, 5961529, 6347985, 6763959, 7212622, 7697666, 8223431, 8795054, 9418681, 10101733,
    10853277, 11684516, 12609490, 13646047, 14817259, 16153523, 17695754, 19500419, 21647774, 24255961,
    27506493, 31693539, 37327717, 45380695};
/* phase increments (2^32 f / fs) of MIDI notes 120..131; lower octaves shifted down */
static const uint32_t VC_INC[12] = {815037662u, 863502323u, 914848843u, 969248586u, 1026883106u, 1087944753u,
                                    1152637315u, 1221176696u, 1293791642u, 1370724496u, 1452232016u, 1538586225u};
/* 2^(k / 768) in Q16: 1/64 semitone steps */
static const uint16_t VC_FR[64] = {
    0, 59, 118, 178, 237, 296, 356, 415, 475, 535, 594, 654, 714, 773, 833, 893, 953, 1013, 1073, 1134, 1194,
    1254, 1314, 1375, 1435, 1496, 1556, 1617, 1677, 1738, 1799, 1859, 1920, 1981, 2042, 2103, 2164, 2225, 2287,
    2348, 2409, 2471, 2532, 2593, 2655, 2716, 2778, 2840, 2902, 2963, 3025, 3087, 3149, 3211, 3273, 3335, 3397,
    3460, 3522, 3584, 3647, 3709, 3772, 3834};          /* (65536 added in vc_inc) */

/* K1..K8 the first eight; TILT, FBOOST, NORM have no knob (their defaults) */
enum { VCP_MIX, VCP_REL, VCP_FMT, VCP_SIBIL, VCP_WAVE, VCP_DET, VCP_GAIN, VCP_LEVEL, VCP_TILT, VCP_FBOOST, VCP_NORM,
       VCP_N };
#define VCP_KNOBS 8u
static const char *const VCP_NAME[VCP_N] = {"MIX", "REL", "FMT", "SIBIL", "WAVE", "DET", "GAIN", "LEVEL", "TILT",
                                            "FBOOST", "NORM"};
/* MIX: the vocoder alone; REL 7: ~26 ms; WAVE: supersaw; GAIN 24: x1; FBOOST 1: +4 dB 1..3 kHz; NORM 64: half */
static const uint8_t VCP_DEF[VCP_N] = {127, 7, 64, 64, 127, 30, 24, 80, 64, 1, 64};
/* 2^(k / 24) in Q12: the synthesis bands' pre-emphasis, +3 dB an octave (a saw's bands fall 3 dB an octave) */
static const uint16_t VC_T24[24] = {4096, 4216, 4340, 4467, 4598, 4732, 4871, 5014, 5161, 5312, 5468, 5628, 5793,
                                    5962, 6137, 6317, 6502, 6693, 6889, 7090, 7298, 7512, 7732, 7959};
#define VC_SIB_IDX 80                      /* VC_G index of the sibilance high-pass: 5 kHz */
#define VC_KBW 23726566                    /* damping 1.414 (Butterworth) in Q24 */
#define VC_I0 8                            /* the lowest band: VC_G index 8, 80 Hz */
#define VC_ISPAN 836                       /* .. 83.6 semitones up: 10 kHz (in 1/10 semitones) */
#define VC_FLOOR 16                        /* an envelope at or below: silence (-66 dBFS) */
#define VC_LP_IDX 78                       /* VC_G index of the decimating low-passes: 4.5 kHz at the full rate, 1.1 kHz
                                            * at a quarter (the same coefficients) */
#define VC_ZC_UV 7                         /* zero crossings in a block (32 samples) above: unvoiced (~0.22 a sample) */

typedef struct { int32_t a1, a2, a3; } vc_bq_t;          /* SVF coefficients, Q30 */

typedef struct {
    uint8_t on, note;                      /* on: held (release when 0 and amp falls to 0) */
    int32_t amp, tgt;                      /* amplitude Q15 (velocity), its target */
    uint32_t ph[3];
} vc_voice_t;

static struct {
    volatile uint8_t p[VCP_N];             /* the controls, 0..127 (main loop) */
    volatile int16_t bend;                 /* pitch bend -8192..8191 (+-2 semitones) */
    volatile int8_t oct;                   /* octave shift of the keys */
    volatile uint8_t set;                  /* the coefficient set the ISR uses (vc_apply writes the other one) */
    vc_bq_t ana[VC_NB], syn[2][VC_NB];
    int32_t tilt[2][VC_NB];                /* Q12 band weights: pre-emphasis, tilt, formant boost */
    int16_t wu[VC_NB];                     /* the unvoiced weight of each band, Q15 (0 below ~1.2 kHz, 1 from ~3 kHz) */
    int32_t ca, cr[2];                     /* envelope coefficients, Q15: attack, release (at the full rate) */
    uint8_t b4, b1;                        /* bands [0, b4) analysed at 1/16 of the rate, [b4, b1) at 1/4, the rest at 1 */
    vc_bq_t lpc;                           /* the decimating low-pass (Butterworth) */
    int32_t lp1[2][2], lp2[2][2];          /* its states: at the full rate, at a quarter */
    uint32_t dcnt;                         /* samples (the decimation's phase) */
    int32_t ai[2][VC_NB][2], si[2][VC_NB][2];   /* filter states: stage, band, (ic1, ic2) */
    int32_t env[VC_NB];                    /* envelopes, Q15 */
    int32_t na[VC_NB], ng[VC_NB];          /* the bands' long average (envelope << 12, ~0.7 s), their normalising gain (Q12) */
    vc_voice_t v[VC_NV];
    uint64_t keys;                         /* the keys as last seen */
    int32_t nst;                           /* noise state */
    uint32_t inc[VC_NV][3];                /* this block's increments */
    uint8_t wave, ready;
    int32_t mix, level, nfl;               /* this block's: mix Q7, level Q7, the carrier's noise floor Q15 */
    uint32_t idle;                         /* samples without a sounding voice: synthesis off after 50 ms */
    /* the input: gain Q8, the high-pass's two low-pass states (Q4), the compressor's detector and gain (Q12) */
    int32_t ig, hp1, hp2, ce, gc, sib;
    vc_bq_t sibc;                          /* the 5 kHz high-pass (sibilance) */
    int32_t sb1, sb2;
    /* voiced / unvoiced: zero crossings this block, their smoothed rate (x16), the unvoiced degree (Q15), the sign */
    int32_t zc, zs, uv, hlast;
    int32_t sibv, tpk;                     /* the sibilance of the last input sample; the track carrier's peak */
#define VC_SQ 128u                         /* (a power of two: two blocks of the audio ISR and more) */
    int32_t sibq[VC_SQ];                   /* the sibilance, a block ahead of the track (vc_track) */
    uint32_t sqw, sqr;
    /* vc_dump: since the last dump */
    int32_t inpk, cpk, envpk[VC_NB];
    uint32_t clips, uvblocks, blocks;
} vc;

/* n / d without a 64-bit divide (the target's library has none): shift and subtract; the main loop only */
static uint32_t vc_div(uint64_t n, uint32_t d)
{
    uint64_t r = 0;
    uint32_t q = 0;
    int32_t i;
    for (i = 63; i >= 0; i--) {                         /* (constant shifts only: no 64-bit shift helper) */
        r = r << 1 | (n >> 63);
        n <<= 1;
        q <<= 1;
        if (r >= d) {
            r -= d;
            q |= 1u;
        }
    }
    return q;
}

/* the SVF coefficients of table entry i, damping k (Q24) (Simper: a1 = 1 / (1 + g (g + k)), a2 = g a1, a3 = g a2) */
static void vc_coef_k(vc_bq_t *c, int32_t i, int64_t k)
{
    int64_t g = VC_G[i < 0 ? 0 : i >= (int32_t)VC_NG ? (int32_t)VC_NG - 1 : i];   /* Q24 */
    int64_t den = (1ll << 24) + (g * (g + k) >> 24);
    c->a1 = (int32_t)vc_div(1ull << 54, (uint32_t)den);   /* (den < 2^28: a1 <= 2^30) */
    c->a2 = (int32_t)((int64_t)c->a1 * g >> 24);
    c->a3 = (int32_t)((int64_t)c->a2 * g >> 24);
}
static void vc_coef(vc_bq_t *c, int32_t i) { vc_coef_k(c, i, (int64_t)VC_QK << 9); }

/* band b's VC_G index: VC_I0 .. VC_I0 + 83.6 semitones, evenly on a log scale */
static int32_t vc_band_idx(uint32_t b)
{
    return VC_I0 + (int32_t)((b * VC_ISPAN + 5u * (VC_NB - 1u)) / (10u * (VC_NB - 1u)));
}

/* the controls -> coefficients (main loop; the ISR picks the new set at its next block) */
static void vc_apply(void)
{
    uint32_t s = vc.set ^ 1u, b;
    int32_t sh = ((int32_t)vc.p[VCP_FMT] - 64) * 12 / 63, tl = (int32_t)vc.p[VCP_TILT] - 64, rel_ms;
    for (b = 0; b < VC_NB; b++) {
        int32_t i = vc_band_idx(b) + sh, e = i - 12, w;     /* e: semitones above 100 Hz */
        int32_t pe = e >= 0 ? (int32_t)VC_T24[e % 24] << (e / 24) : (int32_t)VC_T24[(e + 48) % 24] >> 2;
        w = 4096 + tl * ((int32_t)b * 2 - (int32_t)VC_NB + 1) * 48 / (int32_t)VC_NB;   /* the tilt, +-3 dB-ish */
        w = (w < 256 ? 256 : w > 12000 ? 12000 : w) * pe >> 12;
        if (vc.p[VCP_FBOOST] && i >= 52 && i <= 71)       /* the formant boost: +4 dB, 1..3 kHz */
            w = w * 6492 >> 12;
        vc_coef(&vc.syn[s][b], i);
        vc.tilt[s][b] = w > 65535 ? 65535 : w;
    }
    rel_ms = 10 + (int32_t)vc.p[VCP_REL] * 290 / 127;
    vc.cr[s] = (int32_t)(32768u / ((uint32_t)rel_ms * 44u));                  /* ~1 / (t fs), Q15 */
    if (vc.cr[s] < 1)
        vc.cr[s] = 1;
    RING_PUBLISH();                                       /* the set before its index */
    vc.set = (uint8_t)s;
}

static void vc_init(void)
{
    uint32_t b, k;
    for (k = 0; k < sizeof vc; k++)
        ((uint8_t *)&vc)[k] = 0;
    for (k = 0; k < VCP_N; k++)
        vc.p[k] = VCP_DEF[k];
    vc.b4 = vc.b1 = (uint8_t)VC_NB;
    for (b = VC_NB; b-- > 0;) {                           /* the rate groups: 1 from ~3.2 kHz, 1/4 from 1 kHz */
        int32_t i = vc_band_idx(b);
        if (i < 72 && vc.b1 == VC_NB)
            vc.b1 = (uint8_t)(b + 1u);
        if (i < 52 && vc.b4 == VC_NB)
            vc.b4 = (uint8_t)(b + 1u);
    }
    vc.b4 = vc.b4 > vc.b1 ? vc.b1 : vc.b4;
    vc_coef_k(&vc.lpc, VC_LP_IDX, VC_KBW);
    for (b = 0; b < VC_NB; b++) {
        int32_t i = vc_band_idx(b);
        vc_coef(&vc.ana[b], i + (b < vc.b4 ? 48 : b < vc.b1 ? 24 : 0));   /* (a quarter of the rate: f * 4, +24) */
        vc.wu[b] = (int16_t)(i <= 46 ? 0 : i >= 62 ? 32767 : (i - 46) * 32767 / 16);   /* 1.2 .. 3 kHz */
        vc.ng[b] = 4096;
    }
    vc.ca = 32768 / 66;                                   /* attack ~1.5 ms */
    vc.nst = 0x1234567;
    vc.gc = 4096;
    vc_coef_k(&vc.sibc, VC_SIB_IDX, VC_KBW);
    vc_apply();
    vc_apply();                                           /* (both sets) */
    vc.ready = 1;
}

/* a knob turned: K1..K8 (e 0..7) by d detents, two steps a detent; 1 if vc_apply is due */
static uint32_t vc_turn(uint32_t e, int32_t d)
{
    int32_t v;
    if (e >= VCP_KNOBS || !d)
        return 0;
    v = (int32_t)vc.p[e] + d * 2;
    vc.p[e] = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
    return e == VCP_REL || e == VCP_FMT;
}

/* a control as text: "MIX 127", "FMT +3", "WAVE SAW", "GAIN +6dB" (t: at least 12 characters) */
static void vc_ctl_text(uint32_t c, char *t)
{
    static const char *const WV[3] = {"SAW", "PLS", "SUP"};
    const char *n = VCP_NAME[c];
    int32_t v = vc.p[c];
    char d[8];
    uint32_t k = 0;
    while (*n)
        *t++ = *n++;
    *t++ = ' ';
    if (c == VCP_WAVE) {
        for (n = WV[v * 3 / 128]; *n;)
            *t++ = *n++;
        *t = 0;
        return;
    }
    if (c == VCP_GAIN) {                                 /* the input gain in dB: x (64 + 8 GAIN) / 256 */
        int32_t x = 64 + v * 8, db = 0;
        while (x >= 512) {
            x >>= 1;
            db += 6;
        }
        while (x < 256) {
            x <<= 1;
            db -= 6;
        }
        v = db + (x - 256) * 6 / 256;                     /* (within half a dB) */
    }
    if (c == VCP_FMT || c == VCP_GAIN) {                 /* semitones; dB */
        v = c == VCP_FMT ? (v - 64) * 12 / 63 : v;
        *t++ = v < 0 ? '-' : '+';
        v = v < 0 ? -v : v;
    }
    do
        d[k++] = (char)('0' + v % 10);
    while ((v /= 10) != 0);
    while (k)
        *t++ = d[--k];
    if (c == VCP_GAIN) {
        *t++ = 'd';
        *t++ = 'B';
    }
    *t = 0;
}

/* the phase increment of pitch p (1/64 semitone, MIDI note * 64) */
static uint32_t vc_inc(int32_t p)
{
    int32_t n = p >> 6, sh;
    uint32_t inc;
    if (n < 0)
        n = 0;
    if (n > 131)
        n = 131;
    sh = 10 - n / 12;
    inc = VC_INC[n % 12] >> sh;
    return (uint32_t)((uint64_t)inc * (65536u + VC_FR[p & 63]) >> 16);
}

static void vc_note(uint32_t note, uint32_t vel, uint32_t on)
{
    uint32_t i, best = 0;
    int32_t lo = 0x7FFFFFFF;
    for (i = 0; i < VC_NV; i++)
        if (vc.v[i].note == note && (vc.v[i].on || vc.v[i].amp)) {
            if (!on) {
                vc.v[i].on = 0;
                vc.v[i].tgt = 0;
                return;
            }
            best = i;
            lo = -1;
            break;
        }
    if (!on)
        return;
    if (lo != -1)                                         /* a free voice, else the quietest */
        for (i = 0; i < VC_NV; i++) {
            int32_t a = vc.v[i].on ? vc.v[i].amp + 65536 : vc.v[i].amp;
            if (a < lo) {
                lo = a;
                best = i;
            }
        }
    vc.v[best].on = 1;
    vc.v[best].note = (uint8_t)note;
    vc.v[best].tgt = (int32_t)(vel > 127u ? 127u : vel) * 258;   /* Q15 */
}

static int32_t vc_blep(uint32_t ph, uint32_t inc)        /* (dsp.c's polyBLEP) */
{
    uint32_t d = inc >> 15;
    int32_t x;
    if (!d)
        return 0;
    if (ph < inc) {
        x = (int32_t)(ph / d);
        return x + x - ((x * x) >> 15) - 32768;
    }
    if (ph > 0xFFFFFFFFu - inc) {
        x = -(int32_t)((0xFFFFFFFFu - ph) / d);
        return ((x * x) >> 15) + x + x + 32768;
    }
    return 0;
}
static int32_t vc_saw(uint32_t ph, uint32_t inc) { return ((int32_t)(ph >> 16) - 32768) - vc_blep(ph, inc); }

/* once per block (the audio ISR, 32 samples): keys, voice pitches, the controls; the compressor's gain, the
 * voiced / unvoiced decision, the bands' normalising gains */
static void vc_block(void)
{
    uint64_t k = VC_KEYS(), ch = k ^ vc.keys;
    uint32_t i, j, b;
    int32_t det = (int32_t)vc.p[VCP_DET] * 32 / 127, base = 48 + 12 * vc.oct;   /* detune up to 1/2 semitone */
    for (i = 0; ch && i < 64u; i++)
        if (ch >> i & 1u)
            vc_note(base + i, (k >> i & 1u) ? VC_VEL(i) : 0u, (uint32_t)(k >> i & 1u));
    vc.keys = k;
    vc.wave = (uint8_t)(vc.p[VCP_WAVE] * 3u / 128u);       /* 0 saw, 1 pulse, 2 supersaw */
    for (i = 0; i < VC_NV; i++) {
        int32_t p = (int32_t)vc.v[i].note * 64 + vc.bend / 64;
        vc.inc[i][0] = vc_inc(p);
        if (vc.wave == 2u)
            for (j = 1; j < 3u; j++)
                vc.inc[i][j] = vc_inc(p + (j == 1u ? det : -det));
    }
    vc.mix = vc.p[VCP_MIX];
    vc.level = vc.p[VCP_LEVEL];
    vc.nfl = (int32_t)vc.p[VCP_SIBIL] * 48;              /* the carrier's noise floor: up to ~0.19 of the notes */
    vc.sib = vc.p[VCP_SIBIL];
    vc.ig = 64 + (int32_t)vc.p[VCP_GAIN] * 8;            /* Q8: x0.25 .. x4.2, x1 at 24 */
    {   /* the compressor's gain from its detector: 3:1 above -24 dBFS, x2 makeup (Q12), smoothed per block */
        const int32_t T = 2063;
        int32_t e = vc.ce, g = 8192;
        if (e > T)
            g = (T + (e - T) / 3) * 8192 / e;
        vc.gc += (g - vc.gc) / 4;
    }
    {   /* voiced / unvoiced (~5 ms): many zero crossings and the energy above ~3.5 kHz over that below 1 kHz */
        int32_t lo = 0, hi = 0, ut;
        for (b = 0; b < VC_NB; b++) {
            int32_t i2 = vc_band_idx(b);
            if (i2 <= 52)
                lo += vc.env[b];
            else if (i2 >= 66)
                hi += vc.env[b];
        }
        vc.zs += (vc.zc * 16 - vc.zs) >> 3;
        vc.zc = 0;
        ut = vc.zs > VC_ZC_UV * 16 && hi * 2 > lo ? 32767 : 0;
        vc.uv += (ut - vc.uv) >> 2;                       /* (~3 ms) */
        vc.blocks++;
        vc.uvblocks += vc.uv > 16384;
    }
    {   /* the bands' normalising gains: each band's long average to the bands' mean, +-12 dB at most, by NORM */
        int32_t mean = 0, nm = vc.p[VCP_NORM];
        for (b = 0; b < VC_NB; b++)
            mean += vc.na[b] >> 12;
        mean /= (int32_t)VC_NB;
        for (b = 0; b < VC_NB; b++) {
            int32_t g = mean > 32 ? (mean << 12) / ((vc.na[b] >> 12) + (mean >> 4) + 1) : 4096;   /* Q12 */
            g = g < 1024 ? 1024 : g > 16384 ? 16384 : g;
            vc.ng[b] = 4096 + (g - 4096) * nm / 127;
        }
    }
}

/* one SVF band-pass stage (Simper), its peak gain 1: x in, k v1 out */
static inline int32_t vc_bp(const vc_bq_t *c, int32_t *st, int32_t x)
{
    int32_t v3 = x - st[1], v1, v2;
    v1 = (int32_t)(((int64_t)c->a1 * st[0] + (int64_t)c->a2 * v3) >> 30);
    v2 = st[1] + (int32_t)(((int64_t)c->a2 * st[0] + (int64_t)c->a3 * v3) >> 30);
    st[0] = 2 * v1 - st[0];
    st[1] = 2 * v2 - st[1];
    v1 = (int32_t)((int64_t)v1 * VC_QK >> 15);
    return v1 > 262143 ? 262143 : v1 < -262143 ? -262143 : v1;
}

/* an SVF stage's low-pass output (the decimators) */
static inline int32_t vc_lp(const vc_bq_t *c, int32_t *st, int32_t x)
{
    int32_t v3 = x - st[1], v1, v2;
    v1 = (int32_t)(((int64_t)c->a1 * st[0] + (int64_t)c->a2 * v3) >> 30);
    v2 = st[1] + (int32_t)(((int64_t)c->a2 * st[0] + (int64_t)c->a3 * v3) >> 30);
    st[0] = 2 * v1 - st[0];
    st[1] = 2 * v2 - st[1];
    return v2;
}

/* bands [b0, b1) analysed on x at a rate 4^sh lower: their envelopes (attack / release scaled to the rate), long
 * averages and peaks */
static void vc_analyse(uint32_t b0, uint32_t b1, int32_t x, uint32_t sh, int32_t cr)
{
    int32_t ca = vc.ca << (2u * sh), crr = cr << (2u * sh);
    uint32_t b;
    crr = crr > 32767 ? 32767 : crr;
    for (b = b0; b < b1; b++) {
        int32_t e = vc_bp(&vc.ana[b], vc.ai[1][b], vc_bp(&vc.ana[b], vc.ai[0][b], x));
        e = e < 0 ? -e : e;
        if (e > 65535)
            e = 65535;
        vc.env[b] += (e - vc.env[b]) * (e > vc.env[b] ? ca : crr) >> 15;
        vc.na[b] += ((vc.env[b] << 12) - vc.na[b]) >> (15u - 2u * sh);
        if (vc.env[b] > vc.envpk[b])
            vc.envpk[b] = vc.env[b];
    }
}

/* The sample path in three parts, so that the carrier can come from elsewhere (an instrument's track, a block later:
 * vc_track) as well as from the keys (vc_sample, the module's own synth):
 *   vc_in      the modulator: input chain, the sibilance (vc.sibv), the analysis; returns the processed input (dry)
 *   vc_carrier the keys' synth: one sample of its voices; *amax their loudest amplitude (the noise follows it)
 *   vc_out     the synthesis of a carrier sample through the bands, the sibilance added: the vocoder's signal */
static int32_t vc_in(int32_t m)
{
    int32_t mr = m < 0 ? -m : m, h;
    if (mr > vc.inpk)                                     /* the input as it came (the device's own level) */
        vc.inpk = mr;
    if (mr >= 32000)
        vc.clips++;
    m = m * vc.ig >> 8;                                   /* the input gain */
    m = m > 65535 ? 65535 : m < -65535 ? -65535 : m;
    vc.hp1 += ((m << 4) - vc.hp1) * 747 >> 16;            /* high-pass ~80 Hz: m minus two one-pole low-passes */
    h = m - (vc.hp1 >> 4);
    vc.hp2 += ((h << 4) - vc.hp2) * 747 >> 16;
    h -= vc.hp2 >> 4;
    if ((h > 32 && vc.hlast < 0) || (h < -32 && vc.hlast > 0)) {
        vc.zc++;                                          /* a zero crossing (a dead band of +-32 against hiss) */
        vc.hlast = h > 0 ? 1 : -1;
    } else if (!vc.hlast && (h > 32 || h < -32)) {
        vc.hlast = h > 0 ? 1 : -1;
    }
    mr = h < 0 ? -h : h;                                  /* the compressor's detector: 5 ms up, 100 ms down */
    vc.ce += (mr - vc.ce) * (mr > vc.ce ? 149 : 7) >> 15;
    m = h * vc.gc >> 12;
    m = m > 65535 ? 65535 : m < -65535 ? -65535 : m;
    mr = m < 0 ? -m : m;
    if (mr > vc.cpk)
        vc.cpk = mr;
    {   /* the sibilance: the input above 5 kHz (SVF high-pass: x - k v1 - v2), mostly while unvoiced */
        const vc_bq_t *c = &vc.sibc;
        int32_t v3 = m - vc.sb2, v1, v2, sib;
        v1 = (int32_t)(((int64_t)c->a1 * vc.sb1 + (int64_t)c->a2 * v3) >> 30);
        v2 = vc.sb2 + (int32_t)(((int64_t)c->a2 * vc.sb1 + (int64_t)c->a3 * v3) >> 30);
        vc.sb1 = 2 * v1 - vc.sb1;
        vc.sb2 = 2 * v2 - vc.sb2;
        sib = (m - (v1 * 5793 >> 12) - v2) * vc.sib >> 6;   /* x1 at SIBIL 64 */
        vc.sibv = sib * (4096 + (vc.uv >> 3) * 7) >> 15; /* 1/8 always, all of it unvoiced */
    }
    {   /* the analysis, multirate (the low bands at a quarter and a sixteenth of the rate, decimated through 4-pole
         * low-passes) */
        uint32_t d = vc.dcnt++;
        int32_t cr = vc.cr[vc.set], x1 = vc_lp(&vc.lpc, vc.lp1[1], vc_lp(&vc.lpc, vc.lp1[0], m));
        vc_analyse(vc.b1, VC_NB, m, 0, cr);
        if (!(d & 3u)) {
            vc_analyse(vc.b4, vc.b1, x1, 1, cr);
            x1 = vc_lp(&vc.lpc, vc.lp2[1], vc_lp(&vc.lpc, vc.lp2[0], x1));
            if (!(d & 15u))
                vc_analyse(0, vc.b4, x1, 2, cr);
        }
    }
    return m;
}

static int32_t vc_carrier(int32_t *amax)
{
    int32_t car = 0, am = 0;
    uint32_t i;
    for (i = 0; i < VC_NV; i++) {
        vc_voice_t *v = &vc.v[i];
        int32_t o;
        if (v->amp == v->tgt && !v->amp)
            continue;
        if (v->amp < v->tgt)                              /* attack ~4 ms, release ~60 ms */
            v->amp = v->amp + 190 > v->tgt ? v->tgt : v->amp + 190;
        else if (v->amp > v->tgt)
            v->amp = v->amp - 12 < v->tgt ? v->tgt : v->amp - 12;
        if (vc.wave == 1u) {
            o = (vc_saw(v->ph[0], vc.inc[i][0]) - vc_saw(v->ph[0] + 0x80000000u, vc.inc[i][0])) >> 1;
            v->ph[0] += vc.inc[i][0];
        } else if (vc.wave == 2u) {                       /* two saws, detuned up and down */
            o = (vc_saw(v->ph[1], vc.inc[i][1]) + vc_saw(v->ph[2], vc.inc[i][2])) >> 1;
            v->ph[1] += vc.inc[i][1];
            v->ph[2] += vc.inc[i][2];
        } else {
            o = vc_saw(v->ph[0], vc.inc[i][0]);
            v->ph[0] += vc.inc[i][0];
        }
        car += o * (v->amp >> 1) >> 14;                   /* (one voice at full velocity: ~FS) */
        if (v->amp > am)
            am = v->amp;
    }
    *amax = am;
    return car;
}

/* the carrier sample car (its notes' loudest amplitude amax, Q15: the noise follows it; 0 for 50 ms: the synthesis
 * rests) through the bands, the sibilance sib added: the vocoder's signal (no dry) */
static int32_t vc_out(int32_t car, int32_t amax, int32_t sib)
{
    const uint32_t s = vc.set;
    const vc_bq_t *sy = vc.syn[s];
    const int32_t *tw = vc.tilt[s];
    int32_t nz, acc = 0, uv = vc.uv;
    uint32_t b;
    {   /* noise (xorshift32) at the notes' level: the unvoiced carrier, a floor by SIBIL */
        uint32_t x = (uint32_t)vc.nst;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        vc.nst = (int32_t)x;
        nz = ((int32_t)x >> 16) * amax >> 15;             /* (with the notes: no key, no hiss) */
    }
    if (amax)
        vc.idle = 0;
    else if (vc.idle < 4096u && ++vc.idle == 2205u)      /* 50 ms silent: the synthesis filters cleared (no */
        for (b = 0; b < VC_NB; b++)                       /* rounding limit cycle times an envelope) and left out */
            vc.si[0][b][0] = vc.si[0][b][1] = vc.si[1][b][0] = vc.si[1][b][1] = 0;
    for (b = 0; b < VC_NB && vc.idle < 2205u; b++) {
        int32_t bp, e, x, u;
        /* synthesis: the carrier (towards noise in the upper bands while unvoiced) through the band, times the
         * envelope, the band's normalising gain and its weight */
        u = uv * vc.wu[b] >> 15;
        x = (car * (32767 - u) >> 15) + (nz * (vc.nfl + u) >> 15);
        bp = vc_bp(&sy[b], vc.si[1][b], vc_bp(&sy[b], vc.si[0][b], x));
        bp = bp > 65535 ? 65535 : bp < -65535 ? -65535 : bp;
        e = vc.env[b] - VC_FLOOR;                         /* (a floor: -66 dBFS and below is silence) */
        e = e < 0 ? 0 : e * vc.ng[b] >> 12;
        e = e > 65535 ? 65535 : e;
        acc += (bp * (e >> 1) >> 14) * (tw[b] >> 4) >> 8;   /* (< 2^31: 131071 * 4095) */
    }
    acc = acc * vc.level >> 4;                            /* makeup x8 at LEVEL 64 (the band sum is quiet) */
    acc += vc.idle >= 2205u ? 0 : sib;                    /* the consonants, while notes sound */
    return acc > 3000000 ? 3000000 : acc < -3000000 ? -3000000 : acc;   /* (-4000000 reads as a ROM address to the build check) */
}

/* the sibilance of the sample vc_in took last, for vc_track (the input side calls it after vc_in) */
static void vc_sib_push(void) { vc.sibq[vc.sqw++ & (VC_SQ - 1u)] = vc.sibv; }

/* one sample of the module's own vocoder: the modulator m (Q15, mono) in, the keys' synth the carrier; the output
 * (vocoder and dry by MIX) out */
static int32_t vc_sample(int32_t m)
{
    int32_t amax, car, y;
    m = vc_in(m);
    car = vc_carrier(&amax);
    y = vc_out(car, amax, vc.sibv);
    return (y * vc.mix + m * (127 - vc.mix)) * 258 >> 15;   /* (/ 127) */
}

/* an instrument's track as the carrier (the firmware: its voices' sum, after the block's render): b[0..n) replaced by
 * the vocoder's signal of it. The modulator's analysis runs elsewhere (vc_in, a block later: the sibilance through
 * vc.sibq); the noise follows the carrier's level (a peak follower) */
static void vc_track(int32_t *b, uint32_t n)
{
    uint32_t i;
    if (vc.sqw - vc.sqr > 2u * n || vc.sqw - vc.sqr > VC_SQ)   /* (behind after silent blocks: a block back) */
        vc.sqr = vc.sqw - n;
    for (i = 0; i < n; i++) {
        int32_t a = b[i] < 0 ? -b[i] : b[i];
        vc.tpk += (a > vc.tpk ? (a - vc.tpk) >> 4 : -(vc.tpk >> 11));   /* ~0.7 ms up, ~50 ms down */
        b[i] = vc_out(b[i] << 1, vc.tpk > 64 ? (vc.tpk > 16383 ? 32767 : vc.tpk << 1) : 0,
                      vc.sibq[vc.sqr++ & (VC_SQ - 1u)]);
    }
}
