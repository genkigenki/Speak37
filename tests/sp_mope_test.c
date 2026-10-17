/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* MOPE on the host (src/sp_mope.c with the sound of sp_dsp.c and the words of build/gen/sp_vocab.h):
 *   - the listener: note sets and sequences named right: every interval (seconds to the octave, the tritone), the
 *     triads (major, minor, diminished, augmented, suspended) in every key and inversion, the five sevenths, a
 *     cluster, the same key again and again (counted), a scale up and down, a glissando, a single note, a tune; fast,
 *     slow, loud, soft; the same again (counted), more than before; legato overlaps are not chords
 *   - the grammar: every class has fragments, every word of every fragment is a word Speak37 says (in the vocabulary,
 *     frames to say), the slots' tables complete; 200 lines generated: no class's fragment again within its window,
 *     no line twice within eight, each line within MO_LMAX words
 *   - the rate limit: a comment only after a phrase has ended (MO_GAP_MS), never while a key is held, never within
 *     MO_REST_MS of the last line's end, at most one a phrase; a control: its first touch commented once left alone
 *   - the sulk: bored enough it announces itself, then plays (MO_A_PLAY, its piece in minor), played out it says so;
 *     a key during it: it stops (BACK); praise lowers the gloom; silence raises it
 *   - the activities: ASK (its line, a wrong control, the right one, nothing done; every board control asked for),
 *     GAME (the light to a neighbour, OW, three hits), DARK and LIGHTS (their time, their line); the poses (a new one
 *     every press, never twice in a row, all eight, not faster than 0.4 s); its pieces (24 all different, many tempos,
 *     all in the key's minor)
 *   - the touch sounds: each family its own (a knob OH / HMM by its way, a fader OOH, a wheel OO, a button a click,
 *     a pad a blip), one every 0.22 s at most, none while it talks, quiet, ended; the movers (mo_move): jitter and
 *     drift never a movement, a hand's move is; ASK's mod wheel not answered by its noise
 *   - the sound: the tones end, bounded, the reply and the piece are in the key's minor, the cost of the tone voices;
 *     its words louder than its music, not clipped
 *   - demos (build/sp_mope, or the directory given): mope_*.wav, MOPE commenting, a sulk, a joke */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t t_keys;
static uint8_t t_vel[64];
#define SP_KEYS() t_keys
#define SP_VEL(k) t_vel[k]
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include "../firmware/src/sp_dsp.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("mope: %-94s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* ---- the listener, by itself */
static mo_note_t P[64];
static uint32_t np;
static void pn(uint32_t note, uint32_t on, uint32_t off, uint32_t vel)
{
    P[np].note = (uint8_t)note, P[np].on = on, P[np].off = off, P[np].vel = (uint8_t)vel;
    np++;
}
/* notes struck together (a chord held 600 ms), spread 10 ms */
static mo_heard_t chord(const int *n, uint32_t k)
{
    mo_heard_t h;
    uint32_t i;
    np = 0;
    for (i = 0; i < k; i++)
        pn((uint32_t)n[i], 1000 + i * 10u, 1600, 90);
    mo_analyse(P, np, 3000, 0, &h);
    return h;
}
static mo_heard_t seq(const int *n, uint32_t k, uint32_t ioi, uint32_t dur, uint32_t vel)
{
    mo_heard_t h;
    uint32_t i;
    np = 0;
    for (i = 0; i < k; i++)
        pn((uint32_t)n[i], 1000 + i * ioi, 1000 + i * ioi + dur, vel);
    mo_analyse(P, np, 1000 + k * ioi + 2000, 0, &h);
    return h;
}

static void listener_tests(void)
{
    int ok = 1;
    uint32_t r, q, inv, i;
    char t[32];
    {   /* every interval from every root */
        for (r = 48; r < 60; r++)
            for (i = 1; i <= 12; i++) {
                int n[2] = {(int)r, (int)(r + i)};
                mo_heard_t h = chord(n, 2);
                ok &= h.kind == MO_K_IVL && h.ivl == i && h.root == r % 12u;
            }
        check("every interval (minor second .. octave) on every root: named, its lower note the root", ok);
    }
    {   /* triads in every key and inversion; sevenths */
        static const int8_t T[5][3] = {{0, 4, 7}, {0, 3, 7}, {0, 3, 6}, {0, 4, 8}, {0, 5, 7}};
        static const uint8_t TQ[5] = {MO_Q_MAJ, MO_Q_MIN, MO_Q_DIM, MO_Q_AUG, MO_Q_SUS};
        static const int8_t S[5][4] = {{0, 4, 7, 10}, {0, 4, 7, 11}, {0, 3, 7, 10}, {0, 3, 6, 10}, {0, 3, 6, 9}};
        static const uint8_t SQ[5] = {MO_Q_DOM7, MO_Q_MAJ7, MO_Q_MIN7, MO_Q_HDIM, MO_Q_DIM7};
        ok = 1;
        for (q = 0; q < 5u; q++)
            for (r = 0; r < 12u; r++)
                for (inv = 0; inv < 3u; inv++) {
                    int n[3];
                    mo_heard_t h;
                    for (i = 0; i < 3u; i++)
                        n[i] = 48 + (int)r + T[q][(i + inv) % 3u] + (i + inv >= 3u ? 12 : 0);
                    h = chord(n, 3);
                    /* (augmented: symmetric, the bass is its root; inverted sus4 is a sus2 on another root) */
                    if (q == 3u)
                        ok &= h.kind == MO_K_CHORD && h.qual == MO_Q_AUG && h.root == (uint32_t)n[0] % 12u;
                    else if (q == 4u)
                        ok &= h.kind == MO_K_CHORD && h.qual == MO_Q_SUS;
                    else
                        ok &= h.kind == MO_K_CHORD && h.qual == TQ[q] && h.root == r;
                }
        check("major, minor, diminished triads in every key and inversion; augmented, suspended", ok);
        ok = 1;
        for (q = 0; q < 5u; q++)
            for (r = 0; r < 12u; r++) {
                int n[4];
                mo_heard_t h;
                for (i = 0; i < 4u; i++)
                    n[i] = 48 + (int)r + S[q][i];
                h = chord(n, 4);
                ok &= h.kind == MO_K_CHORD && h.qual == SQ[q] && h.root == r && h.rich >= 8u;
            }
        {
            int n[4] = {43, 47, 50, 53};          /* G7 from G2 */
            mo_heard_t h = chord(n, 4);
            mo_heard_text(&h, t);
            printf("  G B D F: %s\n", t);
            ok &= !strcmp(t, "G7");
        }
        check("the five sevenths in every key (rich), G B D F is G7", ok);
    }
    {   /* a cluster, wrong notes */
        int n[5] = {60, 61, 62, 63, 64}, m[4] = {60, 64, 67, 61};
        mo_heard_t h = chord(n, 5), h2 = chord(m, 4);
        check("a cluster (five neighbours), a major triad with a semitone against it: CLUSTER",
              h.kind == MO_K_CLUSTER && h.count == 5u && h2.kind == MO_K_CLUSTER);
    }
    {   /* sequences */
        int up[8] = {60, 62, 64, 65, 67, 69, 71, 72}, dn[8] = {72, 71, 69, 67, 65, 64, 62, 60};
        int rep[7] = {60, 60, 60, 60, 60, 60, 60}, tune[6] = {60, 67, 64, 72, 62, 69}, one[1] = {57};
        int gl[12] = {60, 62, 64, 65, 67, 69, 71, 72, 74, 76, 77, 79};
        mo_heard_t a = seq(up, 8, 250, 200, 90), b = seq(dn, 8, 250, 200, 90), c = seq(rep, 7, 200, 100, 90);
        mo_heard_t d = seq(tune, 6, 300, 250, 90), e = seq(one, 1, 300, 250, 90), g = seq(gl, 12, 25, 20, 90);
        mo_heard_t f = seq(tune, 6, 80, 60, 90), s = seq(tune, 6, 800, 300, 90);
        mo_heard_t L = seq(tune, 6, 300, 250, 120), S = seq(tune, 6, 300, 250, 25);
        mo_heard_t lg = seq(up, 8, 250, 290, 90);        /* (legato: each note overlaps the next by 40 ms) */
        ok = a.kind == MO_K_UP && b.kind == MO_K_DOWN && c.kind == MO_K_REPEAT && c.count == 7u && c.root == 0u;
        ok &= d.kind == MO_K_TUNE && e.kind == MO_K_SINGLE && e.root == 9u && g.kind == MO_K_GLISS;
        ok &= !d.fast && !d.slow && s.slow && !s.fast && L.loud && S.soft && !d.loud && !d.soft && lg.kind == MO_K_UP;
        {
            int t8[10] = {60, 67, 64, 72, 62, 69, 65, 71, 60, 67};
            mo_heard_t ff = seq(t8, 10, 80, 60, 90);
            ok &= ff.fast;
        }
        (void)f;
        check("a scale up, down (legato too), a glissando, the same key 7 times, one note, a tune; fast, slow, loud, soft",
              ok);
    }
    {   /* change over time */
        int n[3] = {60, 64, 67}, big[9] = {60, 64, 67, 71, 62, 65, 69, 72, 74};
        mo_heard_t a = chord(n, 3), b, c, d;
        np = 0;
        for (i = 0; i < 3u; i++)
            pn((uint32_t)n[i], 1000 + i * 10u, 1600, 90);
        mo_analyse(P, np, 3000, &a, &b);
        mo_analyse(P, np, 3000, &b, &c);
        d = seq(big, 9, 200, 150, 90);
        mo_analyse(P, 0, 0, 0, &d);                   /* (an empty phrase: nothing) */
        np = 0;
        for (i = 0; i < 9u; i++)
            pn((uint32_t)big[i], 1000 + i * 200u, 1150 + i * 200u, 90);
        mo_analyse(P, np, 4000, &a, &d);
        check("the same chord again (counted: 1, 2), much more than before",
              b.same == 1u && c.same == 2u && !a.same && d.more && !d.same);
    }
}

/* ---- the grammar */
static void grammar_tests(void)
{
    uint32_t c, f, k, ok = 1, nw = 0, slots = 0;
    for (c = 0; c < MO_NCLS; c++)
        ok &= MO_CLS[c + 1u] > MO_CLS[c];
    for (f = 0; f < MO_NFRAG; f++)
        for (k = MO_AT[f]; k < MO_AT[f + 1u]; k++) {
            uint32_t w = MO_W[k] & 0xFFFu;
            if (w >= MO_SLOT) {
                ok &= w - MO_SLOT < 7u;
                slots++;
                continue;
            }
            ok &= w < SP_NWORD && SP_WORD[w].nfr > 0u && SP_WNAME[w][0] && strlen(SP_WNAME[w]) <= 11u;
            nw++;
        }
    ok &= MO_CLS[MO_C_NOTE + 1u] - MO_CLS[MO_C_NOTE] == 12u && MO_CLS[MO_C_QUAL + 1u] - MO_CLS[MO_C_QUAL] == MO_NQ &&
          MO_CLS[MO_C_IVL + 1u] - MO_CLS[MO_C_IVL] == 13u && MO_CLS[MO_C_CTL + 1u] - MO_CLS[MO_C_CTL] == MO_NCT &&
          MO_CLS[MO_C_THING + 1u] - MO_CLS[MO_C_THING] == 10u;
    printf("  %u classes, %u fragments, %u words, %u slots\n", MO_NCLS, MO_NFRAG, nw, slots);
    check("every class has fragments; every word sayable (in the vocabulary, frames, 11 letters); the slots' tables", ok);
}
/* 200 lines: no class's fragment within its window again, no line twice within eight, every word sayable */
static void generator_tests(void)
{
    static uint16_t last[MO_NFRAG];
    static uint32_t hs[200];
    uint32_t n, i, ok = 1, rep_frag = 0, rep_line = 0, maxw = 0, words = 0;
    mo_reset(0, 12345);
    for (i = 0; i < MO_NFRAG; i++)
        last[i] = 0;
    for (n = 1; n <= 200u; n++) {
        mo_heard_t h;
        uint32_t c, how, k, before[MO_NFRAG / 32u + 1u];
        memset(&h, 0, sizeof h);
        h.kind = (uint8_t)(1u + mo_rand() % 9u);
        h.root = (uint8_t)(mo_rand() % 12u);
        h.qual = (uint8_t)(mo_rand() % MO_NQ);
        h.ivl = (uint8_t)(1u + mo_rand() % 12u);
        h.count = (uint8_t)(mo_rand() % 30u);
        h.rich = (uint8_t)(mo_rand() % 11u);
        mo.gloom = (int32_t)(mo_rand() % 120u);
        how = n % 7u == 0u ? MO_L_CTL : n % 11u == 0u ? MO_L_JOKE : MO_L_OBS;
        if (how == MO_L_CTL) {
            h.ctl = (uint8_t)(1u + mo_rand() % MO_NCT);
            c = MO_CT_CLS[h.ctl - 1u];
        } else
            c = how == MO_L_JOKE ? MO_C_JOKE : mo_cls_of(&h);
        for (k = 0; k < MO_NFRAG / 32u + 1u; k++)
            before[k] = 0;
        for (k = 0; k < MO_NFRAG; k++)
            if (mo.stamp[k] == mo.lines)
                before[k / 32u] |= 1u << (k % 32u);
        mo_line(c, &h, how);
        for (k = 0; k < MO_NFRAG; k++)                  /* the fragments this line used: when they were used last */
            if (mo.stamp[k] == mo.lines - 1u) {
                uint32_t cl = 0, cn, win;
                while (!(k >= MO_CLS[cl] && k < MO_CLS[cl + 1u]))
                    cl++;
                cn = MO_CLS[cl + 1u] - MO_CLS[cl];
                win = cn > 1u ? (cn - 1u < MO_HIST ? cn - 1u : MO_HIST) : 0u;
                if (last[k] && n - last[k] < win)
                    rep_frag++, printf("  fragment %u (class %u) again after %u lines\n", k, cl, n - last[k]);
                last[k] = (uint16_t)n;
            }
        hs[n - 1u] = 2166136261u;
        for (k = 0; k < mo.nline; k++) {
            uint32_t w = mo.line[k] & 0xFFFu;
            ok &= w < SP_NWORD && SP_WORD[w].nfr > 0u;
            hs[n - 1u] = (hs[n - 1u] ^ mo.line[k]) * 16777619u;
        }
        for (k = n > 8u ? n - 8u : 1u; k < n; k++)
            rep_line += hs[k - 1u] == hs[n - 1u];
        maxw = mo.nline > maxw ? mo.nline : maxw;
        words += mo.nline;
        if (n <= 12u || n == 77u || n == 121u) {         /* a few of them */
            char t[400], *q = t;
            for (k = 0; k < mo.nline && q < t + 340; k++) {
                const char *nm = SP_WNAME[mo.line[k] & 0xFFFu];
                uint32_t pu = mo.line[k] >> 12;
                while (*nm)
                    *q++ = *nm++;
                if (pu)
                    *q++ = pu == 2u ? ',' : pu == 3u ? '?' : '.';
                *q++ = ' ';
            }
            *q = 0;
            printf("  %3u: %s\n", n, t);
        }
        (void)before;
    }
    printf("  200 lines: %u words (%.1f a line, at most %u)\n", words, words / 200.0, maxw);
    check("200 lines: no fragment again within its pool's window, no line twice within 8, every word sayable",
          ok && !rep_frag && !rep_line && maxw <= MO_LMAX);
}

/* ---- the core's timing, with the speech simulated (a word 300 ms) */
static uint32_t sim_until;
static uint32_t sim_step(uint32_t now)
{
    uint32_t a = mo_step(now, now < sim_until, 0);
    if (a & MO_A_SAY)
        sim_until = now + mo.nline * 300u;
    return a;
}
static void timing_tests(void)
{
    uint32_t now, says = 0, ok = 1, phr = 0, t_end_last = 0, a, held = 0;
    int32_t gap_min = 1 << 30;
    mo_reset(0, 777);
    sim_until = 0;
    mo.t_quiet = 0;
    /* 30 short phrases, 2 s apart: C major struck, held 300 ms */
    for (now = 1000; now < 70000u; now += 10u) {
        uint32_t ph = (now - 1000u) % 2000u;
        if (ph == 0u && phr < 30u) {
            mo_note(60, 1, 90, now), mo_note(64, 1, 90, now), mo_note(67, 1, 90, now);
            held = 1;
            phr++;
        }
        if (ph == 300u && held) {
            mo_note(60, 0, 0, now), mo_note(64, 0, 0, now), mo_note(67, 0, 0, now);
            held = 0;
        }
        a = sim_step(now);
        if (a & MO_A_PLAY)
            mo.reply_on = 0;                          /* (the reply plays out at once here) */
        if (a & MO_A_SAY) {
            says++;
            ok &= !mo.nheld;                          /* never while a key is held */
            if (says > 1u)
                gap_min = (int32_t)(now - t_end_last) < gap_min ? (int32_t)(now - t_end_last) : gap_min;
        }
        if (now == sim_until)
            t_end_last = now;
    }
    printf("  30 phrases in 60 s: %u lines, the shortest rest between two %d ms\n", says, gap_min);
    check("rate limit: lines only with no key held, MO_REST_MS from the last one's end, fewer than the phrases",
          ok && says >= 4u && says < 30u && gap_min >= (int32_t)MO_REST_MS - 20);
    /* a phrase ends only after MO_GAP_MS */
    mo_reset(0, 99);
    sim_until = 0;
    mo.t_quiet = 0;
    mo_note(60, 1, 90, 10000);
    mo_note(60, 0, 0, 10200);
    ok = 1;
    for (now = 10200; now < 10200u + MO_GAP_MS - 10u; now += 10u)
        ok &= !(sim_step(now) & MO_A_SAY) && !mo.pend;
    for (; now < 13000u && !mo.have_h; now += 10u)
        sim_step(now);
    check("a phrase ends MO_GAP_MS after the last key, not before", ok && mo.have_h && mo.h.kind == MO_K_SINGLE);
    /* a control: its comment once left alone */
    mo_reset(0, 5);
    mo.on = 1;
    sim_until = 0;
    ok = 1;
    for (now = 20000; now < 20950u + MO_CTL_MS - 10u; now += 10u) {
        if (now <= 20950u && now % 50u == 0u)
            mo_ctl(MO_CT_F2, now);                    /* (a fader moved for a second) */
        ok &= !(sim_step(now) & MO_A_SAY);
    }
    for (a = 0; now < 24000u && !(a & MO_A_SAY); now += 10u)
        a = sim_step(now);
    check("a control: commented once left alone MO_CTL_MS, from its own class or the controls' templates",
          ok && (a & MO_A_SAY) && (mo.line_cls == MO_C_CT_F2 || mo.line_cls == MO_C_CTPL));
}

static void sulk_tests(void)
{
    uint32_t now, a = 0, ok, t_play = 0;
    mo_reset(0, 4242);
    sim_until = 0;
    mo.gloom = 30;
    {   /* simple things raise the gloom, rich ones lower it, praise more */
        int n[2] = {60, 67}, s7[4] = {60, 64, 67, 70};
        mo_heard_t h = chord(n, 2), r = chord(s7, 4);
        int32_t g0 = mo.gloom, g1, g2;
        mo_mood(&h, 1000);
        g1 = mo.gloom;
        mo_mood(&r, 2000);
        g2 = mo.gloom;
        mo_praise();
        mo_step(3000, 0, 0);
        printf("  gloom %d, a fifth %d, a seventh %d, praised %d\n", g0, g1, g2, mo.gloom);
        check("the gloom: a fifth raises it, a seventh lowers it (keen), praise lowers it more",
              g1 > g0 && g2 < g1 && mo.t_interest && mo.gloom == (g2 > 30 ? g2 - 30 : 0) && mo.line_cls == MO_C_PRAISE);
    }
    mo_reset(0, 4243);
    sim_until = 0;
    mo.gloom = 110;
    for (now = 5000; now < 30000u && mo.state != MO_SULK; now += 10u) {
        a = sim_step(now);
        if (a & MO_A_PLAY)
            t_play = now;
    }
    ok = mo.state == MO_SULK && t_play && mo.line_cls == MO_C_SULK && mos.seq_n >= 12u;
    {   /* its piece: in a minor key */
        uint32_t i, pcs = 0, tonic = (uint32_t)(64 + (int32_t)((mo.seq_root % 12u + 8u) % 12u)) % 12u, rel;
        for (i = 0; i < mos.seq_n; i++)
            pcs |= 1u << ((mos.seq[i].note + 12u - tonic) % 12u);
        rel = pcs & ~(1u << 0 | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 10 | 1u << 11);
        ok &= !rel && (pcs >> 3 & 1u) && !(pcs >> 4 & 1u);   /* (the minor third, never the major) */
    }
    check("bored enough: it announces itself (SULK), then plays its piece (in minor, the leading tone over V)", ok);
    a = mo_step(now + 100u, 0, 1);
    check("its piece played out: it says so (ENDSULK), the gloom down",
          (a & MO_A_SAY) && mo.line_cls == MO_C_ENDSULK && mo.state == MO_LISTEN && mo.gloom < 40);
    /* again, interrupted */
    mo.gloom = 110;
    sim_until = 0;
    for (now += 20000u; mo.state != MO_SULK; now += 10u)
        sim_step(now);
    mo_note(64, 1, 100, now);
    a = mo_step(now + 10u, 0, 0);
    check("a key during its piece: it stops (MO_A_STOP), BACK, listening again",
          (a & MO_A_STOP) && (a & MO_A_SAY) && mo.line_cls == MO_C_BACK && mo.state == MO_LISTEN);
    /* silence */
    mo_reset(0, 31);
    sim_until = 0;
    {
        uint32_t n = 0;
        for (now = 0; now < 400000u; now += 50u) {
            a = sim_step(now);
            if (a & MO_A_PLAY)
                mo.reply_on = 0;
            n += (a & MO_A_SAY) && (mo.line_cls == MO_C_SILENCE || mo.line_cls == MO_C_JOKE);
        }
        check("nobody plays: it notices (SILENCE, a joke or an activity for nobody) three times, then waits",
              n >= 1u && mo.nsilent == 3u);
    }
}

/* ---- round two: the activities, the poses, its pieces' variety */
static uint32_t act_run(uint32_t from, uint32_t to)
{
    uint32_t now, a = 0;
    for (now = from; now < to; now += 10u) {
        a = sim_step(now);
        if (a & MO_A_PLAY)
            mo.reply_on = 0;
    }
    return a;
}
static void activity_tests(void)
{
    uint32_t now, ok, i, t;
    /* ASK: the line; another control: a remark (once in 1.5 s); the one asked for: it did not help */
    mo_reset(0, 2024);
    mo.on = 1;
    sim_until = 0;
    mo_start(MO_AC_ASK, 1000);
    sim_until = 1000 + mo.nline * 300u;
    ok = mo.line_cls == mo_ask_cls(mo.target) && mo.act == MO_AC_ASK;
    act_run(1000, sim_until + 100u);
    ok &= mo.aphase == 1u;
    now = sim_until + 200u;
    mo_ctl(mo.target == MO_CT_K5 ? MO_CT_K6 : MO_CT_K5, now);
    act_run(now, now + 2000u);
    ok &= mo.line_cls == MO_C_WRONG && mo.nwrong == 1u && mo.act == MO_AC_ASK;
    now += 6000u;
    act_run(now - 1000u, now);
    if (mo.target == MO_CT_KEY)
        mo_note(60, 1, 90, now);
    else
        mo_ctl(mo.target, now);
    act_run(now, now + 100u);
    ok &= mo.line_cls == MO_C_ANSWER && mo.act == MO_AC_NONE;
    check("ASK: its line for the control (turn, move, press), another control a remark, the one: it did not help", ok);
    /* ASK: nothing done: it gives up after MO_ASK_MS */
    mo_reset(0, 77);
    mo.on = 1;
    sim_until = 0;
    mo_start(MO_AC_ASK, 1000);
    sim_until = 1000 + mo.nline * 300u;
    act_run(1000, sim_until + MO_ASK_MS + 500u);
    check("ASK: nothing done in 15 s: you did not even try", mo.line_cls == MO_C_NOTRY && mo.act == MO_AC_NONE);
    /* ASK picks every board control in the end, each its right line */
    {
        uint64_t seen = 0;
        ok = 1;
        mo_reset(0, 5);
        for (i = 0; i < 2000u; i++) {
            mo_start(MO_AC_ASK, 1000 + i);
            seen |= (uint64_t)1 << mo.target;
            ok &= mo.target != MO_CT_PATCH && mo.target < MO_NCT;
        }
        for (i = 0; i < sizeof MO_ASKABLE; i++)
            ok &= (seen >> MO_ASKABLE[i] & 1u) != 0u;
        check("ASK: every control of the board asked for in the end, never the one that leaves MOPE", ok);
    }
    /* GAME: the light wanders over the pads to neighbours; a wrong pad OW, the right one a hit; three: it gives up */
    mo_reset(0, 99);
    mo.on = 1;
    sim_until = 0;
    mo_start(MO_AC_GAME, 1000);
    sim_until = 1000 + mo.nline * 300u;
    act_run(1000, sim_until + 50u);
    ok = mo.aphase == 1u && mo.gpos < 16u;
    {
        uint32_t p0 = mo.gpos, moved = 0, far = 0;
        now = sim_until + 50u;
        for (t = 0; t < 20u; t++) {                    /* (a move every 600 ms, always to a neighbour) */
            uint32_t q = mo.gpos;
            act_run(now, now + 610u);
            now += 610u;
            moved += mo.gpos != q;
            far += !(mo.gpos / 8u != q / 8u ? mo.gpos % 8u == q % 8u : mo.gpos == q + 1u || mo.gpos + 1u == q);
        }
        ok &= moved == 20u && !far;
        (void)p0;
    }
    mo_pad_in((mo.gpos + 2u) % 16u, now);
    act_run(now, now + 20u);
    ok &= mo.line_cls == MO_C_OW && mo.gmiss == 1u;
    for (i = 0; i < 3u; i++) {
        now += 1000u;
        mo_pad_in(mo.gpos, now);
        act_run(now, now + 20u);
    }
    ok &= mo.act == MO_AC_NONE && mo.line_cls == MO_C_GAMEEND && mo.ghits == 3u;
    check("GAME: the light to a neighbour every 0.6 s, a wrong pad OW, three hits and it gives up", ok);
    /* LIGHTS and DARK: their time, then their line */
    mo_reset(0, 12);
    mo.on = 1;
    sim_until = 0;
    mo_start(MO_AC_DARK, 1000);
    sim_until = 1000 + mo.nline * 300u;
    act_run(1000, sim_until + 4900u);
    ok = mo.act == MO_AC_DARK && mo.aphase == 1u;
    act_run(sim_until + 4900u, sim_until + 5300u);
    ok &= mo.act == MO_AC_NONE && mo.line_cls == MO_C_DARKEND;
    mo_start(MO_AC_LIGHTS, 20000);
    sim_until = 20000 + mo.nline * 300u;
    act_run(20000, sim_until + 4800u);
    ok &= mo.act == MO_AC_NONE && mo.line_cls == MO_C_LIGHTSEND;
    check("DARK: 5 s, then not better either; LIGHTS: 4.5 s, then are you happy now", ok);
    /* poses: a new one at every press, never the same twice in a row, every one of them; at most every 0.4 s */
    {
        uint32_t last, same = 0, seen = 0, quick;
        mo_reset(0, 3);
        mo.on = 1;
        last = mo.pose;
        for (i = 0; i < 300u; i++) {
            mo_ctl(MO_CT_K5, 1000u + i * 500u);
            same += mo.pose == last;
            last = mo.pose;
            seen |= 1u << mo.pose;
        }
        mo_ctl(MO_CT_K5, 1000u + 299u * 500u + 100u);    /* (0.1 s later: not again) */
        quick = mo.pose == last;
        check("poses: a new one at every press (300), never twice in a row, all eight, not faster than 0.4 s",
              !same && seen == 0xFFu && quick);
    }
    /* its pieces: a new one every time, in the key's minor, at their own tempo */
    {
        uint32_t hs[24], distinct = 0, tempos = 0, j, ok2 = 1, lens = 0;
        uint32_t tm[24];
        mo_reset(0, 4711);
        for (i = 0; i < 24u; i++) {
            uint32_t h = 2166136261u, pcs = 0, k, tonic, rel;
            mo_compose(9);
            for (k = 0; k < mos.seq_n; k++) {
                h = (h ^ mos.seq[k].note ^ (uint32_t)mos.seq[k].at << 8 ^ (uint32_t)mos.seq[k].len << 20) * 16777619u;
                tonic = (uint32_t)(57 + (9 + 3) % 12) % 12u;
                pcs |= 1u << ((mos.seq[k].note + 12u - tonic) % 12u);
            }
            rel = pcs & ~(1u << 0 | 1u << 2 | 1u << 3 | 1u << 5 | 1u << 7 | 1u << 8 | 1u << 10 | 1u << 11);
            ok2 &= !rel && (pcs >> 3 & 1u) && !(pcs >> 4 & 1u) && mos.seq_n >= 12u && mos.seq_n <= MO_SEQN;
            hs[i] = h;
            tm[i] = mos.sixteenth;
            for (j = 0; j < i && hs[j] != h; j++)
                ;
            distinct += j == i;
            for (j = 0; j < i && tm[j] != tm[i]; j++)
                ;
            tempos += j == i;
            lens += mos.seq_n;
        }
        printf("  24 pieces: %u different, %u tempos, %.1f notes each\n", distinct, tempos, lens / 24.0);
        check("its pieces: 24 all different, many tempos, every one in A minor (its minor third, never the major)",
              ok2 && distinct == 24u && tempos >= 10u);
    }
}

static void reset_all(void);
/* ---- the touch sounds; the movers against noise */
static void touch_tests(void)
{
    uint32_t ok = 1, q0, i, now, k;
    /* every family its sound at once: a knob (a word, up or down), a fader, a wheel, a button, a pad (a blip) */
    reset_all();
    mo_reset(0, 4);
    mo.on = 1;
    sp.touch_gain = 110;
    {
        static const uint8_t ID[5] = {MO_CT_K5, MO_CT_F2, MO_CT_MODW, MO_CT_FX, 0};
        for (k = 0; k < 5u; k++) {
            q0 = mos.tq_w;
            now = 10000u + k * 1000u;
            if (k < 4u)
                mo_ctl_dir(ID[k], now, k & 1u ? -1 : 1);
            else
                mo_pad_in(5, now);
            ok &= (uint8_t)(mos.tq_w - q0) >= 1u;
            if (k == 0u)
                ok &= mos.tq[q0 % MO_TQ].slide == 2 && mos.tq[q0 % MO_TQ].w < SP_NWORD;   /* (up: rising) */
            if (k == 1u)
                ok &= mos.tq[q0 % MO_TQ].slide == -5;                                    /* (down: falling) */
            if (k == 4u)
                ok &= mos.tq[q0 % MO_TQ].w == 0xFFFFu;                                   /* (a blip) */
        }
    }
    /* at most one every 0.22 s; none while it talks */
    q0 = mos.tq_w;
    for (i = 0; i < 20u; i++)
        mo_ctl_dir(MO_CT_K6, 20000u + i * 20u, 1);        /* (a knob turned on: 20 detents in 0.4 s) */
    ok &= (uint8_t)(mos.tq_w - q0) <= 2u;
    sp.say_w = (uint8_t)(sp.say_r + 1u);                   /* (it talks) */
    q0 = mos.tq_w;
    mo_ctl_dir(MO_CT_K6, 30000u, 1);
    ok &= mos.tq_w == q0;
    sp.say_w = sp.say_r;
    check("touch sounds: a knob OH up / down, a fader OOH, a wheel OO, a button a click, a pad a blip; 1 per 0.22 s; "
          "none while it talks", ok);
    {   /* they sound, quietly, and they end */
        int32_t pk = 0;
        uint32_t j;
        reset_all();
        mo_reset(0, 9);
        mo.on = 1;
        sp.touch_gain = 110;
        for (k = 0; k < 6u; k++) {
            mo_ctl_dir(k < 3u ? MO_CT_K5 : MO_CT_FX, 1000u + k * 1000u, 1);
            for (i = 0; i < 1378u; i++) {
                int32_t o[2u * SP_BLK];
                sp_render(o, SP_BLK);
                for (j = 0; j < 2u * SP_BLK; j++)
                    pk = abs(o[j]) > pk ? abs(o[j]) : pk;
            }
        }
        for (i = 0, j = 0; i < SP_NV; i++)
            j += sp.v[i].on;
        printf("  the touch sounds' peak %d (its words' ~19000); voices left on %u\n", pk, j);
        check("touch sounds: heard (but quiet: under a third of its words), every one ended", pk > 600 && pk < 6500 && !j);
    }
    {   /* the movers: jitter (+-4) and a slow drift (40 steps in 20 s) never count; a hand's move (30 steps in 0.1 s)
         * does, once, its way */
        mo_mv_t m;
        uint32_t rnd = 12345u, n_noise = 0, n_move = 0;
        int32_t dir = 0, raw;
        memset(&m, 0, sizeof m);
        for (now = 0; now < 20000u; now++) {               /* (a reading a ms) */
            int32_t d;
            rnd = rnd * 1103515245u + 12345u;
            raw = 550 + (int32_t)(now * 40u / 20000u) + (int32_t)((rnd >> 16) % 9u) - 4;
            d = mo_move(&m, raw, 8, now);
            n_noise += d != 0;
        }
        for (; now < 20100u; now++) {                      /* the hand: 30 steps down in 0.1 s */
            int32_t d;
            rnd = rnd * 1103515245u + 12345u;
            raw = 590 - (int32_t)((now - 20000u) * 30u / 100u) + (int32_t)((rnd >> 16) % 9u) - 4;
            d = mo_move(&m, raw, 8, now);
            if (d)
                n_move++, dir = d;
        }
        for (; now < 22000u; now++) {                      /* settled again: nothing more */
            rnd = rnd * 1103515245u + 12345u;
            raw = 560 + (int32_t)((rnd >> 16) % 9u) - 4;
            n_noise += mo_move(&m, raw, 8, now) != 0;
        }
        printf("  the mover: noise and drift counted %u times, the hand %u (its way %d)\n", n_noise, n_move, dir);
        check("movers: jitter and drift never a movement, a real one is (its way); then settled again", !n_noise &&
              n_move >= 1u && n_move <= 3u && dir < 0);
        {   /* ASK: move the mod wheel: its noise does not answer it */
            mo_reset(0, 3);
            mo.on = 1;
            mo_start(MO_AC_ASK, 1000);
            mo.target = MO_CT_MODW;
            mo.aphase = 1;
            memset(&m, 0, sizeof m);
            for (now = 1000; now < 9000u; now++) {
                int32_t d;
                rnd = rnd * 1103515245u + 12345u;
                d = mo_move(&m, 540 + (int32_t)((rnd >> 16) % 9u) - 4, 8, now);
                if (d)
                    mo_ctl_dir(MO_CT_MODW, now, d);
            }
            check("ASK move the mod wheel: its noise is no answer", !mo.ask_hit && !mo.ask_wrong);
        }
    }
}

/* ---- the real thing: the sound, demos */
#define MAXS (44118u * 70u)
static int16_t wbuf[2u * MAXS];
static uint32_t nw;
static int32_t peak;
static int32_t sp_limit_t(int32_t x);
static void reset_all(void)
{
    memset(&sp, 0, sizeof sp);
    memset(&mos, 0, sizeof mos);
    memset(&mo, 0, sizeof mo);
    memset(sp_dl, 0, sizeof sp_dl);
    memset(sp_rv, 0, sizeof sp_rv);
    memset(sp_ap, 0, sizeof sp_ap);
    sp_init();
    sp_voice_load(&sp.snd, SP_ROBOT);                      /* (sp_mope_ui.c mo_toggle's voice) */
    sp.snd.speed = 68, sp.snd.dmix = 0, sp.snd.reverb = 14, sp.snd.size = 50;
    sp_apply();
    sp.mode = SP_MODE_MOPE;
    t_keys = 0;
    nw = 0;
    peak = 0;
    memset(t_vel, 100, sizeof t_vel);
}
/* ms of the instrument: a block of sound, MOPE's main loop every ~15 ms */
static void run(uint32_t ms)
{
    uint32_t n = ms * 1378u / 1000u, i, j;
    for (i = 0; i < n; i++) {
        int32_t o[2u * SP_BLK];
        if (i % 20u == 0u)
            mo_drive();
        sp_render(o, SP_BLK);
        for (j = 0; j < 2u * SP_BLK; j++) {
            int32_t v = sp_limit_t(o[j]);
            peak = abs(o[j]) > peak ? abs(o[j]) : peak;
            if (nw < 2u * MAXS)
                wbuf[nw++] = (int16_t)v;
        }
    }
}
static void keys(uint64_t k) { t_keys = k; }
static void wav(const char *dir, const char *name)
{
    char path[512];
    FILE *f;
    uint32_t sr = 44118u, ch = 2, br = sr * 4u, ds = nw * 2u, rs = 36u + ds, s16 = 16;
    uint16_t c16 = 2, ba = 4, bits = 16, fmt = 1;
    snprintf(path, sizeof path, "%s/%s.wav", dir, name);
    f = fopen(path, "wb");
    if (!f) {
        printf("FAIL cannot write %s\n", path);
        bad++;
        return;
    }
    fwrite("RIFF", 1, 4, f); fwrite(&rs, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&s16, 4, 1, f);
    fwrite(&fmt, 2, 1, f); fwrite(&c16, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&ds, 4, 1, f);
    fwrite(wbuf, 2, nw, f);
    fclose(f);
    printf("  %s: %.1f s\n", path, nw / 2.0 / sr);
    (void)ch;
}
/* (sp_main.c's master and limiter: MASTER at 70 %) */
static int32_t sp_limit_t(int32_t x)
{
    int32_t a, y;
    x = (int32_t)((int64_t)x * 2047 >> 12);
    a = x < 0 ? -x : x;
    if (a <= 24576)
        return x;
    a -= 24576;
    a = a > 200000 ? 200000 : a;
    y = 24576 + a * 8192 / (a + 8192);
    return x < 0 ? -y : y;
}
static uint32_t lines_of(uint32_t c0) { (void)c0; return mo.lines; }

static void sound_tests(const char *dir)
{
    uint32_t i, ok;
    /* 1: hello, a C major chord, its comment, its reply */
    reset_all();
    run(9000);
    keys(1ull << 12 | 1ull << 16 | 1ull << 19);
    run(600);
    keys(0);
    run(10000);
    ok = mo.have_h && mo.h.kind == MO_K_CHORD && lines_of(0) >= 3u;
    wav(dir, "mope_major");
    check("demo mope_major: HELLO, C major played, its comment and its reply (the tones, the words)", ok && peak > 3000);
    /* 2: a fifth, the same fifth again, an octave */
    reset_all();
    run(7000);
    for (i = 0; i < 3u; i++) {
        keys(i < 2u ? (1ull << 9 | 1ull << 16) : (1ull << 9 | 1ull << 21));
        run(700);
        keys(0);
        run(8000);
    }
    wav(dir, "mope_fifths");
    check("demo mope_fifths: a fifth, again (the same), an octave: each commented on", mo.lines >= 4u);
    /* 3: the same key over and over until it sulks, its piece, a key stops it */
    reset_all();
    run(6000);
    mo.gloom = 70;
    for (i = 0; i < 3u; i++) {
        uint32_t k;
        for (k = 0; k < 6u; k++) {
            keys(1ull << 12);
            run(120);
            keys(0);
            run(120);
        }
        run(6500);
    }
    {
        uint32_t t;
        for (t = 0; t < 30000u && mo.state != MO_SULK; t += 100u)
            run(100);
        run(9000);
        ok = mo.state == MO_SULK || mo.line_cls == MO_C_ENDSULK;
        keys(1ull << 7);
        run(200);
        keys(0);
        run(400);
        ok &= mo.line_cls == MO_C_BACK && mo.state == MO_LISTEN;
        run(5000);
    }
    printf("  sulk demo: state %u, the last line's class %u (BACK %u), gloom %d\n", mo.state, mo.line_cls, MO_C_BACK,
           mo.gloom);
    wav(dir, "mope_sulk");
    check("demo mope_sulk: the same key again and again, it sulks, plays its piece, a key stops it (BACK)",
          ok);
    /* 4: a rich seventh chord, then a joke (pad 13..16) */
    reset_all();
    run(7000);
    keys(1ull << 7 | 1ull << 11 | 1ull << 14 | 1ull << 17);
    run(900);
    keys(0);
    run(9000);
    mo_joke();
    run(14000);
    wav(dir, "mope_joke");
    check("demo mope_joke: G7 (keen), then a joke told gloomily", mo.line_cls == MO_C_JOKE);
    /* 5: ASK (a wrong control, then the one), then GAME (a wrong pad: OW, then hits) */
    reset_all();
    run(7000);
    mo_start(MO_AC_ASK, sp.now / 44u);
    mo.target = MO_CT_MODW;
    mo_line(MO_C_ASKMOVE, &(mo_heard_t){.ctl = MO_CT_MODW + 1u}, MO_L_PLAIN);
    mo_say_line();
    run(5000);
    mo_ctl(MO_CT_K5, sp.now / 44u);
    run(4000);
    mo_ctl(MO_CT_MODW, sp.now / 44u);
    run(6000);
    ok = mo.line_cls == MO_C_ANSWER;
    mo_start(MO_AC_GAME, sp.now / 44u);
    mo_say_line();
    run(4000);
    for (i = 0; i < 3u; i++) {
        mo_pad_in((mo.gpos + 4u) % 16u, sp.now / 44u);
        run(1500);
    }
    for (i = 0; i < 3u; i++) {
        mo_pad_in(mo.gpos, sp.now / 44u);
        run(3500);
    }
    run(4000);
    wav(dir, "mope_ask_game");
    check("demo mope_ask_game: move the mod wheel (K5: not that one; the wheel: it did not help), the light game",
          ok && mo.line_cls == MO_C_GAMEEND);
    {   /* one line in ROBOT (MOPE's voice), then A / B: the same line in ROBOT, then in LAB AI (round two's) */
        uint32_t k, n0;
        uint16_t line[MO_LMAX];
        uint8_t nl;
        reset_all();
        mo_reset(0, 1234);
        mo.on = 1;
        sp.say_base = 55;
        mo_line(MO_C_FIFTH, &(mo_heard_t){.kind = MO_K_IVL, .ivl = 7, .root = 9}, MO_L_OBS);
        nl = mo.nline;
        memcpy(line, mo.line, sizeof line);
        for (k = 0; k < 3u; k++) {
            if (k == 2u) {
                sp_voice_load(&sp.snd, SP_LABAI);
                sp.snd.speed = 80, sp.snd.dmix = 0, sp.snd.reverb = 14, sp.snd.size = 50;
                sp_apply();
            }
            sp.say_gain = k == 2u ? 896 : 370;
            mo.nline = nl;
            memcpy(mo.line, line, sizeof line);
            mo_say_line();
            for (i = 0; i < 44118u * 12u / SP_BLK && mo_busy(); i++)
                run(1);
            run(1200);
            if (k == 0u) {
                wav(dir, "mope_robot_line");
                n0 = nw;
                nw = 0;
                (void)n0;
            }
        }
        wav(dir, "mope_ab_robot_labai");
        check("demos mope_robot_line (MOPE's voice: ROBOT), mope_ab_robot_labai (the line in ROBOT, then LAB AI)", 1);
    }
    {   /* every tone ends; the cost of 8 tone voices */
        clock_t c0;
        double ns;
        uint32_t n = 0;
        reset_all();
        sp.mode = SP_MODE_KIT;                             /* (no MOPE talking: the tones alone) */
        for (i = 0; i < 8u; i++)
            mos_on(48 + i * 3u, 100, i, MO_TK_KEYS);
        c0 = clock();
        for (i = 0; i < 20000u; i++) {
            int32_t l[SP_BLK] = {0}, r[SP_BLK] = {0};
            uint32_t k;
            for (k = 0; k < MO_NT; k++)
                if (mos.v[k].on)
                    mos.v[k].gate = 1, mos.v[k].env = 20000, mo_tone_run(&mos.v[k], l, r, 6000);
            n += (uint32_t)l[3];
        }
        ns = (double)(clock() - c0) / CLOCKS_PER_SEC * 1e9 / (20000.0 * SP_BLK);
        printf("  the tones on this host: %.1f ns a sample for 8 voices (%.2f a voice) (%u)\n", ns, ns / 8, n & 1u);
        sp.mode = SP_MODE_MOPE;
        for (i = 0; i < 8u; i++)
            mos_on(48 + i * 3u, 127, i, MO_TK_KEYS);
        run(300);
        for (i = 0; i < 8u; i++)
            mos_off(i);
        run(4000);
        ok = 1;
        for (i = 0; i < MO_NT; i++)
            ok &= !mos.v[i].on;
        check("the tones: 8 at once bounded, all of them end after they are let go", ok && peak < 200000);
    }
    {   /* its words in the centre: left and right the same, whatever SPREAD says */
        int32_t dmax = 0, pk = 0;
        uint32_t k;
        reset_all();
        mo_reset(0, 21);
        mo.on = 1;
        sp.say_base = 55;
        sp.say_gain = 370;
        sp.snd.spread = 127;
        sp.snd.reverb = 0;
        sp.snd.dmix = 0;
        for (k = 0; k < 3u; k++) {
            mo_line(MO_C_BORED, &(mo_heard_t){.kind = MO_K_TUNE}, MO_L_PLAIN);
            mo_say_line();
            for (i = 0; i < 44118u * 12u / SP_BLK && mo_busy(); i++) {
                int32_t o[2u * SP_BLK];
                uint32_t j;
                sp_render(o, SP_BLK);
                for (j = 0; j < SP_BLK; j++) {
                    int32_t dlr = abs(o[2u * j] - o[2u * j + 1u]);
                    dmax = dlr > dmax ? dlr : dmax;
                    pk = abs(o[2u * j]) > pk ? abs(o[2u * j]) : pk;
                }
            }
        }
        printf("  its words at SPREAD 127: peak %d, left and right apart by %d at most\n", pk, dmax);
        check("its words in the centre (every word, every voice it lands on: left = right)", pk > 3000 && dmax <= 2);
    }
    {   /* its words louder than its music, and clean (no limiter even at full MASTER) */
        double sw = 0, sm = 0;
        int32_t pw = 0, pm = 0;
        uint32_t nwr = 0, nmr = 0, k;
        reset_all();
        mo_reset(0, 8);
        mo.on = 1;
        sp.say_base = 55;
        sp.say_gain = 370;
        for (k = 0; k < 6u; k++) {
            mo_line(MO_C_BORED, &(mo_heard_t){.kind = MO_K_TUNE}, MO_L_PLAIN);
            mo_say_line();
            for (i = 0; i < 44118u * 12u / SP_BLK && mo_busy(); i++) {
                int32_t o[2u * SP_BLK];
                uint32_t j;
                sp_render(o, SP_BLK);
                for (j = 0; j < 2u * SP_BLK; j++) {
                    pw = abs(o[j]) > pw ? abs(o[j]) : pw;
                    sw += (double)o[j] * o[j];
                    nwr++;
                }
            }
        }
        for (k = 0; k < 3u; k++) {
            mo_compose(9);
            mos.seq_req = 1;
            for (i = 0; i < 44118u * 6u / SP_BLK; i++) {
                int32_t o[2u * SP_BLK];
                uint32_t j;
                sp_render(o, SP_BLK);
                for (j = 0; j < 2u * SP_BLK; j++) {
                    pm = abs(o[j]) > pm ? abs(o[j]) : pm;
                    sm += (double)o[j] * o[j];
                    nmr++;
                }
            }
            mos.seq_req = 2;
        }
        sw = sqrt(sw / (nwr ? nwr : 1u));
        sm = sqrt(sm / (nmr ? nmr : 1u));
        printf("  its words: RMS %.0f, peak %d; its music: RMS %.0f, peak %d (%.1f dB under its words)\n", sw, pw, sm,
               pm, 20.0 * log10(sw / (sm > 1 ? sm : 1)));
        check("its words well above its music (peaks 6 dB apart at least), clean: under the limiter's knee at full MASTER",
              pw >= 2 * pm && pw < 24576 && sw > sm);
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/sp_mope";
    listener_tests();
    grammar_tests();
    generator_tests();
    timing_tests();
    sulk_tests();
    activity_tests();
    touch_tests();
    sound_tests(dir);
    printf("%s\n", bad ? "MOPE TEST FAILED" : "mope test passed");
    return bad != 0;
}
