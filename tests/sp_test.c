/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37's sound on the host (src/sp_dsp.c with the words of build/gen/sp_vocab.h), keys as the keyboard plays them:
 *   - every word of the vocabulary decodes: frames read to the stop frame, no overflow, a level within the range
 *   - pitch: a held vowel (HOLD) at C3, A3, C5 and with FORMANT +-12 / LAB AI's steps: its fundamental (the
 *     autocorrelation of the output) within 8 cents of the key; the pitch bend and the octave move it
 *   - the sentence: each key speaks the next word, after the last the first again; a chord (keys within CHORD ms)
 *     speaks one word; < > move the cursor; a new pad starts at its first word; MIDI program change selects a pad
 *   - LETTERS: key k speaks A..Z, ZERO..NINE; the screen's typed line
 *   - voices: 8 at once; a ninth steals the oldest; the CPU guard's cap (vmax) silences the rest; every voice ends
 *     (none left on after its word, its tail, its release)
 *   - ONE SHOT / GATE / HOLD / LOOP; the envelope
 *   - PLAY: the sentence by itself on the eighths, round and round; the keys held choose its pitch (a chord: every
 *     note), none held: the last; PLAY again: it stops. RHYTHM: a groove's hits on its sixteenths, PLAY started
 *     while it runs begins on the bar; a stop at once, the drums ring out; the MIDI clock drives the transport
 *   - every voice preset, every DELAY type, REVERB: bounded, no DC, silence after the tails
 *   - the cost: ns a sample on this host with 8 voices, and per voice
 *   - the vowels: an open /a/ (FAR HARD DARK ALL) keeps F1 above 600 Hz, /i/ (SEE ME KEY) a low F1 and a high F2, in
 *     CLASSIC, ROBOT and LAB AI; LOFI's grit no longer mirrors the formants above the lattice's band
 *   - the banks: SONGS, MANTRAS, PERCUSSION first; CHOIR sings a note with three voices; a sung syllable holds its
 *     vowel while the key is held and ends with its last consonant when let go; every kit hit short (at most 250 ms,
 *     no tail), loud enough, softer at a low velocity; KIT mode: key k the kit's hit k, the KIT pads' queue
 * And it writes the listening set (build/sp_demo, or the directory given): the first joke spoken across a melody in
 * every voice (joke_<voice>.wav), the sentence spoken at ROOT (speak_<voice>.wav), a chord on a held vowel. */
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
#define DAD 80u                            /* DAD JOKES 1's first pad (bank 6 of banks.txt) */
#define ROBOT_BANK 128u                    /* ROBOT's (bank 9) */
#define SONGS 0u
#define CHURCH 16u
#define MANTRAS 48u
#define PERC 64u

static int bad;
static void check(const char *what, int ok)
{
    printf("speak: %-92s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* ---- a render buffer */
#define MAXS (44118u * 40u)
static int32_t buf[2u * MAXS];
static uint32_t nbuf;
static void run_ms(uint32_t ms)
{
    uint32_t n = ms * 44118u / 1000u;
    while (n >= SP_BLK) {
        int32_t o[2u * SP_BLK];
        uint32_t i;
        sp_render(o, SP_BLK);
        for (i = 0; i < 2u * SP_BLK && nbuf < MAXS; i += 2u) {
            buf[2u * nbuf] = o[i];
            buf[2u * nbuf + 1u] = o[i + 1u];
            nbuf++;
        }
        n -= SP_BLK;
    }
}
static void reset(void)
{
    memset(&sp, 0, sizeof sp);
    memset(sp_dl, 0, sizeof sp_dl);
    memset(sp_rv, 0, sizeof sp_rv);
    memset(sp_ap, 0, sizeof sp_ap);
    sp_init();
    sp_voice_load(&sp.snd, SP_CLASSIC);                    /* (the tests of the speech: CLASSIC, DAD JOKES 1) */
    sp.snd.reverb = 24;
    sp.snd.size = 64;
    sp.snd.spread = 40;
    sp_apply();
    sp.sent = DAD;
    t_keys = 0;
    nbuf = 0;
    memset(t_vel, 100, sizeof t_vel);
}
static void key(uint32_t k, int on)
{
    if (on)
        t_keys |= 1ull << k;
    else
        t_keys &= ~(1ull << k);
}
static uint32_t voices_on(void)
{
    uint32_t i, n = 0;
    for (i = 0; i < SP_NV; i++)
        n += sp.v[i].on;
    return n;
}

static void wav(const char *path, const int32_t *x, uint32_t n, uint32_t ch)
{
    FILE *f = fopen(path, "wb");
    uint32_t i, sr = 44118u, br = sr * 2u * ch, ds = n * 2u * ch, rs = 36u + ds;
    uint16_t c16 = (uint16_t)ch, ba = (uint16_t)(2u * ch), bits = 16, fmt = 1;
    int32_t pk = 1;
    if (!f)
        return;
    for (i = 0; i < n * ch; i++)
        pk = abs(x[i]) > pk ? abs(x[i]) : pk;
    fwrite("RIFF", 1, 4, f); fwrite(&rs, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    { uint32_t s16 = 16; fwrite(&s16, 4, 1, f); }
    fwrite(&fmt, 2, 1, f); fwrite(&c16, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f);
    fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&ds, 4, 1, f);
    for (i = 0; i < n * ch; i++) {   /* normalised to -1 dBFS: the set is for listening */
        int16_t s = (int16_t)((int64_t)x[i] * 29000 / pk);
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
}

static double cents(double f, double ref) { return 1200.0 * log2(f / ref); }
static double note_hz(double n) { return 440.0 * pow(2.0, (n - 69.0) / 12.0); }
/* the fundamental of buf[a .. a + n) (left), Hz, between lo and hi: the frequency whose first harmonics (up to
 * 3.5 kHz) carry the most energy (a DFT at each, Hann window), searched in 4-cent steps, then in 0.25 */
static double hsum(const double *x, uint32_t n, double f)
{
    double s = 0;
    uint32_t h, i;
    for (h = 1; h <= 10u && h * f < 3500.0; h++) {
        double w = 2 * M_PI * h * f / 44118.0, re = 0, im = 0, c = cos(w), sn = sin(w), cr = 1, ci = 0, t;
        for (i = 0; i < n; i++) {
            re += x[i] * cr;
            im += x[i] * ci;
            t = cr * c - ci * sn;
            ci = cr * sn + ci * c;
            cr = t;
        }
        s += sqrt(re * re + im * im);
    }
    return s;
}
static double f0_of(uint32_t a, uint32_t n, double lo, double hi)
{
    static double x[8192];
    double best = 0, bf = lo, c, c0, c1;
    uint32_t i;
    n = n > 8192u ? 8192u : n;
    for (i = 0; i < n; i++)
        x[i] = buf[2u * (a + i)] * (0.5 - 0.5 * cos(2 * M_PI * i / n));
    for (c = 0; lo * pow(2, c / 1200) <= hi; c += 4) {
        double v = hsum(x, n, lo * pow(2, c / 1200));
        if (v > best)
            best = v, bf = c;
    }
    c0 = bf - 4;
    c1 = bf + 4;
    for (c = c0; c <= c1; c += 0.25) {
        double v = hsum(x, n, lo * pow(2, c / 1200));
        if (v > best)
            best = v, bf = c;
    }
    return lo * pow(2, bf / 1200);
}
static double rms_of(uint32_t a, uint32_t n)
{
    double s = 0;
    uint32_t i;
    for (i = 0; i < n && a + i < nbuf; i++)
        s += (double)buf[2u * (a + i)] * buf[2u * (a + i)] + (double)buf[2u * (a + i) + 1u] * buf[2u * (a + i) + 1u];
    return sqrt(s / (2.0 * (n ? n : 1u)));
}
static int32_t peak_of(uint32_t a, uint32_t n)
{
    int32_t p = 0;
    uint32_t i;
    for (i = 0; i < 2u * n && 2u * a + i < 2u * nbuf; i++)
        p = abs(buf[2u * a + i]) > p ? abs(buf[2u * a + i]) : p;
    return p;
}

/* a word id by its name */
static uint32_t word_id(const char *name)
{
    uint32_t i;
    for (i = 0; i < SP_NWORD; i++)
        if (!strcmp(SP_WNAME[i], name) && (i >= 36u || strlen(name) > 1u))
            return i;
    return SP_NWORD;
}

/* the pitch of a held vowel: a key held through a word's end (HOLD), measured on the vowel */
static double held_f0(uint32_t keyi, const char *word, uint32_t voice, int32_t formant, double *dc)
{
    uint32_t a, w = word_id(word);
    reset();
    sp_voice_load(&sp.snd, voice);
    sp.snd.formant = (int8_t)formant;
    sp.snd.reverb = 0;
    sp.snd.dmix = 0;
    sp.snd.vdepth = 0;
    sp.snd.hold = SP_HOLD_HOLD;
    sp_apply();
    /* a sentence of that word alone: the first pad whose first word it is, else speak it directly */
    sp.mode = SP_MODE_SENT;
    key(keyi, 1);
    run_ms(1);
    if (sp.v[0].on)
        sp_word_start(&sp.v[0], w);
    run_ms(1600);
    a = nbuf - 44118u / 2u;
    if (getenv("SP_DUMP")) {                               /* (the held vowels for a closer look) */
        char p[256];
        snprintf(p, sizeof p, "%s/held_%u_%u_%d.wav", getenv("SP_DUMP"), keyi, voice, formant);
        wav(p, buf + 2u * a, 44118u / 2u, 2);
    }
    *dc = 0;
    {
        uint32_t i;
        for (i = 0; i < 44118u / 2u; i++)
            *dc += buf[2u * (a + i)];
        *dc /= 44118.0 / 2.0;
    }
    return f0_of(a, 8000, note_hz(48 + keyi + 12 * sp.snd.oct) / 1.45, note_hz(48 + keyi + 12 * sp.snd.oct) * 1.45);
}

/* ---- the vowels: an open /a/ keeps its high F1, /i/ its low F1 and high F2; the grit voices' images stay away */
/* the LPC envelope's first two peaks (Hz, above 200) of buf[a .. a + n), left, at 8 kHz (every 5th or 6th sample) */
static void env_f12(uint32_t a, uint32_t n, double *f1, double *f2)
{
    static double x[2048];
    double r[11], al[11], e, best[2] = {0, 0}, prev = 0, pprev = 0;
    uint32_t i, m = 0, j, k, np = 0;
    for (i = 0; i < n && m < 2048u; i += 1u, m++) {                /* (to 8 kHz: an average of 5.5) */
        double s = 0;
        uint32_t q, i0 = a + (uint32_t)((double)m * 44118.0 / 8000.0);
        for (q = 0; q < 5u; q++)
            s += buf[2u * (i0 + q)];
        x[m] = s / 5.0;
        if ((double)(m + 1u) * 44118.0 / 8000.0 >= n)
            break;
    }
    for (i = 0; i < m; i++)
        x[i] *= 0.5 - 0.5 * cos(2 * M_PI * i / (m ? m : 1u));
    for (k = 0; k <= 10u; k++) {
        r[k] = 0;
        for (i = 0; i + k < m; i++)
            r[k] += x[i] * x[i + k];
    }
    for (j = 0; j <= 10u; j++)
        al[j] = 0;
    al[0] = 1;
    e = r[0] > 0 ? r[0] : 1;
    for (i = 1; i <= 10u; i++) {
        double acc = r[i], kk, tmp[11];
        for (j = 1; j < i; j++)
            acc += al[j] * r[i - j];
        kk = -acc / e;
        for (j = 0; j <= 10u; j++)
            tmp[j] = al[j];
        for (j = 1; j < i; j++)
            al[j] = tmp[j] + kk * tmp[i - j];
        al[i] = kk;
        e *= 1 - kk * kk;
    }
    for (k = 20; k <= 380u && np < 2u; k++) {                      /* 200 .. 3800 Hz in 10 Hz steps */
        double w = 2 * M_PI * k * 10.0 / 8000.0, re = 0, im = 0, v;
        for (j = 0; j <= 10u; j++)
            re += al[j] * cos(w * j), im -= al[j] * sin(w * j);
        v = 1.0 / (re * re + im * im);
        if (k > 21u && prev > pprev && prev > v)
            best[np++] = (k - 1u) * 10.0;
        pprev = prev;
        prev = v;
    }
    *f1 = best[0];
    *f2 = best[1];
}
/* a word in a voice at C3, its loudest 40 ms: F1, F2, and the energy above the lattice's band against below it (dB) */
static void vowel_of(const char *word, uint32_t voice, double *f1, double *f2, double *img)
{
    uint32_t w = word_id(word), i, best = 0, n = 1764;
    double eb = 0, lo = 0, hi = 0;
    reset();
    sp_voice_load(&sp.snd, voice);
    sp.snd.reverb = 0;
    sp.snd.dmix = 0;
    sp.snd.hold = SP_HOLD_SHOT;
    sp_apply();
    sp_speak(w, 48 * 64, 110, 1);
    sp_release(1);
    run_ms(1500);
    for (i = 0; i + n < nbuf; i += 441u) {
        double s = 0;
        uint32_t j;
        for (j = 0; j < n; j += 4u)
            s += (double)buf[2u * (i + j)] * buf[2u * (i + j)];
        if (s > eb)
            eb = s, best = i;
    }
    env_f12(best, n, f1, f2);
    for (i = best; i + 1u < best + n; i++) {                       /* (above 4 kHz: a first difference's share) */
        double d = (double)buf[2u * (i + 1u)] - buf[2u * i], s = buf[2u * i];
        hi += d * d;
        lo += s * s;
    }
    *img = 10.0 * log10((hi + 1e-9) / (lo + 1e-9));
}
static void vowel_tests(void)
{
    static const char *const A[4] = {"FAR", "HARD", "DARK", "ALL"}, *const I[3] = {"SEE", "ME", "KEY"};
    static const uint8_t V[3] = {SP_CLASSIC, SP_ROBOT, SP_LABAI};
    uint32_t v, k, ok = 1;
    double img_g = 0;
    for (v = 0; v < 3u; v++) {
        double fa = 0, fi1 = 0, fi2 = 0;
        for (k = 0; k < 4u; k++) {
            double f1, f2, im;
            vowel_of(A[k], V[v], &f1, &f2, &im);
            fa += f1 / 4;
            img_g = v < 2u && im > img_g ? im : img_g;
        }
        for (k = 0; k < 3u; k++) {
            double f1, f2, im;
            vowel_of(I[k], V[v], &f1, &f2, &im);
            fi1 += f1 / 3;
            fi2 += f2 / 3;
        }
        printf("        %-8s /a/ F1 %4.0f Hz;  /i/ F1 %4.0f, F2 %4.0f Hz\n", SP_VOICE_NAME[V[v]], fa, fi1, fi2);
        ok &= fi1 < 450 && (v == 2u || fa > 600) && (v == 1u || fi2 > 1900);
        /* (ROBOT: METAL's comb puts peaks at its harmonics, F2 not told apart; LAB AI: its glottal pulse's own low
         * peak (~260 Hz at C3) stands over /a/'s F1, its F2 checked) */
    }
    {   /* the grit voices: the images above the lattice's band */
        double f1, f2, im_on, im_off;
        vowel_of("FAR", SP_CLASSIC, &f1, &f2, &im_on);
        reset();
        sp_voice_load(&sp.snd, SP_CLASSIC);
        sp.snd.reverb = 0;
        sp_apply();
        sp.img_on = 0;                                             /* (as before: the images left in) */
        sp_speak(word_id("FAR"), 48 * 64, 110, 1);
        sp_release(1);
        run_ms(1500);
        {
            uint32_t i;
            double lo = 0, hi = 0;
            for (i = 4410; i + 1u < 4410u + 8820u; i++) {
                double d = (double)buf[2u * (i + 1u)] - buf[2u * i], s = buf[2u * i];
                hi += d * d, lo += s * s;
            }
            im_off = 10.0 * log10((hi + 1e-9) / (lo + 1e-9));
        }
        printf("        CLASSIC FAR: the high band (a first difference) %+.1f dB, without the filter %+.1f dB\n", im_on,
               im_off);
        ok &= im_on < im_off - 3.0;
    }
    check("vowels: /a/ a high F1 (> 600 Hz: CLASSIC, ROBOT), /i/ a low F1 and a high F2 (CLASSIC, LAB AI); no images",
          ok);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "build/sp_demo";
    char path[512];
    uint32_t i, w;
    if (getenv("SP_WORDS")) {                              /* every word alone, one file each (listening, a recogniser) */
        uint32_t vn = getenv("SP_WV") ? (uint32_t)atoi(getenv("SP_WV")) : 0u;
        int32_t note = getenv("SP_NOTE") ? atoi(getenv("SP_NOTE")) : 47;
        for (w = 36; w < SP_NWORD; w++) {
            reset();
            sp_voice_load(&sp.snd, vn);
            sp.snd.reverb = 0;
            sp.snd.dmix = 0;
            sp.snd.hold = SP_HOLD_SHOT;
            sp_apply();
            sp_speak(w, note * 64, 110, 1);
            run_ms(200 + SP_WORD[w].nfr * 10u * 4u);
            snprintf(path, sizeof path, "%s/%s.wav", getenv("SP_WORDS"), SP_WNAME[w]);
            wav(path, buf, nbuf, 2);
        }
        return 0;
    }
    /* ---- every word decodes */
    {
        uint32_t frames_ok = 1, nfr_ok = 1, quiet = 0, loud = 0, ok_lvl = 1;
        int32_t maxpk = 0;
        for (w = 0; w < SP_NWORD; w++) {
            sp_voice_t v;
            uint32_t n = 0;
            memset(&v, 0, sizeof v);
            sp_word_start(&v, w);
            while (sp_frame(&v, &SP_VDEF[0]) && n < 4000u)
                n++;
            frames_ok &= v.bit <= (SP_WORD[w].off + 4000u) * 8u && n < 4000u;
            nfr_ok &= n == SP_WORD[w].nfr;
            reset();
            sp.snd.reverb = 0;
            sp.snd.dmix = 0;
            sp.snd.hold = SP_HOLD_SHOT;
            sp_speak(w, 48 * 64, 127, 1);
            sp_release(1);                                 /* (a sung syllable: not held) */
            run_ms(80 + SP_WORD[w].nfr * 10u);
            {
                int32_t p = peak_of(0, nbuf);
                double r = rms_of(0, nbuf);
                maxpk = p > maxpk ? p : maxpk;
                if ((r < 150.0 && !(SP_WORD[w].flags & SP_W_PERC)) || p < 1500) {
                    quiet++;
                    printf("        quiet: %s (RMS %.0f, peak %d)\n", SP_WNAME[w], r, p);
                }
                if (p >= 32767 * 4)
                    loud++;
            }
            run_ms(200);
            ok_lvl &= voices_on() == 0u;
        }
        check("every word: its frames read to the stop frame (the count the header says)", frames_ok && nfr_ok);
        printf("        %u words, peak %d (Q15), %u quiet, %u over 4x full scale\n", SP_NWORD, maxpk, quiet, loud);
        check("every word: a level (none silent, none overflowing), its voice free after it", !quiet && !loud && ok_lvl);
    }
    /* ---- pitch */
    {
        static const struct { uint32_t key; uint32_t voice; int32_t fmt; } T[] = {
            {0, SP_CLASSIC, 0}, {9, SP_CLASSIC, 0}, {24, SP_CLASSIC, 0}, {33, SP_CLASSIC, 0}, {12, SP_DEEP, 0},
            {12, SP_CHILD, 0}, {12, SP_CLASSIC, 12}, {12, SP_CLASSIC, -12}, {12, SP_LABAI, 0}, {12, SP_VINTAGE, 0},
            {12, SP_ROBOT, 0}, {19, SP_GIANT, 0}};
        double worst = 0;
        int dcok = 1;
        for (i = 0; i < sizeof T / sizeof T[0]; i++) {
            double dc, f = held_f0(T[i].key, "MOON", T[i].voice, T[i].fmt, &dc), c = cents(f, note_hz(48 + T[i].key));
            printf("        key %2u %-8s formant %+3d: %8.2f Hz, %+6.1f cents, DC %.0f\n", T[i].key,
                   SP_VOICE_NAME[T[i].voice], T[i].fmt, f, c, dc);
            worst = fabs(c) > worst ? fabs(c) : worst;
            dcok &= fabs(dc) < 600.0;
        }
        check("pitch: a held vowel sounds at its key (C3 .. A5, every excitation, FORMANT -12 .. +12), 8 cents", worst < 8.0);
        check("pitch: no offset in the output (DC)", dcok);
        {   /* bend +2 semitones (MIDI), the octave */
            double dc, f;
            reset();
            sp.snd.hold = SP_HOLD_HOLD;
            sp.snd.reverb = sp.snd.dmix = sp.snd.vdepth = 0;
            sp.bend_range = 2;
            sp_midi(0x7F7FE000u | 0xE0u << 8);              /* (a bend packet: d1 7F d2 7F) */
            key(12, 1);
            run_ms(1);
            sp_word_start(&sp.v[0], word_id("MOON"));
            run_ms(1600);
            f = f0_of(nbuf - 22059u, 8000, note_hz(62) / 1.45, note_hz(62) * 1.45);
            check("pitch bend: +2 semitones at the top (bend range 2)", fabs(cents(f, note_hz(62))) < 10.0);
            (void)dc;
            reset();
            sp.snd.hold = SP_HOLD_HOLD;
            sp.snd.reverb = sp.snd.dmix = sp.snd.vdepth = 0;
            sp.snd.oct = 1;
            key(12, 1);
            run_ms(1);
            sp_word_start(&sp.v[0], word_id("MOON"));
            run_ms(1600);
            f = f0_of(nbuf - 22059u, 8000, note_hz(72) / 1.45, note_hz(72) * 1.45);
            check("OCTAVE +1: an octave up", fabs(cents(f, note_hz(72))) < 10.0);
        }
    }
    /* ---- the sentence */
    {
        uint32_t s = DAD, n = sp_sent_len(DAD), ok = 1, k;
        reset();
        sp.req_sent = (uint16_t)s;
        sp.snd.hold = SP_HOLD_SHOT;
        for (k = 0; k < n + 2u; k++) {
            key(k % 30u, 1);
            run_ms(5);
            ok &= sp.wlast_id == sp_sent_word(s, k % n) && (uint32_t)sp.wlast == k % n;
            key(k % 30u, 0);
            run_ms(60);
        }
        check("sentence: each key the next word, after the last the first again", ok);
        reset();
        sp.req_sent = DAD;
        key(0, 1); key(4, 1); key(7, 1);                   /* a chord in one block */
        run_ms(5);
        k = sp.wlast;
        key(10, 1);
        run_ms(20);                                        /* a fourth key 20 ms later: the same word */
        ok = k == 0 && sp.wlast == 0 && voices_on() == 4u;
        for (i = 0; i < SP_NV; i++)
            ok &= !sp.v[i].on || sp.v[i].word == sp_sent_word(DAD, 0);
        run_ms(60);
        key(11, 1);                                        /* a key after CHORD: the next word */
        run_ms(5);
        check("sentence: a chord (keys within CHORD ms) speaks one word; a key after it the next", ok && sp.wlast == 1);
        sp.req_step = 3;
        run_ms(1);
        ok = sp.wpos == 5u;
        sp.req_step = -1;
        run_ms(1);
        ok &= sp.wpos == 4u;
        sp.req_sent = 17;
        run_ms(1);
        ok &= sp.sent == 17u && sp.wpos == 0u && sp.wlast == -1;
        sp_midi(0x00000500u | 0xC0u << 8 | 33u << 16);     /* program change 33 */
        run_ms(1);
        ok &= sp.sent == 33u;
        check("sentence: < > move the cursor; a new pad from its first word; program change picks a pad", ok);
    }
    /* ---- LETTERS */
    {
        uint32_t ok = 1, k;
        reset();
        sp.mode = SP_MODE_LETTERS;
        sp.snd.hold = SP_HOLD_SHOT;
        for (k = 0; k < 37u; k++) {
            key(k, 1);
            run_ms(3);
            ok &= sp.wlast_id == k % 36u;
            key(k, 0);
            run_ms(3);
        }
        check("LETTERS: the keys speak A..Z, ZERO..NINE (then A again); the typed line ends ..789A",
              ok && !strcmp(sp.typed + strlen(sp.typed) - 4u, "789A") && !strcmp(SP_WNAME[0], "A") &&
              !strcmp(SP_WNAME[26], "ZERO"));
    }
    /* ---- voices, the guard, endings */
    {
        uint32_t k, ok, a0;
        reset();
        sp.req_sent = DAD + 2u;
        sp.snd.hold = SP_HOLD_HOLD;
        sp.chord_ms = 0;
        for (k = 0; k < SP_NV; k++) {
            key(k * 3u, 1);
            run_ms(30);
        }
        ok = voices_on() == SP_NV;
        a0 = sp.v[0].age;
        key(36, 1);
        run_ms(5);
        check("voices: 12 at once; one more takes the oldest", ok && voices_on() == SP_NV && sp.v[0].age != a0 &&
              sp.v[0].tag == 36u);
        sp.vmax = 3;
        run_ms(200);
        for (k = 3, ok = 1; k < SP_NV; k++)
            ok &= !sp.v[k].on;
        check("voices: the CPU guard's cap (vmax 3) ends the voices above it", ok);
        t_keys = 0;
        run_ms(4000);
        check("voices: all keys up: every voice ends (HOLD released, tails, RELEASE)", voices_on() == 0u);
        for (k = 0; k < SP_NHOLD; k++) {
            uint32_t on1, on2;
            reset();
            sp.req_sent = DAD;
            sp.snd.hold = (uint8_t)k;
            sp.snd.release = 0;
            key(5, 1);
            run_ms(2500);                                  /* (the word I is short) */
            on1 = voices_on();
            key(5, 0);
            run_ms(600);
            on2 = voices_on();
            printf("        %-8s held 2.5 s: %u voice(s); let go: %u\n", SP_HOLD_NAME[k], on1, on2);
            ok = k == SP_HOLD_SHOT || k == SP_HOLD_GATE ? on1 == 0u : on1 == 1u;
            check(k == SP_HOLD_SHOT ? "ONE SHOT: the word plays out, the voice ends" :
                  k == SP_HOLD_GATE ? "GATE: the word plays out while held, the voice ends" :
                  k == SP_HOLD_HOLD ? "HOLD: the vowel held while the key is; let go: it ends" :
                  "LOOP: the word again and again while held; let go: it ends", ok && on2 == 0u);
        }
    }
    /* ---- PLAY, RHYTHM, the transport */
    {
        uint32_t ok = 1, k, n = sp_sent_len(DAD + 4u), w0, eighth, words, firstpos = 99, prev;
        int32_t o[2u * SP_BLK];
        reset();
        sp.snd.tempo = 120;
        sp.req_sent = DAD + 4u;                            /* "THE MOON SKIPPED DINNER. IT WAS ALREADY FULL." */
        run_ms(5);
        sp.req_auto = 1;
        eighth = 44118u * 30u / 120u;
        for (k = 0, prev = 0, w0 = sp.tag_words; k < 20u * 44118u / SP_BLK; k++) {
            sp_render(o, SP_BLK);
            if (sp.tag_words != prev) {                    /* a word began: on an eighth of the bar */
                prev = sp.tag_words;
                if (!(sp.tr_pos % 2u == 0u && sp.tr_t <= SP_BLK))
                    printf("        (a word at step %u, %u samples into it)\n", sp.tr_pos, sp.tr_t);
                ok &= sp.tr_pos % 2u == 0u && sp.tr_t <= SP_BLK;
            }
        }
        words = sp.tag_words - w0;
        printf("        PLAY 120 BPM, a sentence of %u words: %u words in 20 s (an eighth %u samples)\n", n, words,
               eighth);
        check("PLAY: the sentence round and round, every word on an eighth", ok && words > 2u * n && sp.auto_on);
        key(12, 1);                                        /* a key: the pitch of the words from now */
        run_ms(1500);
        ok = 1;
        for (k = 0; k < SP_NV; k++)
            ok &= !sp.v[k].on || sp.v[k].stop || sp.v[k].tag != SP_TAG_PLAY || sp.v[k].pkey == (48 + 12) * 64;
        k = voices_on();
        key(12, 0);
        run_ms(1500);                                      /* let go: the pitch stays */
        for (k = 0; k < SP_NV; k++)
            ok &= !sp.v[k].on || sp.v[k].stop || sp.v[k].tag != SP_TAG_PLAY || sp.v[k].pkey == (48 + 12) * 64;
        check("PLAY: a key held chooses the pitch (no word of its own), let go the pitch stays", ok &&
              sp.npn == 1u && sp.pn[0] == (48 + 12) * 64);
        key(0, 1); key(4, 1); key(7, 1);                   /* a chord: every word on its three notes */
        run_ms(1200);
        {
            uint32_t seen = 0;
            for (k = 0; k < SP_NV; k++)
                if (sp.v[k].on && !sp.v[k].stop && sp.v[k].tag == SP_TAG_PLAY)
                    seen |= sp.v[k].pkey == 48 * 64 ? 1u : sp.v[k].pkey == 52 * 64 ? 2u : sp.v[k].pkey == 55 * 64 ? 4u : 8u;
            check("PLAY: a chord held: the words on all its notes", seen == 7u && sp.npn == 3u);
        }
        key(0, 0); key(4, 0); key(7, 0);
        sp.req_auto = 2;
        run_ms(3000);
        check("PLAY again: it stops, the voices end, the transport too", !sp.auto_on && voices_on() == 0u && !sp.tr_on);
        /* RHYTHM */
        reset();
        sp.snd.tempo = 120;
        sp.snd.reverb = 0;
        sp_apply();
        sp.groove_req = 1;                                 /* BOSSA NOVA alone */
        run_ms(4000);
        check("RHYTHM: a groove starts at once, the drums sound, no word", sp.groove == 1u && sp.tr_on &&
              rms_of(nbuf - 44118u, 44118u) > 300.0 && sp.tag_words == 0u);
        sp.req_sent = DAD;
        run_ms(5);
        while (sp.tr_pos == 0u || sp.tr_pos == 15u)
            run_ms(1);
        sp.req_auto = 1;
        firstpos = 99;
        for (k = 0, prev = sp.tag_words; k < 3u * 44118u / SP_BLK && firstpos == 99u; k++) {
            sp_render(o, SP_BLK);
            if (sp.tag_words != prev)
                firstpos = sp.tr_pos;
            ok = sp.auto_on || sp.play_wait;
        }
        check("RHYTHM + PLAY: PLAY waits for the next bar, its first word on the bar's first step", firstpos == 0u && ok);
        sp.groove_req = 4;                                 /* DUB HOUSE: on the next bar */
        run_ms(5);
        ok = sp.groove == 1u;
        run_ms(2100);
        ok &= sp.groove == 4u;
        sp.req_auto = 2;
        sp.groove_req = 0;                                 /* stop: at once */
        run_ms(5);
        ok &= sp.groove == 0u;
        run_ms(4000);
        check("RHYTHM: another groove on the bar; stop at once, the drums ring out, the transport stops", ok &&
              rms_of(nbuf - 4410u, 4410u) < 2.0 && !sp.tr_on);
        /* the MIDI clock: 6 ticks a sixteenth, START from the bar */
        reset();
        sp.snd.tempo = 60;                                 /* (its own clock would be slow) */
        sp.groove_req = 5;
        sp_midi(0x0000FA0Fu);
        for (k = 0; k < 96u; k++) {                        /* 4 quarters at 150 BPM */
            sp_midi(0x0000F80Fu);
            run_ms(17);
        }
        printf("        MIDI clock: 96 ticks, the transport %u steps\n", sp.tr_step);
        check("MIDI clock: the transport follows it (6 ticks a sixteenth)", sp.tr_step >= 14u && sp.tr_step <= 16u);
    }
    /* ---- every voice, every delay, the reverb: bounded, no DC, silence after */
    {
        uint32_t vn, d, ok = 1, quiet_ok = 1;
        for (vn = 0; vn < SP_NVOICE; vn++)
            for (d = 0; d < SP_NDLY; d++) {
                double dc = 0;
                reset();
                sp_voice_load(&sp.snd, vn);
                sp.snd.dtype = (uint8_t)d;
                sp.snd.dmix = 127;
                sp.snd.dfb = 127;
                sp.snd.reverb = 127;
                sp.snd.size = 127;
                sp.snd.metal = vn == SP_ROBOT ? 127 : sp.snd.metal;
                sp_apply();
                sp.req_sent = DAD;
                for (i = 0; i < 6u; i++) {
                    key(i * 4u, 1);
                    run_ms(150);
                    key(i * 4u, 0);
                    run_ms(100);
                }
                run_ms(1500);
                for (i = 0; i < nbuf; i++)
                    dc += buf[2u * i] + buf[2u * i + 1u];
                dc /= 2.0 * nbuf;
                if (!(peak_of(0, nbuf) < 32767 * 8 && fabs(dc) < 300.0))
                    printf("        %s %s: peak %d, DC %.1f\n", SP_VOICE_NAME[vn], SP_DLY_NAME[d], peak_of(0, nbuf), dc);
                ok &= peak_of(0, nbuf) < 32767 * 8 && fabs(dc) < 300.0;
                if (d == SP_DLY_DIGITAL) {                 /* the send off, everything rings out */
                    sp.snd.dmix = 0;
                    sp.snd.reverb = 0;
                    nbuf = 0;
                    run_ms(10000);
                    if (rms_of(nbuf - 4410u, 4410u) >= 2.0)
                        printf("        %s: after 10 s RMS %.2f\n", SP_VOICE_NAME[vn], rms_of(nbuf - 4410u, 4410u));
                    quiet_ok &= rms_of(nbuf - 4410u, 4410u) < 2.0;
                }
            }
        check("every voice x every DELAY type, feedback and REVERB at the top: bounded, no DC", ok);
        check("the sends off: the delay and the reverb ring out to silence", quiet_ok);
    }
    /* ---- the banks, CHOIR, sung syllables, hits, KIT */
    {
        uint32_t ok, k, w, worst = 0, sing = 0;
        {
            static const char *const BK[20] = {"SONGS", "CHURCH", "ROMANTIC", "MANTRAS", "PERCUSSION", "DAD JOKES 1",
                                               "DAD JOKES 2", "CRINGE", "ROBOT", "LAB AI", "SPELLING", "NUMBERS",
                                               "DEUTSCH", "FRANCAIS", "ESPANOL", "ITALIANO", "NEDERLANDS",
                                               "SVENSKA", "ZHONGWEN", "NIHONGO"};
            ok = SP_NBANK == 20u && !strcmp(SP_SENT[CHURCH], "KY RI E E LE I SON.");
            for (k = 0; k < 20u && ok; k++)
                ok &= !strcmp(SP_BANK_NAME[k], BK[k]);
            check("the banks: SONGS CHURCH ROMANTIC MANTRAS PERCUSSION DAD 1 DAD 2 CRINGE ROBOT LAB AI SPELLING NUMBERS "
                  "+ 8 languages", ok);
        }
        {   /* every sentence once: no line of a bank twice, no sentence that is one phrase said again */
            uint32_t a, b2, dup = 0;
            for (a = 0; a < SP_NBANK * 16u; a++)
                for (b2 = a + 1u; b2 < (a / 16u + 1u) * 16u; b2++)
                    dup += !strcmp(SP_SENT[a], SP_SENT[b2]) && strcmp(SP_SENT[a], "A B C D E F G H I J K L M N O P Q R S T U V W X Y Z.");
            for (a = 0; a < SP_NBANK * 16u; a++) {
                uint32_t L = (uint32_t)strlen(SP_SENT[a]);
                if (L % 2u && SP_SENT[a][L / 2u] == ' ' && !strncmp(SP_SENT[a], SP_SENT[a] + L / 2u + 1u, L / 2u)) {
                    printf("        said twice: %s\n", SP_SENT[a]);
                    dup++;
                }
            }
            check("every sentence once (no line twice in a bank, none the same phrase twice)", dup == 0u);
        }
        for (k = 0, ok = 1; k < sp_sent_len(SONGS); k++)
            ok &= (SP_WORD[sp_sent_word(SONGS, k)].flags & SP_W_SING) != 0u;
        for (k = 0; k < sp_sent_len(CHURCH); k++)
            ok &= (SP_WORD[sp_sent_word(CHURCH, k)].flags & SP_W_SING) != 0u;
        for (k = 0; k < sp_sent_len(PERC); k++)
            ok &= (SP_WORD[sp_sent_word(PERC, k)].flags & SP_W_PERC) != 0u;
        check("SONGS, CHURCH: every word sung (SONGS whole words, CHURCH syllables); PERCUSSION: every word a hit", ok);
        {   /* a hyphenated word: one word, shown with its hyphen */
            uint32_t found = 0;
            for (k = 0; k < sp_sent_len(MANTRAS + 1u); k++)
                found += !strcmp(SP_WNAME[sp_sent_word(MANTRAS + 1u, k)], "BUILD-IT");
            check("MANTRAS: BUILD-IT one word, its hyphen shown", found == 1u && sp_sent_len(MANTRAS + 1u) == 4u);
        }
        reset();
        sp_voice_load(&sp.snd, SP_CHOIR);
        ok = sp.snd.reverb == 96u && sp.snd.attack == 70u;
        sp.snd.reverb = 0;
        sp.sent = CHURCH;
        key(9, 1);
        run_ms(30);
        check("CHOIR: a note sung by three voices (detuned); it brings its big room", ok && voices_on() == 3u &&
              sp.v[0].det != sp.v[1].det && sp.v[1].det != sp.v[2].det);
        key(9, 0);
        run_ms(3000);
        ok = voices_on() == 0u;
        /* a sung syllable: held while the key is, its last consonant when let go */
        reset();
        sp.sent = 0;
        sp.snd.reverb = 0;
        key(12, 1);
        run_ms(5);
        for (k = 0; k < 4u; k++)                           /* SON: the sixth syllable */
            sp_word_start(&sp.v[0], sp_sent_word(0, 5));
        run_ms(2500);
        ok &= sp.v[0].on && sp.v[0].held == 2u && sp.v[0].frame == SP_WORD[sp.v[0].word].nuc + 1u;
        {
            double f = f0_of(nbuf - 8000u, 8000, note_hz(60) / 1.45, note_hz(60) * 1.45);
            ok &= fabs(cents(f, note_hz(60))) < 8.0;
            printf("        a sung SON held 2.5 s: frame %u of %u held, %.2f Hz (C4 261.63)\n", sp.v[0].frame,
                   SP_WORD[sp.v[0].word].nfr, f);
        }
        key(12, 0);
        run_ms(20);
        sing = sp.v[0].held;
        run_ms(1000);
        check("a sung syllable: its vowel held at the key's pitch while held; let go: its end, then the voice ends",
              ok && sing == 0u && sp.v[0].stop && voices_on() == 0u);
        /* every hit: short, loud enough, softer by velocity */
        ok = 1;
        for (k = 0; k < SP_NKIT; k++) {
            uint32_t ms = 0, vel;
            int32_t pk[2];
            w = SP_KIT[k];
            for (vel = 0; vel < 2u; vel++) {
                reset();
                sp.snd.reverb = 0;
                sp.snd.dmix = 0;
                sp.snd.hold = SP_HOLD_LOOP;                /* (a hit ignores it) */
                sp_speak(w, (int32_t)SP_KIT_NOTE[k] * 64, vel ? 127u : 30u, 1);
                while (nbuf < 88236u && voices_on())       /* (a block at a time) */
                    run_ms(1);
                ms = nbuf * 1000u / 44118u;
                pk[vel] = peak_of(0, nbuf);
                if (vel)
                    worst = ms > worst ? ms : worst;
            }
            ok &= ms <= 260u && pk[1] > 1500 && pk[0] < pk[1] * 6 / 10;
            if (!(ms <= 260u && pk[1] > 1500 && pk[0] < pk[1] * 6 / 10))
                printf("        hit %s: %u ms, peak %d (velocity 30: %d)\n", SP_WNAME[w], ms, pk[1], pk[0]);
        }
        printf("        the longest hit: %u ms\n", worst);
        check("every kit hit: at most 260 ms (no tail), peak above 1500, at velocity 30 under 60 % of 127's", ok);
        reset();
        sp.mode = SP_MODE_KIT;
        for (k = 0, ok = 1; k < 20u; k++) {
            key(k, 1);
            run_ms(3);
            ok &= sp.wlast_id == SP_KIT[k % 16u];
            key(k, 0);
            run_ms(3);
        }
        sp.hitq[0] = 4;                                    /* a KIT pad, as sp_hit queues it */
        sp.hitv[0] = 100;
        sp.hit_w = 1;
        run_ms(3);
        ok &= sp.wlast_id == SP_KIT[4] && sp.hit_r == 1u;
        check("KIT: key k plays the kit's hit k % 16; a pad's hit through the queue", ok);
    }
    /* ---- the cost */
    {
        struct timespec t0, t1;
        double ns, ns1;
        uint32_t k, blocks = 44118u * 4u / SP_BLK;
        int32_t o[2u * SP_BLK];
        reset();
        sp.snd.hold = SP_HOLD_LOOP;                        /* (the voices keep speaking) */
        sp.snd.dmix = 60;
        sp.snd.reverb = 60;
        sp.chord_ms = 0;
        sp.req_sent = DAD;
        for (k = 0; k < 8u; k++) {
            key(k * 2u, 1);
            for (i = 0; i < 8u; i++)
                sp_render(o, SP_BLK);
        }
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (i = 0; i < blocks; i++)
            sp_render(o, SP_BLK);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (blocks * (double)SP_BLK);
        t_keys = 0;
        reset();
        sp.snd.dmix = 60;
        sp.snd.reverb = 60;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (i = 0; i < blocks; i++)
            sp_render(o, SP_BLK);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns1 = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (blocks * (double)SP_BLK);
        printf("        cost on this host: %.0f ns a sample with 8 voices speaking (LOOP), %.0f ns the bus alone, "
               "%.1f ns a voice\n", ns, ns1, (ns - ns1) / 8.0);
        {   /* the drums: the densest groove (TECHNO: kick, clap, both hats, rim) and every lane at once */
            double nd, na;
            reset();
            sp.snd.dmix = 60;
            sp.snd.reverb = 60;
            sp.groove_req = 6;
            for (i = 0; i < 64u; i++)
                sp_render(o, SP_BLK);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (i = 0; i < blocks; i++)
                sp_render(o, SP_BLK);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            nd = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (blocks * (double)SP_BLK);
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (i = 0; i < blocks; i++) {
                if (i % 64u == 0u)
                    for (k = 0; k < DV_NLANE; k++)
                        rh_hit(k, 1);
                sp_render(o, SP_BLK);
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);
            na = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / (blocks * (double)SP_BLK);
            printf("        the drums on this host: TECHNO %.1f ns a sample (%.1f voices' worth), all 8 lanes ringing "
                   "%.1f ns (%.1f voices)\n", nd - ns1, (nd - ns1) / ((ns - ns1) / 8.0), na - ns1,
                   (na - ns1) / ((ns - ns1) / 8.0));
        }
        check("cost: measured (8 voices, delay, reverb)", ns > 0);
    }
#if SP_808
    /* ---- the CPU guard's cap on the drums: 4 lanes, the oldest end first, the newest hit kept */
    {
        int32_t o[2u * SP_BLK];
        uint32_t k, on = 0;
        reset();
        sp.groove_req = 6;
        for (i = 0; i < 64u; i++)
            sp_render(o, SP_BLK);
        sp.groove_req = 0;                                 /* (stopped: only these hits ring) */
        sp_render(o, SP_BLK);
        for (k = 0; k < DV_NLANE; k++) {
            rh_hit(k, 1);
            sp_render(o, SP_BLK);
        }
        rh_lmax = 4;
        sp_render(o, SP_BLK);
        for (k = 0; k < DV_NLANE; k++)
            on += rh.v[k].on;
        check("RHYTHM: the CPU guard's cap (4 lanes) ends the oldest ringing lanes, keeps the newest hit",
              on <= 4u && rh.v[DV_NLANE - 1u].on);
        rh_lmax = DV_NLANE;
    }
#endif
    /* ---- the listening set */
    {
        /* the baseball joke over a melody: a note a word, the length of the word; the punchline a chord */
        static const uint8_t MEL[14] = {16, 19, 21, 19, 16, 12, 14, 16, 19, 16, 14, 12, 7, 12};
        static const char *const VN[SP_NVOICE] = {"classic", "vintage", "deep", "child", "whisper", "robot", "labai",
                                                  "giant", "choir"};
        uint32_t vn;
        char cmd[600];
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
        if (system(cmd)) {}
        for (vn = 0; vn < SP_NVOICE; vn++) {
            uint32_t k, s = DAD;
            reset();
            sp_voice_load(&sp.snd, vn);
            sp.snd.dmix = 34;
            sp.snd.dtime = 60;
            sp.snd.dfb = 40;
            sp.snd.reverb = 30;
            sp.snd.spread = 70;
            sp.snd.oct = vn == SP_CHILD ? 0 : -1;          /* (G2 .. A3; the child an octave up) */
            sp_apply();
            sp.req_sent = (uint16_t)s;
            run_ms(100);
            for (k = 0; k < 14u; k++) {
                uint32_t wid = sp_sent_word(s, k), ms = SP_WORD[wid].nfr * 10u * 64u / (64u + 0u);
                ms = ms * 100u / (uint32_t)(100.0 * pow(2.0, ((int32_t)sp.snd.speed - 64) / 32.0) + 0.5);
                if (k == 13u) {                            /* ME: a chord, held */
                    key(MEL[k], 1); key(MEL[k] + 4u, 1); key(MEL[k] + 7u, 1);
                    run_ms(1800);
                    key(MEL[k], 0); key(MEL[k] + 4u, 0); key(MEL[k] + 7u, 0);
                    break;
                }
                key(MEL[k], 1);
                run_ms(ms > 180u ? ms : 180u);
                key(MEL[k], 0);
                run_ms(70);
            }
            run_ms(1500);
            snprintf(path, sizeof path, "%s/joke_%s.wav", dir, VN[vn]);
            wav(path, buf, nbuf, 2);
            /* the same sentence spoken by itself (SPEAK) at ROOT */
            reset();
            sp_voice_load(&sp.snd, vn);
            sp.snd.reverb = 20;
            sp.snd.dmix = 0;
            sp_apply();
            sp.req_sent = (uint16_t)s;
            sp.req_auto = 1;
            run_ms(9000);
            snprintf(path, sizeof path, "%s/speak_%s.wav", dir, VN[vn]);
            wav(path, buf, nbuf, 2);
        }
        /* ROBOT bank, LAB AI: "CONGRATULATIONS. YOU PRESSED A KEY. THAT IS VERY IMPRESSIVE." up a scale */
        reset();
        sp_voice_load(&sp.snd, SP_LABAI);
        sp.snd.dmix = 30;
        sp.snd.reverb = 40;
        sp.snd.spread = 80;
        sp_apply();
        sp.req_sent = ROBOT_BANK + 2u;
        run_ms(100);
        for (i = 0; i < sp_sent_len(ROBOT_BANK + 2u); i++) {
            static const uint8_t SC[9] = {12, 14, 16, 17, 19, 21, 23, 24, 24};
            uint32_t wid = sp_sent_word(ROBOT_BANK + 2u, i), ms = SP_WORD[wid].nfr * 10u;
            key(SC[i % 9u], 1);
            run_ms(ms > 200u ? ms : 200u);
            key(SC[i % 9u], 0);
            run_ms(60);
        }
        run_ms(1500);
        snprintf(path, sizeof path, "%s/labai_scale.wav", dir);
        wav(path, buf, nbuf, 2);
        {   /* SONGS: a syllable a note, held; CHOIR (and LAB AI) */
            static const struct { const char *name; uint32_t voice, sent; uint8_t mel[24], ms[24]; } SG[] = {
                {"kyrie_choir", SP_CHOIR, CHURCH, {2, 5, 7, 9, 7, 5, 2}, {5, 5, 5, 6, 5, 5, 12}},
                {"gloria_choir", SP_CHOIR, CHURCH + 2u, {7, 9, 11, 12, 11, 9, 7, 9, 7}, {5, 4, 4, 8, 4, 4, 5, 5, 12}},
                {"alleluia_choir", SP_CHOIR, CHURCH + 3u, {0, 4, 7, 12}, {4, 4, 6, 14}},
                {"sanctus_choir", SP_CHOIR, CHURCH + 4u, {9, 9, 11, 11, 12, 12, 12}, {6, 6, 6, 6, 8, 6, 14}},
                {"ave_maria_labai", SP_LABAI, CHURCH + 6u, {12, 11, 9, 11, 12}, {6, 4, 6, 6, 14}},
                {"twinkle_classic", SP_CLASSIC, SONGS + 7u, {0, 0, 7, 7, 9, 9, 7, 5, 5, 4, 4, 2, 2, 0},
                 {4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 4, 10}},
                {"entchen_choir", SP_CHOIR, SONGS, {0, 2, 4, 5, 7, 7, 9, 9, 9, 9, 7}, {4, 4, 4, 4, 8, 8, 4, 4, 4, 4, 12}},
            };
            uint32_t g;
            for (g = 0; g < sizeof SG / sizeof SG[0]; g++) {
                uint32_t k, n = sp_sent_len(SG[g].sent);
                reset();
                sp_voice_load(&sp.snd, SG[g].voice);
                sp.snd.dmix = 20;
                sp.snd.dtime = 70;
                sp_apply();
                sp.req_sent = (uint16_t)SG[g].sent;
                run_ms(100);
                for (k = 0; k < n && SG[g].ms[k]; k++) {
                    uint32_t last = k + 1u == n || !SG[g].ms[k + 1u];
                    key(SG[g].mel[k], 1);
                    if (last && SG[g].voice != SP_CHOIR)
                        key(SG[g].mel[k] + 7u, 1);         /* (the last syllable: a fifth with it) */
                    run_ms(SG[g].ms[k] * 100u);
                    key(SG[g].mel[k], 0);
                    key(SG[g].mel[k] + 7u, 0);
                    run_ms(30);
                }
                run_ms(2500);
                snprintf(path, sizeof path, "%s/%s.wav", dir, SG[g].name);
                wav(path, buf, nbuf, 2);
            }
        }
        {   /* MANTRAS: two lines, each twice, a word a beat (120 BPM) on a riff; ROBOT, LAB AI */
            static const uint8_t RIFF[8] = {0, 0, 3, 5, 0, 0, 7, 5};
            static const struct { const char *name; uint32_t voice, sent; } MT[] = {
                {"mantra_robot", SP_ROBOT, MANTRAS}, {"mantra_labai", SP_LABAI, MANTRAS + 2u},
                {"mantra_robot2", SP_ROBOT, MANTRAS + 1u}};
            uint32_t m;
            for (m = 0; m < sizeof MT / sizeof MT[0]; m++) {
                uint32_t k, n = sp_sent_len(MT[m].sent);
                reset();
                sp_voice_load(&sp.snd, MT[m].voice);
                sp.snd.dmix = 40;
                sp.snd.dtime = 40;
                sp.snd.dfb = 50;
                sp.snd.dtype = SP_DLY_PING;
                sp.snd.reverb = 20;
                sp.snd.hold = SP_HOLD_GATE;
                sp_apply();
                sp.req_sent = (uint16_t)MT[m].sent;
                run_ms(100);
                for (k = 0; k < 4u * n; k++) {
                    key(12u + RIFF[k % 8u], 1);
                    run_ms(400);
                    key(12u + RIFF[k % 8u], 0);
                    run_ms(100);
                }
                run_ms(1500);
                snprintf(path, sizeof path, "%s/%s.wav", dir, MT[m].name);
                wav(path, buf, nbuf, 2);
            }
        }
        {   /* KIT: a beat, 16ths at 100 BPM, accents by velocity; and the PERCUSSION bank played on one key */
            static const char *const PAT[4] = {"BOOM", "TS", "KA", "TS"};
            static const uint8_t VEL[16] = {127, 60, 90, 60, 110, 60, 127, 70, 127, 60, 90, 60, 100, 80, 127, 90};
            uint32_t k, b;
            reset();
            sp.snd.reverb = 20;
            sp_apply();
            sp.mode = SP_MODE_KIT;
            for (b = 0; b < 8u; b++)
                for (k = 0; k < 16u; k++) {
                    uint32_t hit = k % 4u == 0u ? (k == 4u || k == 12u ? 7u : (k == 0u || k == 10u ? 0u : 9u)) :
                                   k % 2u ? 8u : (b % 2u && k == 14u ? 12u : 8u);
                    (void)PAT;
                    t_vel[hit] = VEL[k];
                    key(hit, 1);
                    run_ms(30);
                    key(hit, 0);
                    run_ms(120);
                }
            run_ms(800);
            snprintf(path, sizeof path, "%s/beat_kit.wav", dir);
            wav(path, buf, nbuf, 2);
            reset();
            sp.snd.reverb = 15;
            sp.snd.hold = SP_HOLD_SHOT;
            sp_apply();
            sp.req_sent = PERC;                            /* "BOOM TSS KA TSS." */
            run_ms(50);
            for (b = 0; b < 8u; b++)
                for (k = 0; k < sp_sent_len(PERC); k++) {
                    key(0, 1);
                    run_ms(40);
                    key(0, 0);
                    run_ms(260);
                }
            run_ms(600);
            snprintf(path, sizeof path, "%s/beatbox_sentence.wav", dir);
            wav(path, buf, nbuf, 2);
        }
        {   /* PLAY: a song speaking itself (CHOIR, 100 BPM) while the keys play its melody, a key a beat */
            static const uint8_t TW[14] = {12, 12, 19, 19, 21, 21, 19, 17, 17, 16, 16, 14, 14, 12};
            uint32_t k, r;
            reset();
            sp_voice_load(&sp.snd, SP_CHOIR);
            sp.snd.tempo = 100;
            sp.snd.oct = -1;
            sp_apply();
            sp.req_sent = SONGS + 7u;                      /* TWINKLE TWINKLE LITTLE STAR */
            run_ms(20);
            key(TW[0], 1);
            run_ms(5);
            sp.req_auto = 1;
            for (r = 0; r < 2u; r++)
                for (k = 0; k < 14u; k++) {
                    key(TW[k], 1);
                    run_ms(k == 6u || k == 13u ? 1200 : 600);
                    key(TW[k], 0);
                }
            sp.req_auto = 2;
            run_ms(2500);
            snprintf(path, sizeof path, "%s/play_melody_choir.wav", dir);
            wav(path, buf, nbuf, 2);
        }
        {   /* RHYTHM: a joke over a groove (PLAY starts on the bar), the keys changing the chord a bar */
            static const struct { const char *name; uint32_t groove, voice; uint8_t ch[4]; } GR[] = {
                {"joke_bossa_nova", 1, SP_CLASSIC, {14, 7, 12, 9}}, {"joke_dub_house", 4, SP_ROBOT, {9, 9, 5, 7}},
                {"joke_boom_bap", 7, SP_DEEP, {12, 12, 10, 7}}};
            uint32_t g, b;
            for (g = 0; g < sizeof GR / sizeof GR[0]; g++) {
                uint32_t bar;
                reset();
                sp_voice_load(&sp.snd, GR[g].voice);
                sp.snd.reverb = 24;
                sp.snd.dmix = GR[g].groove == 4u ? 30 : 0;
                sp.snd.dtype = SP_DLY_TAPE;
                sp.snd.dtime = 60;
                sp_apply();
                sp.snd.tempo = RH_GROOVE[GR[g].groove - 1u].tempo;
                sp.swing = RH_GROOVE[GR[g].groove - 1u].swing;
                sp.req_sent = DAD;
                sp.groove_req = (uint8_t)GR[g].groove;
                run_ms(20);
                bar = sp_quarter() * 4u * 1000u / 44118u;
                key(GR[g].ch[0], 1);
                run_ms(5);
                sp.req_auto = 1;
                for (b = 0; b < 8u; b++) {
                    key(GR[g].ch[b % 4u], 1);
                    run_ms(bar);
                    key(GR[g].ch[b % 4u], 0);
                }
                sp.req_auto = 2;
                sp.groove_req = 0;
                run_ms(1500);
                snprintf(path, sizeof path, "%s/%s.wav", dir, GR[g].name);
                wav(path, buf, nbuf, 2);
            }
        }
        printf("        listening set: %s (joke_*, speak_*, labai_scale, *_choir, ave_maria_labai, twinkle_classic, "
               "mantra_*, beat_kit, beatbox_sentence, play_melody_choir, joke_bossa_nova, joke_dub_house, "
               "joke_boom_bap)\n", dir);
    }
    vowel_tests();
    printf("%s\n", bad ? "SPEAK TEST FAILED" : "speak test passed");
    return bad != 0;
}
