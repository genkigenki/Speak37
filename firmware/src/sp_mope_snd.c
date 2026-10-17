/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* MOPE's sound (sp_dsp.c's part: the audio ISR). While the keys mode is MOPE the keys play a tone instead of words;
 * MOPE (sp_mope.c, the main loop) listens to them and, when it sulks, plays a melody of its own here.
 *   the tone    MO_NT voices of two-operator FM: a sine's phase moved by another sine (both the parabola sine, as
 *               sp_sin), each voice one of three timbres:
 *                 KEYS   the keys': a soft electric piano (1 : 1, the index falling from 1.3 to 0.25), decaying
 *                        while held, released in ~0.2 s
 *                 SAD    MOPE's melody: a hollow, slightly wavering flute (2 : 1, a low index, an 80 ms attack, a
 *                        late vibrato), long in its release
 *                 BASS   MOPE's bass: a muted pluck (1 : 1, the index falling fast)
 *               the keys' tone follows the pitch bend; mixed before the bus (TONE, DELAY, REVERB as the speech)
 *   the melody  mos.seq: up to MO_SEQN notes in sixteenths (at, note, length, velocity, timbre), written by the main
 *               loop before it sets seq_req (1 start, 2 stop); seq_done when the last note has rung out
 *   the events  every key and MIDI note down (its velocity) and up (0), its key (0xFF: MIDI), the sample it came
 *               at: a ring of 32 the main loop reads (sp_mope.c mo_drive); a key's up has the note its down had
 *   the touch   a control touched in MOPE: a little sound at once (mos.tq, written by sp_mope.c mo_touch): a word of
 *               the vocabulary or a kit hit, quiet (sp.touch_gain), in the centre, sliding from its note by its
 *               slide (a knob's "OH" up or down, a fader's "OOH", a wheel's bending "OO"), or a tone's blip (a
 *               pad); entries may wait (a p-p-p stutter)
 * Its words are louder than its music (sp.say_gain), the music quiet enough for them. */
#define MO_NT 8u                           /* tone voices */
#define MO_SEQN 64u                        /* the melody's notes */
#define MO_TAG_SEQ 200u                    /* (a tone the melody started: + its note's index % 48) */
enum { MO_TK_KEYS, MO_TK_SAD, MO_TK_BASS, MO_TK_BLIP, MO_NTK };
#define MO_TAG_BLIP 250u                   /* (a touch's blip) */
#define MO_TQ 16u                          /* the touch queue */
/* a timbre: the modulator's ratio, the index's start and end (rad x 20861: Q32 phase a Q15 of the modulator) and its
 * fall a block (Q15), the attack's step a block, the decay a block while held (Q15 of what is left), the release, the
 * level (Q15), the vibrato (cents x 64 / 100) */
typedef struct { uint8_t ratio; int32_t i0, i1; int32_t ifall, att, dec, rel, lvl, vib; } mo_tk_t;
static const mo_tk_t MO_TK[MO_NTK] = {
    {1, 27119, 5215, 60, 9000, 16, 120, 3400, 0},          /* KEYS: ~1.5 s decay, 0.2 s release */
    {2, 9000, 7300, 8, 300, 4, 40, 2200, 8},               /* SAD: 80 ms attack, a slow fade, 0.6 s release */
    {1, 20000, 3000, 160, 12000, 30, 150, 1400, 0},        /* BASS: ~0.8 s */
    {3, 12000, 2000, 400, 16000, 400, 400, 1700, 0},       /* BLIP: a short glassy tick (a pad in MOPE) */
};

typedef struct {
    uint8_t on, gate, tag, tk, ad;         /* ad: the attack done */
    int16_t note;                          /* 1/64 semitones (MIDI x 64) */
    uint32_t pc, pm;                       /* the carrier's, the modulator's phase (Q32) */
    int32_t env, idx, g;                   /* the envelope (Q15), the index, the gain at the end of the last block */
    int32_t vel;                           /* Q15 */
    uint32_t age, t, off;                  /* (its age); blocks since it began; (the melody's) the sample it lets go */
    uint32_t lfo;
} mo_tone_t;

static struct {
    mo_tone_t v[MO_NT];
    uint32_t age;
    volatile uint8_t ev_w;                 /* the events: written here, read by the main loop (its own ev_r) */
    uint8_t ev_note[32], ev_vel[32], ev_key[32];
    uint8_t knote[37], mnote[128];         /* the note each key, each MIDI note went down with */
    uint32_t ev_t[32];
    volatile uint8_t seq_req;              /* 1: start the melody (mos.seq is written), 2: stop it */
    volatile uint8_t seq_on, seq_done;
    uint8_t seq_n, seq_i;
    uint32_t seq_t, seq_end, sixteenth;    /* samples into it, its end; a sixteenth (samples) */
    volatile uint32_t seq_notes;           /* notes it has begun (the screen: the robot nods) */
    struct { uint16_t at; uint8_t note, len, vel, tk; } seq[MO_SEQN];
    struct { uint16_t w; uint8_t note, vel, dly; int8_t slide; } tq[MO_TQ];   /* (w 0xFFFF: a blip) */
    volatile uint8_t tq_w;                 /* (the main loop's) */
    uint8_t tq_r, tq_wait;                 /* (the audio's: read, blocks still to wait) */
} mos;

/* the parabola sine (sp_sin's, 32-bit): Q15 */
static inline int32_t mo_sin(uint32_t p)
{
    int32_t x = (int32_t)(p >> 16) - 32768, a = x < 0 ? -x : x;
    return x * (32768 - a) >> 13;
}

static void mos_ev(uint32_t note, uint32_t vel, uint32_t key)
{
    uint8_t w = mos.ev_w;
    mos.ev_note[w % 32u] = (uint8_t)note;
    mos.ev_vel[w % 32u] = (uint8_t)vel;
    mos.ev_key[w % 32u] = (uint8_t)key;
    mos.ev_t[w % 32u] = sp.now;
    RING_PUBLISH();
    mos.ev_w = (uint8_t)(w + 1u);
}

static void mos_on(uint32_t note, uint32_t vel, uint32_t tag, uint32_t tk)
{
    uint32_t i, best = 0, n = sp.vmax >= 6u ? MO_NT : 4u;
    int32_t lo = 0x7FFFFFFF;
    mo_tone_t *v;
    for (i = 0; i < n; i++) {                              /* a free voice, else a released one, else the oldest */
        int32_t a = !mos.v[i].on ? -1 : mos.v[i].gate ? 0x20000000 - (int32_t)(mos.age - mos.v[i].age) :
                    0x10000000 - (int32_t)(mos.age - mos.v[i].age);
        if (a < lo)
            lo = a, best = i;
    }
    v = &mos.v[best];
    if (!v->on)
        v->g = 0, v->env = 0, v->pc = v->pm = 0;
    v->on = v->gate = 1;
    v->tag = (uint8_t)tag;
    v->tk = (uint8_t)tk;
    v->note = (int16_t)(note * 64u);
    v->idx = MO_TK[tk].i0;
    v->vel = 6000 + (int32_t)(vel > 127u ? 127u : vel) * 210;
    v->age = ++mos.age;
    v->t = 0;
    v->ad = 0;
    v->lfo = 0;
}
static void mos_off(uint32_t tag)
{
    uint32_t i;
    for (i = 0; i < MO_NT; i++)
        if (mos.v[i].on && mos.v[i].tag == tag)
            mos.v[i].gate = 0;
}

/* a voice's block: its samples into l, r (out of line: tests/sp_budget.py counts its loop) */
__attribute__((noinline)) static void mo_tone_run(mo_tone_t *v, int32_t *l, int32_t *r, int32_t g1)
{
    const mo_tk_t *T = &MO_TK[v->tk];
    int32_t L = v->note + (v->tag < MO_TAG_SEQ ? sp.bend : 0) - 69 * 64, g = v->g, dg = (g1 - g) / (int32_t)SP_BLK;
    int32_t idx = v->idx;
    uint32_t i, inc, pc = v->pc, pm = v->pm, mi;
    if (T->vib && v->t > 340u) {                           /* (SAD: a vibrato after 0.25 s, 5 Hz) */
        v->lfo += 15560000u;
        L += sp_sin(v->lfo) * T->vib >> 15;
    }
    inc = (uint32_t)((uint64_t)sp_exp2(sp_clampi(L, -6 * 768, 3 * 768)) * 42835690u >> 16);   /* f / fs, Q32 */
    mi = inc * T->ratio;
    for (i = 0; i < SP_BLK; i++) {
        int32_t m = mo_sin(pm), s;
        pm += mi;
        s = mo_sin(pc + (uint32_t)(m * idx));
        pc += inc;
        g += dg;
        s = s * g >> 15;
        l[i] += s;
        r[i] += s;
    }
    v->pc = pc;
    v->pm = pm;
    v->g = g1;
}

/* the melody: its notes on their sixteenths, each let go after its length; done when the last has rung out */
static void mos_seq_run(void)
{
    uint8_t q = mos.seq_req;
    uint32_t i;
    if (q) {
        mos.seq_req = 0;
        for (i = 0; i < MO_NT; i++)
            if (mos.v[i].tag >= MO_TAG_SEQ)
                mos.v[i].gate = 0;
        mos.seq_on = q == 1u;
        mos.seq_done = 0;
        mos.seq_i = 0;
        mos.seq_t = 0;
        mos.seq_end = 0;
        if (!mos.sixteenth)
            mos.sixteenth = SP_FS * 60u / 76u / 4u;        /* (76 BPM) */
        for (i = 0; q == 1u && i < mos.seq_n && i < MO_SEQN; i++) {
            uint32_t e = (uint32_t)(mos.seq[i].at + mos.seq[i].len) * mos.sixteenth;
            mos.seq_end = e > mos.seq_end ? e : mos.seq_end;
        }
    }
    if (!mos.seq_on)
        return;
    while (mos.seq_i < mos.seq_n && mos.seq_i < MO_SEQN && (uint32_t)mos.seq[mos.seq_i].at * mos.sixteenth <= mos.seq_t) {
        uint32_t k = mos.seq_i++, j;
        mos_on(mos.seq[k].note, mos.seq[k].vel, MO_TAG_SEQ + k % 48u, mos.seq[k].tk);
        for (j = 0; j < MO_NT; j++)
            if (mos.v[j].on && mos.v[j].tag == MO_TAG_SEQ + k % 48u && mos.v[j].age == mos.age)
                mos.v[j].off = mos.seq_t + (uint32_t)mos.seq[k].len * mos.sixteenth;
        mos.seq_notes++;
    }
    for (i = 0; i < MO_NT; i++)
        if (mos.v[i].on && mos.v[i].gate && mos.v[i].tag >= MO_TAG_SEQ && mos.seq_t >= mos.v[i].off)
            mos.v[i].gate = 0;
    mos.seq_t += SP_BLK;
    if (mos.seq_i >= mos.seq_n && mos.seq_t >= mos.seq_end + SP_FS * 3u / 2u) {
        mos.seq_on = 0;
        mos.seq_done = 1;
    }
}

/* a block of MOPE's tones into l, r (before the bus); out of MOPE: the keys' tones let go, the melody stopped */
static void mos_render(int32_t *l, int32_t *r)
{
    uint32_t i;
    if (sp.mode != SP_MODE_MOPE) {
        for (i = 0; i < MO_NT; i++)
            mos.v[i].gate = 0;
        if (mos.seq_on)
            mos.seq_on = 0, mos.seq_done = 1;
    }
    mos_seq_run();
    while (mos.tq_r != mos.tq_w) {                         /* the touch sounds, each after its wait */
        uint32_t k = mos.tq_r % MO_TQ;
        RING_PUBLISH();
        if (mos.tq_wait < mos.tq[k].dly) {
            mos.tq_wait++;
            break;
        }
        mos.tq_wait = 0;
        mos.tq_r++;
        if (sp.mode != SP_MODE_MOPE)
            continue;
        if (mos.tq[k].w == 0xFFFFu) {
            mos_on(mos.tq[k].note, mos.tq[k].vel, MO_TAG_BLIP, MO_TK_BLIP);
            mos_off(MO_TAG_BLIP);
        } else if (mos.tq[k].w < SP_NWORD) {
            uint32_t a0 = sp.age, j;
            sp_speak(mos.tq[k].w, mos.tq[k].note * 64, mos.tq[k].vel, SP_TAG_TOUCH);
            for (j = 0; j < SP_NV; j++)                    /* (its slide: from its note to the slide's end) */
                if (sp.v[j].tag == SP_TAG_TOUCH && sp.v[j].age > a0) {
                    sp.v[j].pitch = mos.tq[k].note * 64;
                    sp.v[j].pkey = (mos.tq[k].note + mos.tq[k].slide) * 64;
                }
            sp_release(SP_TAG_TOUCH);
        }
    }
    for (i = 0; i < MO_NT; i++) {
        mo_tone_t *v = &mos.v[i];
        const mo_tk_t *T = &MO_TK[v->tk % MO_NTK];
        int32_t g1;
        if (!v->on)
            continue;
        if (v->gate) {                                     /* the envelope: the attack, then a decay while held */
            if (!v->ad) {
                v->env += T->att;
                if (v->env >= 32767)
                    v->env = 32767, v->ad = 1;
            } else
                v->env -= v->env * T->dec >> 15;
        } else
            v->env -= (v->env * T->rel >> 15) + 1;
        v->idx = v->idx + ((T->i1 - v->idx) * T->ifall >> 15);
        v->t += v->t < 0xFFFFFFu;
        if (v->env <= 0) {
            v->on = 0;
            v->env = 0;
            v->g = 0;
            continue;
        }
        g1 = (int32_t)((int64_t)v->env * v->vel >> 15) * T->lvl >> 15;
        mo_tone_run(v, l, r, g1);
    }
}
