/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37 in the browser: the firmware's own sources (the speech, the 808 and the grooves, MOPE, the UI, the screen
 * and the SMK-37's panel), unchanged, against a simulated SMK-37, compiled to WebAssembly (web/sim/build.sh). The
 * platform below is the host tests' (tests/sp_screen.c) with the wheels, MASTER on K1 and the store added. The
 * page's AudioWorklet calls sim_render(n): the device's clock runs with the audio (44118 Hz, the I2S rate), the main
 * loop of sp_main.c (the controls, MOPE, the LEDs, the screen) every 15 ms of it. Fixed point as on the device. */
#include <stdint.h>
#include <string.h>
#define EXPORT __attribute__((used, visibility("default")))
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")

/* ---- the platform the UI reads (smk_input.h's fm1_in, as the panel scan fills it) */
static volatile uint32_t fm1_ms;
#define BOARD_SMK37 1
#define FM1_NENC 8u
#define SMK_NLED 19u
#define SMK_NPAD 16u
enum { FM1_BTN_OCT_DOWN = 4, FM1_BTN_OCT_UP = 5 };
#define FM1_OCT_BOTH (1u << FM1_BTN_OCT_DOWN | 1u << FM1_BTN_OCT_UP)
static struct {
    uint8_t pad_st[16], fader[4], pad_press[16];
    uint32_t frames, buttons;
    uint64_t keys;
    uint8_t key_vel[64];
    uint16_t wheel[2];
} fm1_in;
#define SP_KEYS() fm1_in.keys
#define SP_VEL(k) fm1_in.key_vel[k]
static uint8_t fm1_led[SMK_NLED];
static uint32_t padled_hold_until;
static uint8_t t_pad[16][3];
static void padled_set(uint32_t p, uint8_t r, uint8_t g, uint8_t b) { t_pad[p][0] = r, t_pad[p][1] = g, t_pad[p][2] = b; }
static int32_t t_enc[8];
static uint32_t t_press, t_rel;
static uint32_t fm1_input_edges(uint32_t *r)
{
    uint32_t p = t_press;
    if (r)
        *r = t_rel;
    t_press = t_rel = 0;
    return p;
}
static int32_t fm1_enc_take(uint32_t e)
{
    int32_t v = t_enc[e & 7u];
    t_enc[e & 7u] = 0;
    return v;
}
static uint32_t fm1__lock(void) { return 0; }
static void fm1__unlock(uint32_t v) { (void)v; }
static unsigned str_len(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    return n;
}
static void str_cpy(char *d, const char *s, unsigned max)
{
    unsigned i = 0;
    while (max && i + 1u < max && s[i]) {
        d[i] = s[i];
        i++;
    }
    if (max)
        d[i] = 0;
}

/* ---- the LCD: a framebuffer (RGB565), a count of the transfers (the page redraws when it moves) */
static uint16_t fb[240 * 240];
static uint32_t fb_draws;
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++) {
            uint16_t c = px[j * w + i];
            fb[(y + j) * 240u + x + i] = (uint16_t)(c >> 8 | c << 8);   /* (the panel's byte order) */
        }
    fb_draws++;
}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
    fb_draws++;
}
#define GFX_HOOK_TEXT(x0, y0, x1, y1, s, flags) ((void)0)
#define GFX_HOOK_BLIT(x, y, r0) ((void)0)
#define GFX_HOOK_BEGIN() ((void)0)
#define GFX_HOOK_PIXELS(n) ((void)0)
#define GFX_DOUBLE 1

/* ---- the firmware */
#include "../../firmware/src/gfx.c"
#include "../../firmware/src/sp_dsp.c"
#define SMO_TX_DONE() 1u                   /* (TRS MIDI OUT: nowhere to go here) */
#define SMO_TX_PUT(b) ((void)(b))
#include "../../firmware/src/smk_midi_out.h"
#define SP_TRS 1
#include "../../firmware/src/sp_state.c"
#include "../../firmware/src/sp_trs.c"
#include "../../firmware/src/sp_view.c"
#include "../../firmware/src/sp_ui.c"

/* the store: the records the device keeps in flash, here handed to the page (it keeps them in the browser) */
static uint32_t st_saves, st_usaves;
static sp_user_rec_t sp_urec;
static void sp_store_save(void)
{
    sp_store_seal(&vst);
    st_saves++;
}
static void sp_user_save(void)
{
    sp_user_pack(&sp_urec);
    st_usaves++;
}
#include "../../firmware/src/sp_panel_smk.c"

/* ---- sp_main.c's parts that touch no hardware: the soft limiter, MASTER (K1, the SMK's virtual pot), the wheels */
static int32_t sp_master_q12 = 2047;
static int32_t smk_master = 724;
static int32_t sp_limit(int32_t x)
{
    int32_t a = x < 0 ? -x : x, y;
    if (a <= 24576)
        return x;
    a -= 24576;
    a = a > 200000 ? 200000 : a;
    y = 24576 + a * 8192 / (a + 8192);
    return x < 0 ? -y : y;
}
static void sp_master(void)
{
    static int32_t last = -1;
    int32_t pot;
    smk_master += fm1_enc_take(0) * 24;    /* (smk_input.h: 24 a detent) */
    smk_master = smk_master < 0 ? 0 : smk_master > 1023 ? 1023 : smk_master;
    pot = smk_master;
    sp_pot = (uint32_t)pot;
    sp_master_q12 = (int32_t)(sp_pot * sp_pot / 256u);
    if (last >= 0 && (int32_t)sp_pot != last)
        sp_touch(0);
    last = (int32_t)sp_pot;
}
#define SP_WHEEL_BAND 8
static void sp_wheels(void)                /* (sp_main.c's, as it is) */
{
    enum { DEAD = 12, SPAN0 = 80, MOD_LO = 485, MOD_HI = 615 };
    static int32_t pc, plo, phi, mlo, mhi, rest, lastb = 99999, lastm = -1;
    int32_t p = fm1_in.wheel[0], m = fm1_in.wheel[1], d, b, mw;
    if (fm1_in.frames < 300u)
        return;
    if (!pc) {
        pc = p;
        plo = p - SPAN0;
        phi = p + SPAN0;
        mlo = m < MOD_LO ? m : MOD_LO;
        mhi = m > MOD_HI ? m : MOD_HI;
    }
    plo = p < plo ? p : plo;
    phi = p > phi ? p : phi;
    mlo = m < mlo ? m : mlo;
    mhi = m > mhi ? m : mhi;
    d = p - pc;
    if (d > -DEAD && d < DEAD) {
        b = 0;
        if (++rest > 200) {
            pc += d > 0 ? 1 : d < 0 ? -1 : 0;
            rest = 0;
        }
    } else {
        rest = 0;
        b = d > 0 ? (d - DEAD) * 8191 / (phi - pc - DEAD > 1 ? phi - pc - DEAD : 1)
                  : (d + DEAD) * 8192 / (pc - plo - DEAD > 1 ? pc - plo - DEAD : 1);
        b = sp_clampi(b, -8192, 8191);
    }
    mw = mhi - m <= DEAD ? 0 : (mhi - m - DEAD) * 127 / (mhi - mlo - DEAD > 1 ? mhi - mlo - DEAD : 1);
    mw = sp_clampi(mw, 0, 127);
    if (sp.mode == SP_MODE_MOPE) {
        static mo_mv_t mvp, mvm;
        int32_t dp = mo_move(&mvp, p, SP_WHEEL_BAND, fm1_ms), dm = mo_move(&mvm, m, SP_WHEEL_BAND, fm1_ms);
        if (dp)
            mo_ui_ctl_dir(MO_CT_BEND, dp);
        if (dm)
            mo_ui_ctl_dir(MO_CT_MODW, -dm);
    }
    sp_trs_bend(b);
    sp_trs_modw(mw);
    if (b != lastb) {
        int32_t was = lastb;
        lastb = b;
        sp.bend = sp.mode == SP_MODE_MOPE ? 0 : (int16_t)(b * (int32_t)sp.bend_range / 128);
        if (was != 99999) {
            char t[20], *q = sp_s(t, "BEND ");
            sp_d1(q, sp.bend * 10 / 64);
            sp_hnote(t);
        }
    }
    if (mw != lastm) {
        if (lastm >= 0) {
            char t[20];
            sp_d(sp_s(t, "VIB "), mw * 100 / 127, 0);
            sp_hnote(t);
        }
        lastm = mw;
        sp.modw = (uint8_t)(sp.mode == SP_MODE_MOPE ? 0 : mw);
    }
}

/* ---- the simulator: the clock, the main loop, the audio out */
#define TICK_MS 15u
static uint32_t ns_lo, ns_hi;              /* samples rendered (64 bit in two) */
static uint32_t boot_t0, booted, next_tick;
static int32_t blk[2u * SP_BLK];
static uint32_t blk_r = SP_BLK;            /* frames of blk already handed out */
static float out_l[2048], out_r[2048];
static uint32_t midi_q[64], midi_r, midi_w;

static void main_tick(void)                /* sp_main.c's loop body, once every 15 ms */
{
    if (!booted) {
        uint32_t t = fm1_ms - boot_t0;
        if (t < SP_BOOT_MS) {
            sp_boot_frame(t);
            return;
        }
        booted = 1;
        lcd_fill(0, 0, 240, 240, SV_BG);
        su.redraw = 1;
    }
    sp_master();
    sp_ui_input();
    sp_wheels();
    sp_ui_autosave(sp_pot);
    sp_ui_leds();
    sp_ui_draw();
}
static void render_block(void)             /* sp_main.c's sp_audio_block, and the clock */
{
    uint32_t i;
    uint64_t ns = (uint64_t)ns_hi << 32 | ns_lo;
    while (midi_r != midi_w)
        sp_midi(midi_q[midi_r++ % 64u]);
    sp_render(blk, SP_BLK);
    for (i = 0; i < 2u * SP_BLK; i++)
        blk[i] = sp_limit((int32_t)((int64_t)blk[i] * sp_master_q12 >> 12));
    ns += SP_BLK;
    ns_lo = (uint32_t)ns, ns_hi = (uint32_t)(ns >> 32);
    fm1_ms = 5000u + (uint32_t)(ns * 1000u / SP_FS);
    fm1_in.frames++;
    blk_r = 0;
    if ((int32_t)(fm1_ms - next_tick) >= 0) {
        next_tick += TICK_MS;
        main_tick();
    }
}

EXPORT void sim_init(uint32_t seed)
{
    uint32_t k;
    palette_set(SP_PALETTE);               /* the boot as sp_main.c's (the store: sim_load) */
    sp_init();
    sp_store_defaults(&vst);
    sp.snd = vst.cur;
    sp.mode = vst.mode;
    sp.sent = vst.sent;
    sp.req_sent = vst.sent;
    sp_glob.bend = vst.bend;
    sp_glob.chord = vst.chord;
    sp.drums = sp_glob.drums = vst.drums;
    sp.swing = sp_glob.swing = vst.swing;
    sp.bend_range = vst.bend;
    sp.chord_ms = vst.chord;
    su.msel = (uint8_t)(vst.loaded < SP_MEMS ? vst.loaded : 0u);
    smk_master = vst.master;
    sp_glob.trs_on = (uint8_t)!vst.trs_off;
    trs_out = sp_glob.trs_on;
    sp_apply();
    for (k = 0; k < 64u; k++)
        fm1_in.key_vel[k] = 100;
    for (k = 0; k < 4u; k++)
        fm1_in.fader[k] = 64;
    fm1_in.wheel[0] = 540;                 /* (the pitch wheel's rest; the mod wheel down) */
    fm1_in.wheel[1] = 615;
    sp_mhz = 320;                          /* (the boot screen's clock: a unit's) */
    ns_lo = seed & 0xFFFu;                 /* (the clock MOPE seeds itself from: another start each time) */
    fm1_ms = 5000u + ns_lo * 1000u / SP_FS;
    boot_t0 = fm1_ms;
    next_tick = fm1_ms;
    lcd_fill(0, 0, 240, 240, SV_BG);
}
/* the store from the page (the records saved last time): before the first sim_render */
static sp_store_t st_in;
EXPORT uint8_t *sim_store(void) { return (uint8_t *)&st_in; }
EXPORT uint32_t sim_store_size(void) { return sizeof st_in; }
EXPORT uint8_t *sim_ustore(void) { return (uint8_t *)&sp_urec; }
EXPORT uint32_t sim_ustore_size(void) { return sizeof sp_urec; }
EXPORT uint8_t *sim_vst(void) { return (uint8_t *)&vst; }
EXPORT uint32_t sim_saves(void) { return st_saves; }
EXPORT uint32_t sim_usaves(void) { return st_usaves; }
EXPORT void sim_load(uint32_t have, uint32_t uhave)
{
    if (have) {
        if (sp_store_take(&st_in, (int32_t)sizeof st_in)) {
            vst = st_in;
            sp.snd = vst.cur;
            sp.mode = vst.mode;
            sp.sent = vst.sent;
            sp.req_sent = vst.sent;
            sp_glob.bend = vst.bend;
            sp_glob.chord = vst.chord;
            sp.drums = sp_glob.drums = vst.drums;
            sp.swing = sp_glob.swing = vst.swing;
            sp.bend_range = vst.bend;
            sp.chord_ms = vst.chord;
            su.msel = (uint8_t)(vst.loaded < SP_MEMS ? vst.loaded : 0u);
            smk_master = vst.master;
            sp_glob.trs_on = (uint8_t)!vst.trs_off;
            trs_out = sp_glob.trs_on;
        }
    }
    sp_user_take(&sp_urec, uhave ? (int32_t)sizeof sp_urec : -1);
    sp_apply();
}
EXPORT uint32_t sim_rate(void) { return SP_FS; }
EXPORT float *sim_out_l(void) { return out_l; }
EXPORT float *sim_out_r(void) { return out_r; }
EXPORT void sim_render(uint32_t n)         /* n frames (at most 2048) into out_l / out_r, -1 .. 1 */
{
    uint32_t i;
    n = n > 2048u ? 2048u : n;
    for (i = 0; i < n; i++) {
        if (blk_r >= SP_BLK)
            render_block();
        out_l[i] = (float)blk[2u * blk_r] * (1.0f / 32768.0f);
        out_r[i] = (float)blk[2u * blk_r + 1u] * (1.0f / 32768.0f);
        blk_r++;
    }
}
EXPORT uint16_t *sim_fb(void) { return fb; }
EXPORT uint32_t sim_draws(void) { return fb_draws; }
EXPORT uint8_t *sim_pads(void) { return &t_pad[0][0]; }     /* 16 x RGB */
EXPORT uint32_t sim_leds(void)
{
    uint32_t k, m = 0;
    for (k = 0; k < SMK_NLED; k++)
        m |= (uint32_t)(fm1_led[k] != 0) << k;
    return m;
}
/* the controls (the HAL's numbers): keys 0..36 (C3..C6), pads 0..15 (top row 0..7), buttons 0..17, K1..K8 0..7 */
EXPORT void sim_key(uint32_t k, uint32_t down, uint32_t vel)
{
    if (k >= 37u)
        return;
    if (down) {
        fm1_in.key_vel[k] = (uint8_t)(vel < 1u ? 1u : vel > 127u ? 127u : vel);
        fm1_in.keys |= 1ull << k;
    } else
        fm1_in.keys &= ~(1ull << k);
}
EXPORT void sim_pad(uint32_t p, uint32_t vel)
{
    if (p >= 16u)
        return;
    fm1_in.pad_st[p] = vel ? 0x80u : 0u;
    fm1_in.pad_press[p] = (uint8_t)(vel > 127u ? 127u : vel);
}
EXPORT void sim_button(uint32_t b, uint32_t down)
{
    if (b >= 18u)
        return;
    if (down) {
        t_press |= 1u << b;
        fm1_in.buttons |= 1u << b;
    } else {
        t_rel |= 1u << b;
        fm1_in.buttons &= ~(1u << b);
    }
}
EXPORT void sim_enc(uint32_t e, int32_t steps) { t_enc[e & 7u] += steps; }
EXPORT void sim_fader(uint32_t i, uint32_t v) { fm1_in.fader[i & 3u] = (uint8_t)(v > 127u ? 127u : v); }
EXPORT void sim_wheel(uint32_t i, uint32_t raw) { fm1_in.wheel[i & 1u] = (uint16_t)(raw > 1023u ? 1023u : raw); }
EXPORT void sim_midi(uint32_t st, uint32_t d1, uint32_t d2)   /* a MIDI message as USB-MIDI brings it */
{
    uint32_t cin = (st >> 4) & 15u;
    if (st == 0xF8u || st == 0xFAu || st == 0xFBu || st == 0xFCu)
        cin = 0xFu;
    if (midi_w - midi_r < 64u)
        midi_q[midi_w++ % 64u] = cin | (st & 0xFFu) << 8 | (d1 & 0x7Fu) << 16 | (d2 & 0x7Fu) << 24;
}
/* (the tests: what the firmware is doing) */
EXPORT uint32_t sim_mode(void) { return sp.mode; }
EXPORT uint32_t sim_booted(void) { return booted; }
EXPORT uint32_t sim_speaking(void) { return sp.speaking; }
EXPORT uint32_t sim_mope(uint32_t what)
{
    switch (what) {
    case 0: return mo.state;
    case 1: return mo.act;
    case 2: return mo.pose;
    case 3: return mo_busy();
    case 4: return (uint32_t)mo.gloom;
    case 5: return mo.lines;
    case 6: return mo.gpos;
    case 7: return (uint32_t)mo_gaming();
    case 8: return mos.seq_on;
    default: return 0;
    }
}
EXPORT void sim_mope_poke(uint32_t what)   /* (the tests: its game now; bored enough to sulk) */
{
    if (sp.mode != SP_MODE_MOPE)
        return;
    if (what == 0u)
        mo_start(MO_AC_GAME, sp.now / 44u);
    else
        mo.gloom = 110;
}
