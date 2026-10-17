/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37's state without hardware (tests/sp_ui_test.c runs it on the host): the stored record (MASTER, the settings,
 * the sound in use, the 16 memories, the pad in use), the pads' roles and colours, a memory pad's tap / hold, the
 * knobs' parameters, their pages and their text on the screen. Uses sp_dsp.c's types.
 *
 * The record (one storage object, storage.c OBJ_SETTINGS, A/B copies; on the SMK-37 in FL_DATA, 0xAD000 / 0xAE000,
 * nothing else): "SPK1", its size and version, MASTER, the pad in use, the keys' mode, the settings (bend range,
 * CHORD, TRS MIDI OUT), the memories in use (a bit each) and the one loaded last, the sound in use, the 16 memories
 * (sp_sound_t, 27 bytes each), an FNV-1a check over all of it. A record that fails any check is not used (the
 * defaults); every value is brought into its range on load.
 *
 * The USER bank's 16 sentences: a record of their own (OBJ_PROJECT0, A/B copies; on the SMK-37 in FL_DATA, 0x9D000 /
 * 0x9E000, nothing else): "SPU1", its size and version, the vocabulary's hash (SP_VOCAB_HASH) it was written with,
 * each sentence's length and words: a word's index in SP_WORD and a 16-bit hash of its text (name, language); an
 * FNV-1a check. Written with another vocabulary (an update): each word found again by its text, one no longer there
 * left out. A record that fails a check: the USER pads empty. */
#define SP_MEMS 16u
#define SP_MAGIC 0x314B5053u               /* "SPK1" */
#define SP_VER 3u                          /* (1, 2: the banks of before, sp_bank_v3 moves their pad on) */
#define SP_HOLD_MS 1000u                   /* a memory pad held this long: saved */

typedef struct {
    uint32_t magic;
    uint16_t size, ver;
    uint16_t master;                       /* K1: the virtual pot 0..1023 (SMK) */
    uint16_t sent;                         /* the pad in use: bank * 16 + pad */
    uint8_t mode;                          /* SP_MODE_SENT, SP_MODE_LETTERS, SP_MODE_KIT, SP_MODE_MOPE */
    uint8_t bend, chord, trs_off;          /* SETUP: bend range 1..12, CHORD 0..100 ms, TRS MIDI OUT off */
    uint16_t used;                         /* the memories in use */
    uint8_t loaded;                        /* the memory loaded last (0xFF none) */
    uint8_t groove;                        /* RHYTHM: the groove picked last (1..RH_N; it does not start by itself) */
    sp_sound_t cur;
    sp_sound_t mem[SP_MEMS];
    uint8_t drums, swing;                  /* RHYTHM: DRUMS 0..127, SWING 0..100 (before version 3: 0, 0) */
    uint32_t sum;                          /* FNV-1a of everything before it */
} sp_store_t;
_Static_assert(sizeof(sp_sound_t) == 27u, "sp_sound_t: the stored layout");
_Static_assert(sizeof(sp_store_t) <= 3840u, "the record fits a store object");

static uint32_t sp_fnv(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

#define SP_UMAGIC 0x31555053u              /* "SPU1" */
typedef struct {
    uint32_t magic;
    uint16_t size, ver;
    uint32_t vocab;                        /* SP_VOCAB_HASH when written */
    uint8_t n[16];                         /* the sentences' lengths, 0..SP_UMAX */
    struct { uint16_t id, h; } w[16][SP_UMAX];
    uint32_t sum;                          /* FNV-1a of everything before it */
} sp_user_rec_t;
_Static_assert(sizeof(sp_user_rec_t) <= 3840u, "the USER record fits a store object");

/* a sound brought into its ranges */
static void sp_sound_fix(sp_sound_t *s)
{
    s->voice = (uint8_t)(s->voice % SP_NVOICE);
    s->speed &= 127u;
    s->intone &= 127u;
    s->glide &= 127u;
    s->root = (uint8_t)sp_clampi(s->root, 36, 84);
    s->formant = (int8_t)sp_clampi(s->formant, -12, 12);
    s->breath &= 127u;
    s->lofi &= 127u;
    s->metal &= 127u;
    s->attack &= 127u;
    s->release &= 127u;
    s->hold = (uint8_t)(s->hold % SP_NHOLD);
    s->velo &= 127u;
    s->vrate &= 127u;
    s->vdepth &= 127u;
    s->wrate &= 127u;
    s->wdepth &= 127u;
    s->dtype = (uint8_t)(s->dtype % SP_NDLY);
    s->dtime &= 127u;
    s->dfb &= 127u;
    s->dmix &= 127u;
    s->reverb &= 127u;
    s->size &= 127u;
    s->spread &= 127u;
    s->tone &= 127u;
    s->oct = (int8_t)sp_clampi(s->oct, -2, 2);
    s->tempo = (uint8_t)(s->tempo ? sp_clampi(s->tempo, 40, 240) : 100);   /* (a record before 3: 0, so 100) */
}

/* a pad of a record before version 3: where its bank is now (1: DAD JOKES 1, DAD JOKES 2, ROBOT, SPELLING, NUMBERS;
 * 2: SONGS (now CHURCH), MANTRAS, PERCUSSION, then those five) */
static uint32_t sp_bank_v3(uint32_t sent, uint32_t ver)
{
    static const uint8_t V1[5] = {5, 6, 8, 10, 11}, V2[8] = {1, 3, 4, 5, 6, 8, 10, 11};
    uint32_t b = sent / 16u;
    if (ver == 1u)
        return b < 5u ? V1[b] * 16u + sent % 16u : 0u;
    return b < 8u ? V2[b] * 16u + sent % 16u : 0u;
}

static void sp_store_defaults(sp_store_t *st)
{
    uint32_t k;
    for (k = 0; k < sizeof *st; k++)
        ((uint8_t *)st)[k] = 0;
    st->magic = SP_MAGIC;
    st->size = (uint16_t)sizeof *st;
    st->ver = SP_VER;
    st->master = 724;
    st->bend = 2;
    st->chord = 40;
    st->loaded = 0xFF;
    st->drums = 100;
    st->cur = SP_SOUND_DEF;
    for (k = 0; k < SP_MEMS; k++)
        st->mem[k] = SP_SOUND_DEF;
}

static void sp_store_seal(sp_store_t *st)
{
    st->magic = SP_MAGIC;
    st->size = (uint16_t)sizeof *st;
    st->ver = SP_VER;
    st->sum = sp_fnv(st, (uint32_t)((const uint8_t *)&st->sum - (const uint8_t *)st));
}

/* a record read back (n bytes into st): 1 if it is ours and whole (then every value brought into its range), else 0 */
static int sp_store_take(sp_store_t *st, int32_t n)
{
    uint32_t k;
    if (n != (int32_t)sizeof *st || st->magic != SP_MAGIC || st->size != sizeof *st || st->ver < 1u ||
        st->ver > SP_VER || st->sum != sp_fnv(st, (uint32_t)((const uint8_t *)&st->sum - (const uint8_t *)st)))
        return 0;
    if (st->ver < 3u) {                                    /* (the banks of before: where they are now; no RHYTHM) */
        st->sent = (uint16_t)sp_bank_v3(st->sent, st->ver);
        st->groove = 0;
        st->drums = 100;
        st->swing = 0;
    }
    st->groove = (uint8_t)(st->groove <= RH_N ? st->groove : 0u);
    st->drums &= 127u;
    st->swing = (uint8_t)(st->swing <= 100u ? st->swing : 100u);
    st->ver = SP_VER;
    st->master = (uint16_t)sp_clampi(st->master, 0, 1023);
    st->sent = (uint16_t)(st->sent < SP_NSENT ? st->sent : 0u);
    st->mode = (uint8_t)(st->mode < SP_MODE_MOPE ? st->mode : SP_MODE_SENT);   /* (MOPE never starts by itself) */
    st->bend = (uint8_t)sp_clampi(st->bend, 1, 12);
    st->chord = (uint8_t)sp_clampi(st->chord, 0, 100);
    st->trs_off = st->trs_off != 0u;
    st->loaded = st->loaded < SP_MEMS && (st->used >> st->loaded & 1u) ? st->loaded : 0xFF;
    sp_sound_fix(&st->cur);
    for (k = 0; k < SP_MEMS; k++)
        sp_sound_fix(&st->mem[k]);
    return 1;
}

/* a word's text as a 16-bit hash: its name and whether it is German (the record finds it again by this) */
static uint32_t sp_word_h(uint32_t w)
{
    const char *p = SP_WNAME[w % SP_NWORD];
    uint32_t h = 2166136261u ^ (SP_WORD[w % SP_NWORD].flags & SP_W_DE ? 0x44u : 0x45u);
    h *= 16777619u;
    while (*p)
        h = (h ^ (uint8_t)*p++) * 16777619u;
    return (h ^ h >> 16) & 0xFFFFu;
}
/* the USER sentences (sp.user_*) into a record, sealed */
static void sp_user_pack(sp_user_rec_t *r)
{
    uint32_t i, j;
    for (j = 0; j < sizeof *r; j++)
        ((uint8_t *)r)[j] = 0;
    r->magic = SP_UMAGIC;
    r->size = (uint16_t)sizeof *r;
    r->ver = 1;
    r->vocab = SP_VOCAB_HASH;
    for (i = 0; i < 16u; i++) {
        r->n[i] = (uint8_t)(sp.user_n[i] <= SP_UMAX ? sp.user_n[i] : SP_UMAX);
        for (j = 0; j < r->n[i]; j++) {
            r->w[i][j].id = (uint16_t)(sp.user_w[i][j] % SP_NWORD);
            r->w[i][j].h = (uint16_t)sp_word_h(sp.user_w[i][j]);
        }
    }
    r->sum = sp_fnv(r, (uint32_t)((const uint8_t *)&r->sum - (const uint8_t *)r));
}
/* a record read back (n bytes) into sp.user_*: 1 if it is ours and whole (then the words this vocabulary has; another
 * vocabulary's found again by their text, the others left out), else 0 (the USER pads empty) */
static int sp_user_take(const sp_user_rec_t *r, int32_t n)
{
    uint32_t i, j, k, m;
    for (i = 0; i < 16u; i++)
        sp.user_n[i] = 0;
    if (n != (int32_t)sizeof *r || r->magic != SP_UMAGIC || r->size != sizeof *r || r->ver != 1u ||
        r->sum != sp_fnv(r, (uint32_t)((const uint8_t *)&r->sum - (const uint8_t *)r)))
        return 0;
    for (i = 0; i < 16u; i++) {
        uint32_t cnt = r->n[i] <= SP_UMAX ? r->n[i] : SP_UMAX;
        for (j = m = 0; j < cnt; j++) {
            uint32_t id = r->w[i][j].id, h = r->w[i][j].h;
            if (r->vocab != SP_VOCAB_HASH || id >= SP_NWORD || sp_word_h(id) != h) {
                for (k = 0; k < SP_NWORD && sp_word_h(k) != h; k++)
                    ;
                if (k == SP_NWORD)
                    continue;                              /* (no longer there: left out) */
                id = k;
            }
            sp.user_w[i][m++] = (uint16_t)id;
        }
        sp.user_n[i] = (uint8_t)m;
    }
    return 1;
}

/* ------------------------------------------------------------------- pads --- */
/* the banks' colours on the pads (and the screen's bank name), all bright, in banks.txt's order: SONGS rose, CHURCH
 * gold, ROMANTIC hot pink-red, MANTRAS violet, PERCUSSION red, DAD JOKES 1 the Speak & Spell's red-orange, DAD JOKES 2
 * yellow, CRINGE lime, ROBOT cyan, LAB AI ice blue, SPELLING white, NUMBERS light green; the languages DEUTSCH gold,
 * FRANCAIS blue, ESPANOL amber, ITALIANO green, NEDERLANDS orange, SVENSKA yellow, ZHONGWEN red, NIHONGO cherry
 * blossom; USER aqua (more banks take these again) */
#define SP_NBANK_RGB 21u
static const uint8_t SP_BANK_RGB[SP_NBANK_RGB][3] = {
    {255, 110, 200}, {255, 196, 70}, {255, 30, 110}, {190, 130, 255}, {255, 40, 40}, {255, 90, 0}, {255, 200, 0},
    {170, 255, 0}, {0, 220, 255}, {130, 180, 255}, {235, 240, 255}, {150, 255, 140}, {255, 214, 90}, {100, 150, 255},
    {255, 165, 40}, {70, 235, 120}, {255, 128, 30}, {255, 235, 70}, {255, 70, 60}, {255, 160, 185},
    {0, 255, 200}};
_Static_assert(SP_NBANKS <= SP_NBANK_RGB, "a colour for every bank");

typedef struct {
    uint8_t mempage;                       /* PATCH: the pads are the memories */
    uint8_t kit;                           /* KIT: the pads are the hits */
    uint16_t hit;                          /* bit k: hit k just played (lit at full) */
    uint8_t bank, pad;                     /* the bank shown, the pad in use (0xFF: another bank's) */
    uint8_t speaking;                      /* a word of the pad in use sounding */
    uint16_t used;                         /* the memories in use */
    uint8_t loaded;                        /* the memory loaded (0xFF none) */
    uint8_t blink;                         /* 1..16: that memory flashes (saved) */
    uint16_t held;                         /* bit k: memory pad k is down */
    uint16_t empty;                        /* bit k: pad k an empty USER sentence */
    uint8_t rhythm;                        /* RHYTHM: the pads are the grooves */
    uint8_t groove, pending, beat;         /* the groove playing (+1), the one waiting for the bar (+1), on a beat */
    uint8_t edit;                          /* the editor: the pads its actions (sp_user.c SP_EDIT_RGB) */
    uint8_t mope;                          /* MOPE: the top row praises, the bottom row pokes (sp_mope_ui.c) */
} sp_padview_t;
static void sp_edit_pad_rgb(uint32_t pad, uint8_t *rgb);
static void mo_pad_rgb(uint32_t pad, uint8_t *rgb);

/* a pad's colour (0..255 each, as the LED gets it). The pads (0..7 top row, 8..15 bottom, left to right) are the
 * bank's 16 sentences: a sentence 10 % of its bank's colour, the one in use 45 %, at full while it speaks; KIT: the
 * 16 hits, 22 % red, at full as one plays;
 * PATCH: the memories, an empty one 6 % white, one in use 30 %, the one loaded at full, held 60 %, saved red */
static void sp_pad_rgb(uint32_t pad, const sp_padview_t *v, uint8_t *rgb)
{
    static const uint8_t WHITE[3] = {200, 200, 200}, RED[3] = {255, 30, 30};
    const uint8_t *c = SP_BANK_RGB[v->bank % SP_NBANK_RGB];
    uint32_t k, pct;
    if (v->edit) {
        sp_edit_pad_rgb(pad, rgb);
        return;
    }
    if (v->mope && !v->rhythm && !v->mempage) {            /* MOPE: praise (top), poke (bottom) */
        mo_pad_rgb(pad, rgb);
        return;
    }
    if (v->rhythm && !v->mempage) {                        /* RHYTHM: a groove a pad, the one playing pulses */
        c = RH_RGB[pad % RH_N];
        pct = v->groove == pad + 1u ? (v->beat ? 100u : 40u) : v->pending == pad + 1u ? 60u : 10u;
        for (k = 0; k < 3u; k++)
            rgb[k] = (uint8_t)(c[k] * pct / 100u);
        return;
    }
    if (v->kit && !v->mempage) {                           /* KIT: every pad a hit, PERCUSSION's red, lit while it plays */
        c = SP_BANK_RGB[2];
        pct = v->hit >> pad & 1u ? 100u : 22u;
    } else if (v->mempage) {
        c = WHITE;
        if (v->blink == pad + 1u)
            c = RED, pct = 100u;
        else if (v->held >> pad & 1u)
            pct = 60u;
        else if (v->loaded == pad)
            pct = 100u;
        else if (v->used >> pad & 1u)
            pct = 30u;
        else
            pct = 6u;
    } else if (v->empty >> pad & 1u)                       /* (USER: an empty pad a faint white) */
        c = WHITE, pct = pad == v->pad ? 20u : 4u;
    else
        pct = pad == v->pad ? (v->speaking ? 100u : 45u) : 10u;
    for (k = 0; k < 3u; k++)
        rgb[k] = (uint8_t)(c[k] * pct / 100u);
}

/* a memory pad: a tap (released within SP_HOLD_MS) loads, held SP_HOLD_MS saves (once; its release then does
 * nothing). Returns SS_LOAD, SS_SAVE or 0 */
enum { SS_NONE, SS_LOAD, SS_SAVE };
typedef struct { uint8_t down, done; uint32_t t0; } sp_slotkey_t;
static uint32_t sp_slot_key(sp_slotkey_t *k, uint32_t down, uint32_t now)
{
    if (down && !k->down) {
        k->down = 1;
        k->done = 0;
        k->t0 = now;
        return SS_NONE;
    }
    if (down && !k->done && now - k->t0 >= SP_HOLD_MS) {
        k->done = 1;
        return SS_SAVE;
    }
    if (!down && k->down) {
        k->down = 0;
        return k->done ? SS_NONE : SS_LOAD;
    }
    return SS_NONE;
}

/* ------------------------------------------------------ the knobs, the text --- */
/* the parameters a page's four knobs (SMK K5..K8, FM-1 KNOB 1..4) turn */
enum { SK_TEMPO, SK_SPEED, SK_INTONE, SK_GLIDE, SK_ROOT, SK_FORMANT, SK_BREATH, SK_LOFI, SK_METAL, SK_ATTACK, SK_RELEASE,
       SK_HOLD, SK_VELO, SK_VRATE, SK_VDEPTH, SK_WRATE, SK_WDEPTH, SK_DTYPE, SK_DTIME, SK_DFB, SK_DMIX, SK_REVERB,
       SK_SIZE, SK_SPREAD, SK_TONE, SK_BEND, SK_CHORD, SK_REPEAT, SK_TRS, SK_GROOVE, SK_DRUMS, SK_SWING, SK_N,
       SK_NONE = 255 };
static const char *const SK_NAME[SK_N] = {"TEMPO", "SPEED", "INTONE", "GLIDE", "ROOT", "FORMANT", "BREATH", "LOFI", "METAL",
                                          "ATTACK", "RELEASE", "HOLD", "VELO", "VIB RATE", "VIB DEPTH", "WOB RATE",
                                          "WOB DEPTH", "DELAY", "TIME", "REPEATS", "MIX", "REVERB", "SIZE",
                                          "SPREAD", "TONE", "BEND", "CHORD", "REPEAT", "TRS OUT", "GROOVE",
                                          "DRUMS", "SWING"};
/* the pages (K2 / SELECT): grouped by what they do; SETUP (GLOBE / GLO) is global */
enum { SP_PG_PLAY, SP_PG_VOICE, SP_PG_ENV, SP_PG_MOD, SP_PG_DELAY, SP_PG_SPACE, SP_PG_RHYTHM, SP_NPAGE,
       SP_PG_SETUP = SP_NPAGE };
static const char *const SP_PAGE_NAME[SP_NPAGE + 1] = {"PLAY", "VOICE", "ENVELOPE", "MOD", "DELAY", "SPACE", "RHYTHM",
                                                       "SETUP"};
static const uint8_t SP_PAGE[SP_NPAGE + 1][4] = {
    {SK_TEMPO, SK_SPEED, SK_INTONE, SK_ROOT}, {SK_FORMANT, SK_BREATH, SK_LOFI, SK_METAL},
    {SK_ATTACK, SK_RELEASE, SK_HOLD, SK_GLIDE}, {SK_VRATE, SK_VDEPTH, SK_WRATE, SK_WDEPTH},
    {SK_DTYPE, SK_DTIME, SK_DFB, SK_DMIX}, {SK_REVERB, SK_SIZE, SK_SPREAD, SK_TONE},
    {SK_TEMPO, SK_DRUMS, SK_SWING, SK_GROOVE},
#ifdef SP_TRS
    {SK_BEND, SK_CHORD, SK_VELO, SK_TRS}};                 /* (SMK-37: TRS MIDI OUT; REPEAT has its button) */
#else
    {SK_BEND, SK_CHORD, SK_VELO, SK_REPEAT}};
#endif

/* the global settings the SETUP page turns (sp_ui.c keeps them; the record stores them) */
static struct { uint8_t bend, chord, lock, trs_on, groove, drums, swing; } sp_glob = {2, 40, 0, 1, 0, 100, 0};

/* a parameter's place: its byte in the sound (or the settings), its range and its step a detent */
static uint8_t *sk_ptr(sp_sound_t *s, uint32_t k, int32_t *lo, int32_t *hi, int32_t *step)
{
    *lo = 0;
    *hi = 127;
    *step = 2;
    switch (k) {
    case SK_TEMPO: *lo = 40; *hi = 240; *step = 2; return &s->tempo;
    case SK_SPEED: return &s->speed;
    case SK_INTONE: return &s->intone;
    case SK_GLIDE: return &s->glide;
    case SK_ROOT: *lo = 36; *hi = 84; *step = 1; return &s->root;
    case SK_FORMANT: *lo = -12; *hi = 12; *step = 1; return (uint8_t *)&s->formant;
    case SK_BREATH: return &s->breath;
    case SK_LOFI: return &s->lofi;
    case SK_METAL: return &s->metal;
    case SK_ATTACK: return &s->attack;
    case SK_RELEASE: return &s->release;
    case SK_HOLD: *hi = SP_NHOLD - 1; *step = 1; return &s->hold;
    case SK_VELO: return &s->velo;
    case SK_VRATE: return &s->vrate;
    case SK_VDEPTH: return &s->vdepth;
    case SK_WRATE: return &s->wrate;
    case SK_WDEPTH: return &s->wdepth;
    case SK_DTYPE: *hi = SP_NDLY - 1; *step = 1; return &s->dtype;
    case SK_DTIME: return &s->dtime;
    case SK_DFB: return &s->dfb;
    case SK_DMIX: return &s->dmix;
    case SK_REVERB: return &s->reverb;
    case SK_SIZE: return &s->size;
    case SK_SPREAD: return &s->spread;
    case SK_TONE: return &s->tone;
    case SK_BEND: *lo = 1; *hi = 12; *step = 1; return &sp_glob.bend;
    case SK_CHORD: *hi = 100; *step = 5; return &sp_glob.chord;
    case SK_REPEAT: *hi = 1; *step = 1; return &sp_glob.lock;
    case SK_TRS: *hi = 1; *step = 1; return &sp_glob.trs_on;
    case SK_GROOVE: *hi = RH_N; *step = 1; return &sp_glob.groove;
    case SK_DRUMS: return &sp_glob.drums;
    case SK_SWING: *hi = 100; *step = 5; return &sp_glob.swing;
    default: return 0;
    }
}
static int32_t sk_get(sp_sound_t *s, uint32_t k)
{
    int32_t lo, hi, st;
    uint8_t *p = sk_ptr(s, k, &lo, &hi, &st);
    return !p ? 0 : lo < 0 ? (int32_t)(int8_t)*p : (int32_t)*p;
}
/* the value as 0..127 (the cells' bars) */
static int32_t sk_bar(sp_sound_t *s, uint32_t k)
{
    int32_t lo, hi, st, v = sk_get(s, k);
    sk_ptr(s, k, &lo, &hi, &st);
    return hi > lo ? (v - lo) * 127 / (hi - lo) : 0;
}
/* a knob turned d detents: the parameter moved and kept in its range; 1 if it changed */
static uint32_t sk_turn(sp_sound_t *s, uint32_t k, int32_t d)
{
    int32_t lo, hi, st, v;
    uint8_t *p = sk_ptr(s, k, &lo, &hi, &st);
    if (!p || !d)
        return 0;
    v = lo < 0 ? (int32_t)(int8_t)*p : (int32_t)*p;
    d = sp_clampi(v + d * st, lo, hi);
    if (d == v)
        return 0;
    *p = (uint8_t)d;
    return 1;
}

static char *sp_s(char *t, const char *s)
{
    while (*s)
        *t++ = *s++;
    *t = 0;
    return t;
}
static char *sp_d(char *t, int32_t v, int sign)
{
    char d[12];
    uint32_t k = 0, u;
    if (v < 0)
        *t++ = '-';
    else if (sign && v > 0)
        *t++ = '+';
    u = (uint32_t)(v < 0 ? -v : v);
    do
        d[k++] = (char)('0' + u % 10u);
    while ((u /= 10u) != 0u);
    while (k)
        *t++ = d[--k];
    *t = 0;
    return t;
}
/* tenths: 5.7 */
static char *sp_d1(char *t, int32_t v10)
{
    if (v10 < 0) {
        *t++ = '-';
        v10 = -v10;
    }
    t = sp_d(t, v10 / 10, 0);
    *t++ = '.';
    *t++ = (char)('0' + v10 % 10);
    *t = 0;
    return t;
}
static const char *const SP_NOTE_NAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static char *sp_note_text(char *t, int32_t n) { return sp_d(sp_s(t, SP_NOTE_NAME[n % 12]), n / 12 - 1, 0); }

/* the envelope's and the glide's times as the DSP takes them (sp_block_setup: a step of 32767 x SP_BLK / den a
 * block, so den samples in all), ms */
static int32_t sp_ms_of(int32_t den) { return den * 1000 / (int32_t)SP_FS; }

/* a parameter's value as the screen shows it (t: 16 characters or more) */
static void sk_text(sp_sound_t *s, uint32_t k, char *t)
{
    int32_t v = sk_get(s, k);
    t[0] = 0;
    switch (k) {
    case SK_TEMPO:
        sp_s(sp_d(t, v, 0), " BPM");
        return;
    case SK_SPEED: {                                       /* 25 .. 400 %: 2^((v - 64) / 32) */
        uint32_t p = (uint32_t)((uint64_t)100u * sp_exp2((v - 64) * 24) >> 16);
        sp_s(sp_d(t, (int32_t)p, 0), "%");
        return;
    }
    case SK_INTONE:
        v ? (void)sp_s(sp_d(t, v * 200 / 128, 0), "%") : (void)sp_s(t, "FLAT");
        return;
    case SK_GLIDE:
        v ? (void)sp_s(sp_d(t, sp_ms_of(30 + v * v * 3), 0), " ms") : (void)sp_s(t, "OFF");
        return;
    case SK_ROOT:
        sp_note_text(t, v);
        return;
    case SK_FORMANT:
        sp_s(sp_d(t, v, 1), " st");
        return;
    case SK_LOFI:
        sp_s(sp_d(t, v * 100 / 127, 0), v >= 64 ? " GRIT" : "");
        return;
    case SK_ATTACK:
        v ? (void)sp_s(sp_d(t, sp_ms_of(1 + v * v * 2), 0), " ms") : (void)sp_s(t, "0 ms");
        return;
    case SK_RELEASE:
        sp_s(sp_d(t, sp_ms_of(200 + v * v * 6), 0), " ms");
        return;
    case SK_HOLD:
        sp_s(t, SP_HOLD_NAME[v % SP_NHOLD]);
        return;
    case SK_VRATE: {                                       /* 0.5 * 2^(v * 28 / 768) Hz */
        uint32_t hz10 = (uint32_t)((uint64_t)5u * sp_exp2(v * 28) >> 16);
        sp_s(sp_d1(t, (int32_t)hz10), " Hz");
        return;
    }
    case SK_VDEPTH:
        v ? (void)sp_s(sp_d(t, v * 100 / 127, 0), " ct") : (void)sp_s(t, "OFF");
        return;
    case SK_WRATE: {                                       /* 2^((v - 64) * 40 / 768) Hz */
        uint32_t hz10 = (uint32_t)((uint64_t)10u * sp_exp2((v - 64) * 40) >> 16);
        sp_s(sp_d1(t, (int32_t)hz10), " Hz");
        return;
    }
    case SK_WDEPTH:
        v ? (void)sp_s(sp_d1(t, v * 60 / 127), " st") : (void)sp_s(t, "OFF");
        return;
    case SK_DTYPE:
        sp_s(t, SP_DLY_NAME[v % SP_NDLY]);
        return;
    case SK_DTIME:
        sp_s(sp_d(t, 30 + v * 710 / 127, 0), " ms");
        return;
    case SK_DMIX:
    case SK_REVERB:
        v ? (void)sp_s(sp_d(t, v * 100 / 127, 0), "%") : (void)sp_s(t, "OFF");
        return;
    case SK_TONE:
        sp_d(t, v - 64, 1);
        return;
    case SK_BEND:
        sp_s(sp_d(t, v, 0), v == 1 ? " semi" : " semis");
        return;
    case SK_CHORD:
        v ? (void)sp_s(sp_d(t, v, 0), " ms") : (void)sp_s(t, "OFF");
        return;
    case SK_REPEAT:
    case SK_TRS:
        sp_s(t, v ? "ON" : "OFF");
        return;
    case SK_GROOVE:
        sp_s(t, v ? RH_GROOVE[(v - 1) % RH_N].name : "OFF");
        return;
    case SK_DRUMS:
        v ? (void)sp_s(sp_d(t, v * 100 / 127, 0), "%") : (void)sp_s(t, "OFF");
        return;
    case SK_SWING:
        v ? (void)sp_s(sp_d(t, v, 0), "%") : (void)sp_s(t, "OFF");
        return;
    case SK_NONE:
        return;
    default:
        sp_s(sp_d(t, v * 100 / 127, 0), "%");
    }
}
