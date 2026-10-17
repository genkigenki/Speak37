/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* MOPE: the keys mode in which Speak37 is a gloomy, sarcastic, rather well-read keyboard. It listens to what is
 * played, says what it thinks of it in its own words, and when it is bored enough it plays something itself. The main
 * loop's side (mo_drive, sp_ui.c calls it); no hardware, no float: tests/sp_mope_test.c runs it on the host.
 *   the listener  the keys' and MIDI notes as they came (sp_mope_snd.c's events): a phrase ends MO_GAP_MS after the
 *                 last key was let go (or after MO_LONG_MS, MO_PN notes). Pure rules: the notes sounding together
 *                 (still held MO_HELD_MS after another began): one pitch class an octave, two an interval (seconds
 *                 to sevenths, the tritone), three or more a chord by its pitch classes (major, minor, diminished,
 *                 augmented, suspended, the five sevenths; the bass preferred as the root), else a cluster; the
 *                 notes one after another: a run of steps (a scale up or down), very fast a glissando, the same key
 *                 again and again, a single note, else a tune; how fast, how slow, how loud, how soft; and against
 *                 the phrase before: the same again (a signature), much more (more notes, a richer sound)
 *   the comments  a line from the grammar of assets/speak/mope.txt (sp_vocab.h MO_*): sometimes an opener, the
 *                 observation (its class, its slots filled: the note, the chord, the interval, the count), sometimes
 *                 a sigh and a conclusion by the mood; each class's fragments avoid the ones used within its last
 *                 (n - 1, at most MO_HIST) lines, and a whole line never repeats one of the last eight. It speaks
 *                 only after a phrase has ended, no key held, MO_REST_MS after its last line ended (stale after
 *                 MO_STALE_MS); the words go to the say queue (sp_dsp.c sp_say_run) at its own low note, falling at
 *                 a full stop, rising at a question, a pause after each
 *   the mood      gloom 0..120: simple things (a fifth, an octave, a major triad, the same key, the same again)
 *                 raise it, rich ones (sevenths, diminished, augmented, a long varied tune) lower it, praise lowers
 *                 it most; long silence raises it
 *   the sulk      gloom at MO_SULK_AT: it announces it will play itself (SULK), then plays a short sad minor piece
 *                 (mo_compose: i iv V i, a falling melody over a plucked bass, its own timbres); any key (or a pad)
 *                 interrupts it (BACK); played out, it says so (ENDSULK)
 *   every control its own lines too: K1..K8, the faders, both wheels, the buttons (OCTAVE, PATCH, SEQ PLAY, the
 *                 page buttons ...; the FM-1's: its knobs, SELECT, the page buttons ...): mo_ctl, said once the control
 *                 has been left alone MO_CTL_MS (the first touch of each always, later ones about every other time),
 *                 from the control's own class (CT_*) or a template for any control (CTPL), the control's name a slot
 *   the pools     a line is built fresh each time: openers, the observation's own lines or general templates (TPL),
 *                adjectives (ADJ) and asides (ASIDE) in their slots or on their own, sighs, closers by the mood
 *                (CLOSE, BORED, KIND, RICH); each pool avoids its last fragments
 *   the music     after each line a short reply of its own (mo_reply: three or four notes, falling, in the key it
 *                heard; something rich gets a figure that rises before it falls), or after SULK its whole piece
 *   the pads      the top row praises it (PRAISE), pads 9..12 poke it (POKE: a stray thought, or one of its
 *                activities), 13..16 ask for a joke (JOKE, a synthesizer player's misfortune, told gloomily)
 *   activities    (a poke, now and then by itself, or instead of noticing the silence) LIGHTS: it lets a few lights
 *                shine (the pads and the buttons blink), then asks if that made you happy; DARK: it turns the light
 *                off (the screen dark for 5 s), then finds that was not better either; ASK: it asks for one control
 *                (turn this knob, move that wheel, press that button, a key, a pad), and when that one is used it
 *                did not help (another control: a remark, up to four; nothing in 15 s: you did not even try); GAME:
 *                a light wanders over the pads (the FM-1: over its first 16 keys' lights), a wrong one hurts (OW),
 *                three hits, 30 s or twelve misses and it gives up
 *   the poses     each press (a key, a control, a pad) puts the robot in another pose (never the same twice in a
 *                row, at most every 0.4 s): slumped, one arm half raised, sitting, head in hands, lying flat,
 *                hunched, a shrug, kneeling (sp_mope_ui.c draws them) */
#define MO_PN 48u                          /* notes a phrase */
#define MO_GAP_MS 1300u                    /* no key for this long: the phrase has ended */
#define MO_LONG_MS 12000u                  /* .. or it has run this long */
#define MO_HELD_MS 100u                    /* a note still held this long after another began: they sound together */
#define MO_REST_MS 3500u                   /* from the end of a line to the next one */
#define MO_STALE_MS 8000u                  /* a phrase not yet commented on is forgotten */
#define MO_SILENT_MS 30000u                /* no key for this long: it notices */
#define MO_SULK_AT 100                     /* the gloom it sulks at */
#define MO_HIST 16u                        /* the most lines a fragment waits before it comes again */
#define MO_LMAX 44u                        /* words a line */
#define MO_LSHORT 22u                      /* (a line longer than this gets no closer: said in one go) */
#define MO_CTL_MS 800u                     /* a control left alone this long: its comment */
#define MO_REQ_JOKE 0xFEu                  /* (mo.req: a joke) */
#define MO_ASK_MS 15000u                   /* ASK: how long it waits for the control it asked for */
#define MO_GAME_MS 30000u                  /* GAME: how long it plays at most */
#define MO_NPOSE 8u
#ifdef BOARD_SMK37
#define MO_SMK BOARD_SMK37                 /* (the SMK-37: pads, faders, wheels; the FM-1: none of them) */
#else
#define MO_SMK 1
#endif
#define MO_NFRAG ((uint32_t)(sizeof MO_AT / sizeof MO_AT[0]) - 1u)

enum { MO_K_NONE, MO_K_SINGLE, MO_K_IVL, MO_K_CHORD, MO_K_CLUSTER, MO_K_REPEAT, MO_K_UP, MO_K_DOWN, MO_K_GLISS,
       MO_K_TUNE };
/* the chord qualities, in the order of mope.txt's [=QUAL] */
enum { MO_Q_MAJ, MO_Q_MIN, MO_Q_DIM, MO_Q_AUG, MO_Q_SUS, MO_Q_DOM7, MO_Q_MAJ7, MO_Q_MIN7, MO_Q_HDIM, MO_Q_DIM7, MO_NQ };
enum { MO_LISTEN, MO_ANNOUNCE, MO_SULK };
/* the controls (mope.txt [=CTL]'s order): K1..K8, the faders, the wheels, the buttons; the FM-1's KNOB 1..4 */
enum { MO_CT_MASTER, MO_CT_PAGE, MO_CT_VOICE, MO_CT_BANK, MO_CT_K5, MO_CT_K6, MO_CT_K7, MO_CT_K8, MO_CT_F1, MO_CT_F2,
       MO_CT_F3, MO_CT_F4, MO_CT_BEND, MO_CT_MODW, MO_CT_OCTAVE, MO_CT_PATCH, MO_CT_PLAY, MO_CT_REC, MO_CT_RHYTHM,
       MO_CT_FILL, MO_CT_REPEAT, MO_CT_CHORD, MO_CT_HOLD, MO_CT_FX, MO_CT_SETUP, MO_CT_ARROW, MO_CT_PAGEBTN,
       MO_CT_SAVE, MO_CT_FK1, MO_CT_FK2, MO_CT_FK3, MO_CT_FK4, MO_CT_KEY, MO_CT_PAD, MO_CT_SCALE, MO_CT_EDIT,
       MO_NCT };
static const uint8_t MO_CT_CLS[MO_NCT] = {
    MO_C_CT_MASTER, MO_C_CT_PAGE, MO_C_CT_VOICE, MO_C_CT_BANK, MO_C_CT_K5, MO_C_CT_K6, MO_C_CT_K7, MO_C_CT_K8,
    MO_C_CT_F1, MO_C_CT_F2, MO_C_CT_F3, MO_C_CT_F4, MO_C_CT_BEND, MO_C_CT_MODW, MO_C_CT_OCTAVE, MO_C_CT_PATCH,
    MO_C_CT_PLAY, MO_C_CT_REC, MO_C_CT_RHYTHM, MO_C_CT_FILL, MO_C_CT_REPEAT, MO_C_CT_CHORD, MO_C_CT_HOLD, MO_C_CT_FX,
    MO_C_CT_SETUP, MO_C_CT_ARROW, MO_C_CT_PAGEBTN, MO_C_CT_SAVE, MO_C_CT_K5, MO_C_CT_K6, MO_C_CT_K7, MO_C_CT_K8,
    MO_C_CTPL, MO_C_CTPL, MO_C_CT_SCALE, MO_C_CT_SCALE};
/* what ASK may ask for (each board's own controls; not the one that leaves MOPE) */
#if MO_SMK
static const uint8_t MO_ASKABLE[] = {
    MO_CT_MASTER, MO_CT_PAGE, MO_CT_VOICE, MO_CT_BANK, MO_CT_K5, MO_CT_K6, MO_CT_K7, MO_CT_K8, MO_CT_F1, MO_CT_F2,
    MO_CT_F3, MO_CT_F4, MO_CT_BEND, MO_CT_MODW, MO_CT_OCTAVE, MO_CT_PLAY, MO_CT_REC, MO_CT_RHYTHM, MO_CT_FILL,
    MO_CT_REPEAT, MO_CT_CHORD, MO_CT_HOLD, MO_CT_FX, MO_CT_SETUP, MO_CT_ARROW, MO_CT_PAGEBTN, MO_CT_SCALE, MO_CT_KEY,
    MO_CT_PAD};
#else
static const uint8_t MO_ASKABLE[] = {MO_CT_MASTER, MO_CT_PAGE, MO_CT_VOICE, MO_CT_BANK, MO_CT_FK1, MO_CT_FK2,
                                     MO_CT_FK3, MO_CT_FK4, MO_CT_OCTAVE, MO_CT_FX, MO_CT_SETUP, MO_CT_ARROW,
                                     MO_CT_PAGEBTN, MO_CT_SAVE, MO_CT_EDIT, MO_CT_KEY};
#endif
enum { MO_AC_NONE, MO_AC_LIGHTS, MO_AC_DARK, MO_AC_ASK, MO_AC_GAME, MO_NAC };
enum { MO_P_SLUMP, MO_P_HALF, MO_P_SIT, MO_P_HEAD, MO_P_LIE, MO_P_HUNCH, MO_P_SHRUG, MO_P_KNEEL };
#define MO_A_SAY 1u                        /* mo_step: a line to say (mo.line) */
#define MO_A_PLAY 2u                       /* .. the melody to start (mos.seq) */
#define MO_A_STOP 4u                       /* .. the melody and the words stopped */

typedef struct { uint8_t note, vel; uint32_t on, off; } mo_note_t;   /* ms; off 0: still held */
typedef struct {
    uint8_t kind;                          /* MO_K_* */
    uint8_t root;                          /* the pitch class: the chord's root, the interval's lower note, the key's */
    uint8_t qual, ivl;                     /* MO_Q_*; the interval in semitones 0..12 */
    uint8_t n, count;                      /* notes in the phrase; REPEAT: the times, CLUSTER: the notes together */
    uint8_t rich;                          /* 0..10 */
    uint8_t fast, slow, loud, soft, same, more;   /* same: how many times in a row (0: new) */
    uint8_t ctl;                           /* a control's comment: MO_CT_* + 1 (its name the $THING) */
    uint16_t sig;                          /* what it was (the same again: the same signature) */
} mo_heard_t;

static struct {
    uint8_t on;                            /* MOPE running (the mode) */
    uint8_t ev_r;                          /* the audio's events read */
    mo_note_t p[MO_PN];                    /* the phrase */
    uint8_t np, nheld;
    uint32_t held[4];
    uint32_t t_last, t_start;              /* the last key event, the phrase's first */
    mo_heard_t h, prev;                    /* the phrase heard last, the one before */
    uint8_t have_prev, have_h;
    int32_t gloom;
    uint32_t t_interest, t_heard;          /* (the screen: something rich; when it heard) */
    uint32_t rnd;
    uint16_t stamp[MO_NFRAG];              /* the line each fragment was used in (0: never) */
    uint16_t lines;
    uint32_t lhash[8];
    uint8_t last_cls;
    uint16_t line[MO_LMAX];                /* the line: word ids, punctuation << 12 */
    uint8_t nline, line_cls;
    uint8_t pend, req, intr;               /* a phrase to comment on; a line asked for (its class + 1: PRAISE, POKE,
                                              HELLO); a key in its sulk */
    uint8_t nsilent;                       /* SILENCE lines since the last key */
    uint8_t ctl;                           /* a control touched (MO_CT_* + 1), when last */
    uint32_t t_ctl;
    uint64_t seen;                         /* bit k: control k has had its first comment */
    uint8_t act, aphase;                   /* an activity (MO_AC_*), its phase: 0 its line, 1 under way */
    uint8_t target, ask_hit, ask_wrong, nwrong;   /* ASK: the control asked for; it came, another came, how often */
    uint8_t gpos, ghit, gow, ghits, gmiss;        /* GAME: the light's pad; a hit, a miss to answer; the counts */
    uint8_t pose;                          /* MO_P_* (the screen) */
    uint32_t t_act, t_lastact, t_wrong, t_gmove, t_ow, t_pose;
    uint32_t t_touch;                      /* the last touch sound */
    uint16_t w_oh, w_hmm, w_ooh, w_oo, w_um;   /* (its words) */
    uint8_t reply_due, reply_on, cut;      /* its reply after a line: due, playing; a key cut it short */
    uint32_t t_pend;
    uint8_t busy_was;
    uint32_t t_quiet, t_said;              /* when the last line ended, when it began */
    uint8_t state;
    uint8_t q0, qn, qword[64];             /* the line in the say queue: its first entry, how many, each one's word */
    uint8_t seq_root;
} mo;

static void mo_pose_next(uint32_t now);
static int mo_asked(uint32_t id);
static uint32_t mo_busy(void);

/* ---------------------------------------------------------- the movers --- */
/* a wheel, a fader, a pot: a movement only when it leaves a dead band around its settled value by more than its noise
 * and stays out for MO_MOVE_MS (then the band is centred on it again); within the band the centre follows slowly
 * (drift, 1/8 every 50 ms): ADC jitter and drift never count, a hand always does. Returns +1 / -1 (its direction),
 * else 0 */
#define MO_MOVE_MS 40u
typedef struct { int32_t ref; uint32_t t_out, t_follow; uint8_t init; } mo_mv_t;
static int32_t mo_move(mo_mv_t *m, int32_t raw, int32_t band, uint32_t now)
{
    int32_t d;
    if (!m->init) {
        m->ref = raw * 16;
        m->init = 1;
        m->t_out = 0;
        m->t_follow = now;
        return 0;
    }
    d = raw - m->ref / 16;
    if (d <= band && d >= -band) {
        m->t_out = 0;
        if (now - m->t_follow >= 50u) {
            m->ref += (raw * 16 - m->ref) / 8;
            m->t_follow = now;
        }
        return 0;
    }
    if (!m->t_out)
        m->t_out = now | 1u;
    if ((int32_t)(now - m->t_out) < (int32_t)MO_MOVE_MS)
        return 0;
    m->ref = raw * 16;
    m->t_out = 0;
    m->t_follow = now;
    return d > 0 ? 1 : -1;
}

/* ------------------------------------------------------------- listener --- */
static uint32_t mo_popc(uint32_t m)
{
    uint32_t n = 0;
    for (; m; m &= m - 1u)
        n++;
    return n;
}
static uint32_t mo_rot(uint32_t m, uint32_t r) { return r ? ((m >> r) | (m << (12u - r))) & 0xFFFu : m; }

/* a set of pitch classes (bit k: pitch class k) as a chord: 1 and its root and quality, the bass preferred as the root
 * (C6 = A MINOR SEVENTH: the templates have no sixths) */
static uint32_t mo_chord(uint32_t mask, uint32_t bass, uint8_t *root, uint8_t *qual)
{
    static const uint16_t T[14][2] = {
        {0x091, MO_Q_MAJ}, {0x089, MO_Q_MIN}, {0x049, MO_Q_DIM}, {0x111, MO_Q_AUG}, {0x0A1, MO_Q_SUS}, {0x085, MO_Q_SUS},
        {0x491, MO_Q_DOM7}, {0x891, MO_Q_MAJ7}, {0x489, MO_Q_MIN7}, {0x449, MO_Q_HDIM}, {0x249, MO_Q_DIM7},
        {0x411, MO_Q_DOM7}, {0x811, MO_Q_MAJ7}, {0x409, MO_Q_MIN7}};   /* (the last three without their fifth) */
    uint32_t t, r, found = 0;
    for (t = 0; t < 14u; t++)
        for (r = 0; r < 12u; r++)
            if (mo_rot(mask, r) == T[t][0] && (!found || r == bass)) {
                *root = (uint8_t)r;
                *qual = (uint8_t)T[t][1];
                if (r == bass)
                    return 1;
                found = 1;
            }
    return found;
}

/* a phrase (n notes in the order they began, tend: its end, ms) -> what it was; prev: the phrase before (or 0) */
static void mo_analyse(const mo_note_t *p, uint32_t n, uint32_t tend, const mo_heard_t *prev, mo_heard_t *h)
{
    uint32_t i, j, best = 0, bi = 0, sum = 0, run = 1, brun = 1, bdir = 0, dir = 0, rs = 0, brs = 0, bre = 0;
    uint32_t rep = 0, rnote = 0, nioi = 0, ioi = 0, sig;
    for (i = 0; i < sizeof *h; i++)
        ((uint8_t *)h)[i] = 0;
    h->n = (uint8_t)n;
    if (!n)
        return;
    for (i = 0; i < n; i++) {
        uint32_t T = p[i].on + MO_HELD_MS, k = 0, c = 0;
        sum += p[i].vel;
        for (j = 0; j < n; j++)                            /* sounding together at T */
            k += p[j].on <= T && (p[j].off ? p[j].off : tend) > T;
        if (k > best)
            best = k, bi = i;
        for (j = 0; j < n; j++)                            /* (the same key: how often) */
            c += p[j].note == p[i].note;
        if (c > rep)
            rep = c, rnote = p[i].note;
        if (i) {
            int32_t d = (int32_t)p[i].note - (int32_t)p[i - 1u].note;
            uint32_t a = (uint32_t)(d < 0 ? -d : d), dd = d > 0 ? 1u : 2u;
            if (p[i].on - p[i - 1u].on >= 30u)             /* (not struck together) */
                ioi += p[i].on - p[i - 1u].on, nioi++;
            if (a >= 1u && a <= 2u && (run == 1u || dd == dir))
                run++;
            else if (a >= 1u && a <= 2u)
                run = 2, rs = i - 1u;                      /* (the direction turned) */
            else
                run = 1, rs = i;
            dir = dd;
            if (run > brun)
                brun = run, bdir = dir, brs = rs, bre = i;
        }
    }
    h->loud = n >= 2u && sum >= 110u * n;
    h->soft = n >= 2u && sum <= 40u * n;
    if (best <= 2u && brun >= 7u && (p[bre].on - p[brs].on) <= 60u * (brun - 1u)) {   /* a glissando: a fast run */
        h->kind = MO_K_GLISS;
        h->root = (uint8_t)(p[brs].note % 12u);
    } else if (best <= 2u && brun >= 5u && brun * 2u >= n) {   /* a scale (not held as a chord) */
        h->kind = bdir == 1u ? MO_K_UP : MO_K_DOWN;
        h->root = (uint8_t)(p[brs].note % 12u);
    } else if (best >= 2u) {                               /* together: an interval, a chord, a cluster */
        uint32_t T = p[bi].on + MO_HELD_MS, mask = 0, k = 0, lo = 127;
        for (j = 0; j < n; j++)
            if (p[j].on <= T && (p[j].off ? p[j].off : tend) > T) {
                k++;
                mask |= 1u << (p[j].note % 12u);
                lo = p[j].note < lo ? p[j].note : lo;
            }
        h->count = (uint8_t)k;
        if (mo_popc(mask) == 1u) {
            h->kind = MO_K_IVL;
            h->ivl = 12;
            h->root = (uint8_t)(lo % 12u);
        } else if (mo_popc(mask) == 2u) {
            uint32_t other = mask & ~(1u << (lo % 12u)), pc = 0;
            while (!(other >> pc & 1u))
                pc++;
            h->kind = MO_K_IVL;
            h->root = (uint8_t)(lo % 12u);
            h->ivl = (uint8_t)((pc + 12u - lo % 12u) % 12u);
        } else if (mo_chord(mask, lo % 12u, &h->root, &h->qual)) {
            h->kind = MO_K_CHORD;
        } else {
            h->kind = MO_K_CLUSTER;
            h->root = (uint8_t)(lo % 12u);
        }
    } else if (rep >= 4u && rep * 10u >= n * 7u) {         /* the same key again and again */
        h->kind = MO_K_REPEAT;
        h->count = (uint8_t)rep;
        h->root = (uint8_t)(rnote % 12u);
    } else if (n == 1u) {
        h->kind = MO_K_SINGLE;
        h->root = (uint8_t)(p[0].note % 12u);
    } else {
        h->kind = MO_K_TUNE;
        h->root = (uint8_t)(p[0].note % 12u);
    }
    if (nioi) {
        h->fast = n >= 8u && ioi < 120u * nioi && h->kind != MO_K_GLISS;
        h->slow = nioi >= 2u && ioi > 650u * nioi;
    }
    {   /* how rich it was */
        static const uint8_t QR[MO_NQ] = {1, 4, 7, 7, 5, 9, 9, 9, 9, 8};
        static const uint8_t IR[13] = {0, 3, 3, 2, 2, 1, 6, 0, 3, 3, 5, 5, 0};
        uint32_t r = 0, pcs = 0;
        for (i = 0; i < n; i++)
            pcs |= 1u << (p[i].note % 12u);
        switch (h->kind) {
        case MO_K_CHORD: r = QR[h->qual]; break;
        case MO_K_IVL: r = IR[h->ivl % 13u]; break;
        case MO_K_CLUSTER: r = 2; break;
        case MO_K_UP: case MO_K_DOWN: r = 3; break;
        case MO_K_GLISS: r = 2; break;
        case MO_K_TUNE: r = mo_popc(pcs) >= 6u ? 7u : mo_popc(pcs) >= 4u ? 4u : 2u; break;
        default: r = 0;
        }
        h->rich = (uint8_t)(r + (n >= 16u && r && r < 10u));
    }
    /* its signature: the kind, the root, the chord or interval; a tune its first notes, a scale its direction */
    sig = h->kind * 977u + h->root * 31u + (h->kind == MO_K_CHORD ? h->qual : h->kind == MO_K_IVL ? h->ivl : 0u) * 7u;
    if (h->kind == MO_K_TUNE)
        for (i = 0; i < n && i < 8u; i++)
            sig = sig * 33u + p[i].note;
    h->sig = (uint16_t)(sig ^ sig >> 16);
    if (prev) {
        h->same = (uint8_t)(prev->sig == h->sig && prev->n ? (prev->same < 20u ? prev->same + 1u : 20u) : 0u);
        h->more = !h->same && prev->n && ((n >= 6u && n >= 2u * prev->n) || h->rich >= prev->rich + 4u);
    }
}

/* the heard thing as the header shows it: C MAJOR, G7, FIFTH ON C, CLUSTER 5 (t: 24 characters) */
static const char *const MO_PC_NAME[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
static char *mo_cat(char *t, const char *s)
{
    while (*s)
        *t++ = *s++;
    *t = 0;
    return t;
}
static void mo_heard_text(const mo_heard_t *h, char *t)
{
    static const char *const Q[MO_NQ] = {" MAJOR", " MINOR", " DIM", " AUG", " SUS", "7", " MAJ7", " MIN7",
                                         " HALF DIM", " DIM7"};
    static const char *const I[13] = {"UNISON", "MIN 2ND", "MAJ 2ND", "MIN 3RD", "MAJ 3RD", "FOURTH", "TRITONE",
                                      "FIFTH", "MIN 6TH", "MAJ 6TH", "MIN 7TH", "MAJ 7TH", "OCTAVE"};
    const char *pc = MO_PC_NAME[h->root % 12u];
    char *q = t;
    t[0] = 0;
    switch (h->kind) {
    case MO_K_CHORD: mo_cat(mo_cat(q, pc), Q[h->qual % MO_NQ]); return;
    case MO_K_IVL: mo_cat(mo_cat(mo_cat(q, I[h->ivl % 13u]), " ON "), pc); return;
    case MO_K_CLUSTER:
    case MO_K_REPEAT:
        q = mo_cat(q, h->kind == MO_K_CLUSTER ? "CLUSTER " : pc);
        if (h->kind == MO_K_REPEAT)
            q = mo_cat(q, " x");
        if (h->count >= 10u)
            *q++ = (char)('0' + h->count / 10u % 10u);
        *q++ = (char)('0' + h->count % 10u);
        *q = 0;
        return;
    case MO_K_UP: mo_cat(q, "SCALE UP"); return;
    case MO_K_DOWN: mo_cat(q, "SCALE DOWN"); return;
    case MO_K_GLISS: mo_cat(q, "GLISSANDO"); return;
    case MO_K_TUNE: mo_cat(q, "MELODY"); return;
    case MO_K_SINGLE: mo_cat(q, pc); return;
    default: return;
    }
}

/* ------------------------------------------------------------- comments --- */
static uint32_t mo_rand(void)
{
    uint32_t x = mo.rnd ? mo.rnd : 0x9E3779B9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    mo.rnd = x;
    return x;
}
/* a fragment of class c: one not used within the class's window of lines (n - 1, at most MO_HIST), else the oldest */
static uint32_t mo_pick(uint32_t c)
{
    uint32_t a = MO_CLS[c], n = MO_CLS[c + 1u] - a, win = n > 1u ? (n - 1u < MO_HIST ? n - 1u : MO_HIST) : 0u;
    uint32_t i, best = a, bage = 0, f, age;
    for (i = 0; i < 2u * n + 2u; i++) {
        f = i < 6u ? a + mo_rand() % n : a + (i - 6u) % n;   /* (six at random, then each in turn) */
        age = mo.stamp[f] ? (uint16_t)(mo.lines - mo.stamp[f]) : 0xFFFFu;
        if (age >= win) {
            best = f;
            break;
        }
        if (age > bage)
            bage = age, best = f;
    }
    mo.stamp[best] = mo.lines;
    return best;
}
static void mo_put(uint32_t e)
{
    if (mo.nline < MO_LMAX)
        mo.line[mo.nline++] = (uint16_t)e;
}
/* fragment f as a slot's value: its words, the last with the punctuation pu */
static void mo_fval(uint32_t f, uint32_t pu)
{
    uint32_t k;
    for (k = MO_AT[f]; k < MO_AT[f + 1u]; k++)
        mo_put((MO_W[k] & 0xFFFu) | (k + 1u == MO_AT[f + 1u] ? pu << 12 : 0u));
}
/* value i of the value class c */
static void mo_val(uint32_t c, uint32_t i, uint32_t pu)
{
    mo_fval(MO_CLS[c] + (i < (uint32_t)(MO_CLS[c + 1u] - MO_CLS[c]) ? i : 0u), pu);
}
/* $THING: the control touched, or what was played (its chord, its interval, else what kind of thing it was) */
static void mo_thing(const mo_heard_t *h, uint32_t pu)
{
    if (h->ctl)
        mo_val(MO_C_CTL, h->ctl - 1u, pu);
    else if (h->kind == MO_K_CHORD)
        mo_val(MO_C_NOTE, h->root % 12u, 0), mo_val(MO_C_QUAL, h->qual, pu);
    else if (h->kind == MO_K_IVL)
        mo_val(MO_C_IVL, h->ivl, pu);
    else
        mo_val(MO_C_THING, h->kind, pu);
}
static void mo_frag(uint32_t f, const mo_heard_t *h)
{
    uint32_t k;
    for (k = MO_AT[f]; k < MO_AT[f + 1u]; k++) {
        uint32_t e = MO_W[k], w = e & 0xFFFu, pu = e >> 12;
        if (w < MO_SLOT) {
            mo_put(e);
            continue;
        }
        switch (w - MO_SLOT) {
        case 0: mo_val(MO_C_NOTE, h->root % 12u, pu); break;
        case 1: mo_val(MO_C_NOTE, h->root % 12u, 0); mo_val(MO_C_QUAL, h->qual, pu); break;
        case 2: mo_val(MO_C_IVL, h->ivl, pu); break;
        case 4: mo_fval(mo_pick(MO_C_ADJ), pu); break;
        case 5: mo_fval(mo_pick(MO_C_ASIDE), pu); break;
        case 6: mo_thing(h, pu); break;
        default: mo_val(MO_C_NUM, h->count < MO_CLS[MO_C_NUM + 1u] - MO_CLS[MO_C_NUM] - 1u ? h->count :
                        MO_CLS[MO_C_NUM + 1u] - MO_CLS[MO_C_NUM] - 1u, pu);
        }
    }
}

/* how a line is built (mo_line) */
enum { MO_L_PLAIN, MO_L_OBS, MO_L_CTL, MO_L_JOKE };
/* a line, fresh from the pools. PLAIN: the class's fragment. OBS (what was played) and CTL (a control): sometimes an
 * opener, the class's own fragment or a general template (TPL, CTPL), sometimes an aside on its own, a sigh, a closer
 * by the mood (not after a long line). JOKE: sometimes its run-up, the joke, sometimes its sad end. A line like one of
 * the last eight is built again (four times at most) */
static void mo_line(uint32_t c, const mo_heard_t *h0, uint32_t how)
{
    uint32_t tries, i, hs = 0;
    mo_heard_t h = *h0;
    for (tries = 0; tries < 4u; tries++) {
        uint32_t r = mo_rand() % 100u, r2 = mo_rand() % 100u, r3 = mo_rand() % 100u, conc = MO_NCLS;
        mo.nline = 0;
        if (how == MO_L_JOKE) {
            if (r < 55u)
                mo_frag(mo_pick(MO_C_JOKEIN), &h);
            mo_frag(mo_pick(MO_C_JOKE), &h);
            if (r2 < 60u)
                mo_frag(mo_pick(MO_C_JOKEOUT), &h);
        } else if (how == MO_L_PLAIN) {
            mo_frag(mo_pick(c), &h);
        } else {
            if (r < 35u)
                mo_frag(mo_pick(MO_C_OPEN), &h);
            if (r2 < (how == MO_L_CTL ? 35u : 40u)) {      /* a general template */
                if (h.kind != MO_K_REPEAT && h.kind != MO_K_CLUSTER)
                    h.count = (uint8_t)(mo_rand() % 4u + (mo.gloom < 40));   /* (I GIVE IT N OUT OF TEN) */
                mo_frag(mo_pick(how == MO_L_CTL ? MO_C_CTPL : MO_C_TPL), &h);
            } else
                mo_frag(mo_pick(c), &h);
            if (r3 < 20u)                                  /* an aside on its own */
                mo_fval(mo_pick(MO_C_ASIDE), 1u);
            if (mo.nline < MO_LSHORT) {
                uint32_t r4 = mo_rand() % 100u;
                if (h.rich >= 7u)
                    conc = c != MO_C_RICH && r4 < 50u ? MO_C_RICH : MO_NCLS;
                else if (mo.gloom >= 60)
                    conc = r4 < 25u ? MO_C_BORED : r4 < 40u ? MO_C_CLOSE : MO_NCLS;
                else if (h.rich >= 4u)
                    conc = r4 < 25u ? MO_C_KIND : MO_NCLS;
                else
                    conc = r4 < 18u ? MO_C_CLOSE : r4 < 30u ? MO_C_BORED : MO_NCLS;
                if ((mo_rand() % 100u) < (mo.gloom >= 60 ? 22u : 10u))
                    mo_frag(mo_pick(MO_C_SIGH), &h);
                if (conc < MO_NCLS)
                    mo_frag(mo_pick(conc), &h);
            }
        }
        hs = 2166136261u;
        for (i = 0; i < mo.nline; i++)
            hs = (hs ^ mo.line[i]) * 16777619u;
        for (i = 0; i < 8u && mo.lhash[i] != hs; i++)
            ;
        if (i == 8u)
            break;
    }
    mo.lhash[mo.lines % 8u] = hs;
    mo.lines++;
    if (!mo.lines)
        mo.lines = 1;
    mo.line_cls = (uint8_t)c;
}

/* the observation's class */
static uint32_t mo_cls_of(const mo_heard_t *h)
{
    static const uint8_t IV[13] = {MO_C_SINGLE, MO_C_SECOND, MO_C_SECOND, MO_C_THIRD, MO_C_THIRD, MO_C_FOURTH,
                                   MO_C_TRITONE, MO_C_FIFTH, MO_C_SIXTH, MO_C_SIXTH, MO_C_SEVENTHS, MO_C_SEVENTHS,
                                   MO_C_OCTAVE};
    static const uint8_t QC[MO_NQ] = {MO_C_MAJOR, MO_C_MINOR, MO_C_DIM, MO_C_AUG, MO_C_SUS, MO_C_SEVENTH, MO_C_SEVENTH,
                                      MO_C_SEVENTH, MO_C_SEVENTH, MO_C_SEVENTH};
    switch (h->kind) {
    case MO_K_IVL: return IV[h->ivl % 13u];
    case MO_K_CHORD: return QC[h->qual % MO_NQ];
    case MO_K_CLUSTER: return MO_C_CLUSTER;
    case MO_K_REPEAT: return MO_C_REPEAT;
    case MO_K_UP: return MO_C_UP;
    case MO_K_DOWN: return MO_C_DOWN;
    case MO_K_GLISS: return MO_C_GLISS;
    case MO_K_SINGLE: return MO_C_SINGLE;
    default: return MO_C_TUNE;
    }
}
/* what to say about it: the same again, more than before, how it was played (when the thing itself was said last
 * time), a rich tune, else the thing itself */
static uint32_t mo_choose(mo_heard_t *h)
{
    uint32_t c = mo_cls_of(h), r = mo_rand() % 100u, alt = h->fast ? MO_C_FAST : h->slow ? MO_C_SLOW :
                 h->loud ? MO_C_LOUD : h->soft ? MO_C_SOFT : MO_NCLS;
    if (h->same && (r < 65u || c == mo.last_cls)) {
        h->count = (uint8_t)(h->same + 1u);
        return MO_C_SAME;
    }
    if (h->more && r < 45u)
        return MO_C_MORE;
    if (h->kind == MO_K_TUNE && h->rich >= 7u && r < 50u)
        return MO_C_RICH;
    if (alt < MO_NCLS && (c == mo.last_cls || r < 30u))
        return alt;
    return c;
}

/* the mood after a phrase */
static void mo_mood(const mo_heard_t *h, uint32_t now)
{
    int32_t d = h->rich <= 1u ? 14 : h->rich <= 3u ? 6 : h->rich <= 6u ? -6 : -22;
    d += h->same ? 10 : 0;
    d -= h->more ? 5 : 0;
    d += h->kind == MO_K_REPEAT ? 6 : 0;
    mo.gloom = sp_clampi(mo.gloom + d, 0, 120);
    if (h->rich >= 7u)
        mo.t_interest = now | 1u;
}

/* ---------------------------------------------------------------- input --- */
static void mo_reset(uint32_t now, uint32_t seed)
{
    uint32_t i;
    for (i = 0; i < sizeof mo; i++)
        ((uint8_t *)&mo)[i] = 0;
    mo.rnd = seed | 1u;
    mo.gloom = 40;
    mo.t_last = mo.t_quiet = mo.t_said = now;
    mo.lines = 1;
    mo.state = MO_LISTEN;
    mo.seq_root = 9;
}
/* a key or MIDI note: on (vel) or off, at t ms */
static void mo_note(uint32_t note, uint32_t on, uint32_t vel, uint32_t t)
{
    uint32_t i, w = (note & 127u) >> 5, b = 1u << (note & 31u);
    if (on) {
        if (mo.state != MO_LISTEN)
            mo.intr = 1;                                   /* (a key in its sulk: it stops) */
        if (!mo.np)
            mo.t_start = t;
        if (mo.np < MO_PN) {
            mo.p[mo.np].note = (uint8_t)note;
            mo.p[mo.np].vel = (uint8_t)vel;
            mo.p[mo.np].on = t;
            mo.p[mo.np].off = 0;
            mo.np++;
        }
        if (mo.reply_on)
            mo.cut = 1;                                    /* (a key: its reply stops) */
        if (!(mo.held[w] & b))
            mo.nheld++;
        mo.held[w] |= b;
        mo.nsilent = 0;
        mo_pose_next(t);
        mo_asked(MO_CT_KEY);
    } else {
        for (i = mo.np; i-- > 0;)
            if (mo.p[i].note == note && !mo.p[i].off) {
                mo.p[i].off = t > mo.p[i].on ? t : mo.p[i].on + 1u;
                break;
            }
        if (mo.held[w] & b)
            mo.nheld--;
        mo.held[w] &= ~b;
    }
    mo.t_last = t;
}
/* a press of anything: the robot in another pose (never the same twice in a row, at most every 0.4 s) */
static void mo_pose_next(uint32_t now)
{
    if (mo.t_pose && (int32_t)(now - mo.t_pose) < 400)
        return;
    mo.pose = (uint8_t)((mo.pose + 1u + mo_rand() % (MO_NPOSE - 1u)) % MO_NPOSE);
    mo.t_pose = now | 1u;
}
/* an answer to ASK (the control used: the one asked for, or another) */
static int mo_asked(uint32_t id)
{
    if (mo.act != MO_AC_ASK || mo.aphase != 1u)
        return 0;
    if (id == mo.target)
        mo.ask_hit = 1;
    else
        mo.ask_wrong = 1;
    return 1;
}
static void mo_praise(void) { if (!mo.act) mo.req = MO_C_PRAISE + 1u; else mo_asked(MO_CT_REC); }
static void mo_poke(void) { if (!mo.act) mo.req = MO_C_POKE + 1u; else mo_asked(MO_CT_PLAY); }
static void mo_joke(void) { if (!mo.act) mo.req = MO_REQ_JOKE; else mo_asked(MO_CT_ARROW); }
/* a touch sound (the audio plays it, sp_mope_snd.c): a word (or 0xFFFF a blip) at note, sliding by slide, after dly
 * blocks */
static void mo_tq(uint32_t w, int32_t note, int32_t slide, uint32_t vel, uint32_t dly)
{
    uint8_t q = mos.tq_w;
    if ((uint8_t)(q - mos.tq_r) >= MO_TQ || w == 0xFFFEu)
        return;
    mos.tq[q % MO_TQ].w = (uint16_t)w;
    mos.tq[q % MO_TQ].note = (uint8_t)sp_clampi(note, 24, 96);
    mos.tq[q % MO_TQ].slide = (int8_t)slide;
    mos.tq[q % MO_TQ].vel = (uint8_t)vel;
    mos.tq[q % MO_TQ].dly = (uint8_t)dly;
    RING_PUBLISH();
    mos.tq_w = (uint8_t)(q + 1u);
}
/* a word's id by its name (0xFFFE: none) */
static uint32_t mo_wid(const char *n)
{
    uint32_t i, j;
    for (i = SP_NLETTER; i < SP_NWORD; i++) {
        for (j = 0; n[j] && SP_WNAME[i][j] == n[j]; j++)
            ;
        if (!n[j] && !SP_WNAME[i][j] && !(SP_WORD[i].flags & (SP_W_SING | SP_W_DE)))
            return i;
    }
    return 0xFFFEu;
}
/* the little sound of a control touched in MOPE, by its family: a knob a soft OH or HMM up or down with its turn, a
 * fader an OOH rising or falling, a wheel a bending OO, a button a P or a T, sometimes P-P-P, an UM, a sigh's SH, a pad
 * a blip on its note; quiet, at most one every 0.22 s (a knob turned on: not every detent), none while it talks */
#define MO_TOUCH_MS 220u
static void mo_touch(uint32_t id, int32_t dir, uint32_t now)
{
    static const uint8_t PENTA[16] = {57, 60, 62, 64, 67, 69, 72, 74, 76, 79, 81, 84, 86, 88, 91, 93};
    const uint32_t r = mo_rand() % 100u;
    if (!mo.on || mo_busy() || mos.seq_on || (mo.t_touch && now - mo.t_touch < MO_TOUCH_MS))
        return;
    if (!mo.w_oh) {                                        /* (the words, once) */
        mo.w_oh = (uint16_t)mo_wid("OH"), mo.w_hmm = (uint16_t)mo_wid("HMM"), mo.w_ooh = (uint16_t)mo_wid("OOH");
        mo.w_oo = (uint16_t)mo_wid("OO"), mo.w_um = (uint16_t)mo_wid("UM");
    }
    mo.t_touch = now | 1u;
    if (id >= 0x100u) {                                    /* a pad: a blip */
        mo_tq(0xFFFFu, PENTA[(id - 0x100u) & 15u], 0, 70, 0);
        return;
    }
    if (id <= MO_CT_K8 || (id >= MO_CT_FK1 && id <= MO_CT_FK4)) {   /* a knob: OH / HMM, up or down */
        int32_t d = dir ? dir : (r & 1u ? 1 : -1);
        mo_tq(r < 60u ? mo.w_oh : mo.w_hmm, 57 + 3 * d, 2 * d, 34, 0);
    } else if (id <= MO_CT_F4)                             /* a fader: OOH rising or falling */
        mo_tq(mo.w_ooh, 55 + (dir < 0 ? 4 : 0), dir < 0 ? -5 : 5, 34, 0);
    else if (id <= MO_CT_MODW)                             /* a wheel: a bending OO */
        mo_tq(mo.w_oo, 52, dir < 0 ? -4 : 4, 34, 0);
    else if (id == MO_CT_KEY)
        return;
    else if (r < 45u)                                      /* a button: a click */
        mo_tq(SP_KIT[r & 1u ? 3u : 5u], SP_KIT_NOTE[r & 1u ? 3u : 5u], 0, 30, 0);
    else if (r < 70u) {                                    /* .. a p-p-p stutter */
        mo_tq(SP_KIT[3], SP_KIT_NOTE[3], 0, 26, 0);
        mo_tq(SP_KIT[3], SP_KIT_NOTE[3], 0, 22, 90);
        mo_tq(SP_KIT[3], SP_KIT_NOTE[3], 0, 18, 90);
    } else if (r < 88u)                                    /* .. a mumble */
        mo_tq(mo.w_um, 50, -2, 30, 0);
    else                                                   /* .. a sigh's breath */
        mo_tq(SP_KIT[11], SP_KIT_NOTE[11], 0, 24, 0);
}
/* a control touched (MO_CT_*), at now ms, moved dir (-1, +1; 0 unknown): its little sound at once, its comment once
 * it is left alone (ASK: an answer; the other activities: nothing) */
static void mo_ctl_dir(uint32_t id, uint32_t now, int32_t dir)
{
    if (!mo.on || id >= MO_NCT)
        return;
    mo_touch(id, dir, now);
    mo_pose_next(now);
    if (mo_asked(id) || mo.act)
        return;
    mo.ctl = (uint8_t)(id + 1u);
    mo.t_ctl = now;
}
static void mo_ctl(uint32_t id, uint32_t now) { mo_ctl_dir(id, now, 0); }
/* GAME: a pad (the FM-1: a key) pressed */
static void mo_game_press(uint32_t k, uint32_t now)
{
    mo_pose_next(now);
    if (mo.act != MO_AC_GAME || mo.aphase != 1u)
        return;
    if (k == mo.gpos)
        mo.ghit = 1;
    else
        mo.gow = 1, mo.gmiss++;
}
/* a pad (SMK): the game's, ASK's answer, or praise (1..8), a poke (9..12), a joke (13..16) */
static void mo_pad_in(uint32_t k, uint32_t now)
{
    k &= 15u;
    mo_touch(0x100u + k, 0, now);                          /* (its blip) */
    if (mo.act == MO_AC_GAME) {
        mo_game_press(k, now);
        return;
    }
    mo_pose_next(now);
    if (mo_asked(MO_CT_PAD) || mo.act)
        return;
    if (k < 8u)
        mo_praise();
    else if (k < 12u)
        mo_poke();
    else
        mo_joke();
}
/* ASK's line for its control: turn a knob, move a fader or a wheel, press anything else */
static uint32_t mo_ask_cls(uint32_t id)
{
    if (id <= MO_CT_K8 || (id >= MO_CT_FK1 && id <= MO_CT_FK4))
        return MO_C_ASKTURN;
    return id <= MO_CT_MODW ? MO_C_ASKMOVE : MO_C_ASKPRESS;
}

/* the sulk's piece, a new one each time (mo_rand): a minor key on root, one of six progressions (i iv V i, i VI III
 * VII, i VII VI V, i iv i V, i VI iv V, i ii V i: four bars, then home), one of three styles (a LAMENT in 4/4 over a
 * plucked bass a quarter a note, a WALTZ in 3/4 with its bass on one and the chord after, RAIN in 4/4 over a soft
 * eighth-note arpeggio), 56 .. 86 BPM, its melody's register and contour (falling, an arch, low and wandering) and
 * rhythms its own; on a chord's note at each bar's start, the leading tone over V; home at the end (mos.seq) */
static void mo_compose(uint32_t root)
{
    static const int8_t SC[7] = {0, 2, 3, 5, 7, 8, 10};                       /* the natural minor */
    static const uint8_t PROG[6][4] = {{0, 3, 4, 0}, {0, 5, 2, 6}, {0, 6, 5, 4}, {0, 3, 0, 4}, {0, 5, 3, 4},
                                       {0, 1, 4, 0}};                          /* (scale degrees) */
    static const uint8_t R4[6][5] = {{4, 4, 4, 4, 0}, {4, 2, 2, 8, 0}, {8, 4, 4, 0, 0}, {6, 2, 8, 0, 0},
                                     {2, 2, 4, 8, 0}, {12, 4, 0, 0, 0}};       /* (4/4: sixteenths a note) */
    static const uint8_t R3[4][4] = {{4, 4, 4, 0}, {6, 2, 4, 0}, {8, 4, 0, 0}, {4, 8, 0, 0}};   /* (3/4) */
    const uint32_t style = mo_rand() % 3u, prog = mo_rand() % 6u, contour = mo_rand() % 3u;
    const uint32_t bar = style == 1u ? 12u : 16u, tempo = 56u + mo_rand() % 31u;
    const int32_t tonic = 57 + (int32_t)((root % 12u + 3u) % 12u) + (mo_rand() % 2u ? 12 : 0);   /* (A3 .. G#5) */
    const int32_t bass = tonic - 24 - (tonic >= 69 ? 12 : 0);
    int32_t si = contour == 2u ? 7 : 11;
    static const uint8_t LA[4] = {0, 2, 1, 2}, RA[8] = {0, 1, 2, 1, 0, 2, 1, 2};
    uint32_t n = 0, b, k, at;
#define MO_SEQ(a, nt, ln, vl, t)                                                                                  \
    do {                                                                                                          \
        if (n < MO_SEQN) {                                                                                        \
            mos.seq[n].at = (uint16_t)(a), mos.seq[n].note = (uint8_t)(nt), mos.seq[n].len = (uint8_t)(ln);        \
            mos.seq[n].vel = (uint8_t)(vl), mos.seq[n].tk = (uint8_t)(t);                                          \
            n++;                                                                                                  \
        }                                                                                                         \
    } while (0)
    for (b = 0; b < 4u; b++) {
        const uint32_t d = PROG[prog][b], b0 = b * bar;
        int32_t ct[3];
        const uint8_t *r = style == 1u ? R3[mo_rand() % 4u] : R4[mo_rand() % 6u];
        for (k = 0; k < 3u; k++) {                         /* the chord's notes (V: the leading tone) */
            uint32_t q = d + 2u * k;
            ct[k] = SC[q % 7u] + 12 * (int32_t)(q / 7u);
            if (d == 4u && q % 7u == 6u)
                ct[k]++;
        }
        if (style == 0u)                                   /* LAMENT: a quarter a note, root fifth third fifth */
            for (k = 0; k < 4u; k++)
                MO_SEQ(b0 + k * 4u, bass + ct[LA[k]], 4, k ? 60 : 76, MO_TK_BASS);
        else if (style == 1u) {                            /* WALTZ: the root, then the chord twice */
            MO_SEQ(b0, bass + ct[0], 4, 78, MO_TK_BASS);
            for (k = 1; k < 3u; k++) {
                MO_SEQ(b0 + k * 4u, bass + 12 + ct[1], 3, 50, MO_TK_BASS);
                MO_SEQ(b0 + k * 4u, bass + 12 + ct[2], 3, 46, MO_TK_BASS);
            }
        } else                                             /* RAIN: an eighth-note arpeggio, softly */
            for (k = 0; k < 8u; k++)
                MO_SEQ(b0 + k * 2u, bass + 12 + ct[RA[k]] + (k == 5u ? 12 : 0), 3, k ? 44 : 58, MO_TK_BASS);
        for (at = b0, k = 0; k < 5u && r[k] && at < b0 + bar; at += r[k], k++) {
            int32_t pc, j;
            if (!k) {                                      /* a chord's note at the bar's start: the nearest one */
                static const int8_t OFF[5] = {0, -1, 1, -2, 2};
                for (j = 0; j < 5; j++) {
                    int32_t p2 = SC[(((si + OFF[j]) % 7) + 7) % 7];
                    if (p2 == ct[0] % 12 || p2 == ct[1] % 12 || p2 == ct[2] % 12 || (d == 4u && p2 == 10))
                        break;
                }
                si += j < 5 ? OFF[j] : 0;
            } else {                                       /* the contour: falling, an arch, low and wandering */
                static const int8_t FALL[8] = {-1, -1, -2, 0, 1, -1, 2, -1}, WANDER[6] = {-1, 1, 0, -1, 1, -2};
                int32_t st = contour == 2u ? WANDER[mo_rand() % 6u] : FALL[mo_rand() % 8u];
                if (contour == 1u)
                    st = b < 2u ? (st < 0 ? -st : st) : (st > 0 ? -st : st);
                si += st;
            }
            si = sp_clampi(si, contour == 2u ? 3 : 4, contour == 2u ? 10 : 13);
            pc = SC[si % 7];
            if (d == 4u && pc == 10)
                pc = 11;
            MO_SEQ(at, tonic - 12 + 12 * (si / 7) + pc, r[k], 80 + (k == 0 ? 10 : 0) - (style == 2u ? 8 : 0),
                   MO_TK_SAD);
        }
    }
    MO_SEQ(4u * bar, bass, bar, 78, MO_TK_BASS);           /* home: the tonic, its fifth, the melody's tonic */
    MO_SEQ(4u * bar + 2u, bass + 7, bar - 2u, 54, MO_TK_BASS);
    MO_SEQ(4u * bar, tonic, bar, 90, MO_TK_SAD);
#undef MO_SEQ
    mos.seq_n = (uint8_t)n;
    mos.sixteenth = SP_FS * 60u / tempo / 4u;
}

/* its reply after a line: three or four notes of the minor key of root, falling (rich: rising first), the last one
 * longer over the bass's tonic (mos.seq) */
static void mo_reply(uint32_t root, uint32_t rich)
{
    static const int8_t SC[7] = {0, 2, 3, 5, 7, 8, 10};
    static const int8_t P[5][5] = {{11, 10, 9, 7, -1}, {9, 8, 7, -1, -1}, {7, 9, 8, 7, -1}, {12, 11, 9, -1, -1},
                                   {7, 9, 11, 14, 12}};
    const int8_t *f = P[rich >= 7u ? 4u : mo_rand() % 4u];
    int32_t tonic = 64 + (int32_t)((root % 12u + 8u) % 12u);
    uint32_t n = 0, k, at = 0;
    for (k = 0; k < 5u && f[k] >= 0; k++) {
        uint32_t last = k == 4u || f[k + 1u] < 0, len = last ? 6u : 2u;
        int32_t si = f[k];
        mos.seq[n].at = (uint16_t)at;
        mos.seq[n].note = (uint8_t)(tonic - 12 + 12 * (si / 7) + SC[si % 7]);
        mos.seq[n].len = (uint8_t)len;
        mos.seq[n].vel = (uint8_t)(last ? 88 : 76);
        mos.seq[n].tk = MO_TK_SAD;
        n++;
        if (last) {
            mos.seq[n].at = (uint16_t)at;
            mos.seq[n].note = (uint8_t)(tonic - 24);
            mos.seq[n].len = 6;
            mos.seq[n].vel = 64;
            mos.seq[n].tk = MO_TK_BASS;
            n++;
        }
        at += len;
    }
    mos.seq_n = (uint8_t)n;
    mos.sixteenth = SP_FS * 60u / 76u / 4u;
}

/* ------------------------------------------------------------ the steps --- */
/* the phrase has ended: what it was, the mood, a comment waiting */
static void mo_phrase_end(uint32_t now)
{
    mo_analyse(mo.p, mo.np, now, mo.have_prev ? &mo.prev : 0, &mo.h);
    mo.np = 0;
    mo_mood(&mo.h, now);
    mo.prev = mo.h;
    mo.have_prev = mo.have_h = 1;
    mo.t_heard = now | 1u;
    mo.seq_root = (uint8_t)(mo.h.root % 12u);
    mo.pend = 1;
    mo.t_pend = now;
}

/* an activity begins: its line (ASK: a control picked; GAME: the light placed) */
static void mo_start(uint32_t kind, uint32_t now)
{
    static const uint8_t IN[MO_NAC] = {0, MO_C_LIGHTS, MO_C_DARK, 0, MO_C_GAMEIN};
    mo_heard_t h;
    uint32_t i;
    for (i = 0; i < sizeof h; i++)
        ((uint8_t *)&h)[i] = 0;
    mo.act = (uint8_t)kind;
    mo.aphase = 0;
    mo.t_act = mo.t_lastact = now;
    mo.ask_hit = mo.ask_wrong = 0;
    mo.nwrong = 0;
    mo.ghit = mo.gow = 0;
    mo.ghits = mo.gmiss = 0;
    mo.reply_on = 0;
    if (kind == MO_AC_ASK) {
        mo.target = MO_ASKABLE[mo_rand() % sizeof MO_ASKABLE];
        h.ctl = (uint8_t)(mo.target + 1u);
        mo_line(mo_ask_cls(mo.target), &h, MO_L_PLAIN);
    } else
        mo_line(IN[kind % MO_NAC], &h, MO_L_PLAIN);
}
/* an activity's end: its last line */
static uint32_t mo_act_end(uint32_t c, uint32_t now, uint32_t stop)
{
    static const mo_heard_t none;
    mo_heard_t h = none;
    h.ctl = (uint8_t)(mo.target + 1u);
    mo.act = MO_AC_NONE;
    mo.t_lastact = mo.t_said = now;
    mo_line(c, &h, MO_L_PLAIN);
    mo.reply_due = 1;
    return MO_A_SAY | (stop ? MO_A_STOP : 0u);
}
/* the activity under way: its timing, its answers */
static uint32_t mo_act_step(uint32_t now, uint32_t busy)
{
    static const mo_heard_t none;
    mo_heard_t h = none;
    if (!mo.aphase) {                                      /* its line still being said */
        mo.ask_hit = mo.ask_wrong = mo.ghit = mo.gow = 0;
        if (!busy) {
            mo.aphase = 1;
            mo.t_act = mo.t_gmove = now;
            mo.gpos = (uint8_t)(mo_rand() % 16u);
        }
        return 0;
    }
    switch (mo.act) {
    case MO_AC_LIGHTS:
        return now - mo.t_act >= 4500u ? mo_act_end(MO_C_LIGHTSEND, now, 0) : 0u;
    case MO_AC_DARK:
        return now - mo.t_act >= 5000u ? mo_act_end(MO_C_DARKEND, now, 0) : 0u;
    case MO_AC_ASK:
        if (mo.ask_hit)
            return mo_act_end(MO_C_ANSWER, now, 1);
        if (mo.ask_wrong && !busy && now - mo.t_wrong >= 1500u) {   /* another control: a remark */
            mo.ask_wrong = 0;
            mo.t_wrong = now;
            if (++mo.nwrong >= 4u)
                return mo_act_end(MO_C_NOTRY, now, 0);
            h.ctl = (uint8_t)(mo.target + 1u);
            mo_line(MO_C_WRONG, &h, MO_L_PLAIN);
            mo.t_said = now;
            return MO_A_SAY;
        }
        if (now - mo.t_act >= MO_ASK_MS && !busy)
            return mo_act_end(MO_C_NOTRY, now, 0);
        return 0;
    default:                                               /* GAME */
        if (mo.ghit) {
            mo.ghit = 0;
            if (++mo.ghits >= 3u)
                return mo_act_end(MO_C_GAMEEND, now, 1);
            mo.gpos = (uint8_t)((mo.gpos + 5u + mo_rand() % 7u) % 16u);   /* (somewhere else) */
            mo.t_gmove = now;
            mo_line(MO_C_GAMEHIT, &h, MO_L_PLAIN);
            mo.t_said = now;
            return MO_A_STOP | MO_A_SAY;
        }
        if (mo.gow) {                                      /* a wrong one: it hurts */
            mo.gow = 0;
            if (now - mo.t_ow >= 300u) {
                mo.t_ow = now;
                mo_line(MO_C_OW, &h, MO_L_PLAIN);
                mo.t_said = now;
                return MO_A_STOP | MO_A_SAY;
            }
        }
        if (now - mo.t_act >= MO_GAME_MS || mo.gmiss >= 12u)
            return mo_act_end(MO_C_GAMEEND, now, 1);
        if (now - mo.t_gmove >= 600u) {                    /* the light wanders to a neighbour (2 rows of 8) */
            uint32_t r = mo.gpos / 8u, c = mo.gpos % 8u, m = mo_rand() % 4u;
            if (m == 0u)
                r ^= 1u;
            else if (m == 1u || c == 0u)
                c = c < 7u ? c + 1u : c - 1u;
            else
                c--;
            mo.gpos = (uint8_t)(r * 8u + c);
            mo.t_gmove = now;
        }
        return 0;
    }
}

/* a step of MOPE (the main loop, often): now ms; busy: words still being said; seq_done: its melody has rung out.
 * Returns MO_A_*: what the caller does (the line is in mo.line) */
static uint32_t mo_step(uint32_t now, uint32_t busy, uint32_t seq_done)
{
    static const mo_heard_t none;
    uint32_t act = 0;
    if (!busy && mo.busy_was)
        mo.t_quiet = now;
    mo.busy_was = (uint8_t)(busy != 0u);
    if (mo.np && ((!mo.nheld && now - mo.t_last >= MO_GAP_MS) || now - mo.t_start >= MO_LONG_MS || mo.np >= MO_PN))
        mo_phrase_end(now);
    if (mo.cut) {                                          /* a key over its reply: the reply stops */
        mo.cut = 0;
        if (mo.reply_on) {
            mo.reply_on = 0;
            act = MO_A_STOP;
        }
    }
    if (mo.intr || mo.req) {                           /* a key in its sulk, praise, a poke, a joke, hello: at once */
        uint32_t c = mo.req == MO_REQ_JOKE ? MO_C_JOKE : mo.req ? mo.req - 1u : MO_C_BACK;
        if (c == MO_C_PRAISE)
            mo.gloom = sp_clampi(mo.gloom - 30, 0, 120);
        if (mo.state != MO_LISTEN) {                       /* (its sulk interrupted) */
            act |= MO_A_STOP;
            mo.state = MO_LISTEN;
            mo.gloom = c == MO_C_PRAISE ? 20 : 45;
            mo.t_last = now;
        } else if (!mo.req) {
            mo.intr = 0;                                   /* (a key while it announced nothing) */
            return act;
        }
        mo.intr = 0;
        mo.req = 0;
        mo.pend = 0;
        mo.reply_on = 0;
        mo.t_said = now;
        if (c == MO_C_POKE && mo_rand() % 100u < 55u) {    /* (a poke: half the time one of its activities) */
            mo_start(1u + mo_rand() % 4u, now);
            return act | MO_A_STOP | MO_A_SAY;
        }
        mo_line(c, &none, c == MO_C_JOKE ? MO_L_JOKE : MO_L_PLAIN);
        mo.reply_due = 1;
        return act | MO_A_STOP | MO_A_SAY;
    }
    if (act)
        return act;
    if (mo.act && mo.state == MO_LISTEN) {                 /* an activity: nothing else meanwhile */
        mo.pend = 0;
        mo.ctl = 0;
        return mo_act_step(now, busy);
    }
    switch (mo.state) {
    case MO_LISTEN:
        if (mo.reply_on && seq_done)
            mo.reply_on = 0;
        if (mo.reply_due && !busy) {                       /* the line said: its music */
            mo.reply_due = 0;
            if (!mo.nheld && now - mo.t_last >= 250u) {
                mo_reply(mo.seq_root, mo.have_h ? mo.h.rich : 0u);
                mo.reply_on = 1;
                return MO_A_PLAY;
            }
        }
        if (mo.pend && now - mo.t_pend > MO_STALE_MS)
            mo.pend = 0;
        if (mo.ctl && (int32_t)(now - mo.t_ctl) > (int32_t)MO_STALE_MS)
            mo.ctl = 0;
        if (busy || mo.reply_on || mo.nheld)
            break;
        if (mo.pend && now - mo.t_last >= 250u && now - mo.t_quiet >= MO_REST_MS) {
            mo_heard_t h = mo.h;
            uint32_t c = mo_choose(&h);
            mo.pend = 0;
            mo_line(c, &h, MO_L_OBS);
            mo.last_cls = (uint8_t)mo_cls_of(&mo.h);
            mo.t_said = now;
            mo.reply_due = 1;
            return MO_A_SAY;
        }
        if (mo.ctl && !mo.pend && (int32_t)(now - mo.t_ctl) >= (int32_t)MO_CTL_MS && now - mo.t_quiet >= MO_REST_MS * 2u / 3u) {
            uint32_t id = mo.ctl - 1u, first = !(mo.seen >> id & 1u);   /* (seen: 64 bits) */
            mo.ctl = 0;
            mo.seen |= (uint64_t)1 << id;
            if (first || mo_rand() % 100u < 55u) {         /* (the first touch always, later ones about half) */
                mo_heard_t h = none;
                h.ctl = (uint8_t)(id + 1u);
                mo_line(MO_CT_CLS[id], &h, MO_L_CTL);
                mo.t_said = now;
                mo.reply_due = 1;
                return MO_A_SAY;
            }
        }
        if (!mo.np && !mo.pend && mo.gloom >= MO_SULK_AT && now - mo.t_last >= 1500u &&
            now - mo.t_quiet >= 1500u) {                   /* enough: it will play itself */
            mo_line(MO_C_SULK, &none, MO_L_PLAIN);
            mo.state = MO_ANNOUNCE;
            mo.t_said = now;
            return MO_A_SAY;
        }
        if (!mo.np && now - mo.t_said >= 20000u && now - mo.t_lastact >= 45000u && now - mo.t_last >= 5000u &&
            now - mo.t_last < 60000u) {                    /* somebody is there, nothing is said: now and then */
            mo.t_lastact = now;
            if (mo_rand() % 100u < 50u) {
                mo_start(1u + mo_rand() % 4u, now);
                mo.t_said = now;
                return MO_A_SAY;
            }
        }
        if (!mo.np && now - mo.t_last >= MO_SILENT_MS && now - mo.t_said >= MO_SILENT_MS &&
            mo.nsilent < 3u) {                             /* nobody plays (three times, then it waits) */
            uint32_t r = mo_rand() % 100u;
            mo.nsilent++;
            mo.gloom = sp_clampi(mo.gloom + 20, 0, 120);
            mo.t_said = now;
            if (r < 25u) {                                 /* (or an activity, for nobody) */
                mo_start(1u + mo_rand() % 4u, now);
                return MO_A_SAY;
            }
            if (r < 50u)                                   /* (or a joke to nobody) */
                mo_line(MO_C_JOKE, &none, MO_L_JOKE);
            else
                mo_line(MO_C_SILENCE, &none, MO_L_PLAIN);
            mo.t_said = now;
            mo.reply_due = 1;
            return MO_A_SAY;
        }
        break;
    case MO_ANNOUNCE:
        if (!busy && now - mo.t_said > 400u) {             /* said: now it plays */
            mo_compose(mo.seq_root);
            mo.state = MO_SULK;
            return MO_A_PLAY;
        }
        break;
    default:
        if (seq_done) {                                    /* played out */
            mo.state = MO_LISTEN;
            mo.gloom = 25;
            mo_line(MO_C_ENDSULK, &none, MO_L_PLAIN);
            mo.t_said = now;
            mo.t_last = now;
            return MO_A_SAY;
        }
    }
    return 0;
}

/* --------------------------------------------------------------- the glue --- */
/* MOPE's words still being said (the say queue, a voice of it sounding) */
static uint32_t mo_busy(void)
{
    uint32_t i;
    if (sp.say_r != sp.say_w || sp.say_wait)
        return 1;
    for (i = 0; i < SP_NV; i++)
        if (sp.v[i].on && sp.v[i].tag == SP_TAG_SAY && !sp.v[i].stop)
            return 1;
    return 0;
}
/* the line into the say queue, said in one go: each word at MOPE's note (the line's first a semitone up, a sentence's
 * last two down, a question's last three up), a short breath after a full stop (100 ms) or a comma (50 ms) */
static void mo_say_line(void)
{
    uint8_t w = sp.say_w;
    uint32_t i, k = 0;
    mo.q0 = w;
    for (i = 0; i < mo.nline && k + 2u <= 62u; i++) {
        uint32_t e = mo.line[i], pu = e >> 12, nt = !i ? 9u : pu == 1u ? 6u : pu == 3u ? 11u : 8u;
        sp.sayq[(uint8_t)(w + k) % 64u] = (uint16_t)((e & 0xFFFu) | nt << 12);
        mo.qword[k++] = (uint8_t)i;
        if (pu) {
            sp.sayq[(uint8_t)(w + k) % 64u] = (uint16_t)(SP_SAY_PAUSE | (pu == 2u ? 1u : 2u) << 12);
            mo.qword[k++] = 0xFF;
        }
    }
    mo.qn = (uint8_t)k;
    RING_PUBLISH();
    sp.say_w = (uint8_t)(w + k);
}
/* the word of the line being said now (-1: none) */
static int32_t mo_cur_word(void)
{
    uint8_t c = (uint8_t)(sp.say_r - mo.q0);
    if (!mo.nline || !c || c > mo.qn || !mo_busy())
        return -1;
    for (c--; c > 0u && mo.qword[c] == 0xFFu; c--)
        ;
    return mo.qword[c] == 0xFFu ? -1 : mo.qword[c];
}

/* the main loop: MOPE while the keys mode is MOPE (entered: its voice's note, HELLO; left: its words stopped) */
static void mo_drive(void)
{
    uint32_t now = sp.now / 44u, act;
    if (sp.mode != SP_MODE_MOPE) {
        if (mo.on) {
            mo.on = 0;
            sp.say_r = sp.say_w;
            sp.say_gain = 256;
            mos.seq_req = 2;
        }
        return;
    }
    if (!mo.on) {
        mo_reset(now, sp.now ^ 0x5EEDu);
        mo.on = 1;
        mo.ev_r = mos.ev_w;
        mo.req = MO_C_HELLO + 1u;
        sp.say_base = 55;                                  /* (G3: a recogniser's best) */
        sp.say_gain = 370;                                 /* (ROBOT: its words well above its music, 1.45 x) */
        sp.touch_gain = 110;                               /* (its touch sounds: quiet) */
    }
    while (mo.ev_r != mos.ev_w) {
        uint32_t k = mo.ev_r % 32u;
        RING_PUBLISH();
        if (mo.act == MO_AC_GAME && !MO_SMK && mos.ev_vel[k] && mos.ev_key[k] >= 5u && mos.ev_key[k] < 21u)
            mo_game_press(mos.ev_key[k] - 5u, now);       /* (the FM-1: the game on its first 16 keys) */
        else
            mo_note(mos.ev_note[k], mos.ev_vel[k] != 0u, mos.ev_vel[k], mos.ev_t[k] / 44u);
        mo.ev_r++;
    }
    {   /* the keys as they are held now (the audio's view): no lost key-up ever leaves a phrase open */
        uint32_t n = mo_popc(sp.held[0]) + mo_popc(sp.held[1] & 0x1Fu), j, i;
        for (j = 0; j < 4u; j++)
            n += mo_popc(sp.mheld[j]);
        if (!n && mo.nheld) {
            for (i = 0; i < mo.np; i++)
                if (!mo.p[i].off)
                    mo.p[i].off = now > mo.p[i].on ? now : mo.p[i].on + 1u;
            mo.held[0] = mo.held[1] = mo.held[2] = mo.held[3] = 0;
        }
        mo.nheld = (uint8_t)(n < 255u ? n : 255u);
    }
    act = mo_step(now, mo_busy(), mos.seq_done);
    if (act & MO_A_STOP) {
        mos.seq_req = 2;
        sp.say_r = sp.say_w;
        sp.say_wait = 0;
    }
    if (act & MO_A_SAY)
        mo_say_line();
    if (act & MO_A_PLAY) {
        mos.seq_done = 0;
        RING_PUBLISH();
        mos.seq_req = 1;
    }
    if (mos.seq_done && mo.state == MO_LISTEN && !mo.reply_on)
        mos.seq_done = 0;
}
