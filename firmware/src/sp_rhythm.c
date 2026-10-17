/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37's RHYTHM: a drum groove under the speech, 16 of them (a pad each). sp_dsp.c's transport runs it: the
 * groove and PLAY share one tempo (TEMPO, or the MIDI clock) and one bar, a groove or PLAY started while the other
 * runs begins on the next bar.
 *
 * The kit: an 808, FM1 Open's 808 drum voices (M-VAVE's DaisySP / Plaits adaptation, MIT) in a fixed-point port
 * (sp_808.c): kick, snare, clap, closed and open hat, and per groove a tom or a conga, a rim or a clave, a cowbell
 * or a cymbal. SP_808=0 builds the earlier kit instead: Felucca's own drum voices (drum_voice.c, GPL-3.0, Leo
 * Kuroshita / Hugelton Instruments: its 8 lanes, its own kit and its model kits, used as they are).
 *
 * A groove: its kit's swaps (TOM a conga, RIM a clave, BELL a cymbal; for SP_808=0 also Felucca's model kits), its
 * bar (16 sixteenths, or 12: the shuffle's triplets, the waltz's 3/4), its tempo (picking it sets TEMPO), its swing,
 * a pattern a lane (x a hit, X an accented one) and the lane its fill rolls on. FILL: the next bar's last beat a roll.
 * Every voice rings out on its own; while one is quiet it costs nothing. The drums are mixed in after the speech's
 * effects (dry), at DRUMS: at 100 a groove sits 2..6 dB under the words (a sentence on PLAY, CLASSIC). */
#ifndef SP_808
#define SP_808 1
#endif
#pragma push_macro("FS")
#undef FS
#include "felucca_tables.h"                /* (SINE, PITCH_OCT: dsp.c's sine, drum_voice.c's pitch) */
#undef FS
#undef CTL
#pragma pop_macro("FS")
#define CTL 32
typedef struct { int32_t amp0, amp1; } vmod_t;   /* (dsp.c's amplitude-ramp helpers have it; the drums do not) */
#include "dsp.c"
#if SP_808
#include "sp_808.c"
#define DV_NLANE 8u
#ifndef RH_LVL
#define RH_LVL 22                          /* the lanes' mix: x RH_LVL / 256 at the centre (the 16 grooves as loud as
                                            * with Felucca's kit, on average) */
#endif
enum { RH_K, RH_S, RH_C, RH_H, RH_O, RH_T, RH_R, RH_B };
#else
#include "drum_voice.c"

static uint32_t (*const rh_keep)(uint32_t) __attribute__((unused)) = dv_run_type;   /* (drum_voice.c's; not used) */
enum { RH_K = DV_KICK, RH_S = DV_SNARE, RH_C = DV_CLAP, RH_H = DV_HATC, RH_O = DV_HATO, RH_T = DV_TOM, RH_R = DV_RIM,
       RH_B = DV_BELL };
#define RH_LVL 24                          /* (x 3/32: -21 dB) */
#endif
#define RH_N 16u
#define RH_CONGA 1u                        /* (Felucca's kit: TOM a conga, RIM a clave, BELL a cymbal) */
#define RH_CLAVE 2u
#define RH_CYM 4u
typedef struct {
    const char *name;
    uint8_t kit;                           /* SP_808=0: 0 Felucca's own kit (with the swaps), 1..5 its model kits 80 10 66
                                            * 55 77; the 808 plays every groove (with the swaps) */
    uint8_t swap, steps, beat, tempo, swing, fill;   /* steps a bar (16 / 12), a beat (4 / 3), BPM, swing 0..100,
                                                 * the fill's lane */
    const char *lane[DV_NLANE];            /* K S C H O T R B; 0: silent */
} rh_groove_t;
static const rh_groove_t RH_GROOVE[RH_N] = {
    {"BOSSA NOVA", 0, RH_CONGA, 16, 4, 132, 0, RH_T,
     {"x..xX..xx..xX..x", 0, 0, "XxxxXxxxXxxxXxxx", 0, "......x.....x.x.", "x..x..x...x..x..", 0}},
    {"SAMBA", 0, RH_CONGA, 16, 4, 100, 10, RH_S,
     {"x..Xx..Xx..Xx..X", "..x...x..x.x..x.", 0, "XxxxXxxxXxxxXxxx", 0, "....x.......x...", 0, "x.x...x.x.x...x."}},
    {"REGGAE DUB", 1, 0, 16, 4, 76, 25, RH_T,
     {"........X.......", 0, 0, "x.x.x.x.x.x.x.x.", "..............x.", 0, "........X.......", 0}},
    {"DUB HOUSE", 1, 0, 16, 4, 118, 20, RH_R,
     {"X...X...X...X...", 0, "....x.......x...", "x...x...x...x...", "..x...x...x...x.", 0, "...x..x....x..x.", 0}},
    {"HOUSE", 0, 0, 16, 4, 124, 0, RH_C,
     {"X...X...X...X...", 0, "....X.......X...", 0, "..x...x...x...x.", 0, 0, 0}},
    {"TECHNO", 0, 0, 16, 4, 132, 0, RH_R,
     {"X...X...X...X...", 0, "....x.......x...", "x.xxx.xxx.xxx.xx", "..X...X...X...X.", 0, "...x..x...x..x..", 0}},
    {"BOOM BAP", 1, 0, 16, 4, 90, 55, RH_S,
     {"X......xx.x.....", "....X.......X...", 0, "x.x.x.x.x.x.x.x.", 0, 0, 0, 0}},
    {"TRAP", 1, 0, 16, 4, 140, 0, RH_H,
     {"X......x..x.....", 0, "........X.......", "xxxxxxxxxxxxxxxx", 0, 0, 0, 0}},
    {"DRUM AND BASS", 0, 0, 16, 4, 172, 0, RH_S,
     {"X.........x.....", "....X.......X...", 0, "x.x.x.x.x.x.x.x.", 0, 0, 0, "x...............", }},
    {"FUNK", 0, 0, 16, 4, 100, 15, RH_S,
     {"X.x...x...x..x..", "....X..x.x..X..x", 0, "xxxxxxxxxxxxxxxx", 0, 0, 0, 0}},
    {"DISCO", 0, 0, 16, 4, 120, 0, RH_S,
     {"X...X...X...X...", "....X.......X...", 0, "x...x...x...x...", "..X...X...X...X.", 0, 0, 0}},
    {"ROCK", 0, 0, 16, 4, 112, 0, RH_T,
     {"X.....x.x.......", "....X.......X...", 0, "x.x.x.x.x.x.x.x.", 0, 0, 0, 0}},
    {"SHUFFLE", 0, 0, 12, 3, 100, 0, RH_S,
     {"X.....x.....", "...X.....X..", 0, "x.xx.xx.xx.x", 0, 0, 0, 0}},
    {"WALTZ", 0, RH_CYM, 12, 4, 150, 0, RH_T,
     {"X...........", "....x...x...", 0, "x.x.x.x.x.x.", 0, 0, 0, "x..........."}},
    {"AFROBEAT", 0, RH_CONGA, 16, 4, 110, 10, RH_T,
     {"X......xx.x.....", "....x..x....X.x.", 0, "xxxxxxxxxxxxxxxx", 0, "..x...x...xx..x.", 0, "x.x.xx.x.x.xx.x."}},
    {"ELECTRO", 1, 0, 16, 4, 126, 0, RH_C,
     {"X.....x...x.x...", 0, "....X.......X...", "x.x.x.x.x.x.x.x.", 0, 0, 0, "...x......x....."}},
};
/* the grooves' pad colours (sp_state.c): warm Latin, dub greens, house blues, hip-hop violets, the rest */
static const uint8_t RH_RGB[RH_N][3] = {
    {255, 170, 40}, {255, 110, 0}, {40, 255, 80}, {0, 230, 160}, {0, 160, 255}, {80, 90, 255}, {200, 90, 255},
    {255, 60, 220}, {255, 40, 40}, {255, 220, 0}, {255, 120, 200}, {255, 255, 255}, {150, 255, 140}, {120, 200, 255},
    {255, 150, 90}, {0, 255, 255}};

/* the lanes allowed to ring at once: the CPU guard's (sp_main.c: when the speech is down to its 3 voices and the
 * audio is still short, 8 .. 4; the oldest tail ends first). SP_808 only */
static uint8_t rh_lmax = DV_NLANE;

#if SP_808
static struct {
    s8_voice_t v[DV_NLANE];
    int32_t g[DV_NLANE];                   /* the hit's level (Q15: an accent at full) */
    uint8_t snd[DV_NLANE];                 /* the lane's 808 sound (S8_...) */
    uint8_t set;                           /* the groove the sounds are for (+1) */
} rh;

/* the groove's sounds into the lanes */
static void rh_kit(uint32_t gi)
{
    static const uint8_t OWN[DV_NLANE] = {S8_BD, S8_SD, S8_CP, S8_CH, S8_OH, S8_MT, S8_RS, S8_CB};
    const rh_groove_t *G = &RH_GROOVE[gi % RH_N];
    uint32_t l;
    if (rh.set == gi + 1u)
        return;
    rh.set = (uint8_t)(gi + 1u);
    for (l = 0; l < DV_NLANE; l++)
        rh.snd[l] = OWN[l];
    if (G->swap & RH_CONGA)
        rh.snd[RH_T] = S8_MC;
    if (G->swap & RH_CLAVE)
        rh.snd[RH_R] = S8_CL;
    if (G->swap & RH_CYM)
        rh.snd[RH_B] = S8_CY;
}

/* a hit on lane l (accent: at full, else a little softer) */
static void rh_hit(uint32_t l, uint32_t accent)
{
    l &= DV_NLANE - 1u;
    rh.g[l] = accent ? 32767 : 22000;
    s8_hit(&rh.v[l], rh.snd[l], rh.g[l]);
    if (l == RH_H && rh.v[RH_O].on)                       /* a closed hat chokes the open one */
        s8_choke(&rh.v[RH_O]);
}
#else
static struct {
    dv_coef_t c[DV_NLANE];
    dv_voice_t v[DV_NLANE];
    dv_metal_t m[DV_NLANE];
    int32_t g[DV_NLANE];                   /* the hit's level (Q15: an accent at full) */
    uint8_t set;                           /* the groove the coefficients are for (+1) */
} rh;

/* the groove's kit into the lanes' coefficients */
static void rh_kit(uint32_t gi)
{
    const rh_groove_t *G = &RH_GROOVE[gi % RH_N];
    uint32_t l;
    if (rh.set == gi + 1u)
        return;
    rh.set = (uint8_t)(gi + 1u);
    for (l = 0; l < DV_NLANE; l++) {
        static const uint8_t OWN[DV_NLANE] = {DVT_PUNCH, DVT_SNARE, DVT_CLAP, DVT_HATC, DVT_HATO, DVT_TOM, DVT_RIM,
                                              DVT_BELL};
        uint32_t t = OWN[l];
        dv_param_t p;
        if (G->kit)
            t = DV_KTYPE(G->kit, l);
        else if (l == RH_T && G->swap & RH_CONGA)
            t = DVT_CONGA;
        else if (l == RH_R && G->swap & RH_CLAVE)
            t = DVT_CLAVE;
        else if (l == RH_B && G->swap & RH_CYM)
            t = DVT_CYM;
        dv_default(&p, t);
        if (l == RH_H || l == RH_O)
            p.level = 80;                                  /* (the hats a little back under the words) */
        dv_setup(&rh.c[l], &p);
        dv_init(&rh.v[l], rh.c[l].type);
        if (dv_uses_metal(rh.c[l].type))
            dv_metal_tune(&rh.m[l], &rh.c[l]);
    }
}

/* a hit on lane l (accent: at full, else a little softer) */
static void rh_hit(uint32_t l, uint32_t accent)
{
    l &= DV_NLANE - 1u;
    dv_trigger(&rh.v[l]);
    rh.g[l] = accent ? 32767 : 22000;
    if (l == RH_H && rh.v[RH_O].live)                      /* a closed hat chokes the open one */
        dv_choke(&rh.v[RH_O]);
}

#endif

/* the groove's sixteenth pos of its bar (fill: the bar's last beat a roll on the fill's lane) */
static void rh_step(uint32_t gi, uint32_t pos, uint32_t fill)
{
    const rh_groove_t *G = &RH_GROOVE[gi % RH_N];
    uint32_t l, beat = G->beat;
    rh_kit(gi);
    if (fill && pos >= G->steps - beat) {                  /* the fill: every sixteenth, louder to the bar line */
        rh_hit(G->fill, pos + 2u >= G->steps);
        if (pos == G->steps - beat)
            rh_hit(RH_K, 1);
        return;
    }
    for (l = 0; l < DV_NLANE; l++) {
        const char *p = G->lane[l];
        if (p && pos < G->steps && (p[pos] == 'x' || p[pos] == 'X'))
            rh_hit(l, p[pos] == 'X');
    }
}

/* the drums' block, added to l / r at level (0..127) */
static void rh_render(int32_t *L, int32_t *R, uint32_t n, uint32_t level)
{
    static const int8_t PAN[DV_NLANE] = {0, 0, 1, -3, -3, 3, 2, 2};   /* (eighths: hats left, toms and rims right) */
    int32_t y[CTL];
    uint32_t l, i;
#if !SP_808
    int32_t mb[CTL];
#endif
    if (!rh.set || n > CTL)
        return;
#if SP_808
    {   /* the guard's cap: the oldest ringing lanes end first */
        uint32_t on = 0;
        for (l = 0; l < DV_NLANE; l++)
            on += rh.v[l].on;
        while (on > rh_lmax) {
            uint32_t old = 0, age = 0;
            for (l = 0; l < DV_NLANE; l++)
                if (rh.v[l].on && rh.v[l].age >= age)
                    old = l, age = rh.v[l].age;
            rh.v[old].on = 0;
            on--;
        }
    }
#endif
    for (l = 0; l < DV_NLANE; l++) {
        int32_t gl, gr;
#if SP_808
        if (!rh.v[l].on)
            continue;
        s8_run(&rh.v[l], y, n);
#else
        if (!rh.v[l].live && !rh.v[l].trig)
            continue;
        if (dv_uses_metal(rh.c[l].type))
            dv_metal_run(&rh.m[l], mb, n);
        dv_run(&rh.c[l], &rh.v[l], mb, y, n);
#endif
        gl = (int32_t)((uint32_t)rh.g[l] * level >> 7) * (8 - PAN[l]) * RH_LVL >> 11;   /* (under the words, */
        gr = (int32_t)((uint32_t)rh.g[l] * level >> 7) * (8 + PAN[l]) * RH_LVL >> 11;   /* which run quiet) */
        for (i = 0; i < n; i++) {
            L[i] += (int32_t)((int64_t)y[i] * gl >> 15);
            R[i] += (int32_t)((int64_t)y[i] * gr >> 15);
        }
    }
}
