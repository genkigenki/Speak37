/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* MOPE on the screen, the pads and the LEDs, MOPE on and off (sp_ui.c's part; MOPE itself is sp_mope.c). MOPE is a
 * world of its own: PATCH (the FM-1: EDIT held 0.6 s) switches it on and off, and while it is on every knob, fader,
 * wheel, button, pad and key only talks to MOPE (sp_panel_*.c route them here: mo_ui_ctl, mo_pad), MASTER still
 * sets the volume, OCT- and OCT+ held 5 s still the update mode; off again, the keys' mode, the page, SETUP, PATCH
 * and the sound are as they were. Its screen, always, while it is on:
 *   the robot    left (4..94): a small, boxy, greyish-white robot in chunky pixels (3 px a pixel): shoulders slumped,
 *                heavy lids over its eyes in a dark visor, its mouth turned well down; its antenna drooping further
 *                and its head sinking lower the gloomier it is; a blink now and then, wide open and its antenna's bulb
 *                flickering for a moment after something rich; while it talks its mouth's bars move with the voice;
 *                a sigh lifts and drops its shoulders and lets out a puff; gloomier still a cloud, then rain; three
 *                gloom lights on its chest; a new pose at every press (MO_PJ: slumped, one arm half raised, sitting,
 *                head in hands, lying flat, hunched, a shrug, kneeling), reached like a puppet's (mo_pp_*: jointed
 *                limbs on damped springs, a hop, a drop and a settle); while it plays itself its eyes closed, nodding
 *                to its notes, notes rising
 *   the line     right (100..236): what it says in the display's sixteen-segment characters on black with a light-blue
 *                rim (the popups' look), four lines of 11, the word being said brightest, glowing and underlined
 *   the header   MOPE, and what it heard last (C MAJOR, FIFTH ON C, CLUSTER 5 ..)
 *   the legend   (the knob block's place) its gloom, what it asked for (ASK) or the game's pads and hits, the pads'
 *                meaning (the FM-1: its buttons'), how to leave
 * DARK: the whole screen black, the pads and LEDs off; LIGHTS: the pads sparkle, the button LEDs blink; GAME: the
 * light on its pad (the FM-1: its key's LED). The SMK-37's pads: the top row praises it (green), 9..12 poke it
 * (blue), 13..16 ask for a joke (violet); the FM-1: REC praises, PLAY pokes, SEQ a joke. */
#define MO_WHITE RGB(204, 208, 214)          /* (greyish: it has seen better days) */
#define MO_SHADE RGB(140, 148, 160)
#define MO_OUT RGB(56, 62, 72)
#define MO_VISOR RGB(10, 18, 26)
#define MO_EYE RGB(56, 150, 134)
#define MO_FEET RGB(92, 98, 110)
#define MO_CLOUD RGB(112, 120, 134)
#define MO_RAIN RGB(110, 190, 255)
#define MO_PUFF RGB(150, 160, 172)
#define MO_SHADOW RGB(18, 24, 30)
#define MO_S 3                             /* screen pixels a robot pixel */
#define MO_COLS 11                         /* the line: 11 characters, 4 lines */

static struct {
    uint32_t t_pad[16];
    uint16_t hit;
    uint8_t saved, prev, page, setup, mempage;             /* (MOPE on: what to give back) */
    sp_sound_t snd;
} mu;
static int32_t mo_ox, mo_oy;

/* the screen shows MOPE: always, while it is on */
static int mo_screen(void) { return sp.mode == SP_MODE_MOPE; }

/* ------------------------------------------------------------- the robot --- */
static void mr(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    sv_rect(mo_ox + x * MO_S, mo_oy + y * MO_S, w * MO_S, h * MO_S, c);
}
/* a limb: white with its outline */
static void mo_limb(int32_t x, int32_t y, int32_t w, int32_t h)
{
    mr(x, y, w, h, MO_OUT);
    mr(x + 1, y + 1, w - 2, h - 2, MO_WHITE);
}

/* its gloom lights (three, from green), its bulb's colour */
static const uint16_t MO_LC[3] = {RGB(80, 200, 100), RGB(240, 170, 40), RGB(240, 60, 50)};
static uint16_t mo_bulb(uint32_t keen, uint32_t sulk, uint32_t now)
{
    static const uint16_t BULB[3] = {RGB(110, 50, 50), RGB(255, 230, 120), RGB(255, 120, 90)};
    uint32_t b = keen ? ((now - mo.t_interest) / 90u & 1u) + (now - mo.t_interest > 700u) : sulk ? 2u : 0u;
    return BULB[b > 2u ? 1u : b];
}
/* lying flat on its back: the head at the left, its face to the side, an arm across its chest */
static void mo_robot_lie(uint32_t t, uint32_t talk, uint32_t keen, uint32_t sulk, uint32_t now)
{
    int32_t i;
    mr(1, 40, 28, 1, MO_SHADOW);
    mo_limb(21, 33, 7, 3);
    mo_limb(21, 36, 7, 3);
    mr(27, 32, 2, 4, MO_FEET);
    mr(27, 36, 2, 4, MO_FEET);
    mo_limb(9, 31, 13, 9);
    mr(10, 38, 11, 1, MO_SHADE);
    mr(12, 34, 7, 3, MO_VISOR);
    for (i = 0; i < 3; i++)
        mr(13 + 2 * i, 35, 1, 1, mo.gloom >= 40 * i ? MO_LC[i] : RGB(30, 36, 44));
    mo_limb(12, 29, 8, 3);                                 /* (an arm across its chest) */
    mr(0, 29, 10, 11, MO_OUT);
    mr(1, 30, 8, 9, MO_WHITE);
    mr(0, 29, 1, 1, SV_BG);
    mr(0, 39, 1, 1, SV_BG);
    mr(2, 31, 6, 7, MO_VISOR);
    mr(6, 32, 1, 2, keen ? SV_ON : MO_EYE);                /* (its eyes, the face turned: one above the other) */
    mr(6, 36, 1, 2, keen ? SV_ON : MO_EYE);
    if (talk)
        for (i = 0; i < 3; i++)
            mr(3, 33 + i, 1 + (int32_t)(((uint32_t)i * 2654435761u ^ t / 70u) % 2u), 1, SV_ON);
    else
        mr(3, 33, 1, 4, MO_EYE);
    mr(0, 27, 1, 2, MO_OUT);
    mr(0, 25, 2, 2, mo_bulb(keen, sulk, now));
}

/* ------------------------------------------------------------ the puppet --- */
/* The robot is a jointed puppet: its body and head drop on springs, each arm and leg two segments (shoulder - elbow -
 * hand, hip - knee - foot) whose angles follow their pose's on damped springs, a little underdamped, the forearms and
 * shins looser than the upper parts and pushed the other way when those move (follow-through): a new pose makes it
 * hop a little, drop into it, its limbs swinging and dangling until they settle. Fixed steps of 16 ms (mo_pp_step),
 * as many as the frames' time took; drawn in screen pixels (the motion smooth, not a robot pixel at a time).
 * Angles: 256 a turn, 0 straight down, 64 out to the side (mirrored on the right), 128 straight up. */
enum { MO_J_UL, MO_J_FL, MO_J_UR, MO_J_FR, MO_J_TL, MO_J_SL, MO_J_TR, MO_J_SR, MO_J_BODY, MO_J_HEAD, MO_NJ };
/* a pose's targets: the upper arm, the forearm (left, right), the thigh, the shin (left, right), the body's and the
 * head's drop (screen px) */
static const int16_t MO_PJ[MO_NPOSE][MO_NJ] = {
    {6, 2, 6, 2, 2, 0, 2, 0, 0, 0},                        /* SLUMP: arms hanging, standing */
    {6, 2, 62, 40, 2, 0, 2, 0, 0, 2},                      /* HALF: the right arm out, its forearm limply up */
    {12, -2, 12, -2, 40, -32, 40, -32, 9, 3},              /* SIT: on the floor, legs splayed */
    {98, 70, 98, 70, 40, -32, 40, -32, 9, 9},              /* HEAD: sitting, its hands against its head */
    {70, 20, 70, 20, 50, 20, 50, 20, 30, 30},              /* LIE: collapsing (then lying flat: mo_robot_lie) */
    {2, -2, 2, -2, 4, 0, 4, 0, 3, 9},                      /* HUNCH: head low, arms dangling */
    {58, 62, 58, 62, 3, 0, 3, 0, 0, 3},                    /* SHRUG: both forearms half up */
    {4, -2, 10, -4, 30, -70, 30, -70, 7, 6},               /* KNEEL: thighs out, shins in */
};
/* (the forearm's and the shin's angles are against the upper part's: the elbow's, the knee's bend) */
/* stiffness, damping (Q8 a step): the upper parts, the lower ones, the body, the head */
static const uint8_t MO_PK[MO_NJ] = {40, 26, 40, 26, 46, 34, 46, 34, 34, 30};
static const uint8_t MO_PC[MO_NJ] = {64, 38, 64, 38, 80, 56, 80, 56, 46, 44};
static struct {
    int32_t a[MO_NJ], v[MO_NJ];            /* Q8 */
    uint8_t ok, shown;                     /* (set up; the pose it has taken) */
    uint32_t t, t_pose;                    /* the last step's time (ms), the pose's change */
} mpp;

/* sin(2 pi a / 256), Q14 (SV_SIN64 interpolated) */
static int32_t mo_sin256(int32_t a)
{
    int32_t i = (a >> 2) & 63, f = a & 3, s0 = SV_SIN64[i], s1 = SV_SIN64[(i + 1) & 63];
    return s0 + (s1 - s0) * f / 4;
}
/* the targets now: the pose's, the head lower the gloomier, the shoulders up in a sigh, a nod in its sulk */
static void mo_pp_target(int32_t *t, uint32_t pose, int32_t gloom, int32_t sigh, int32_t nod)
{
    uint32_t j;
    for (j = 0; j < MO_NJ; j++)
        t[j] = MO_PJ[pose % MO_NPOSE][j];
    t[MO_J_HEAD] += (gloom >= 90 ? 9 : gloom >= 60 ? 6 : gloom >= 30 ? 3 : 0) + 3 * nod;
    t[MO_J_BODY] -= 3 * sigh;
}
/* one step of 16 ms: every joint on its spring, the lower parts pushed back by the upper ones' acceleration */
static void mo_pp_step(const int32_t *t)
{
    uint32_t j;
    for (j = 0; j < MO_NJ; j++) {
        int32_t acc = (MO_PK[j] * (t[j] * 256 - mpp.a[j]) >> 8) - (MO_PC[j] * mpp.v[j] >> 8);
        mpp.v[j] += acc;
        if (j < MO_J_BODY && !(j & 1u))                    /* (follow-through: the forearm, the shin swing behind) */
            mpp.v[j + 1u] -= acc * 3 / 4;
        mpp.a[j] += mpp.v[j];
    }
}
/* a new pose: a little hop, the limbs given a kick (the drop and the swing come from the springs) */
static void mo_pp_kick(uint32_t pose)
{
    uint32_t j;
    for (j = 0; j < MO_J_BODY; j++)
        mpp.v[j] += (j & 2u ? -1 : 1) * (int32_t)(160 + (pose * 37u + j * 53u) % 160u);
    mpp.v[MO_J_BODY] -= 900;                               /* (up: then it falls into the pose) */
    mpp.v[MO_J_HEAD] -= 500;
}
/* the puppet up to now (ms): set up the first time, a kick at a new pose, the steps the time took (at most 8) */
static void mo_pp_run(uint32_t now, uint32_t pose, int32_t gloom, int32_t sigh, int32_t nod)
{
    int32_t t[MO_NJ];
    uint32_t n, j;
    mo_pp_target(t, pose, gloom, sigh, nod);
    if (!mpp.ok) {
        for (j = 0; j < MO_NJ; j++)
            mpp.a[j] = t[j] * 256, mpp.v[j] = 0;
        mpp.ok = 1;
        mpp.shown = (uint8_t)pose;
        mpp.t = now;
    }
    if (pose != mpp.shown) {
        if (mpp.shown == MO_P_LIE)                         /* (getting up from lying: from the collapse) */
            for (j = 0; j < MO_NJ; j++)
                mpp.a[j] = MO_PJ[MO_P_LIE][j] * 256;
        mo_pp_kick(pose);
        mpp.shown = (uint8_t)pose;
        mpp.t_pose = now;
    }
    n = (now - mpp.t) / 16u;
    mpp.t += n * 16u;
    for (n = n > 8u ? 8u : n; n; n--)
        mo_pp_step(t);
}
static int32_t mo_pp(uint32_t j) { return (mpp.a[j] + 128) >> 8; }

/* a thick segment in screen px from (x0, y0) to (x1, y1): stamps along it (pass 0 its outline, 1 its fill) */
static void mo_seg(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t w, uint32_t pass)
{
    int32_t dx = x1 - x0, dy = y1 - y0, n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) :
              (dy < 0 ? -dy : dy), i, h = pass ? w / 2 - 1 : w / 2;
    n = n < 1 ? 1 : n;
    for (i = 0; i <= n; i += 2)
        sv_rect(x0 + dx * i / n - h, y0 + dy * i / n - h, 2 * h + 1, 2 * h + 1, pass ? MO_WHITE : MO_OUT);
    sv_rect(x1 - h, y1 - h, 2 * h + 1, 2 * h + 1, pass ? MO_WHITE : MO_OUT);
}
/* a limb from (x, y): its upper part (dir a1, length l1), its lower part (dir a2), mirrored to the right when right;
 * its end (a hand, a foot) */
static void mo_limb2(int32_t x, int32_t y, int32_t a1, int32_t a2, int32_t l1, int32_t l2, int32_t w, int right,
                     int foot)
{
    const int32_t sg = right ? 1 : -1;
    int32_t ex = x + sg * l1 * mo_sin256(a1) / 16384, ey = y + l1 * mo_sin256(a1 + 64) / 16384;
    int32_t hx = ex + sg * l2 * mo_sin256(a2) / 16384, hy = ey + l2 * mo_sin256(a2 + 64) / 16384;
    uint32_t pass;
    for (pass = 0; pass < 2u; pass++) {
        mo_seg(x, y, ex, ey, w, pass);
        mo_seg(ex, ey, hx, hy, w, pass);
    }
    if (foot) {
        sv_rect(hx - 9 + sg * 2, hy - 1, 18, 6, MO_FEET);
    } else {
        sv_rect(hx - 3, hy - 3, 7, 7, MO_OUT);
        sv_rect(hx - 2, hy - 2, 5, 5, MO_SHADE);
    }
}

static void mo_robot(int32_t ox, int32_t oy, uint32_t t)
{
    const uint32_t now = sp.now / 44u, talk = mo.nline && mo_busy(), sulk = mo.state == MO_SULK;
    const uint32_t keen = mo.t_interest && now - mo.t_interest < 1600u && !sulk;
    const int32_t g = mo.gloom, d = keen ? 0 : g >= 90 ? 3 : g >= 60 ? 2 : g >= 30 ? 1 : 0;
    const uint32_t ph = t % 7000u, sigh = g >= 30 && !talk && !sulk && !keen && ph < 1400u;
    int32_t y, i, lid, bp, hp, top = 25;
    mo_pp_run(t, mo.pose, keen ? 0 : g, sigh && ph < 700u, sulk ? (int32_t)(mos.seq_notes & 1u) : 0);
    bp = mo_pp(MO_J_BODY);
    hp = mo_pp(MO_J_HEAD);
    mo_ox = ox;
    mo_oy = oy;
    if (g >= 55 && !keen) {                                /* the cloud; gloomier: its rain */
        mr(4, 1, 5, 1, MO_CLOUD);
        mr(2, 2, 9, 1, MO_CLOUD);
        mr(1, 3, 11, 2, MO_CLOUD);
        mr(2, 5, 9, 1, MO_CLOUD);
        if (g >= 80)
            for (i = 0; i < 3; i++)
                mr(3 + 3 * i, 7 + (int32_t)((t / 110u + 2u * (uint32_t)i) % 4u), 1, 1, MO_RAIN);
    }
    if (mo.pose == MO_P_LIE && bp >= 24) {                 /* fallen flat: lying (bouncing on what is left) */
        mo_oy = oy + sp_clampi(bp - MO_PJ[MO_P_LIE][MO_J_BODY], -4, 4);
        mo_robot_lie(t, talk, keen, sulk, now);
        return;
    }
    mr(5, 40, 20, 1, MO_SHADOW);
    {   /* the legs: from the hips (they drop with the body), thigh and shin, a foot */
        const int32_t hy0 = oy + 35 * MO_S + bp;
        mo_limb2(ox + 11 * MO_S, hy0, mo_pp(MO_J_TL), mo_pp(MO_J_TL) + mo_pp(MO_J_SL), 8, 8, 10, 0, 1);
        mo_limb2(ox + 19 * MO_S, hy0, mo_pp(MO_J_TR), mo_pp(MO_J_TR) + mo_pp(MO_J_SR), 8, 8, 10, 1, 1);
    }
    {   /* the arms (behind the body while they hang; raised high: in front of the head, below) */
        const int32_t sy = oy + 27 * MO_S + bp;
        if (mo_pp(MO_J_UL) + mo_pp(MO_J_FL) < 110)
            mo_limb2(ox + 6 * MO_S, sy, mo_pp(MO_J_UL), mo_pp(MO_J_UL) + mo_pp(MO_J_FL), 13, 12, 8, 0, 0);
        if (mo_pp(MO_J_UR) + mo_pp(MO_J_FR) < 110)
            mo_limb2(ox + 24 * MO_S, sy, mo_pp(MO_J_UR), mo_pp(MO_J_UR) + mo_pp(MO_J_FR), 13, 12, 8, 1, 0);
    }
    /* the body (it drops on its spring): its shoulders slumped (a staircase), shaded at the right and the bottom */
    mo_oy = oy + bp;
    mr(10, top - 1, 10, 1, MO_OUT);
    for (y = top; y <= 34; y++) {
        int32_t x0 = y - top >= 2 ? 8 : 10 - (y - top);
        mr(x0 - 1, y, 32 - 2 * x0, 1, MO_OUT);
        mr(x0, y, 30 - 2 * x0, 1, MO_WHITE);
    }
    mr(7, 35, 16, 1, MO_OUT);
    mr(21, top + 2, 1, 33 - top - 1, MO_SHADE);
    mr(8, 34, 14, 1, MO_SHADE);
    mr(11, top + 4, 8, 4, MO_VISOR);                       /* its chest: three lights, its gloom */
    for (i = 0; i < 3; i++)
        mr(12 + 2 * i, top + 5, 1, 2, g >= 40 * i ? MO_LC[i] : RGB(30, 36, 44));
    mr(13, 24, 4, 1, MO_SHADE);                            /* the neck (the head sinks over it) */
    /* the head (its own spring on the body's): a wide box, its corners cut, the side bolts */
    mo_oy = oy + bp + hp;
    {
        const int32_t hy = 11;
        mr(5, hy - 1, 20, 14, MO_OUT);
        mr(6, hy, 18, 12, MO_WHITE);
        mr(5, hy - 1, 1, 1, SV_BG);
        mr(24, hy - 1, 1, 1, SV_BG);
        mr(5, hy + 12, 1, 1, SV_BG);
        mr(24, hy + 12, 1, 1, SV_BG);
        mr(23, hy + 1, 1, 10, MO_SHADE);
        mr(7, hy + 11, 16, 1, MO_SHADE);
        mr(4, hy + 4, 1, 3, MO_OUT);
        mr(25, hy + 4, 1, 3, MO_OUT);
        mr(7, hy + 2, 16, 8, MO_VISOR);
        /* the eyes: heavy lids (a blink, its sulk: closed; something rich: open), the outer corners drooping */
        lid = sulk || (t % 4300u) < 150u ? 3 : keen ? 0 : 2;
        for (i = 0; i < 2; i++) {
            const int32_t ex = i ? 17 : 9;
            const uint16_t c = keen || (talk && sp.vis_e > 900) ? SV_ON : MO_EYE;
            if (lid >= 3) {
                mr(ex, hy + 5, 4, 1, RGB(36, 90, 84));
                continue;
            }
            for (y = hy + 3 + lid; y <= hy + 5; y++)
                mr(ex + (y == hy + 3 + lid && !i), y, 4 - (y == hy + 3 + lid), 1, c);
            mr(ex + (i ? 3 : 0), hy + 3 + lid, 1, 1, MO_VISOR);
        }
        /* the mouth: its bars with the voice while it talks, a small o while it plays, else turned well down */
        if (talk) {
            int32_t e = sp.vis_e;
            for (i = 0; i < 6; i++) {
                uint32_t hsh = (uint32_t)(i * 2654435761u) ^ (t / 70u) * 40503u;
                int32_t hb = e < 80 ? 0 : (int32_t)((hsh >> 7) % 3u) + (e > 1500);
                hb = hb > 2 ? 2 : hb;
                if (hb)
                    mr(12 + i, hy + 9 - hb, 1, hb, SV_ON);
                else
                    mr(12 + i, hy + 8, 1, 1, MO_EYE);
            }
        } else if (sulk) {
            mr(14, hy + 7, 2, 2, MO_EYE);
        } else {
            mr(13, hy + 7, 4, 1, MO_EYE);
            mr(12, hy + 8, 1, 1, MO_EYE);
            mr(17, hy + 8, 1, 1, MO_EYE);
            mr(11, hy + 9, 1, 1, MO_EYE);
            mr(18, hy + 9, 1, 1, MO_EYE);
        }
        {   /* the antenna: upright when keen, else bent over and down, the more the gloomier; its bulb */
            const uint16_t bc = mo_bulb(keen, sulk, now);
            switch (keen ? 0 : d) {
            case 0: mr(19, hy - 5, 1, 4, MO_OUT); mr(18, hy - 7, 3, 2, bc); break;
            case 1: mr(19, hy - 4, 1, 3, MO_OUT); mr(20, hy - 5, 1, 1, MO_OUT); mr(21, hy - 6, 2, 2, bc); break;
            case 2: mr(19, hy - 3, 1, 2, MO_OUT); mr(20, hy - 4, 2, 1, MO_OUT); mr(22, hy - 4, 2, 2, bc); break;
            default: mr(19, hy - 3, 1, 2, MO_OUT); mr(20, hy - 4, 3, 1, MO_OUT); mr(23, hy - 3, 1, 1, MO_OUT);
                     mr(23, hy - 2, 2, 2, bc);
            }
        }
        if (sigh && ph >= 600u) {                          /* the sigh's puff, rising */
            int32_t k = (int32_t)(ph - 600u) / 160;
            mr(2, hy + 6 - 2 * k, 2, 2, MO_PUFF);
            if (k >= 2)
                mr(1, hy + 9 - 2 * k, 1, 1, MO_PUFF);
        }
    }
    {   /* the arms raised high: in front */
        const int32_t sy = oy + 27 * MO_S + bp;
        if (mo_pp(MO_J_UL) + mo_pp(MO_J_FL) >= 110)
            mo_limb2(ox + 6 * MO_S, sy, mo_pp(MO_J_UL), mo_pp(MO_J_UL) + mo_pp(MO_J_FL), 13, 12, 8, 0, 0);
        if (mo_pp(MO_J_UR) + mo_pp(MO_J_FR) >= 110)
            mo_limb2(ox + 24 * MO_S, sy, mo_pp(MO_J_UR), mo_pp(MO_J_UR) + mo_pp(MO_J_FR), 13, 12, 8, 1, 0);
    }
    mo_oy = oy;
    if (sulk)                                              /* its notes, rising (smoothly: a pixel at a time) */
        for (i = 0; i < 2; i++) {
            int32_t ny = 66 - (int32_t)((t / 30u + 27u * (uint32_t)i) % 60u);
            sv_rect(ox + 84, oy + ny, 3, 12, MO_RAIN);
            sv_rect(ox + 78, oy + ny + 9, 6, 6, MO_RAIN);
            sv_rect(ox + 87, oy + ny, 3, 3, MO_RAIN);
        }
}

/* ---------------------------------------------------------------- the line --- */
static void mo_text(void)
{
    const int32_t x0 = 109, y0 = 33;
    uint8_t wl[MO_LMAX], wc[MO_LMAX];
    uint32_t i, line = 0, col = 0, n = mo.nline, top = 0, k;
    int32_t cur = mo_cur_word();
    const uint32_t busy = mo_busy();
    sv_rbox(100, 26, 136, 132, RGB(0, 0, 0), SV_POP_RIM);
    sv_rbox(101, 27, 134, 130, RGB(0, 0, 0), SV_POP_RIM);
    if (!n) {                                              /* nothing said yet: it waits */
        for (k = 0; k < 3u; k++)
            sv_char(x0 + 44 + (int32_t)k * SV_CW, y0 + 45, ' ', 1, (fm1_ms / 500u) % 4u > k ? SV_MID : SV_GHOST, 0);
        return;
    }
    for (i = 0; i < n; i++) {                              /* the words wrapped at 11 */
        const char *nm = SP_WNAME[mo.line[i] & 0xFFFu];
        uint32_t len = str_len(nm) + ((mo.line[i] >> 12) == 3u);
        len = len > MO_COLS ? MO_COLS : len;
        if (col && col + len > MO_COLS)
            line++, col = 0;
        wl[i] = (uint8_t)line;
        wc[i] = (uint8_t)col;
        col += len + 1u;
    }
    if (cur >= 0)                                          /* the word being said on the second line, or nearer the top */
        top = wl[cur] > 1u ? wl[cur] - 1u : 0u;
    else if (line > 3u)
        top = line - 3u;
    if (top + 3u > line)
        top = line > 3u ? line - 3u : 0u;
    if (cur >= 0 && wl[cur] < top)
        top = wl[cur];
    for (i = 0; i < n; i++) {
        const char *nm = SP_WNAME[mo.line[i] & 0xFFFu];
        uint32_t pu = mo.line[i] >> 12, L, c, lvl;
        if (wl[i] < top || wl[i] >= top + 4u)
            continue;
        L = wl[i] - top;
        lvl = cur < 0 ? (busy ? 1u : 0u) : (int32_t)i < cur ? 0u : (int32_t)i == cur ? 2u : 1u;
        for (c = 0; nm[c] && wc[i] + c < MO_COLS; c++) {
            int32_t cx = x0 + (int32_t)(wc[i] + c) * SV_CW, cy = y0 + (int32_t)L * SV_LH;
            uint32_t dp = !nm[c + 1u] && (pu == 1u || pu == 2u);
            if (lvl == 2u) {
                sv_rect(cx - 1, cy + 25, SV_CW + 1, 4, SV_GLOW);
                sv_rect(cx, cy + 26, SV_CW - 1, 2, SV_ON);
            }
            sv_char(cx, cy, nm[c], dp, lvl == 2u ? SV_ON : lvl ? SV_MID : SV_DIM, lvl == 2u);
        }
        if (pu == 3u && wc[i] + c < MO_COLS)
            sv_char(x0 + (int32_t)(wc[i] + c) * SV_CW, y0 + (int32_t)L * SV_LH, '?', 0,
                    lvl == 2u ? SV_ON : lvl ? SV_MID : SV_DIM, 0);
    }
}

static void mo_scene(void)
{
    mo_robot(4, 30, fm1_ms);
    mo_text();
}
static int mo_dark(void);
static void mo_black(uint32_t y, uint32_t h);
static void mo_draw_display(void)
{
    if (mo_dark()) {
        mo_black(SP_DISP_Y, SP_INFO_Y - SP_DISP_Y);
        return;
    }
    sv_begin(SP_DISP_Y, SP_INFO_Y - SP_DISP_Y);
    mo_scene();
    sp_popup_draw();
    cv_blit(0, SP_DISP_Y);
}
static void mo_draw_info(void)
{
    if (mo_dark()) {
        mo_black(SP_INFO_Y, SP_KNOB_Y - SP_INFO_Y);
        return;
    }
    sv_begin(SP_INFO_Y, SP_KNOB_Y - SP_INFO_Y);
    mo_scene();
    cv_blit(0, SP_INFO_Y);
}
/* the header's right side: what it heard (else the voice) */
static const char *mo_header_note(void)
{
    static char t[24];
    if (!mo.have_h)
        return 0;
    mo_heard_text(&mo.h, t);
    return t[0] ? t : 0;
}

/* ------------------------------------------------------- dark, lights --- */
static int mo_dark(void) { return sp.mode == SP_MODE_MOPE && mo.act == MO_AC_DARK && mo.aphase == 1u; }
static int mo_lights(void) { return mo.act == MO_AC_LIGHTS && mo.aphase == 1u; }
static int mo_gaming(void) { return mo.act == MO_AC_GAME && mo.aphase == 1u; }
/* a strip of the screen black (DARK) */
static void mo_black(uint32_t y, uint32_t h)
{
    cv_begin(240, h, RGB(0, 0, 0));
    cv_blit(0, y);
}

/* ------------------------------------------------------------ the legend --- */
/* a control's name (mope.txt [=CTL]) as text */
static void mo_ctl_text(uint32_t id, char *t)
{
    uint32_t f = MO_CLS[MO_C_CTL] + (id < MO_NCT ? id : 0u), k;
    char *q = t;
    for (k = MO_AT[f]; k < MO_AT[f + 1u]; k++) {
        if (k > MO_AT[f])
            *q++ = ' ';
        q = mo_cat(q, SP_WNAME[MO_W[k] & 0xFFFu]);
    }
}
/* the knob block's place in MOPE: its gloom, what it waits for (ASK) or the game's pads, the pads (the FM-1: its
 * buttons) and how to leave */
static void mo_draw_legend(void)
{
    const int32_t y = SP_KNOB_Y;
    char t[40];
    if (mo_dark()) {
        mo_black(SP_KNOB_Y, 240 - SP_KNOB_Y);
        return;
    }
    sv_begin(y, 240 - y);
    sv_rect(0, y, 240, 1, SV_GRID);
    sv_text(4, y + 5, &AF_S, "GLOOM", SV_LAB);
    sv_bar(54, y + 8, 120, 8, mo.gloom * 127 / 120, mo.gloom >= 90 ? SV_RED : SV_HOT);
    sp_d(t, mo.gloom * 100 / 120, 0);
    sv_text(180, y + 5, &AF_S, t, SV_TEXT);
    if (mo.act == MO_AC_ASK && mo.aphase) {                /* what it asked for */
        char c[32];
        mo_ctl_text(mo.target, c);
        sp_s(sp_s(t, "WANTS: "), c);
        sp_fit(t, 232);
        sv_text(4, y + 26, &AF_S, t, SV_HOT);
    } else if (mo_gaming()) {                              /* the game: the pads, its light, the hits */
        uint32_t k;
        for (k = 0; k < 16u; k++)
            sv_rect(4 + (int32_t)(k % 8u) * 14, y + 24 + (int32_t)(k / 8u) * 11, 12, 9,
                    k == mo.gpos ? SV_ON : SV_GHOST);
        sp_s(sp_d(sp_s(t, "HITS "), mo.ghits, 0), " / 3");
        sv_text(126, y + 26, &AF_S, t, SV_HOT);
        sp_d(sp_s(t, "OW "), mo.gmiss, 0);
        sv_text(126, y + 40, &AF_S, t, SV_TEXT);
    } else {
#if BOARD_SMK37                                            /* the pads: their colours */
        static const char *const L[3] = {"1-8 PRAISE", "9-12 POKE", "13-16 JOKE"};
        static const uint16_t C[3] = {RGB(80, 255, 120), RGB(110, 160, 255), RGB(190, 120, 255)};
        uint32_t k;
        for (k = 0; k < 3u; k++) {
            int32_t x = (int32_t)(k % 2u) * 120 + 4, yy = y + 24 + (int32_t)(k / 2u) * 18;
            sv_rect(x, yy + 3, 10, 10, C[k]);
            sv_text(x + 15, yy + 1, &AF_S, L[k], C[k]);
        }
#else
        sv_text(4, y + 26, &AF_S, "REC PRAISE  PLAY POKE  SEQ JOKE", SV_TEXT);
#endif
    }
#if BOARD_SMK37
    sv_text(4, y + 62, &AF_S, "PATCH: BACK TO SPEAK37", SV_TAG);
#else
    sv_text(4, y + 62, &AF_S, "HOLD EDIT: BACK TO SPEAK37", SV_TAG);
#endif
    cv_blit(0, (uint32_t)y);
}

/* ------------------------------------------------------- in, out, pads --- */
/* MOPE on (its voice: ROBOT, quick, dry, in the centre; a speech recogniser on 40 of its lines, word for word, finds
 * it as clear as LAB AI: ROBOT at 141 % 36 %, LAB AI 37 %; a second voice on each word, a unison or an octave, made
 * ROBOT less clear, 30 % and 35 %) and off again: the keys' mode, the page, SETUP, PATCH and the sound as they were */
static void mo_toggle(void)
{
    if (sp.mode != SP_MODE_MOPE) {
        mu.prev = sp.mode;
        mu.page = su.page;
        mu.setup = su.setup;
        mu.mempage = su.mempage;
        mu.snd = sp.snd;
        mu.saved = 1;
        su.setup = 0;
        su.mempage = 0;
        if (sp.auto_on)
            sp.req_auto = 2;
        sp_voice_load(&sp.snd, SP_ROBOT);
        sp.snd.speed = 80;
        sp.snd.dmix = 0;
        sp.snd.reverb = 14;
        sp.snd.size = 50;
        sp.bend = 0;
        sp.modw = 0;
        sp.mode = SP_MODE_MOPE;
        sp_changed();
        sp_popup("MOPE", "IS LISTENING");
    } else {
        if (mu.saved) {
            int8_t o = sp.snd.oct;
            uint8_t tp = sp.snd.tempo;
            sp.snd = mu.snd;
            sp.snd.oct = o;
            sp.snd.tempo = tp;
            mu.saved = 0;
        }
        sp.mode = mu.prev < SP_MODE_MOPE ? mu.prev : SP_MODE_SENT;
        su.page = mu.page;
        su.setup = mu.setup;
        su.mempage = mu.mempage;
        sp_changed();
        sp_popup("MOPE", "GONE");
    }
    su.redraw = 1;
}
/* a control touched while MOPE listens (dir: which way it moved, 0 unknown) */
static void mo_ui_ctl_dir(uint32_t id, int32_t dir)
{
    if (sp.mode == SP_MODE_MOPE)
        mo_ctl_dir(id, sp.now / 44u, dir);
}
static void mo_ui_ctl(uint32_t id) { mo_ui_ctl_dir(id, 0); }
/* a pad (SMK) */
static void mo_pad(uint32_t k)
{
    k &= 15u;
    mo_pad_in(k, sp.now / 44u);
    mu.hit |= (uint16_t)(1u << k);
    mu.t_pad[k] = fm1_ms | 1u;
}
/* the pads' colours: the game's light, the lights' show, dark, ASK for a pad (all of them pulsing), else praise
 * (green), poke (blue), joke (violet), at full a moment when pressed */
static void mo_pad_rgb(uint32_t pad, uint8_t *rgb)
{
    static const uint8_t PR[3] = {80, 255, 120}, PO[3] = {110, 160, 255}, JO[3] = {190, 120, 255};
    static const uint8_t SH[6][3] = {{255, 60, 40}, {255, 180, 0}, {80, 255, 80}, {0, 200, 255}, {120, 80, 255},
                                     {255, 80, 200}};
    const uint8_t *c = pad < 8u ? PR : pad < 12u ? PO : JO;
    uint32_t k, pct = pad < 8u ? 30u : 20u;
    if (mo_dark()) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        return;
    }
    if (mo_gaming()) {
        static const uint8_t L[3] = {255, 240, 170}, O[3] = {0, 20, 60};
        c = pad == mo.gpos ? L : O;
        pct = 100u;
    } else if (mo_lights()) {                              /* a few little lights: a sparkle wandering round */
        uint32_t s = (pad * 7u + fm1_ms / 140u) % 5u;
        c = SH[(pad + fm1_ms / 400u) % 6u];
        pct = s < 2u ? 100u : s == 2u ? 25u : 0u;
    } else if (mo.act == MO_AC_ASK && mo.aphase && mo.target == MO_CT_PAD) {
        static const uint8_t W[3] = {220, 220, 220};
        c = W;
        pct = (fm1_ms / 300u) & 1u ? 40u : 8u;
    } else if (mu.t_pad[pad & 15u] && fm1_ms - mu.t_pad[pad & 15u] < 200u)
        pct = 100u;
    for (k = 0; k < 3u; k++)
        rgb[k] = (uint8_t)(c[k] * pct / 100u);
}
/* a button's LED in MOPE (k: its LED; on: what it shows outside the shows) */
static int mo_led(uint32_t k, int on)
{
    if (mo_dark())
        return 0;
    if (mo_lights())
        return (int)((k * 5u + fm1_ms / 150u) % 3u == 0u);
    return on;
}
/* (the FM-1's PLAY, REC, SEQ in MOPE: a press, its pose) */
static void mo_ctl_pose(void) { mo_pose_next(sp.now / 44u); }
