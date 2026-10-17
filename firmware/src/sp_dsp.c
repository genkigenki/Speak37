/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37's sound: speech, played musically. Every key speaks a word at the key's pitch.
 * Fixed point, no float, no 64-bit divide in the audio path. No hardware: the platform comes through SP_KEYS() (the
 * 37 keys, bit k = key k), SP_VEL(k) and RING_PUBLISH() (the includer's; the host tests their own).
 *
 * The voice: an LPC decoder in the style of the TI TMS5220 (the Speak & Spell's family of chips). The decoder is our
 * own, after the structure of the chip as described by Talkie (Peter Knight; ArminJo's Talkie, GPL-3.0) and MAME's
 * tms5220 (BSD-3-Clause): an excitation (a pulse train, the chip's chirp, a glottal pulse or noise) through a
 * ten-stage lattice filter whose reflection coefficients K1..K10 come from the word's frames (tools/gen_vocab.py
 * encodes every word from espeak-ng's speech; no TI vocabulary is used).
 *   frames     10 ms of the word each (SPEED: 25 .. 400 %): energy, pitch (or unvoiced), K1..K10; K1..K10
 *              interpolated over the frame unless the voicing changes, the energy a step (1.4 ms ramp)
 *   pitch      the key's note (+ OCTAVE, pitch bend, vibrato, glide); the word's own intonation (its frames' pitch
 *              against its median, INTONE 0 .. 200 %; LAB AI: in semitone steps) on top; unvoiced frames stay noise
 *   formants   the lattice runs at 8 kHz x 2^(FORMANT / 12) (and LFO 2): the vocal tract scaled; its output is
 *              resampled to the I2S rate (linear, or held for the grit, LOFI)
 *   voices     8 (fewer when the CPU runs short), each speaking its own word; chords: keys within CHORD ms speak the
 *              same word
 *   per voice  de-emphasis (the encoder's pre-emphasis taken back), METAL (a comb tuned to the voice's pitch),
 *              the envelope (ATTACK, RELEASE, velocity), the pan (SPREAD: each voice slot its own place, so words
 *              played one after the other land left and right; the said words, the editor's and MOPE's, always
 *              in the centre)
 *   the bus    TONE (a tilt), DELAY (DIGITAL, TAPE, PING-PONG), REVERB (combs and all-passes), the master
 * The instrument: the sentence of the selected pad; each key-down speaks its next word, after the last the first
 * again (sp_key_on); LETTERS mode: each key a fixed letter or number (A..Z, ZERO..NINE); KIT mode: each key (and
 * pad) a speech-percussion hit (BOOM, TSS, KA ..: tight, one shot, by velocity); SPEAK: the sentence by itself;
 * MOPE mode: the keys play a tone (sp_mope_snd.c) and MOPE (sp_mope.c) comments on what they play, in words.
 * A sung syllable (SONGS, CHURCH) holds its vowel while its key is held and sings its last consonant when let go;
 * a hit is always short (no tail, no hold). CHOIR sings every note with three slightly detuned voices.
 * The transport: one clock of sixteenths (TEMPO, or the MIDI clock: 6 ticks a sixteenth, START from the bar) and
 * one bar for PLAY and RHYTHM; the one started while the other runs begins on the next bar.
 * PLAY: the sentence speaks itself in time, a word or syllable on the eighths, round and
 * round; the keys only choose its pitch (the keys held: all of them, a chord; none: the last). The USER bank's 16
 * sentences live in RAM (sp.user_*, the editor writes them, sp_user.c). Run sp_render per block of SP_BLK samples at the I2S rate (44,117.6 Hz). */
#ifndef RING_PUBLISH
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#endif
#include "sp_vocab.h"                      /* the words, the sentences, the chip's tables (tools/gen_vocab.py) */
#include "sp_rhythm.c"                     /* RHYTHM: the grooves, Felucca's drum voices */

#define SP_NV 12u                          /* voices (CHOIR: three a note) */
#define SP_BLK 32u                         /* samples a block */
#define SP_FS 44118u                       /* the I2S rate: 24 MHz / 544 */
#define SP_EAMP 24                         /* the excitation's scale: SP_ENERGY 114 -> 2736 */
#define SP_PREF 64                         /* the reference period (the encoder's P_REF) */
#define SP_COMBN 512u                      /* METAL's line (lattice samples) */
#define SP_DLN 32768u                      /* DELAY's line, frames (743 ms) */
#define SP_IDLE 4410u                      /* samples after the last voice: the bus rests (100 ms past the tails) */
#define SP_TAG_AUTO 0xFEu                  /* (a voice no key holds) */
#define SP_TAG_PLAY 0xFDu                  /* PLAY's voices */
#define SP_TAG_SAY 0xFCu                   /* the editor's words (sp_say) */
#define SP_TAG_TOUCH 0xFBu                 /* MOPE's little sounds when a control is touched (sp_mope_snd.c) */
#define SP_BANK_USER SP_NBANK              /* the USER bank: after the banks of banks.txt */
#define SP_NBANKS (SP_NBANK + 1u)
#define SP_NSENT (SP_NBANKS * 16u)         /* every pad of every bank */
#define SP_USENT (SP_NBANK * 16u)          /* the first USER sentence */
#define SP_UMAX 40u                        /* words a USER sentence (the display lays out 40) */
#define SP_NONE 0xFFu

/* the voices: what the VOICE knob loads */
enum { SP_CLASSIC, SP_VINTAGE, SP_DEEP, SP_CHILD, SP_WHISPER, SP_ROBOT, SP_LABAI, SP_GIANT, SP_CHOIR, SP_NVOICE };
static const char *const SP_VOICE_NAME[SP_NVOICE] = {"CLASSIC", "VINTAGE", "DEEP", "CHILD", "WHISPER", "ROBOT",
                                                     "LAB AI", "GIANT", "CHOIR"};
enum { SP_EX_PULSE, SP_EX_CHIRP, SP_EX_GLOT, SP_EX_NOISE };
enum { SP_HOLD_SHOT, SP_HOLD_GATE, SP_HOLD_HOLD, SP_HOLD_LOOP, SP_NHOLD };
static const char *const SP_HOLD_NAME[SP_NHOLD] = {"ONE SHOT", "GATE", "HOLD", "LOOP"};
enum { SP_DLY_DIGITAL, SP_DLY_TAPE, SP_DLY_PING, SP_NDLY };
static const char *const SP_DLY_NAME[SP_NDLY] = {"DIGITAL", "TAPE", "PING-PONG"};
enum { SP_MODE_SENT, SP_MODE_LETTERS, SP_MODE_KIT, SP_MODE_MOPE, SP_NMODE };
static const char *const SP_MODE_NAME[SP_NMODE] = {"SENTENCE", "LETTERS", "KIT", "MOPE"};

/* one sound: what a memory keeps (sp_state.c stores it byte by byte) */
typedef struct {
    uint8_t voice;                         /* SP_CLASSIC .. (the excitation, de-emphasis and steps come with it) */
    uint8_t speed;                         /* 0..127: 25 .. 400 % (64: 100 %) */
    uint8_t intone;                        /* 0..127: the word's own intonation 0 .. 200 % (64: as spoken) */
    uint8_t glide;                         /* 0..127: portamento 0 .. ~1 s */
    uint8_t root;                          /* the note LETTERS and SPEAK speak at, 36..84 */
    int8_t formant;                        /* -12..+12 semitones */
    uint8_t breath, lofi, metal;           /* 0..127 */
    uint8_t attack, release;               /* 0..127 */
    uint8_t hold;                          /* SP_HOLD_* */
    uint8_t velo;                          /* 0..127: velocity sensitivity */
    uint8_t vrate, vdepth;                 /* LFO 1: vibrato rate 0.5 .. 12 Hz, depth 0 .. 100 cents */
    uint8_t wrate, wdepth;                 /* LFO 2: the formants' wobble, 0.1 .. 10 Hz, 0 .. 6 semitones */
    uint8_t dtype, dtime, dfb, dmix;       /* DELAY: SP_DLY_*, 30 .. 740 ms, feedback, the send */
    uint8_t reverb, size, spread, tone;    /* REVERB's send, its size; SPREAD (stereo); TONE (64: flat) */
    int8_t oct;                            /* OCTAVE- / +: -2..+2 */
    uint8_t tempo;                         /* PLAY: 40 .. 240 BPM (0: 100) */
} sp_sound_t;

/* a voice's character: its excitation, de-emphasis (Q15 pole), intonation in semitone steps, and the knobs it
 * loads: SPEED INTONE FORMANT BREATH LOFI METAL ATTACK RELEASE VRATE VDEPTH; its voices a note (CHOIR: 3) and the
 * room it brings along (REVERB SIZE SPREAD; 255: as they are) */
typedef struct {
    uint8_t ex, steps;
    uint16_t deq;
    uint8_t speed, intone;
    int8_t formant;
    uint8_t breath, lofi, metal, attack, release, vrate, vdepth;
    uint8_t uni, rev, size, spread;
} sp_vdef_t;
static const sp_vdef_t SP_VDEF[SP_NVOICE] = {
    /* ex          steps deq    speed int  fmt br  lofi met att rel vr vd */
    /* ex          steps deq    speed int  fmt br  lofi met att rel vr  vd  uni rev  size spr */
    {SP_EX_PULSE,  0,    22000, 64,   56,  0,  6,  80,  0,  0,  30, 96, 0,  1,  255, 255, 255},   /* CLASSIC */
    {SP_EX_CHIRP,  0,    16000, 64,   64,  0,  0,  100, 0,  0,  24, 96, 0,  1,  255, 255, 255},   /* VINTAGE: the chirp */
    {SP_EX_PULSE,  0,    26000, 60,   40,  -3, 10, 40,  0,  4,  40, 90, 8,  1,  255, 255, 255},   /* DEEP */
    {SP_EX_PULSE,  0,    22000, 68,   84,  4,  8,  50,  0,  0,  30, 100, 10, 1, 255, 255, 255},   /* CHILD */
    {SP_EX_NOISE,  0,    20000, 60,   0,   0,  0,  40,  0,  6,  50, 96, 0,  1,  255, 255, 255},   /* WHISPER */
    {SP_EX_PULSE,  0,    18000, 62,   0,   0,  0,  90,  80, 0,  30, 96, 0,  1,  255, 255, 255},   /* ROBOT: monotone */
    {SP_EX_GLOT,   1,    8000,  62,   72,  2,  0,  0,   10, 4,  50, 96, 0,  1,  255, 255, 255},   /* LAB AI: steps (deq: its pulse is dark) */
    {SP_EX_PULSE,  0,    28000, 54,   30,  -6, 12, 30,  0,  8,  60, 80, 6,  1,  255, 255, 255},   /* GIANT */
    {SP_EX_GLOT,   0,    26000, 60,   20,  0,  24, 0,   0,  70, 80, 90, 16, 3,  96,  110, 110},   /* CHOIR: 3 a note */
};
/* the sound at the start: CHOIR in a big room (the first bank is SONGS) */
static const sp_sound_t SP_SOUND_DEF = {SP_CHOIR, 60, 20, 0, 60, 0, 24, 0, 0, 70, 80, SP_HOLD_HOLD, 64,
                                        90, 16, 40, 0, SP_DLY_DIGITAL, 50, 40, 0, 96, 110, 110, 64, 0, 100};

typedef struct {
    uint8_t on;                            /* sounding (0: free) */
    uint8_t tag;                           /* the key / MIDI note (+128) that started it, SP_TAG_AUTO, SP_NONE */
    uint8_t gate;                          /* its key held */
    uint8_t stop;                          /* the word ended (the stop frame): ringing out */
    uint8_t held;                          /* HOLD: frozen on the word's vowel (2: a sung syllable's, until let go) */
    uint8_t wflags;                        /* the word's SP_W_* */
    int16_t det;                           /* CHOIR: this voice's detune (1/64 semitones) */
    uint8_t note_i;                        /* PLAY: the note of the chord it sings (sp.pn) */
    uint32_t rmul;                         /* .. its formants' shift, Q16 of the lattice's rate */
    uint8_t tail;                          /* blocks since the word ended */
    uint8_t voiced, pv;                    /* the frame voiced; the frame before voiced */
    uint8_t lsb;                           /* (the noise's last bit) */
    uint16_t word;
    uint16_t frame;                        /* frames read */
    uint16_t pidx;                         /* the frame's period (SP_PERIOD index; contour) */
    uint8_t emax;                          /* the loudest energy index so far (the vowel to hold) */
    uint8_t nuc_ok;
    uint32_t bit;                          /* the next frame in SP_LPC */
    uint32_t fpos;                         /* output samples into the frame, Q8 */
    int32_t kf[10], kt[10];                /* K1..K10 Q15: from, to (the frame's) */
    int32_t ke[10];                        /* K1..K10 as interpolated now */
    int32_t ef, et;                        /* the excitation's amplitude: from, to */
    int32_t nuc_k[10], nuc_e;              /* the vowel to hold (HOLD) */
    uint16_t nuc_p;
    uint32_t ph, pinc;                     /* the pitch's phase (Q32 a period), its increment a lattice sample */
    int32_t amp_q12;                       /* the pulse's amplitude for this period: 2 sqrt(P / P_REF), Q12 */
    uint32_t cpos;                         /* the chirp's place, lattice samples since the pulse, Q8 */
    int32_t pend;                          /* the pulse's share for the next lattice sample */
    uint16_t nz;                           /* the noise's LFSR */
    int32_t x[10];                         /* the lattice's state */
    int32_t de;                            /* de-emphasis */
    int16_t comb[SP_COMBN];                /* METAL */
    uint16_t cw, cper;                     /* its write place, its delay (lattice samples) */
    int32_t y0, y1;                        /* the last two lattice outputs */
    uint32_t rph;                          /* the resampler's phase, Q16 lattice samples */
    int32_t pitch;                         /* the key's pitch now (1/64 semitones, glides) */
    int32_t pkey;                          /* .. its target */
    int32_t cont, ctgt;                    /* the intonation now, its target (1/64 semitones) */
    int32_t env;                           /* Q15 */
    int32_t gl, gr;                        /* the gains at the end of the last block (Q15) */
    uint8_t vel, slot;
    uint32_t age;
} sp_voice_t;

static struct {
    sp_sound_t snd;                        /* the main loop's: read by the ISR a block at a time */
    uint8_t mode;                          /* SP_MODE_* */
    uint8_t bend_range;                    /* semitones 1..12 (the wheel, MIDI) */
    uint8_t chord_ms;                      /* keys this close speak the same word, 0..100 ms */
    uint8_t lock;                          /* REPEAT: every key the word spoken last (the cursor stays) */
    volatile uint16_t req_sent;            /* the main loop asks for a sentence (bank * 16 + pad; 0xFFFF none) */
    volatile uint8_t req_restart;          /* .. to start the sentence again */
    volatile int8_t req_step;              /* .. to move the cursor a word (< >) */
    volatile uint8_t req_auto;             /* PLAY: 1 start, 2 stop */
    uint16_t sent;                         /* the sentence in use */
    uint16_t wpos;                         /* the next word's place in it */
    int16_t wlast;                         /* the word spoken last: its place (-1 none) */
    uint16_t wlast_id;
    uint32_t t_last;                       /* samples: the last word's start (CHORD) */
    uint32_t now;                          /* samples */
    uint8_t auto_on;                       /* PLAY running */
    uint8_t play_wait;                     /* PLAY starts on the next bar */
    uint8_t tr_on, tr_fresh, tr_pos;       /* the transport: running, its first step due, the sixteenth of the bar */
    uint32_t tr_t, tr_step;                /* samples into the sixteenth, sixteenths since it started */
    uint8_t groove;                        /* RHYTHM: the groove playing (1..RH_N, 0 none) */
    volatile uint8_t groove_req;           /* .. asked for (0xFF none; 0 stop: at once; a groove: on the bar) */
    volatile uint8_t fill_req;             /* FILL: the next bar's last beat */
    uint8_t fill_now;
    uint8_t drums, swing;                  /* the drums' level 0..127, the swing 0..100 */
    volatile uint8_t clk_sync;             /* MIDI START: the next tick the bar's first */
    uint32_t play_step;                    /* eighths since PLAY began */
    uint8_t play_left, play_rest;          /* eighths left of the word, the rest at the sentence's end */
    int32_t pn[6];                         /* PLAY's notes: the keys held (1/64 semitones), npn of them */
    uint8_t npn, play_vel;
    volatile uint32_t clk_ticks, t_clk;    /* MIDI clock: ticks (24 a quarter), when the last came */
    uint32_t clk_used;
    uint8_t user_n[16];                    /* the USER sentences: their words (SP_WORD ids; 0: empty) */
    uint16_t user_w[16][SP_UMAX];
    volatile uint16_t sayq[64];            /* the words to say (the editor's, MOPE's), one after the other: a word's id,
                                              its note << 12 (0: ROOT, else say_base + it - 8); SP_SAY_PAUSE: a pause
                                              of its << 12 x 50 ms */
    volatile uint8_t say_w, say_r;
    uint8_t say_base;                      /* (MOPE's note) */
    uint16_t say_gain;                     /* the said words' level, Q8 (MOPE: louder) */
    uint16_t touch_gain;                   /* MOPE's touch sounds' level, Q8 (quiet) */
    int16_t say_layer;                     /* (MOPE) a second voice for each word: 0 none, else its offset in 1/64
                                              semitones (-768 an octave down, a few: a unison a little apart) */
    uint32_t say_gap, say_wait;
    volatile uint8_t edit;                 /* the editor open: the keys type (kq), they play nothing */
    volatile uint8_t kq[16], kq_w;         /* the keys pressed while it is (the UI reads them, its own kq_r) */
    char typed[24];                        /* LETTERS: the last letters spoken (the screen) */
    uint8_t ntyped;
    int16_t bend;                          /* 1/64 semitones */
    uint8_t modw;                          /* 0..127 */
    uint32_t mheld[4];                     /* MIDI notes held */
    uint8_t mvel[128];
    uint32_t held[2];                      /* the keys as last seen */
    volatile uint8_t hitq[16], hitv[16];   /* KIT pads (the main loop): hit, velocity; a ring */
    volatile uint8_t hit_w, hit_r;
    uint32_t t_hit;                        /* (the screen: when the last hit came) */
    uint32_t lfo1, lfo2;                   /* the LFOs' phases */
    int32_t last_pitch;                    /* the last key's pitch (GLIDE starts there) */
    uint8_t vmax;                          /* voices allowed (the CPU guard) */
    uint32_t age, idle;
    sp_voice_t v[SP_NV];
    uint32_t speaking;                     /* bit i: voice i sounding (the screen) */
    uint32_t tag_words;                    /* (words started: the tests) */
    int32_t peak;                          /* the output's peak since the screen looked */
    int32_t vis_k[10];                     /* the loudest voice's K1..K10 now, its energy (the screen) */
    int32_t vis_e;
    /* the bus */
    int32_t lp1, lp2, lpa;                 /* the anti-image low-pass (Q15 coefficient) */
    uint8_t img_on;                        /* LOFI's grit (held samples): its images taken away (sp_img) */
    int32_t ibc[2][5];                     /* .. a 4th-order Butterworth: two biquads, b0 b1 b2 a1 a2 Q20 */
    int32_t ibz[2][2][4];                  /* .. their state: biquad, channel, x1 x2 y1 y2 */
    int32_t tl, tr, tlo, tro;              /* TONE */
    uint32_t dw, dtail;                    /* DELAY: write place; samples it still runs */
    int32_t dcur;                          /* its delay now, Q8 frames */
    int32_t dlp_l, dlp_r;                  /* TAPE: the feedback low-pass */
    uint32_t wow;
    int32_t hp_l, hp_r, hpx_l, hpx_r;      /* the DC blocker */
    uint32_t rvw[8], apw[4];               /* REVERB: write places */
    int32_t rvlp[8];
} sp;

static int16_t sp_dl[2u * SP_DLN] __attribute__((section(".pool")));      /* DELAY: L R interleaved */
#define SP_RVN 1700u
static int16_t sp_rv[8][SP_RVN] __attribute__((section(".pool")));       /* REVERB's combs */
static int16_t sp_ap[4][700] __attribute__((section(".pool")));          /* .. its all-passes */
static const uint16_t SP_RVLEN[8] = {1116, 1188, 1277, 1356, 1139, 1211, 1300, 1379};
static const uint16_t SP_APLEN[4] = {556, 441, 579, 464};

/* ------------------------------------------------------------------ maths --- */
static int32_t sp_clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t sp_sat16(int32_t v) { return v > 32767 ? 32767 : v < -32767 ? -32767 : v; }
/* a step of a one-pole glide over the distance d (coefficient c, Q15) that always arrives (no floor left over) */
static int32_t sp_toward(int32_t d, int32_t c)
{
    int32_t s = (int32_t)((int64_t)d * c >> 15);
    return s == 0 || (s < 0) != (d < 0) ? d : s;
}
/* 2^(i / 12), Q15 */
static const uint16_t SP_SEMI[12] = {32768, 34716, 36781, 38968, 41285, 43740, 46341, 49097, 52016, 55109, 58386, 61858};
/* 2^(k / 768) in Q16 - 65536: 1/64 semitone steps */
static const uint16_t SP_FINE[64] = {
    0, 59, 118, 178, 237, 296, 356, 415, 475, 535, 594, 654, 714, 773, 833, 893, 953, 1013, 1073, 1134, 1194,
    1254, 1314, 1375, 1435, 1496, 1556, 1617, 1677, 1738, 1799, 1859, 1920, 1981, 2042, 2103, 2164, 2225, 2287,
    2348, 2409, 2471, 2532, 2593, 2655, 2716, 2778, 2840, 2902, 2963, 3025, 3087, 3149, 3211, 3273, 3335, 3397,
    3460, 3522, 3584, 3647, 3709, 3772, 3834};
/* 2^(x / 768) in Q16 (x in 1/64 semitones), x -12288 .. 11519 */
static uint32_t sp_exp2(int32_t x)
{
    int32_t o, r;
    uint32_t m;
    x = sp_clampi(x, -12288, 11519);
    o = (x + 12288) / 768 - 16;            /* (floor) */
    r = x - o * 768;
    m = (uint32_t)SP_SEMI[r >> 6] * (65536u + SP_FINE[r & 63]) >> 15;
    return o >= 0 ? m << o : m >> -o;
}
/* sin(2 pi p / 2^32), Q15, a parabola pair (the LFOs) */
static int32_t sp_sin(uint32_t p)
{
    int32_t x = (int32_t)(p >> 16) - 32768, a = x < 0 ? -x : x;   /* -32768..32767, |x| */
    int32_t y = (int32_t)((int64_t)x * (32768 - a) >> 13);          /* 4 x (1 - |x|) */
    return -y;
}

/* ------------------------------------------------------------ the frames --- */
static uint32_t sp_bits(sp_voice_t *v, uint32_t n)
{
    uint32_t r = 0, p = v->bit;
    while (n--) {
        r = r << 1 | ((uint32_t)SP_LPC[p >> 3] >> (7u - (p & 7u)) & 1u);
        p++;
    }
    v->bit = p;
    return r;
}

/* the next frame: its targets; 0 at the stop frame */
static uint32_t sp_frame(sp_voice_t *v, const sp_vdef_t *vd)
{
    uint32_t e = sp_bits(v, 4), rep, p, i;
    int32_t et;
    for (i = 0; i < 10u; i++)                              /* the last frame's end: the new frame's start */
        v->kf[i] = v->kt[i];
    v->ef = v->et;
    v->pv = v->voiced;
    if (e == 15u)
        return 0;
    v->frame++;
    if (e == 0u) {                                         /* silence */
        v->et = 0;
        v->voiced = 0;
        return 1;
    }
    rep = sp_bits(v, 1);
    p = sp_bits(v, 6);
    if (!rep) {
        v->kt[0] = SP_K1[sp_bits(v, 5)];
        v->kt[1] = SP_K2[sp_bits(v, 5)];
        for (i = 2; i < 4u; i++)
            v->kt[i] = SP_K3_7[i - 2u][sp_bits(v, 4)];
        if (p) {
            for (i = 4; i < 7u; i++)
                v->kt[i] = SP_K3_7[i - 2u][sp_bits(v, 4)];
            for (i = 7; i < 10u; i++)
                v->kt[i] = SP_K8_10[i - 7u][sp_bits(v, 3)];
        } else
            for (i = 4; i < 10u; i++)
                v->kt[i] = 0;
    }
    v->voiced = p != 0u && vd->ex != SP_EX_NOISE;
    if (p)
        v->pidx = (uint16_t)p;
    et = (int32_t)(SP_ENERGY[e] * SP_EAMP * SP_WORD[v->word].gain >> 8);   /* (a hit: brought up) */
    if (p && vd->ex == SP_EX_NOISE)                        /* WHISPER: a voiced frame as quiet as it would be */
        et >>= 2;
    v->et = et;
    if (v->voiced && e + 3u >= v->emax) {                  /* the vowel to hold: a loud voiced frame, the latest */
        if (e > v->emax)
            v->emax = (uint8_t)e;
        for (i = 0; i < 10u; i++)
            v->nuc_k[i] = v->kt[i];
        v->nuc_e = et;
        v->nuc_p = (uint16_t)p;
        v->nuc_ok = 1;
    }
    if (v->voiced != v->pv || !v->ef || !et)               /* the voicing changes, silence: no interpolation */
        for (i = 0; i < 10u; i++)
            v->kf[i] = v->kt[i];
    return 1;
}

static void sp_word_start(sp_voice_t *v, uint32_t w)
{
    uint32_t i;
    v->word = (uint16_t)w;
    v->bit = SP_WORD[w].off * 8u;
    v->frame = 0;
    v->stop = 0;
    v->held = 0;
    v->tail = 0;
    v->emax = 0;
    v->nuc_ok = 0;
    v->fpos = 0;
    v->et = 0;
    v->voiced = 0;
    v->pidx = SP_WORD[w].p0;
    v->wflags = SP_WORD[w].flags;
    for (i = 0; i < 10u; i++)
        v->kt[i] = 0;
}

/* --------------------------------------------------------------- voices --- */
static uint32_t sp_alloc(void)
{
    uint32_t i, best = 0, n = sp.vmax > SP_NV ? SP_NV : sp.vmax ? sp.vmax : 1u;
    int32_t lo = 0x7FFFFFFF;
    for (i = 0; i < n; i++) {                              /* a free voice, else a released one, else the oldest */
        int32_t a = !sp.v[i].on ? -1 : sp.v[i].gate ? 0x20000000 - (int32_t)(sp.age - sp.v[i].age) :
                    0x10000000 - (int32_t)(sp.age - sp.v[i].age);
        if (a < lo) {
            lo = a;
            best = i;
        }
    }
    return best;
}

/* one voice speaking word w (sp_speak): detune det (1/64 semitones), its formants' shift rm (Q16) */
static void sp_speak1(uint32_t w, int32_t pitch, uint32_t vel, uint32_t tag, int32_t det, uint32_t rm, uint32_t skew)
{
    uint32_t i = sp_alloc(), k;
    sp_voice_t *v = &sp.v[i];
    k = v->on && v->env > 64;                              /* (a stolen voice: no click: its lattice state kept) */
    sp_word_start(v, w);
    v->on = 1;
    v->gate = 1;
    v->tag = (uint8_t)tag;
    v->vel = (uint8_t)(vel > 127u ? 127u : vel);
    v->age = ++sp.age;
    v->pkey = pitch;
    v->pitch = sp.snd.glide && sp.last_pitch ? sp.last_pitch : pitch;
    v->cont = v->ctgt = 0;
    if (!k) {
        v->env = 0;
        v->gl = v->gr = 0;
        v->y0 = v->y1 = 0;
        v->de = 0;
        v->pend = 0;
        for (k = 0; k < 10u; k++)
            v->x[k] = 0;
        for (k = 0; k < SP_COMBN; k++)
            v->comb[k] = 0;
    }
    if (!v->nz)
        v->nz = (uint16_t)(0xACE1u + i * 977u);
    v->ph = 0xF0000000u - skew * 0x10000000u;              /* (the first pulse soon; the unison's not at once) */
    v->det = (int16_t)det;
    v->rmul = rm;
    v->fpos = skew * 64u * 256u;                           /* (CHOIR: the voices a little apart in time) */
}

/* speak word w at pitch (1/64 semitones), velocity vel, started by tag; CHOIR: three voices, a hit: one */
static void sp_speak(uint32_t w, int32_t pitch, uint32_t vel, uint32_t tag)
{
    static const int16_t DET[3] = {0, 9, -9};
    static const uint32_t RM[3] = {65536u, 66500u, 64600u};
    uint32_t u, n = SP_VDEF[sp.snd.voice % SP_NVOICE].uni;
    if (w >= SP_NWORD)
        return;
    if (SP_WORD[w].flags & SP_W_PERC || sp.vmax < 6u)
        n = 1;
    for (u = 0; u < n; u++)
        sp_speak1(w, pitch, vel, tag, DET[u], RM[u], u);
    sp.last_pitch = pitch;
    sp.tag_words++;
}

static void sp_release(uint32_t tag)
{
    uint32_t i;
    for (i = 0; i < SP_NV; i++)
        if (sp.v[i].on && sp.v[i].tag == tag)
            sp.v[i].gate = 0;
}

#include "sp_mope_snd.c"                   /* MOPE's tones, its melody, the keys' events for it */

/* --------------------------------------------------------- the sentence --- */
static uint32_t sp_sent_len(uint32_t s)
{
    s %= SP_NSENT;
    return s >= SP_USENT ? sp.user_n[s - SP_USENT] : (uint32_t)(SP_SENT_AT[s + 1u] - SP_SENT_AT[s]);
}
static uint32_t sp_sent_word(uint32_t s, uint32_t i)
{
    uint32_t n = sp_sent_len(s);
    s %= SP_NSENT;
    if (!n)
        return 0;
    return s >= SP_USENT ? sp.user_w[s - SP_USENT][i % n] % SP_NWORD : SP_SENT_W[SP_SENT_AT[s] + i % n];
}

/* LETTERS: key k (0..36) -> its word: A..Z, then ZERO..NINE, from the lowest key; then again */
static uint32_t sp_letter_word(uint32_t k) { return k % 36u; }

/* the screen's line of LETTERS / KIT: s appended (the oldest characters drop off) */
static void sp_typed_add(const char *s)
{
    uint32_t n = sp.ntyped;
    for (; *s; s++) {
        if (n >= sizeof sp.typed - 1u) {
            uint32_t j;
            for (j = 0; j + 1u < n; j++)
                sp.typed[j] = sp.typed[j + 1u];
            n--;
        }
        sp.typed[n++] = *s;
    }
    sp.typed[n] = 0;
    sp.ntyped = (uint8_t)n;
}

/* a key or MIDI note down: the instrument decides the word */
static void sp_key_on(uint32_t key, int32_t pitch, uint32_t vel, uint32_t tag)
{
    uint32_t w;
    if (sp.mode == SP_MODE_KIT) {                          /* KIT: hit k of the kit, at its note */
        w = SP_KIT[key % SP_NKIT];
        sp_speak(w, (int32_t)SP_KIT_NOTE[key % SP_NKIT] * 64 + pitch, vel, tag);
        if (sp.ntyped)
            sp_typed_add(" ");
        sp_typed_add(SP_WNAME[w]);
        sp.wlast_id = (uint16_t)w;
        sp.t_hit = sp.now;
        return;
    }
    if (sp.mode == SP_MODE_LETTERS) {
        w = sp_letter_word(key);
        sp_speak(w, (int32_t)sp.snd.root * 64 + pitch, vel, tag);
        {   /* the screen: the letters typed, the newest at the end (a number as its digit) */
            char c = w < 26u ? (char)('A' + w) : (char)('0' + (w - 26u));
            uint32_t n = sp.ntyped;
            if (n >= sizeof sp.typed - 1u) {
                uint32_t j;
                for (j = 0; j + 1u < n; j++)
                    sp.typed[j] = sp.typed[j + 1u];
                n--;
            }
            sp.typed[n++] = c;
            sp.typed[n] = 0;
            sp.ntyped = (uint8_t)n;
        }
        sp.wlast_id = (uint16_t)w;
        return;
    }
    if (!sp_sent_len(sp.sent))
        return;                                            /* (an empty USER pad) */
    if (sp.wlast >= 0 && tag != SP_TAG_AUTO && tag != SP_TAG_PLAY &&
        (sp.lock || sp.now - sp.t_last < (uint32_t)sp.chord_ms * 44u)) {
        w = sp_sent_word(sp.sent, (uint32_t)sp.wlast);     /* a chord (or REPEAT): the same word */
        sp.t_last = sp.now;
    } else {
        sp.wlast = (int16_t)(sp.wpos % sp_sent_len(sp.sent));
        w = sp_sent_word(sp.sent, (uint32_t)sp.wlast);
        sp.wpos = (uint16_t)((sp.wlast + 1) % (int32_t)sp_sent_len(sp.sent));
        sp.t_last = sp.now;
    }
    sp.wlast_id = (uint16_t)w;
    sp_speak(w, pitch, vel, tag);
}

/* the main loop's requests, taken at a block's start */
static void sp_requests(void)
{
    uint16_t r = sp.req_sent;
    RING_PUBLISH();
    if (r != 0xFFFFu) {
        sp.req_sent = 0xFFFFu;
        if (r < SP_NSENT) {
            sp.sent = r;
            sp.wpos = 0;
            sp.wlast = -1;
            sp.auto_on = 0;
        }
    }
    if (sp.req_restart) {
        sp.req_restart = 0;
        sp.wpos = 0;
        sp.wlast = -1;
    }
    if (sp.req_step && !sp_sent_len(sp.sent))
        sp.req_step = 0;
    if (sp.req_step) {
        int32_t n = (int32_t)sp_sent_len(sp.sent), p = (int32_t)sp.wpos + sp.req_step;
        sp.req_step = 0;
        p = ((p % n) + n) % n;
        sp.wpos = (uint16_t)p;
        sp.wlast = (int16_t)((p + n - 1) % n);
        sp.wlast_id = (uint16_t)sp_sent_word(sp.sent, (uint32_t)sp.wlast);
    }
    while (sp.hit_r != sp.hit_w) {                        /* KIT pads: a hit each, one shot */
        uint32_t k = sp.hitq[sp.hit_r % 16u], v = sp.hitv[sp.hit_r % 16u];
        uint8_t m = sp.mode;
        RING_PUBLISH();
        sp.hit_r++;
        sp.mode = SP_MODE_KIT;
        sp_key_on(k, 12 * 64 * sp.snd.oct, v, 200u + (k & 15u));
        sp.mode = m;
        sp_release(200u + (k & 15u));
    }
    if (sp.req_auto) {
        if (sp.req_auto == 1u) {
            sp.play_step = 0;
            sp.play_left = 0;
            sp.play_rest = 0;
            sp.wpos = 0, sp.wlast = -1;
            if (!sp.npn)
                sp.pn[0] = (int32_t)sp.snd.root * 64, sp.npn = 1, sp.play_vel = 100;
            if (sp.tr_on)
                sp.play_wait = 1;                          /* (the drums run: on the next bar) */
            else
                sp.auto_on = 1;
        } else {
            sp.auto_on = 0;
            sp.play_wait = 0;
            sp_release(SP_TAG_PLAY);
        }
        sp.req_auto = 0;
    }
    if (sp.groove_req != 0xFFu) {
        if (sp.groove_req == 0u || !sp.tr_on) {            /* (a stop at once; a groove: now, or on the bar) */
            sp.groove = sp.groove_req <= RH_N ? sp.groove_req : 0u;
            sp.groove_req = 0xFFu;
            if (sp.groove)
                rh_kit(sp.groove - 1u);
        }
    }
}

/* the transport's lengths at the tempo: a quarter, an eighth (a word's measure), a step of the bar (a sixteenth, a
 * triplet's third in a bar of beats of 3; SWING: the even sixteenths longer, the odd ones shorter, up to 2:1) */
static uint32_t sp_quarter(void)
{
    uint32_t bpm = sp.snd.tempo ? sp.snd.tempo : 100u;
    return SP_FS * 60u / (bpm < 40u ? 40u : bpm > 240u ? 240u : bpm);
}
static uint32_t sp_eighth(void) { return sp_quarter() / 2u; }
static uint32_t sp_beat(void) { return sp.groove ? RH_GROOVE[sp.groove - 1u].beat : 4u; }
static uint32_t sp_bar(void) { return sp.groove ? RH_GROOVE[sp.groove - 1u].steps : 16u; }
static uint32_t sp_steplen(uint32_t pos)
{
    uint32_t b = sp_beat(), e = sp_quarter() / b, d;
    if (b != 4u || !sp.swing)
        return e;
    d = e * (sp.swing > 100u ? 100u : sp.swing) / 300u;
    return pos & 1u ? e - d : e + d;
}
/* PLAY: the length of a word in eighths at the tempo: a sung syllable a beat, a hit an eighth, a word its own length
 * (at SPEED) rounded up, 1 .. 8 */
static uint32_t sp_play_len(uint32_t w)
{
    uint32_t e = sp_eighth(), ms, n;
    if (SP_WORD[w].flags & SP_W_SING)
        return 2;
    if (SP_WORD[w].flags & SP_W_PERC)
        return 1;
    ms = (uint32_t)((uint64_t)SP_WORD[w].nfr * 441u * sp_exp2(-((int32_t)sp.snd.speed - 64) * 24) >> 16);   /* samples */
    n = (ms + e * 3u / 4u) / e;                            /* (a word a little longer than an eighth: still one) */
    return n < 1u ? 1u : n > 8u ? 8u : n;
}

/* PLAY: the next word, sung at every note of the chord (PLAY's notes) */
static void sp_play_word(void)
{
    uint32_t i, j, w, n = sp_sent_len(sp.sent);
    sp_release(SP_TAG_PLAY);
    if (!n) {
        sp.play_left = 2;
        return;
    }
    if (sp.wlast >= 0 && sp.wpos == 0u && !sp.play_rest) { /* the sentence said: a beat's rest, then again */
        sp.play_rest = 1;
        sp.play_left = 2;
        return;
    }
    sp.play_rest = 0;
    sp.wlast = (int16_t)(sp.wpos % n);
    w = sp_sent_word(sp.sent, (uint32_t)sp.wlast);
    sp.wpos = (uint16_t)((sp.wlast + 1) % (int32_t)n);
    sp.wlast_id = (uint16_t)w;
    for (j = 0; j < (sp.npn ? sp.npn : 1u); j++) {
        uint32_t a0 = sp.age;
        sp_speak(w, sp.npn ? sp.pn[j] : (int32_t)sp.snd.root * 64, sp.play_vel ? sp.play_vel : 100u, SP_TAG_PLAY);
        for (i = 0; i < SP_NV; i++)
            if (sp.v[i].tag == SP_TAG_PLAY && sp.v[i].age > a0)
                sp.v[i].note_i = (uint8_t)j;
    }
    sp.play_left = (uint8_t)sp_play_len(w);
}

/* PLAY: the keys held chose the notes; the word sounding follows at once */
static void sp_play_notes(void)
{
    uint32_t j, b, n = 0, i;
    int32_t pn[6];
    for (j = 0; j < 2u && n < 6u; j++)
        for (b = 0; b < 32u && n < 6u; b++)
            if (sp.held[j] >> b & 1u)
                pn[n++] = (48 + (int32_t)(j * 32u + b) + 12 * sp.snd.oct) * 64;
    for (j = 0; j < 4u && n < 6u; j++)
        for (b = 0; b < 32u && n < 6u; b++)
            if (sp.mheld[j] >> b & 1u)
                pn[n++] = (int32_t)(j * 32u + b) * 64 + 12 * 64 * sp.snd.oct;
    if (!n)
        return;                                            /* (none held: the last notes stay) */
    for (i = 0; i < n; i++)
        sp.pn[i] = pn[i];
    sp.npn = (uint8_t)n;
    for (i = 0; i < SP_NV; i++)
        if (sp.v[i].on && sp.v[i].tag == SP_TAG_PLAY && !sp.v[i].stop)
            sp.v[i].pkey = sp.pn[sp.v[i].note_i % n];
}

/* a step of the transport: on the bar's first, what waits for it (a groove, PLAY, the fill); the groove's hits;
 * PLAY's eighths (the even steps of a beat: of 4 the eighths, of 3 the swung ones) */
static void sp_tr_tick(void)
{
    uint32_t p = sp.tr_pos;
    if (p == 0u) {
        if (sp.groove_req != 0xFFu) {
            sp.groove = sp.groove_req <= RH_N ? sp.groove_req : 0u;
            sp.groove_req = 0xFFu;
        }
        if (sp.play_wait)
            sp.play_wait = 0, sp.auto_on = 1;
        sp.fill_now = sp.fill_req;
        sp.fill_req = 0;
    }
    if (sp.groove)
        rh_step(sp.groove - 1u, p, sp.fill_now);
    if (sp.auto_on && (p % sp_beat() == 0u || p % sp_beat() == 2u)) {
        sp.play_step++;
        if (sp.play_left)
            sp.play_left--;
        if (!sp.play_left)
            sp_play_word();
    }
}

/* the transport: a step at the tempo, or every 24 / beat ticks of a MIDI clock while one comes; it runs while PLAY
 * or a groove does */
static void sp_play(void)
{
    uint32_t steps = 0, tpq;
    if (sp.auto_on && sp.mode != SP_MODE_SENT)
        sp.auto_on = 0, sp_release(SP_TAG_PLAY);
    if (!sp.auto_on && !sp.play_wait && !sp.groove && sp.groove_req == 0xFFu) {
        sp.tr_on = 0;
        sp.clk_used = sp.clk_ticks;
        return;
    }
    if (!sp.tr_on) {
        sp.tr_on = 1;
        sp.tr_fresh = 1;
        sp.tr_t = 0;
        sp.tr_step = 0;
        sp.tr_pos = 0;
        sp.clk_used = sp.clk_ticks;
    }
    tpq = 24u / sp_beat();
    if (sp.now - sp.t_clk < 22059u && sp.clk_ticks != sp.clk_used) {
        if (sp.clk_sync) {                                 /* (MIDI START: from the bar's first) */
            sp.clk_sync = 0;
            sp.tr_fresh = 1;
            sp.tr_pos = 0;
            sp.clk_used = sp.clk_ticks - 1u;
        }
        while (sp.clk_ticks - sp.clk_used >= tpq) {
            sp.clk_used += tpq;
            steps++;
        }
        sp.tr_t = 0;
    } else {
        sp.clk_used = sp.clk_ticks;
        sp.tr_t += SP_BLK;
        while (sp.tr_t >= sp_steplen(sp.tr_pos)) {
            sp.tr_t -= sp_steplen(sp.tr_pos);
            steps++;
        }
    }
    if (sp.tr_fresh) {
        sp.tr_fresh = 0;
        sp_tr_tick();
        steps = steps ? steps - 1u : 0u;
    }
    while (steps--) {
        sp.tr_step++;
        sp.tr_pos = (uint8_t)((sp.tr_pos + 1u) % sp_bar());
        sp_tr_tick();
    }
}

/* the editor's words (at ROOT) and MOPE's (at their notes): said one after the other (40 ms apart, a pause longer) */
#define SP_SAY_PAUSE 0xFFFu
static void sp_say_run(void)
{
    uint32_t i, busy = 0;
    if (sp.say_wait) {
        sp.say_wait = sp.say_wait > SP_BLK ? sp.say_wait - SP_BLK : 0u;
        return;
    }
    if (sp.say_r == sp.say_w)
        return;
    for (i = 0; i < SP_NV; i++)
        busy |= sp.v[i].on && sp.v[i].tag == SP_TAG_SAY && !sp.v[i].stop;
    if (busy) {
        sp.say_gap = 0;
        return;
    }
    if ((sp.say_gap += SP_BLK) < (sp.sayq[sp.say_r % 64u] >> 12 ? 220u : 1764u))   /* (MOPE's: 5 ms, in one go) */
        return;
    sp.say_gap = 0;
    {
        uint32_t e = sp.sayq[sp.say_r % 64u], w = e & 0xFFFu, nt = e >> 12;
        RING_PUBLISH();
        sp.say_r++;
        if (w == SP_SAY_PAUSE) {
            sp.say_wait = nt * 2205u;
            return;
        }
        int32_t pitch = (nt ? (int32_t)sp.say_base + (int32_t)nt - 8 : (int32_t)sp.snd.root) * 64;
        sp_speak(w % SP_NWORD, pitch, 110, SP_TAG_SAY);
        if (nt && sp.say_layer)                            /* (MOPE: its second voice) */
            sp_speak(w % SP_NWORD, pitch + sp.say_layer, 90, SP_TAG_SAY);
        sp_release(SP_TAG_SAY);
    }
}

/* a USB-MIDI event (any channel): notes, pitch bend, CC1, program change (a sentence), CC123 all off */
static void sp_midi(uint32_t pkt)
{
    uint32_t st = (pkt >> 8) & 0xF0u, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu, w = d1 >> 5, bit = 1u << (d1 & 31u);
    if (st == 0x90u && d2) {
        sp.mheld[w] |= bit;
        sp.mvel[d1] = (uint8_t)d2;
    } else if (st == 0x80u || st == 0x90u) {
        sp.mheld[w] &= ~bit;
    } else if (st == 0xE0u) {
        sp.bend = (int16_t)(((int32_t)(d1 | d2 << 7) - 8192) * sp.bend_range / 128);
    } else if (st == 0xB0u && d1 == 1u) {
        sp.modw = (uint8_t)d2;
    } else if (st == 0xB0u && (d1 == 123u || d1 == 120u)) {
        sp.mheld[0] = sp.mheld[1] = sp.mheld[2] = sp.mheld[3] = 0;
    } else if (st == 0xC0u) {
        sp.req_sent = (uint16_t)(d1 % SP_NSENT);
    } else if ((pkt & 0xFFFFu) == 0xF80Fu) {               /* MIDI clock (a single byte: CIN 0xF) */
        sp.clk_ticks++;
        sp.t_clk = sp.now;
    } else if ((pkt & 0xFFFFu) == 0xFA0Fu || (pkt & 0xFFFFu) == 0xFB0Fu) {   /* START, CONTINUE: from the bar */
        sp.clk_sync = 1;
    }
}

/* the keys and MIDI notes: changes since the last block, in order */
static void sp_track_keys(void)
{
    uint64_t keys = SP_KEYS();
    uint32_t j, h[2] = {(uint32_t)keys, (uint32_t)(keys >> 32) & 0x1Fu};
    static uint32_t mprev[4];
    for (j = 0; j < 2u; j++) {
        uint32_t ch = h[j] ^ sp.held[j];
        while (ch) {
            uint32_t bit = ch & (0u - ch), k = j * 32u, q = bit;
            while (q >>= 1)
                k++;
            if (sp.edit) {                                 /* the editor: a key a letter */
                if (h[j] & bit) {
                    sp.kq[sp.kq_w % 16u] = (uint8_t)k;
                    RING_PUBLISH();
                    sp.kq_w++;
                }
            } else if (sp.mode == SP_MODE_MOPE) {          /* MOPE: a tone; the listener hears it */
                uint32_t n = (uint32_t)sp_clampi(48 + (int32_t)k + 12 * sp.snd.oct, 0, 127);
                if (h[j] & bit) {
                    mos.knote[k % 37u] = (uint8_t)n;
                    mos_on(n, SP_VEL(k), k, MO_TK_KEYS);
                } else {
                    n = mos.knote[k % 37u];                /* (its up: the note its down had) */
                    mos_off(k);
                }
                mos_ev(n, h[j] & bit ? (SP_VEL(k) ? SP_VEL(k) : 1u) : 0u, k);
            } else if (sp.auto_on && sp.mode == SP_MODE_SENT) {   /* PLAY: a key only chooses the pitch */
                if (h[j] & bit)
                    sp.play_vel = (uint8_t)SP_VEL(k);
            } else if (h[j] & bit)                         /* (LETTERS, KIT: their own notes; else the key's) */
                sp_key_on(k, sp.mode != SP_MODE_SENT ? 12 * 64 * sp.snd.oct :
                          (48 + (int32_t)k + 12 * sp.snd.oct) * 64, SP_VEL(k), k);
            else
                sp_release(k);
            ch &= ch - 1u;
        }
        sp.held[j] = h[j];
    }
    for (j = 0; j < 4u; j++) {                             /* MIDI: the note itself */
        uint32_t ch = sp.mheld[j] ^ mprev[j];
        while (ch) {
            uint32_t bit = ch & (0u - ch), n = j * 32u, q = bit;
            while (q >>= 1)
                n++;
            if (sp.edit) {
                if (sp.mheld[j] & bit && n >= 48u) {
                    sp.kq[sp.kq_w % 16u] = (uint8_t)(n - 48u);
                    RING_PUBLISH();
                    sp.kq_w++;
                }
            } else if (sp.mode == SP_MODE_MOPE) {
                uint32_t nn = (uint32_t)sp_clampi((int32_t)n + 12 * sp.snd.oct, 0, 127);
                if (sp.mheld[j] & bit) {
                    mos.mnote[n & 127u] = (uint8_t)nn;
                    mos_on(nn, sp.mvel[n], 64u + n, MO_TK_KEYS);
                } else {
                    nn = mos.mnote[n & 127u];
                    mos_off(64u + n);
                }
                mos_ev(nn, sp.mheld[j] & bit ? (sp.mvel[n] ? sp.mvel[n] : 1u) : 0u, 0xFFu);
            } else if (sp.auto_on && sp.mode == SP_MODE_SENT) {
                if (sp.mheld[j] & bit)
                    sp.play_vel = sp.mvel[n];
            } else if (sp.mheld[j] & bit) {
                uint32_t k = n >= 48u ? n - 48u : n % 12u;
                sp_key_on(k, sp.mode != SP_MODE_SENT ? 12 * 64 * sp.snd.oct : (int32_t)n * 64 + 12 * 64 * sp.snd.oct,
                          sp.mvel[n], 64u + n);
            } else
                sp_release(64u + n);
            ch &= ch - 1u;
        }
        mprev[j] = sp.mheld[j];
    }
    if (sp.auto_on && sp.mode == SP_MODE_SENT && !sp.edit)
        sp_play_notes();
}

/* ------------------------------------------------------------ rendering --- */
/* SPEED: the frame's length in output samples, Q8 (10 ms at 100 %) */
static uint32_t sp_flen(void)
{
    int32_t s = sp.snd.speed;                              /* 0..127: 2^((s - 64) / 32): 25 % .. 400 % */
    return (uint32_t)((uint64_t)112896u * sp_exp2(-(s - 64) * 24) >> 16);   /* (441 x 256) */
}

typedef struct {
    uint32_t flen;                         /* Q8 output samples a frame */
    int32_t fs;                            /* the formant shift (1/64 semitones) */
    uint32_t rinc;                         /* lattice samples an output sample, Q16 */
    int32_t breath, deq, deg, mfb;
    uint32_t crush;                        /* LOFI: the bits kept away (mask), the held output */
    uint8_t hold_out, ex, steps;
    int32_t vib;                           /* 1/64 semitones now */
    int32_t gattack, grelease;             /* the envelope's steps a block (Q15) */
    int32_t glide;                         /* the glide's coefficient a block (Q15) */
} sp_blk_t;

static void sp_block_setup(sp_blk_t *b)
{
    const sp_sound_t *s = &sp.snd;
    const sp_vdef_t *vd = &SP_VDEF[s->voice % SP_NVOICE];
    int32_t w, vd_cents, mw;
    b->flen = sp_flen();
    sp.lfo2 += sp_exp2(((int32_t)s->wrate - 64) * 40) * 48u;                 /* 0.1 .. 10 Hz (1 Hz at 64) */
    w = sp_sin(sp.lfo2) * ((int32_t)s->wdepth * 6 * 64 / 127) >> 15;        /* +-6 semitones */
    b->fs = (int32_t)s->formant * 64 + w;
    b->rinc = (uint32_t)((uint64_t)11884u * sp_exp2(b->fs) >> 16);
    if (b->rinc > 32768u)
        b->rinc = 32768u;                                  /* (the lattice at most at half the output rate) */
    b->ex = vd->ex;
    b->steps = vd->steps;
    b->breath = s->breath;
    b->deq = vd->deq;
    b->deg = 4096 * (32768 - b->deq) / 16384;               /* (the de-emphasis' gain at the voice's band, ~1) */
    b->mfb = s->metal * 230;                               /* METAL: up to 0.9 */
    b->hold_out = s->lofi >= 64u;
    b->crush = s->lofi >= 80u ? ~((1u << (4u + (s->lofi - 80u) / 8u)) - 1u) : ~0u;   /* 16 -> 10 bits */
    sp.lfo1 += sp_exp2((int32_t)s->vrate * 28) * 24u;      /* vibrato 0.5 .. 12 Hz (96: 5.7 Hz) */
    mw = sp.modw;
    vd_cents = (int32_t)s->vdepth * 64 / 127 + (mw * (254 - mw) >> 7) * 96 / 127;   /* (the wheel: up to 1.5) */
    b->vib = sp_sin(sp.lfo1) * vd_cents >> 15;
    b->gattack = s->attack ? 32767 * (int32_t)SP_BLK / (1 + (int32_t)s->attack * s->attack * 2) : 32767;
    b->grelease = 32767 * (int32_t)SP_BLK / (200 + (int32_t)s->release * s->release * 6);
    b->glide = s->glide ? 32767 * (int32_t)SP_BLK / (30 + (int32_t)s->glide * s->glide * 3) : 32767;
}

/* one lattice sample of voice v (the frame's K interpolated at the block's start) */
static inline int32_t sp_lattice(sp_voice_t *v, const sp_blk_t *b, int32_t e)
{
    int32_t exc, u, i;
    uint32_t ph0 = v->ph;
    if (v->voiced && e) {
        v->ph += v->pinc;
        if (b->ex == SP_EX_GLOT) {
            exc = (int32_t)((int64_t)e * SP_GLOT[v->ph >> 24] >> 15);
        } else if (b->ex == SP_EX_CHIRP) {
            if (v->ph < ph0)                               /* a new period: the chirp from the pulse's place */
                v->cpos = v->ph / ((v->pinc >> 8) + 1u);   /* (Q8 lattice samples since the pulse) */
            else
                v->cpos += 256u;
            {
                uint32_t ci = v->cpos >> 8, f = v->cpos & 255u;
                int32_t c0 = ci < sizeof SP_CHIRP ? SP_CHIRP[ci] : 0, c1 = ci + 1u < sizeof SP_CHIRP ? SP_CHIRP[ci + 1u] : 0;
                int32_t c = c0 * 256 + (c1 - c0) * (int32_t)f;     /* Q8 */
                exc = (int32_t)((int64_t)c * e * v->amp_q12 >> 28);   /* (chirp / 128, Q12 / 2: as the pulse) */
            }
        } else {                                           /* the pulse at its fractional place (one sample late) */
            exc = v->pend;
            v->pend = 0;
            if (v->ph < ph0) {
                int32_t a = (int32_t)((int64_t)e * v->amp_q12 >> 12);
                int32_t fr = (int32_t)(((v->ph >> 8) << 7) / ((v->pinc >> 8) + 1u));   /* Q15: how far past */
                exc += (int32_t)((int64_t)a * fr >> 15);
                v->pend = a - (int32_t)((int64_t)a * fr >> 15);
            }
        }
        if (b->breath) {
            v->nz = (uint16_t)((v->nz >> 1) ^ (v->nz & 1u ? 0xB400u : 0u));
            exc += (v->nz & 1u ? e : -e) * b->breath >> 9;
        }
    } else {
        v->nz = (uint16_t)((v->nz >> 1) ^ (v->nz & 1u ? 0xB400u : 0u));
        exc = v->nz & 1u ? e : -e;
        v->pend = 0;
    }
    u = exc;
    for (i = 9; i >= 0; i--) {                             /* the lattice (TMS5220's), K Q15 */
        u -= (int32_t)((int64_t)v->ke[i] * v->x[i] >> 15);
        if (i < 9)
            v->x[i + 1] = sp_sat16(v->x[i] + (int32_t)((int64_t)v->ke[i] * u >> 15));
    }
    u = sp_sat16(u);
    v->x[0] = u;
    v->de = sp_sat16(u + (int32_t)((int64_t)b->deq * v->de >> 15));   /* de-emphasis */
    u = v->de * b->deg >> 12;
    if (b->mfb) {                                          /* METAL: a comb at the voice's period */
        uint32_t r = (v->cw - v->cper) & (SP_COMBN - 1u);
        u = sp_sat16(u + (b->mfb * v->comb[r] >> 15));
        v->comb[v->cw] = (int16_t)u;
        v->cw = (uint16_t)((v->cw + 1u) & (SP_COMBN - 1u));
    }
    return (int32_t)((uint32_t)sp_sat16(u) & b->crush);
}

/* a voice's block: frames, pitch, envelope, the lattice resampled, panned into l / r */
static void sp_voice_block(sp_voice_t *v, uint32_t vi, const sp_blk_t *b, int32_t *l, int32_t *r)
{
    static const int16_t PAN[SP_NV] = {-24576, 24576, -12288, 12288, -32767, 32767, -6144, 6144, -18432, 18432,
                                       -28672, 28672};
    const sp_sound_t *s = &sp.snd;
    uint32_t i, t, n = SP_BLK;
    int32_t e, gl, gr, dl, dr, tgt, pan, L, sh = 0;
    /* the frames (a hit at its own speed: tight) */
    const uint32_t flen = v->wflags & SP_W_PERC ? 112896u : b->flen,
                   hold = v->wflags || v->tag >= SP_TAG_TOUCH ? SP_HOLD_SHOT : s->hold;   /* (PLAY, the editor: once) */
    if (v->held == 2u && !v->gate)
        v->held = 0;                                       /* a sung syllable let go: on to its last consonant */
    v->fpos += SP_BLK * 256u;
    while (!v->stop && v->fpos >= flen) {
        v->fpos -= flen;
        if (v->held)
            continue;
        if (!sp_frame(v, &SP_VDEF[s->voice % SP_NVOICE])) {   /* the word's end */
            if (v->gate && hold == SP_HOLD_LOOP) {
                sp_word_start(v, v->word);
            } else if (v->gate && hold == SP_HOLD_HOLD && v->nuc_ok) {
                v->held = 1;                               /* the vowel, held while the key is */
                for (i = 0; i < 10u; i++)
                    v->kf[i] = v->ke[i], v->kt[i] = v->nuc_k[i];
                v->ef = v->et;
                v->et = v->nuc_e;
                v->voiced = 1;
                v->pidx = v->nuc_p;
            } else {
                v->stop = 1;
                v->et = 0;
            }
        } else if (v->wflags & SP_W_SING && v->gate && v->frame == SP_WORD[v->word].nuc + 1u) {
            v->held = 2;                                   /* a sung syllable: its vowel, while the key is held */
        }
    }
    t = v->held || v->fpos >= flen ? 32767u : (v->fpos << 11) / ((flen >> 4) + 1u);   /* Q15 */
    for (i = 0; i < 10u; i++)
        v->ke[i] = v->kf[i] + (int32_t)((int64_t)(v->kt[i] - v->kf[i]) * (int32_t)t >> 15);
    {   /* the energy: a step a frame, over its first ~1.4 ms (HOLD: over the frame) */
        uint32_t ramp = v->held ? flen : flen >> 3;
        e = v->fpos >= ramp || !ramp ? v->et : v->ef + (v->et - v->ef) * (int32_t)(v->fpos >> 4) / (int32_t)((ramp >> 4) + 1u);
    }
    /* the pitch: the key (glide), the word's own intonation, bend, vibrato */
    v->pitch += sp_toward(v->pkey - v->pitch, v->tag == SP_TAG_TOUCH ? 110 : b->glide);   /* (a touch: its slide) */
    if (v->held || v->wflags)
        v->ctgt = 0;                                       /* (a held vowel, a sung syllable, a hit: the key's pitch) */
    else if (v->voiced) {
        int32_t c = SP_PLOG[SP_WORD[v->word].p0] - SP_PLOG[v->pidx];   /* (a shorter period: higher) */
        c = c * (int32_t)s->intone / 64;
        if (b->steps)                                      /* LAB AI: whole semitones, a quick step between */
            c = (c + (c >= 0 ? 32 : -32)) / 64 * 64;
        v->ctgt = sp_clampi(c, -12 * 64, 12 * 64);
    }
    v->cont += sp_toward(v->ctgt - v->cont, b->steps ? 12000 : 6000);
    L = v->pitch + v->cont + v->det + sp.bend + b->vib - 69 * 64 - b->fs;
    L = sp_clampi(L, -8 * 768, 2 * 768 + 640);            /* (f0 below the lattice's Nyquist) */
    v->pinc = (uint32_t)((uint64_t)sp_exp2(L) * 236223201u >> 16);   /* f0 / R, Q32 */
    v->amp_q12 = (int32_t)((uint64_t)4366u * sp_exp2(-L / 2) >> 16); /* 2 sqrt(P / P_REF), Q12 */
    v->cper = (uint16_t)sp_clampi((int32_t)(0xFFFFFFFFu / (v->pinc | 1u)), 8, (int32_t)SP_COMBN - 1);
    /* the envelope */
    tgt = v->stop || (!v->gate && (hold == SP_HOLD_GATE || v->held)) ? 0 : 32767;
    if (v->env < tgt)                                      /* (a hit: at once) */
        v->env = v->env + b->gattack > tgt || v->wflags & SP_W_PERC ? tgt : v->env + b->gattack;
    else if (v->env > tgt)
        v->env = v->env - b->grelease < tgt ? tgt : v->env - b->grelease;
    if ((v->stop && ++v->tail >= (v->wflags & SP_W_PERC ? 8u : 40u)) || (!tgt && !v->env)) {   /* ended: its tail rang out (29 ms), or faded */
        v->on = 0;
        v->env = 0;
        v->gl = v->gr = 0;
        return;
    }
    {   /* the gains: the envelope, the velocity, the pan; a ramp across the block */
        int32_t vg = 32767 - (int32_t)(v->wflags & SP_W_PERC ? 127u : s->velo) * (127 - v->vel) * 2, a;
        vg = vg < 2000 ? 2000 : vg;
        a = (int32_t)((int64_t)v->env * vg >> 15);
        if (v->tag == SP_TAG_TOUCH)                        /* (MOPE's touch sounds: quiet) */
            a = (int32_t)((int64_t)a * sp.touch_gain >> 8);
        if (v->tag == SP_TAG_SAY && sp.say_gain) {        /* (the said words: their own level, up to 4 x) */
            a = (int32_t)((int64_t)a * (sp.say_gain < 1024u ? sp.say_gain : 1024u) >> 8);
            while (a > 32767 && sh < 2)
                a >>= 1, sh++;                             /* (the gain kept in Q15, its octaves a shift) */
        }
        pan = v->tag >= SP_TAG_TOUCH && v->tag <= SP_TAG_SAY ? 0 : PAN[vi] * s->spread / 127;   /* (said: centred) */
        gl = (int32_t)((int64_t)a * (32767 - (pan > 0 ? pan : 0)) >> 15);
        gr = (int32_t)((int64_t)a * (32767 + (pan < 0 ? pan : 0)) >> 15);
        dl = (gl - v->gl) / (int32_t)SP_BLK;
        dr = (gr - v->gr) / (int32_t)SP_BLK;
    }
    {
        uint32_t rph = v->rph, rinc = (uint32_t)((uint64_t)b->rinc * v->rmul >> 16);
        int32_t y0 = v->y0, y1 = v->y1, cgl = v->gl, cgr = v->gr;
        for (i = 0; i < n; i++) {
            int32_t y;
            rph += rinc;
            if (rph >= 65536u) {
                rph -= 65536u;
                y1 = y0;
                y0 = sp_lattice(v, b, v->stop ? 0 : e);
            }
            y = b->hold_out ? y0 : y1 + (int32_t)((int64_t)(y0 - y1) * (int32_t)(rph >> 1) >> 15);
            cgl += dl;
            cgr += dr;
            l[i] += (y * cgl >> 15) << sh;
            r[i] += (y * cgr >> 15) << sh;
        }
        v->rph = rph;
        v->y0 = y0;
        v->y1 = y1;
        v->gl = gl;
        v->gr = gr;
    }
}

/* the bus: TONE, DELAY, REVERB, the DC blocker */
/* LOFI's grit (the lattice's output held between its samples) mirrors every formant around the lattice's rate:
 * /a/'s F1 and F2 (750, 1200 Hz) came back at 7.2 and 6.8 kHz only 13 dB down, a bright "ee" over every vowel (an
 * open A heard as an I). The Speak & Spell filtered its DAC's output; here a 4th-order Butterworth at 0.92 of the
 * lattice's Nyquist takes the images away (some 25 dB more) and leaves the band and the held samples' grit */
static inline int32_t sp_img(int32_t x, uint32_t ch)
{
    uint32_t q;
    for (q = 0; q < 2u; q++) {
        int32_t *z = sp.ibz[q][ch];
        const int32_t *c = sp.ibc[q];
        int64_t acc = (int64_t)c[0] * x + (int64_t)c[1] * z[0] + (int64_t)c[2] * z[1] - (int64_t)c[3] * z[2] -
                      (int64_t)c[4] * z[3];
        int32_t y = (int32_t)(acc >> 20);
        z[1] = z[0];
        z[0] = x;
        z[3] = z[2];
        z[2] = y;
        x = y;
    }
    return x;
}

static void sp_bus(int32_t *l, int32_t *r, uint32_t n)
{
    const sp_sound_t *s = &sp.snd;
    uint32_t i, k;
    int32_t tone = (int32_t)s->tone - 64;                  /* -64..63 */
    int32_t dsend = s->dmix * 200, dfb = s->dfb * 230;     /* (feedback up to 0.9) */
    int32_t rsend = s->reverb * 180, rfb = 24000 + s->size * 60, damp = 8000;   /* rfb 0.73 .. 0.96 */
    int32_t dtgt = (int32_t)((30u + s->dtime * 710u / 127u) * 441u / 10u) * 256;   /* Q8 frames */
    if (dtgt > (int32_t)(SP_DLN - 64u) * 256)
        dtgt = (int32_t)(SP_DLN - 64u) * 256;
    for (i = 0; i < n; i++) {
        int32_t x = l[i], y = r[i], dl = 0, dr = 0, wl = 0, wr = 0;
        if (sp.img_on) {                                   /* (LOFI's grit: its images away) */
            x = sp_img(x, 0);
            y = sp_img(y, 1);
        }
        /* the anti-image low-pass (LOFI below 64: smooth) */
        if (sp.lpa < 32767) {
            sp.lp1 += (int32_t)((int64_t)(x - sp.lp1) * sp.lpa >> 15);
            sp.lp2 += (int32_t)((int64_t)(y - sp.lp2) * sp.lpa >> 15);
            x = sp.lp1;
            y = sp.lp2;
        }
        if (tone) {                                        /* TONE: a tilt around ~700 Hz */
            sp.tlo += (int32_t)((int64_t)(x - sp.tlo) * 3300 >> 15);
            sp.tro += (int32_t)((int64_t)(y - sp.tro) * 3300 >> 15);
            x += (int32_t)((int64_t)((x - sp.tlo) - sp.tlo) * tone >> 7);
            y += (int32_t)((int64_t)((y - sp.tro) - sp.tro) * tone >> 7);
        }
        if (dsend)
            sp.dtail = SP_DLN * 12u;                       /* (its repeats ring out, ~9 s, after the send is off) */
        if (sp.dtail) {                                    /* DELAY */
            sp.dtail--;
            int32_t dq, fr;
            uint32_t rp, a, b2;
            sp.dcur += sp.dcur < dtgt ? (dtgt - sp.dcur > 64 ? 64 : dtgt - sp.dcur) :
                       -(sp.dcur - dtgt > 64 ? 64 : sp.dcur - dtgt);   /* (a time change glides) */
            dq = sp.dcur;
            if (s->dtype == SP_DLY_TAPE) {                 /* wow: +-1.5 ms at 0.6 Hz */
                sp.wow += 58400u;
                dq += sp_sin(sp.wow) * 66 >> 7;
            }
            rp = sp.dw - (uint32_t)(dq >> 8) - 1u;
            fr = dq & 255;
            a = (rp & (SP_DLN - 1u)) * 2u;
            b2 = ((rp - 1u) & (SP_DLN - 1u)) * 2u;
            dl = sp_dl[a] + ((sp_dl[b2] - sp_dl[a]) * fr >> 8);
            dr = sp_dl[a + 1u] + ((sp_dl[b2 + 1u] - sp_dl[a + 1u]) * fr >> 8);
            {
                int32_t inl = (x * dsend) >> 15, inr = (y * dsend) >> 15, fl, frr;
                if (s->dtype == SP_DLY_PING) {             /* the input into the left, each side into the other */
                    fl = ((inl + inr) >> 1) + (dr * dfb >> 15);
                    frr = dl * dfb >> 15;
                } else {
                    fl = inl + (dl * dfb >> 15);
                    frr = inr + (dr * dfb >> 15);
                }
                if (s->dtype == SP_DLY_TAPE) {             /* darker each time, softly saturated */
                    sp.dlp_l += (fl - sp.dlp_l) * 9000 >> 15;
                    sp.dlp_r += (frr - sp.dlp_r) * 9000 >> 15;
                    fl = sp.dlp_l;
                    frr = sp.dlp_r;
                    fl = fl - (fl * (fl < 0 ? -fl : fl) >> 17);
                    frr = frr - (frr * (frr < 0 ? -frr : frr) >> 17);
                }
                sp_dl[(sp.dw & (SP_DLN - 1u)) * 2u] = (int16_t)sp_sat16(fl);
                sp_dl[(sp.dw & (SP_DLN - 1u)) * 2u + 1u] = (int16_t)sp_sat16(frr);
            }
            sp.dw++;
        }
        if (rsend) {                                       /* REVERB: 4 + 4 damped combs, 2 + 2 all-passes */
            int32_t in = ((x + y) >> 1) * rsend >> 15;
            for (k = 0; k < 8u; k++) {
                uint32_t p = sp.rvw[k];
                int32_t o = sp_rv[k][p];
                sp.rvlp[k] += (o - sp.rvlp[k]) * (32767 - damp) >> 15;
                sp_rv[k][p] = (int16_t)sp_sat16((in >> 2) + (sp.rvlp[k] * rfb >> 15));
                sp.rvw[k] = p + 1u >= SP_RVLEN[k] ? 0u : p + 1u;
                if (k < 4u)
                    wl += o;
                else
                    wr += o;
            }
            for (k = 0; k < 4u; k++) {
                uint32_t p = sp.apw[k];
                int32_t *w = k < 2u ? &wl : &wr, bo = sp_ap[k][p], o = bo - (*w >> 1);
                sp_ap[k][p] = (int16_t)sp_sat16(*w + (bo >> 1));
                *w = o;
                sp.apw[k] = p + 1u >= SP_APLEN[k] ? 0u : p + 1u;
            }
        }
        x += dl + (wl >> 1);
        y += dr + (wr >> 1);
        /* the DC blocker (~7 Hz; its state x 256, so its rounding leaves no offset) */
        sp.hp_l += (x - sp.hpx_l) * 256 - (sp.hp_l >> 10) - (sp.hp_l > 0);
        sp.hpx_l = x;
        sp.hp_r += (y - sp.hpx_r) * 256 - (sp.hp_r >> 10) - (sp.hp_r > 0);
        sp.hpx_r = y;
        l[i] = sp.hp_l >> 8;
        r[i] = sp.hp_r >> 8;
    }
}

/* one block: out[2 n] (L R), Q15-ish; n = SP_BLK */
static void sp_render(int32_t *out, uint32_t n)
{
    int32_t l[SP_BLK], r[SP_BLK];
    uint32_t i, any = 0;
    sp_blk_t b;
    sp_requests();
    sp_track_keys();
    sp_play();
    sp_say_run();
    sp_block_setup(&b);
    for (i = 0; i < SP_BLK; i++)
        l[i] = r[i] = 0;
    for (i = sp.vmax; i < SP_NV; i++)                      /* (the CPU ran short: fewer voices) */
        if (sp.v[i].on)
            sp.v[i].gate = 0, sp.v[i].stop = 1;
    {   /* the screen: the loudest voice's filter */
        int32_t best = 0;
        uint32_t bi = SP_NV;
        for (i = 0; i < SP_NV; i++)
            if (sp.v[i].on && sp.v[i].et * (sp.v[i].env >> 8) > best)
                best = sp.v[i].et * (sp.v[i].env >> 8), bi = i;
        if (bi < SP_NV) {
            for (i = 0; i < 10u; i++)
                sp.vis_k[i] = sp.v[bi].ke[i];
            sp.vis_e = sp.v[bi].et;
        } else
            sp.vis_e = 0;
    }
    sp.speaking = 0;
    for (i = 0; i < SP_NV; i++)
        if (sp.v[i].on) {
            sp_voice_block(&sp.v[i], i, &b, l, r);
            any = 1;
            if (sp.v[i].on && !sp.v[i].stop)
                sp.speaking |= 1u << i;
        }
    for (i = 0; i < MO_NT; i++)                            /* MOPE's tones (the speech's bus too) */
        any |= mos.v[i].on;
    mos_render(l, r);
    if (any)
        sp.idle = 0;
    else if (sp.idle < 0x7FFFFFFFu)
        sp.idle += SP_BLK;
    sp_bus(l, r, n);
    rh_render(l, r, n, sp.drums);                          /* the drums, dry, after the speech's effects */
    for (i = 0; i < n; i++) {
        int32_t a = l[i] < 0 ? -l[i] : l[i];
        if (a > sp.peak)
            sp.peak = a;
        out[2u * i] = l[i];
        out[2u * i + 1u] = r[i];
    }
    sp.now += n;
}

/* the main loop's side: the anti-image low-pass for LOFI and FORMANT (two one-poles' coefficient, Q15) */
static void sp_apply(void)
{
    const sp_sound_t *s = &sp.snd;
    int32_t fc, lo = s->lofi, x;
    if (lo >= 64) {                                        /* (held: the grit; its images filtered, sp_img) */
        /* fc 3.7 kHz x the formants' shift; the bilinear Butterworth's K = tan(pi fc / fs) by its series, Q20 */
        static const int32_t QI[2] = {1937516, 802545};    /* 1 / Q of the two sections (1.8478, 0.7654), Q20 */
        int32_t w, w2, w3, K, K2, nrm;
        uint32_t q;
        fc = (int32_t)((uint64_t)3700u * sp_exp2((int32_t)s->formant * 64) >> 16);
        fc = sp_clampi(fc, 1500, 9000);
        w = fc * 74668 / 1000;                             /* pi fc / fs, Q20 (no 64-bit divide: the target has none) */
        w2 = (int32_t)((int64_t)w * w >> 20);
        w3 = (int32_t)((int64_t)w * w2 >> 20);
        K = w + w3 / 3 + (int32_t)((int64_t)w3 * w2 >> 20) * 2 / 15 +
            (int32_t)((int64_t)((int64_t)w3 * w2 >> 20) * w2 >> 20) * 17 / 315;
        K2 = (int32_t)((int64_t)K * K >> 20);
        for (q = 0; q < 2u; q++) {
            int32_t kq = (int32_t)((int64_t)K * QI[q] >> 20);
            nrm = (int32_t)(0x40000000u / (uint32_t)(((1 << 20) + kq + K2) >> 10));   /* 1 / (1 + K / Q + K^2), Q20 */
            sp.ibc[q][0] = (int32_t)((int64_t)K2 * nrm >> 20);
            sp.ibc[q][1] = 2 * sp.ibc[q][0];
            sp.ibc[q][2] = sp.ibc[q][0];
            sp.ibc[q][3] = (int32_t)((int64_t)2 * (K2 - (1 << 20)) * nrm >> 20);
            sp.ibc[q][4] = (int32_t)((int64_t)((1 << 20) - kq + K2) * nrm >> 20);
        }
        if (!sp.img_on) {
            uint32_t a, b, c;
            for (a = 0; a < 2u; a++)
                for (b = 0; b < 2u; b++)
                    for (c = 0; c < 4u; c++)
                        sp.ibz[a][b][c] = 0;
        }
        sp.img_on = 1;
        sp.lpa = 32767;
        return;
    }
    sp.img_on = 0;
    /* 3.4 kHz x the formants' shift, up to twice that at LOFI 63 */
    fc = (int32_t)((uint64_t)3400u * sp_exp2((int32_t)s->formant * 64) >> 16) * (64 + lo) / 64;
    x = fc * 4779 >> 10;                                   /* 2 pi fc / fs, Q15 */
    if (x > 32767) {
        sp.lpa = 32767;
        return;
    }
    {
        int32_t x2 = x * x >> 15, x3 = x2 * x >> 15;
        x = x - x2 / 2 + x3 / 6;                           /* 1 - e^-x */
    }
    sp.lpa = sp_clampi(x, 500, 32767);
}

/* the VOICE knob: its character's knobs loaded */
static void sp_voice_load(sp_sound_t *s, uint32_t vn)
{
    const sp_vdef_t *d = &SP_VDEF[vn % SP_NVOICE];
    s->voice = (uint8_t)(vn % SP_NVOICE);
    s->speed = d->speed;
    s->intone = d->intone;
    s->formant = d->formant;
    s->breath = d->breath;
    s->lofi = d->lofi;
    s->metal = d->metal;
    s->attack = d->attack;
    s->release = d->release;
    s->vrate = d->vrate;
    s->vdepth = d->vdepth;
    if (d->rev != 255u)
        s->reverb = d->rev;
    if (d->size != 255u)
        s->size = d->size;
    if (d->spread != 255u)
        s->spread = d->spread;
}

static void sp_init(void)
{
    uint32_t i;
    sp.snd = SP_SOUND_DEF;
    sp.vmax = SP_NV;
    sp.groove_req = 0xFFu;
    sp.drums = 100;
    sp.req_sent = 0xFFFFu;
    sp.wlast = -1;
    sp.bend_range = 2;
    sp.chord_ms = 40;
    sp.say_gain = 256;
    sp.touch_gain = 256;
    for (i = 0; i < SP_NV; i++) {
        sp.v[i].nz = (uint16_t)(0xACE1u + i * 977u);
        sp.v[i].rmul = 65536u;
    }
    sp.dcur = 8000 * 256;
    sp_apply();
}

#include "sp_mope.c"                       /* MOPE: the listener, its comments, its sulk (the main loop's) */
