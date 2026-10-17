/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Host test of Speak37's panel and screen (src/sp_ui.c, sp_view.c, sp_state.c, and the board's panel: sp_panel_smk.c,
 * or with -DT_FM1=1 sp_panel_fm1.c) with the real sound (sp_dsp.c) and drawing (gfx.c and its fonts) against a stubbed
 * keyboard. Built twice by tests/run_tests.sh (SMK-37, FM-1):
 *   - the knobs: the page, the voice, the bank (SMK K4) or the sentence (FM-1 PRESETS, across the banks), the page's
 *     four on every page and SETUP, far past both ends: always in range
 *   - the pads (SMK): a tap selects the sentence from its first word, one pad a press; PATCH: a tap loads a memory,
 *     held 1 s saves it (an empty one says so); the pads' colours: a sentence 10 %, the one in use 45 %, at full while
 *     it speaks; memories: empty 6 %, in use 30 %, loaded 100 %. FM-1: SAVE held 1 s saves, SAVE + PRESETS picks a
 *     memory and loads it when SAVE is let go
 *   - the buttons: LETTERS, PLAY, the cursor, REPEAT, CHORD, HOLD, the octave, the pages; their LEDs
 *   - every bank's name in the header: its colour against the background at least 4.5 : 1 (WCAG AA)
 *   - USER: an empty pad's hint, a long press opens the editor; letters, ADD, the list (PICK), NEXT (a word not in the
 *     vocabulary spelled), DEL WORD, EN / DE, PREVIEW, OK (saved, said), CANCEL (nothing kept); the record found again
 *     by the words' text under another vocabulary, a damaged one refused
 *   - RHYTHM: the page, a groove a pad (SMK) or KNOB 4 (FM-1), its pad pulsing, stop
 *   - the record: written, read back the same; a damaged one refused (the defaults); every value brought into range
 *   - the knob block's tags: the SMK's K1..K8, the FM-1's POT SEL ALG PRE and 1..4 (the SMK's K5..K8 are its KNOB 1..4)
 *   - MOPE (its own button: SMK PATCH, FM-1 EDIT held): its voice (the clearest: LAB AI, quick, dry), its screen even
 *     over the RHYTHM page, every knob, fader and button changing nothing but getting its comment, a chord commented
 *     on, a lost key-up not leaving it deaf, a new pose at every press (never twice in a row), DARK (the screen
 *     black), LIGHTS (the pads and LEDs), GAME (its light, OW, a hit, it gives up), ASK (the wrong control, the right
 *     one), its sulk and BACK, the pads; off again: the mode, the page, the sound as before; its screens (mope_talk,
 *     mope_pose_*, mope_game, mope_ask, mope_sulk)
 *   - the screens: every page, SETUP, PATCH, LETTERS, a popup, the faders' strip, the boot; each text inside the screen
 *     and inside its knob cell; the frame's drawing time (host). PPMs into the directory given (run_tests.sh: PNGs);
 *     "main" is the screenshot of the home screen (a joke, its word four spoken). */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")

/* ---- the platform the UI reads, stubbed */
static volatile uint32_t fm1_ms;

#if T_FM1
#define BOARD_SMK37 0
#define FM1_NENC 7u
enum { FM1_BTN_OCT_DOWN = 0, FM1_BTN_OCT_UP = 1 };
static struct { uint32_t notes, buttons, frames; } fm1_in;
#define SP_KEYS() ((uint64_t)fm1_in.notes << 5)
#define SP_VEL(k) 100u
static uint8_t t_led[64];
static void fm1_led_key(uint32_t id, int on) { t_led[id & 63u] = (uint8_t)(on != 0); }
#else
#define BOARD_SMK37 1
#define FM1_NENC 8u
#define SMK_NLED 19u
#define SMK_NPAD 16u
enum { FM1_BTN_OCT_DOWN = 4, FM1_BTN_OCT_UP = 5 };
static struct { uint8_t pad_st[16], fader[4], pad_press[16]; uint32_t frames, buttons; uint64_t keys; uint8_t key_vel[64]; } fm1_in;
#define SP_KEYS() fm1_in.keys
#define SP_VEL(k) fm1_in.key_vel[k]
static uint8_t fm1_led[SMK_NLED];
static uint32_t padled_hold_until;
static uint8_t t_pad[16][3];
static void padled_set(uint32_t p, uint8_t r, uint8_t g, uint8_t b) { t_pad[p][0] = r, t_pad[p][1] = g, t_pad[p][2] = b; }
#endif
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

/* ---- the LCD: a framebuffer; the text boxes for the layout lint */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++) {
            uint16_t c = px[j * w + i];
            fb[(y + j) * 240u + x + i] = (uint16_t)(c >> 8 | c << 8);
        }
}
static int32_t t_boxes[400][4], t_nbox;
static char t_box_s[400][24];
static int32_t sv_y0_hook;
#define GFX_HOOK_TEXT(x0, y0, x1, y1, s, flags)                                                         \
    do {                                                                                                \
        if (t_nbox < 400) {                                                                             \
            t_boxes[t_nbox][0] = (x0), t_boxes[t_nbox][1] = (y0) + sv_y0_hook, t_boxes[t_nbox][2] = (x1), \
            t_boxes[t_nbox][3] = (y1) + sv_y0_hook;                                                     \
            str_cpy(t_box_s[t_nbox], (s), 24);                                                          \
            t_nbox++;                                                                                   \
        }                                                                                               \
    } while (0)
#define GFX_HOOK_BLIT(x, y, r0) ((void)0)
#define GFX_HOOK_BEGIN() ((void)0)
#define GFX_HOOK_PIXELS(n) ((void)0)
#define GFX_DOUBLE 1
#include "../firmware/src/gfx.c"
#include "../firmware/src/sp_dsp.c"
#if !T_FM1
#define SMO_TX_DONE() 1u                   /* (TRS MIDI OUT: a UART that takes a byte at once) */
#define SMO_TX_PUT(b) ((void)(b))
#include "../firmware/src/smk_midi_out.h"
#define SP_TRS 1
#endif
#include "../firmware/src/sp_state.c"
#if !T_FM1
#include "../firmware/src/sp_trs.c"
#endif
static uint32_t t_saves;
static sp_store_t t_flash;
#include "../firmware/src/sp_view.c"
#include "../firmware/src/sp_ui.c"
static void sp_store_save(void)
{
    sp_store_seal(&vst);
    t_flash = vst;
    t_saves++;
}
static uint32_t t_usaves;
static sp_user_rec_t t_uflash;
static void sp_user_save(void)
{
    sp_user_pack(&t_uflash);
    t_usaves++;
}
#if T_FM1
#include "../firmware/src/sp_panel_fm1.c"
#define BOARD "fm1"
#else
#include "../firmware/src/sp_panel_smk.c"
#define BOARD "smk"
#endif

static int fails;
/* a pixel's relative luminance (WCAG: sRGB linearised) */
static double lin(double c) { c /= 255.0; return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
static double lum(uint16_t c)
{
    double r = (c >> 11) << 3, g = ((c >> 5) & 63u) << 2, b = (c & 31u) << 3;
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                 \
            fails++;                                                            \
        }                                                                       \
    } while (0)
static void check(const char *what, int ok)
{
    printf("speak %s: %-84s %s\n", BOARD, what, ok ? "ok" : "FAIL");
    fails += !ok;
}

/* the sound: n ms */
static void run_ms(uint32_t ms)
{
    uint32_t n = ms * 1378u / 1000u, i;
    int32_t o[2u * SP_BLK];
    for (i = 0; i < n; i++)
        sp_render(o, SP_BLK);
}
/* the main loop for n ms: input, LEDs, the sound between */
static void loop_ms(uint32_t ms)
{
    uint32_t t;
    for (t = 0; t < ms; t += 15u) {
        sp_ui_input();
        sp_ui_leds();
        run_ms(15);
        fm1_ms += 15;
    }
}
static void press(uint32_t b)
{
    t_press |= 1u << b;
    fm1_in.buttons |= 1u << b;
    loop_ms(15);
    t_rel |= 1u << b;
    fm1_in.buttons &= ~(1u << b);
    loop_ms(15);
}
static void turn(uint32_t e, int32_t n)
{
    t_enc[e] = n;
    loop_ms(15);
}

/* a frame drawn whole; the text boxes collected; then the lint: inside the screen, inside a knob cell */
static uint32_t lint_bad;
static void draw_all(void)
{
    t_nbox = 0;
    sv_y0_hook = 0;
    su.redraw = 1;
    fm1_ms += 40;
    sp_draw_header();
    sv_y0_hook = SP_DISP_Y;
    ue.open ? ue_draw_display() : sp_draw_display();
    sv_y0_hook = SP_INFO_Y;
    ue.open ? ue_draw_info() : sp_draw_info();
    sv_y0_hook = SP_KNOB_Y;
    ue.open ? ue_draw_legend() : sp_draw_knobs();
    {
        int32_t i, cw = ue.open ? 80 : sp.mode == SP_MODE_MOPE ? 240 : 60;   /* (MOPE's legend: no cells) */
        for (i = 0; i < t_nbox; i++) {
            int32_t x0 = t_boxes[i][0], x1 = t_boxes[i][2], y0 = t_boxes[i][1];
            int bad = x0 < 0 || x1 > 240;
            if (y0 >= SP_KNOB_Y + 1 && y0 < SP_KNOB_Y + 60) {   /* a knob cell: 60 px each (the legend's: 80) */
                int32_t c = (x0 + 1) / cw;
                bad |= x0 < c * cw + 1 || x1 > c * cw + cw - 1;
            }
            if (bad) {
                printf("  lint: '%s' at x %d..%d y %d\n", t_box_s[i], x0, x1, y0);
                lint_bad++;
            }
        }
    }
}
static const char *dir = ".";
static void save(const char *name)
{
    char path[512];
    FILE *f;
    uint32_t i, ink = 0;
    snprintf(path, sizeof path, "%s/%s_%s.ppm", dir, BOARD, name);
    f = fopen(path, "wb");
    if (!f) {
        printf("FAIL cannot write %s\n", path);
        fails++;
        return;
    }
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = fb[i];
        uint8_t r = (uint8_t)((c >> 11) << 3), g = (uint8_t)(((c >> 5) & 63u) << 2), b = (uint8_t)((c & 31u) << 3);
        fputc(r, f);
        fputc(g, f);
        fputc(b, f);
        ink += r + g + b > 150u;
    }
    fclose(f);
    CHECK(ink > 1500);
}


/* a button held ms, then let go */
static void hold(uint32_t b, uint32_t ms)
{
    t_press |= 1u << b;
    fm1_in.buttons |= 1u << b;
    loop_ms(ms);
    t_rel |= 1u << b;
    fm1_in.buttons &= ~(1u << b);
    loop_ms(15);
}
/* MOPE's own button: SMK PATCH (a tap), FM-1 EDIT (held 0.6 s) */
static void mope_button(void)
{
#if T_FM1
    hold(SF_EDIT, 700);
#else
    press(SB_PATCH);
#endif
}
/* MOPE: the keys (SMK key k, the FM-1's key k - 5) */
static void mo_keys(uint64_t k)
{
#if T_FM1
    fm1_in.notes = (uint32_t)(k >> 5);
#else
    fm1_in.keys = k;
#endif
}
/* the main loop until MOPE says its word w of a line (or ms ran out); 1 if it did */
static int mo_wait_word(int32_t w, uint32_t ms)
{
    uint32_t t;
    for (t = 0; t < ms; t += 15u) {
        loop_ms(15);
        if (mo_cur_word() >= w)
            return 1;
    }
    return 0;
}
static void mo_wait_quiet(uint32_t ms)
{
    uint32_t t;
    for (t = 0; t < ms && (mo_busy() || mo.reply_on); t += 15u)
        loop_ms(15);
}
/* a whole frame drawn and a pixel of it read (the dark screen) */
static uint32_t ink_of(void)
{
    uint32_t i, ink = 0;
    for (i = 0; i < 240u * 240u; i++)
        ink += fb[i] != 0u;
    return ink;
}
/* MOPE's puppet: a new pose: a hop, a drop past it and back (settled within 1.5 s), the forearms lagging behind,
 * never a jump (each 16 ms step small); a strip of its frames (mope_transition: 100 x 136 px each, 48 ms apart) */
static void mope_puppet(void)
{
    static uint8_t strip[136][12 * 100][3];
    int32_t t0[MO_NJ], prev[MO_NJ], minb = 1 << 30, maxb = -(1 << 30), maxstep = 0, lag = 0, j, k;
    uint32_t i, ok, from = MO_P_SLUMP, to = MO_P_SIT;
    mpp.ok = 0;
    mo_pp_run(0, from, 0, 0, 0);                   /* (settled in SLUMP) */
    for (i = 1; i <= 200u; i++)
        mo_pp_run(i * 16u, from, 0, 0, 0);
    for (j = 0; j < MO_NJ; j++)
        t0[j] = mo_pp((uint32_t)j), prev[j] = mpp.a[j];
    for (i = 201; i <= 300u; i++) {                /* SIT: 1.6 s of it */
        mo_pp_run(i * 16u, to, 0, 0, 0);
        minb = mo_pp(MO_J_BODY) < minb ? mo_pp(MO_J_BODY) : minb;
        maxb = mo_pp(MO_J_BODY) > maxb ? mo_pp(MO_J_BODY) : maxb;
        for (j = 0; j < MO_NJ; j++) {
            int32_t dd = (mpp.a[j] - prev[j]) / 256;
            dd = dd < 0 ? -dd : dd;
            maxstep = dd > maxstep ? dd : maxstep;
            prev[j] = mpp.a[j];
        }
        k = mo_pp(MO_J_FL) - MO_PJ[to][MO_J_FL];
        lag = (k < 0 ? -k : k) > lag ? (k < 0 ? -k : k) : lag;
    }
    ok = minb < t0[MO_J_BODY] && maxb > MO_PJ[to][MO_J_BODY] && maxstep <= 12 && lag >= 4;
    for (j = 0; j < MO_NJ; j++)                    /* settled: every joint on its pose */
        ok &= mo_pp((uint32_t)j) >= MO_PJ[to][j] - 1 && mo_pp((uint32_t)j) <= MO_PJ[to][j] + 1;
    printf("  puppet SLUMP -> SIT: the body %d .. %d px (its pose %d), the largest step %d, the forearm off by %d\n",
           minb, maxb, MO_PJ[to][MO_J_BODY], maxstep, lag);
    check("MOPE's puppet: a hop, a drop past the pose and back, settled in 1.6 s, no jumps, the forearms lagging", ok);
    {   /* the strip: frames 48 ms apart through a change of pose (drawn as the screen draws them) */
        uint32_t f, x, y, base = fm1_ms;
        mo.pose = MO_P_SLUMP;
        mpp.ok = 0;
        fm1_ms = base;
        draw_all();
        mo.pose = MO_P_HEAD;
        for (f = 0; f < 12u; f++) {
            fm1_ms = base + 40u + f * 48u - 40u;   /* (draw_all adds 40) */
            draw_all();
            for (y = 0; y < 136u; y++)
                for (x = 0; x < 100u; x++) {
                    uint16_t c = fb[(24u + y) * 240u + x];
                    strip[y][f * 100u + x][0] = (uint8_t)((c >> 11) << 3);
                    strip[y][f * 100u + x][1] = (uint8_t)(((c >> 5) & 63u) << 2);
                    strip[y][f * 100u + x][2] = (uint8_t)((c & 31u) << 3);
                }
        }
        {
            char path[512];
            FILE *fp;
            snprintf(path, sizeof path, "%s/%s_mope_transition.ppm", dir, BOARD);
            fp = fopen(path, "wb");
            if (fp) {
                fprintf(fp, "P6\n%u 136\n255\n", 12u * 100u);
                fwrite(strip, 1, sizeof strip, fp);
                fclose(fp);
            }
        }
    }
}

/* MOPE: its button in, every control only talking to it, the screen always its own, the activities, the poses; its
 * button out, everything as it was */
static void mope_screens(void)
{
    int ok = 1;
    uint32_t v0, page0, mode0, i;
    int32_t tempo0;
    uint8_t oct0;
#if !T_FM1
    press(SB_SEQ);                                 /* (the RHYTHM page open: MOPE's screen must still be shown) */
#else
    turn(SF_E_SELECT, 6);
#endif
    loop_ms(100);
    page0 = su.page, mode0 = sp.mode, v0 = sp.snd.voice, tempo0 = sp.snd.tempo, oct0 = (uint8_t)sp.snd.oct;
    mope_button();
    ok &= sp.mode == SP_MODE_MOPE && sp.snd.voice == SP_ROBOT && sp.snd.speed == 80u && sp.snd.dmix == 0u;
    ok &= mo_screen() && mo_wait_word(0, 3000);   /* HELLO */
    draw_all();
    check("MOPE on (its own button), its clear voice, HELLO, its screen even over the RHYTHM page", ok);
    mo_wait_quiet(8000);
    loop_ms(4000);
    {   /* every control only talks to MOPE */
        int32_t k5 = sk_get(&sp.snd, SP_PAGE[su.page % SP_NPAGE][0]);
        uint32_t s0 = sp.snd.speed, pg = su.page;
#if T_FM1
        turn(SF_E_K1, 4);
        turn(SF_E_SELECT, 1);
        press(FM1_BTN_OCT_UP);
        press(SF_FX);
        ok = sk_get(&sp.snd, SP_PAGE[su.page % SP_NPAGE][0]) == k5 && su.page == pg && sp.snd.oct == (int8_t)oct0 &&
             sp.mode == SP_MODE_MOPE;
#else
        turn(4, 4);                               /* K5 */
        turn(1, 1);                               /* K2: no page */
        press(FM1_BTN_OCT_UP);
        press(SB_SCALE);
        press(SB_FX);
        fm1_in.fader[0] = 90;                     /* SPEED's fader */
        loop_ms(30);
        ok = sk_get(&sp.snd, SP_PAGE[su.page % SP_NPAGE][0]) == k5 && su.page == pg && sp.snd.oct == (int8_t)oct0 &&
             sp.mode == SP_MODE_MOPE && sp.snd.speed == s0 && !su.mempage;
#endif
        {
            uint32_t t, said = 0;
            for (t = 0; t < 9000u && !said; t += 15u) {
                loop_ms(15);
                said = mo_busy() && mo.line_cls >= MO_C_CTPL && mo.line_cls <= MO_C_CT_SCALE;
            }
            ok &= said;
        }
        (void)s0;
        check("MOPE: the knobs, the faders, OCT, SCALE, FX change nothing; a control's own comment comes", ok);
    }
    mo_wait_quiet(8000);
    loop_ms(4000);
    mo_keys(1ull << 12 | 1ull << 16 | 1ull << 19);   /* C4 E4 G4 */
    loop_ms(500);
    mo_keys(0);
    ok = mo_wait_word(2, 9000) && mo.have_h && mo.h.kind == MO_K_CHORD && mo.h.qual == MO_Q_MAJ && mo.h.root == 0u;
    draw_all();
    save("mope_talk");
    check("MOPE: C major played, heard, commented on after the phrase; the line on its screen", ok);
    {   /* a key-up lost on the way: the phrase still ends, MOPE still talks */
        mo_wait_quiet(9000);
        loop_ms(4000);
        mo_note(70, 1, 100, sp.now / 44u);       /* (its key-up never comes) */
        loop_ms(2500);
        ok = mo.nheld == 0u && mo.np == 0u && mo.have_h;
        check("MOPE: a key-up lost: the keys really held count, the phrase ends anyway", ok);
    }
    {   /* poses: a new one at every press, never the same twice in a row; screenshots of a few */
        uint32_t last = mo.pose, same = 0, seen = 0;
        static const char *const PN[MO_NPOSE] = {"slump", "half", "sit", "head", "lie", "hunch", "shrug", "kneel"};
        for (i = 0; i < 60u; i++) {
            mo_ui_ctl(MO_CT_K6);
            loop_ms(450);
            same += mo.pose == last;
            last = mo.pose;
            if (!(seen >> mo.pose & 1u) && (mo.pose == MO_P_HEAD || mo.pose == MO_P_LIE || mo.pose == MO_P_HALF)) {
                char n[24];
                snprintf(n, sizeof n, "mope_pose_%s", PN[mo.pose]);
                draw_all();
                save(n);
            }
            seen |= 1u << mo.pose;
        }
        check("MOPE: a new pose at every press, never the same twice in a row, all eight", !same && seen == 0xFFu);
    }
    mo_wait_quiet(9000);
    loop_ms(4000);
    {   /* DARK: the screen black for 5 s, then its line */
        uint32_t t;
        mo_start(MO_AC_DARK, sp.now / 44u);
        mo_say_line();
        for (t = 0; t < 8000u && !mo_dark(); t += 15u)
            loop_ms(15);
        draw_all();
        ok = mo_dark() && ink_of() == 0u;
        loop_ms(5200);
        ok &= !mo_dark() && mo.line_cls == MO_C_DARKEND;
        check("MOPE DARK: the light off (the whole screen black, 5 s), then not better either", ok);
    }
    mo_wait_quiet(9000);
#if !T_FM1
    {   /* LIGHTS: the pads sparkle and the LEDs blink, then its line */
        uint32_t t, lit = 0;
        mo_start(MO_AC_LIGHTS, sp.now / 44u);
        mo_say_line();
        for (t = 0; t < 8000u && !mo_lights(); t += 15u)
            loop_ms(15);
        for (t = 0; t < 1500u; t += 15u) {
            loop_ms(15);
            sp_ui_leds();
            for (i = 0; i < 16u; i++)
                if (t_pad[i][0] | t_pad[i][1] | t_pad[i][2])
                    lit |= 1u << i;
            for (i = 0; i < SMK_NLED; i++)
                lit |= fm1_led[i] ? 1u << 16 : 0u;
        }
        loop_ms(4000);
        check("MOPE LIGHTS: every pad lit some time, the LEDs blinking, then its line",
              (lit & 0xFFFFu) == 0xFFFFu && (lit >> 16) && mo.line_cls == MO_C_LIGHTSEND);
    }
    mo_wait_quiet(9000);
    {   /* GAME: a wrong pad hurts, the right one gives a grudging remark; its screen */
        uint32_t t, k;
        mo_start(MO_AC_GAME, sp.now / 44u);
        mo_say_line();
        for (t = 0; t < 8000u && !mo_gaming(); t += 15u)
            loop_ms(15);
        sp_ui_leds();
        ok = mo_gaming() && t_pad[mo.gpos][0] == 255u && t_pad[(mo.gpos + 1u) % 16u][0] == 0u;
        draw_all();
        save("mope_game");
        k = (mo.gpos + 3u) % 16u;
        fm1_in.pad_st[k] = 0x80;
        loop_ms(30);
        fm1_in.pad_st[k] = 0;
        loop_ms(60);
        ok &= mo.line_cls == MO_C_OW && mo.gmiss == 1u;
        loop_ms(600);
        k = mo.gpos;
        fm1_in.pad_st[k] = 0x80;
        loop_ms(30);
        fm1_in.pad_st[k] = 0;
        loop_ms(60);
        ok &= mo.line_cls == MO_C_GAMEHIT && mo.ghits == 1u;
        check("MOPE GAME: its light on a pad (the others dark), a wrong pad OW, the right one a hit", ok);
        for (t = 0; t < 40000u && mo.act == MO_AC_GAME; t += 15u)
            loop_ms(15);
        check("MOPE GAME: it gives up in the end", mo.act == MO_AC_NONE && mo.line_cls == MO_C_GAMEEND);
    }
    mo_wait_quiet(9000);
    {   /* ASK: the control asked for; another one: a remark; the one asked for: it did not help */
        uint32_t t, tries;
        for (tries = 0; tries < 20u; tries++) {   /* (asked for the mod wheel's neighbour K6, say: a knob) */
            mo_start(MO_AC_ASK, sp.now / 44u);
            if (mo.target == MO_CT_K6)
                break;
            mo.act = MO_AC_NONE;
        }
        mo.target = MO_CT_K6;                     /* (whatever it picked: K6 here) */
        mo_line(MO_C_ASKTURN, &(mo_heard_t){.ctl = MO_CT_K6 + 1u}, MO_L_PLAIN);
        mo_say_line();
        for (t = 0; t < 8000u && !mo.aphase; t += 15u)
            loop_ms(15);
        draw_all();
        save("mope_ask");
#if !T_FM1
        turn(6, 1);                               /* K7: the wrong one */
#endif
        for (t = 0; t < 4000u && mo.line_cls != MO_C_WRONG; t += 15u)
            loop_ms(15);
        ok = mo.line_cls == MO_C_WRONG;
        mo_wait_quiet(6000);
#if !T_FM1
        turn(5, 1);                               /* K6: the one */
#endif
        loop_ms(100);
        ok &= mo.line_cls == MO_C_ANSWER && mo.act == MO_AC_NONE;
        check("MOPE ASK: turn K6: K7 a remark (the wrong one), K6: it did not help", ok);
    }
#endif
    mo_wait_quiet(9000);
    loop_ms(9000);
    mo.gloom = 110;                               /* bored enough: its piece */
    {
        uint32_t t;
        for (t = 0; t < 25000u && mo.state != MO_SULK; t += 15u)
            loop_ms(15);
        loop_ms(2500);
        ok = mo.state == MO_SULK && mos.seq_on;
    }
    draw_all();
    save("mope_sulk");
    mo_keys(1ull << 12);                          /* a key: it stops, BACK */
    loop_ms(60);
    mo_keys(0);
    loop_ms(300);
    ok &= mo.state == MO_LISTEN && mo.line_cls == MO_C_BACK;
    check("MOPE: sulks (its piece playing), a key interrupts it (BACK)", ok);
#if !T_FM1
    loop_ms(6000);
    fm1_in.pad_st[0] = 0x80;                      /* the top row: praise */
    loop_ms(30);
    fm1_in.pad_st[0] = 0;
    loop_ms(60);
    ok = mo.line_cls == MO_C_PRAISE;
    sp_ui_leds();
    ok &= t_pad[0][1] == 255u && t_pad[9][2] == 255u * 20u / 100u && fm1_led[SB_PATCH];
    check("MOPE: the pads praise (green), poke (blue), ask for a joke (violet); PATCH lit", ok);
#endif
    mope_puppet();
    loop_ms(6000);
    mope_button();
    loop_ms(100);
    check("MOPE off: the keys' mode, the page, the voice, the tempo, the octave as before; its words stopped",
          sp.mode == mode0 && su.page == page0 && sp.snd.voice == v0 && sp.snd.tempo == tempo0 &&
          sp.snd.oct == (int8_t)oct0 && !mo.on);
#if !T_FM1
    press(SB_SEQ);
#else
    turn(SF_E_SELECT, -6);
#endif
}

int main(int argc, char **argv)
{
    uint32_t k, ok;
    if (argc > 1)
        dir = argv[1];
    palette_set(SP_PALETTE);                         /* (as sp_main.c) */
    sp_init();
    sp_store_defaults(&vst);
    sp.req_sent = 0;
    sp_apply();
    fm1_in.frames = 1000;
#if !T_FM1
    for (k = 0; k < 64u; k++)
        fm1_in.key_vel[k] = 100;
#endif
    loop_ms(100);
    /* ---- the knob block's tags */
    check("the knob block's tags: " BOARD,
#if T_FM1
          !strcmp(sp_panel_tag(0), "POT") && !strcmp(sp_panel_tag(3), "PRE") && !strcmp(sp_panel_tag(4), "1") &&
          !strcmp(sp_panel_tag(7), "4")
#else
          !strcmp(sp_panel_tag(0), "K1") && !strcmp(sp_panel_tag(3), "K4") && !strcmp(sp_panel_tag(4), "K5") &&
          !strcmp(sp_panel_tag(7), "K8")
#endif
    );
    /* ---- the knobs far past both ends, every page and SETUP */
    {
        uint32_t p, c;
        int32_t d;
        ok = 1;
        for (p = 0; p <= SP_NPAGE; p++) {
            if (p == SP_NPAGE)
                su.setup = 1;
            else
                su.setup = 0, su.page = (uint8_t)p;
            for (c = 0; c < 4u; c++)
                for (d = -1; d <= 1; d += 2) {
                    uint32_t i;
                    for (i = 0; i < 80u; i++)
#if T_FM1
                        turn(SF_E_K1 + c, d * 3);
#else
                        turn(4u + c, d * 3);
#endif
                }
        }
        su.setup = 0;
        {
            sp_sound_t s = sp.snd;
            sp_sound_fix(&s);
            ok &= !memcmp(&s, &sp.snd, sizeof s) && sp_glob.bend >= 1u && sp_glob.bend <= 12u && sp_glob.chord <= 100u;
        }
        check("every page's knobs and SETUP's, far past both ends: in range", ok);
        sp.snd = SP_SOUND_DEF;
        sp.groove_req = 0;                           /* (GROOVE turned: the drums stop) */
        loop_ms(30);
        sp_glob.bend = 2;
        sp_glob.chord = 40;
        sp_glob.lock = 0;
        sp.lock = 0;
        sp.chord_ms = 40;
        sp.bend_range = 2;
        su.page = 0;
    }
    /* ---- page, voice, bank / sentence (from CLASSIC) */
    sp_voice_load(&sp.snd, SP_CLASSIC);
    {
#if T_FM1
        turn(SF_E_SELECT, 1);
        ok = su.page == 1u;
        turn(SF_E_ALGO, 1);
        ok &= sp.snd.voice == 1u;
        for (k = 0; k < 20u; k++)
            turn(SF_E_PRESET, 1);
        ok &= vst.sent == 20u && sp.sent == 20u && sp.wpos == 0u;
        turn(SF_E_PRESET, -1);
        ok &= vst.sent == 19u;
        check("SELECT the page, ALGORITHM the voice, PRESETS the sentence across the banks (B2 P5)", ok);
#else
        turn(1, 1);
        ok = su.page == 1u;
        turn(2, 1);
        ok &= sp.snd.voice == 1u;
        turn(3, 1);
        ok &= vst.sent == 16u && sp.sent == 16u;
        turn(3, 1);
        turn(3, -1);
        ok &= vst.sent == 16u;
        check("K2 the page, K3 the voice, K4 the bank (the pad kept)", ok);
        /* the pads: one pad a press, its sentence from its first word */
        fm1_in.keys = 1;
        loop_ms(30);
        fm1_in.keys = 0;
        loop_ms(30);
        ok = sp.wpos == 1u;
        fm1_in.pad_st[5] = 0x80;
        loop_ms(200);
        ok &= sp.sent == 21u && sp.wpos == 0u && sp.wlast == -1;
        fm1_in.keys = 2;                            /* a key while the pad is held: the first word */
        loop_ms(30);
        ok &= sp.wlast == 0;
        fm1_in.keys = 0;
        fm1_in.pad_st[5] = 0;
        loop_ms(30);
        ok &= sp.sent == 21u && sp.wpos == 1u;      /* (held: no second press) */
        check("a pad: its sentence, from its first word; held: one press", ok);
        loop_ms(1500);                              /* (the word said) */
        sp_ui_leds();
        printf("  pads: in use %u, another %u (bank colour %u)\n", t_pad[5][0], t_pad[4][0], SP_BANK_RGB[1][0]);
        ok = t_pad[5][0] == 255u * 45u / 100u * 0 + (uint8_t)(SP_BANK_RGB[1][0] * 45u / 100u) &&
             t_pad[4][0] == (uint8_t)(SP_BANK_RGB[1][0] * 10u / 100u);
        fm1_in.keys = 4;
        loop_ms(15);
        sp_ui_leds();
        ok &= t_pad[5][0] == SP_BANK_RGB[1][0];
        fm1_in.keys = 0;
        loop_ms(1500);
        check("the pads' light: a sentence 10 %, the one in use 45 %, speaking 100 %", ok);
#endif
    }
    /* ---- memories */
    {
        uint32_t s0 = t_saves;
#if T_FM1
        sp.snd.speed = 99;
        fm1_in.buttons |= 1u << SF_SAVE;
        t_press |= 1u << SF_SAVE;
        loop_ms(1200);
        fm1_in.buttons &= ~(1u << SF_SAVE);
        t_rel |= 1u << SF_SAVE;
        loop_ms(30);
        ok = t_saves == s0 + 1u && (vst.used & 1u) && vst.mem[0].speed == 99u;
        sp.snd.speed = 64;
        fm1_in.buttons |= 1u << SF_SAVE;            /* SAVE + PRESETS: memory 2 (empty), back to 1, let go: loaded */
        t_press |= 1u << SF_SAVE;
        loop_ms(30);
        turn(SF_E_PRESET, 1);
        ok &= su.msel == 1u;
        turn(SF_E_PRESET, -1);
        loop_ms(1300);                              /* (held long after turning: no save) */
        fm1_in.buttons &= ~(1u << SF_SAVE);
        t_rel |= 1u << SF_SAVE;
        loop_ms(30);
        ok &= t_saves == s0 + 1u && sp.snd.speed == 99u && vst.loaded == 0u;
        check("SAVE held 1 s saves into the memory shown; SAVE + PRESETS picks one, loaded when let go", ok);
#else
        hold(SB_PATCH, 700);                       /* (PATCH held: the memories; a tap: MOPE) */
        ok = su.mempage && sp.mode != SP_MODE_MOPE;
        sp.snd.speed = 99;
        fm1_in.pad_st[2] = 0x80;
        loop_ms(1200);
        fm1_in.pad_st[2] = 0;
        loop_ms(30);
        ok &= t_saves == s0 + 1u && (vst.used >> 2 & 1u) && vst.mem[2].speed == 99u;
        sp.snd.speed = 64;
        fm1_in.pad_st[2] = 0x80;
        loop_ms(100);
        fm1_in.pad_st[2] = 0;
        loop_ms(30);
        ok &= sp.snd.speed == 99u && vst.loaded == 2u;
        fm1_in.pad_st[7] = 0x80;                    /* an empty one */
        loop_ms(100);
        fm1_in.pad_st[7] = 0;
        loop_ms(30);
        ok &= !strcmp(su.pop_v, "EMPTY 8");
        loop_ms(700);                               /* (the save's red flash over) */
        sp_ui_leds();
        printf("  memories: loaded %u, empty %u %u\n", t_pad[2][0], t_pad[7][0], t_pad[3][0]);
        ok &= t_pad[2][0] == 200u && t_pad[7][0] == 200u * 6u / 100u && t_pad[3][0] == 200u * 6u / 100u;
        check("PATCH: a pad held 1 s saves, a tap loads (an empty one says so); empty 6 %, loaded 100 %", ok);
        draw_all();
        save("patch");
        hold(SB_PATCH, 700);
#endif
    }
    /* ---- buttons */
    {
#if T_FM1
        press(SF_EDIT);
        ok = sp.mode == SP_MODE_LETTERS && t_led[SF_EDIT];
        fm1_in.notes = 1u;                          /* F3: SMK key 5 = F */
        loop_ms(30);
        ok &= sp.wlast_id == 5u;
        fm1_in.notes = 0;
        loop_ms(400);
        draw_all();
        save("letters");
        press(SF_EDIT);
        ok &= sp.mode == SP_MODE_KIT;
        fm1_in.notes = 1u << 3;                     /* (FM-1 key 3 = SMK key 8: the kit's hit 8) */
        loop_ms(30);
        ok &= sp.wlast_id == SP_KIT[8];
        fm1_in.notes = 0;
        loop_ms(100);
        draw_all();
        save("kit");
        press(SF_EDIT);
        ok &= sp.mode == SP_MODE_SENT;             /* (MOPE: its own button, mope_screens) */
        press(SF_PLAY);
        ok &= sp.auto_on && t_led[SF_PLAY];
        press(SF_PLAY);
        ok &= !sp.auto_on;
        press(SF_REC);
        press(SF_SEQ);
        press(SF_SEQ);
        ok &= sp.wpos == 2u;
        press(SF_ARP);
        ok &= sp.wpos == 1u;
        press(SF_REC);
        ok &= sp.wpos == 0u;
        press(SF_GLO);
        ok &= su.setup && t_led[SF_GLO];
        draw_all();
        save("setup");
        press(SF_GLO);
        press(SF_FX);
        ok &= su.page == SP_PG_DELAY;
        press(SF_FX);
        ok &= su.page == SP_PG_SPACE;
        press(SF_ENV);
        ok &= su.page == SP_PG_ENV;
        press(SF_LFO);
        ok &= su.page == SP_PG_MOD;
        press(SF_HOME);
        ok &= su.page == SP_PG_PLAY;
        press(FM1_BTN_OCT_UP);
        ok &= sp.snd.oct == 1 && t_led[FM1_BTN_OCT_UP];
        press(FM1_BTN_OCT_DOWN);
        check("EDIT LETTERS, PLAY PLAY, ARP / SEQ the cursor, REC the start, GLO SETUP, FX ENV LFO HOME pages, OCT", ok);
#else
        press(SB_SCALE);
        ok = sp.mode == SP_MODE_LETTERS && fm1_led[SB_SCALE];
        fm1_in.keys = 1ull << 7;                   /* key 7: H */
        loop_ms(30);
        ok &= sp.wlast_id == 7u;
        fm1_in.keys = 0;
        loop_ms(400);
        draw_all();
        save("letters");
        press(SB_SCALE);
        ok &= sp.mode == SP_MODE_KIT && fm1_led[SB_SCALE];
        fm1_in.pad_press[3] = 120;                  /* KIT: a pad a hit, its pressure the velocity */
        fm1_in.pad_st[3] = 0x80;
        loop_ms(30);
        sp_ui_leds();
        ok &= sp.wlast_id == SP_KIT[3] && t_pad[3][0] == 255u && t_pad[2][0] == 255u * 22u / 100u;
        fm1_in.pad_st[3] = 0;
        loop_ms(200);
        sp_ui_leds();
        ok &= t_pad[3][0] == 255u * 22u / 100u;
        fm1_in.keys = 1ull << 9;                    /* a key: the kit's hit 9 */
        loop_ms(30);
        ok &= sp.wlast_id == SP_KIT[9];
        fm1_in.keys = 0;
        loop_ms(60);
        draw_all();
        save("kit");
        press(SB_SCALE);
        ok &= sp.mode == SP_MODE_SENT;             /* (MOPE: its own button, mope_screens) */
        press(SB_SEQPLAY);
        ok &= sp.auto_on && fm1_led[SB_SEQPLAY];
        loop_ms(900);
        draw_all();
        save("play");                               /* PLAY running: its tempo, the beat */
        press(SB_SEQPLAY);
        ok &= !sp.auto_on;
        press(SB_SEQREC);
        press(SB_RIGHT);
        press(SB_RIGHT);
        ok &= sp.wpos == 2u;
        press(SB_LEFT);
        ok &= sp.wpos == 1u;
        press(SB_SEQREC);
        ok &= sp.wpos == 0u;
        press(SB_REPEAT);
        ok &= sp.lock && fm1_led[SB_REPEAT];
        press(SB_REPEAT);
        press(SB_CHORD);
        ok &= sp.chord_ms == 0u && !fm1_led[SB_CHORD];
        press(SB_CHORD);
        ok &= sp.chord_ms == 40u && fm1_led[SB_CHORD];
        press(SB_ARP);
        ok &= sp.snd.hold == SP_HOLD_LOOP;
        press(SB_ARP);
        press(SB_ARP);
        press(SB_ARP);
        ok &= sp.snd.hold == SP_HOLD_HOLD;
        press(SB_GLOBE);
        ok &= su.setup && fm1_led[SB_GLOBE];
        draw_all();
        save("setup");
        press(SB_GLOBE);
        press(SB_FX);
        ok &= su.page == SP_PG_DELAY;
        press(SB_FX);
        ok &= su.page == SP_PG_SPACE;
        press(SB_KNOB_BANK);
        press(SB_FADER_BANK);
        press(SB_FADER_BANK);
        ok &= su.page == SP_PG_DELAY;                /* (SPACE, RHYTHM, back two) */
        press(FM1_BTN_OCT_UP);
        ok &= sp.snd.oct == 1 && fm1_led[FM1_BTN_OCT_UP];
        press(FM1_BTN_OCT_DOWN);
        check("SCALE LETTERS, SEQ PLAY PLAY, < > the cursor, SEQ REC, REPEAT, CHORD, ARP HOLD, GLOBE, FX, pages, OCT", ok);
        /* the faders take over once moved */
        fm1_in.fader[1] = 127;
        loop_ms(30);
        ok = sp.snd.formant == 12 && su.t_fader;
        fm1_in.fader[1] = 64;
        loop_ms(30);
        ok &= sp.snd.formant == 0;
        draw_all();
        save("faders");
        check("the faders: FORMANT -12..+12 once moved, their strip on the screen", ok);
        loop_ms(1600);
#endif
    }
    /* ---- the record */
    {
        sp_store_t a, b;
        sp.snd.voice = SP_LABAI;
        sp.snd.formant = -5;
        vst.cur = sp.snd;
        vst.sent = 37;
        sp_store_save();
        a = t_flash;
        b = a;
        ok = sp_store_take(&b, sizeof b) && !memcmp(&a, &b, sizeof a);
        b = a;
        ((uint8_t *)&b)[40] ^= 1u;
        ok &= !sp_store_take(&b, sizeof b);
        ok &= !sp_store_take(&a, (int32_t)sizeof a - 1);
        b = a;
        b.cur.root = 200;
        b.cur.formant = 99;
        b.sent = 999;
        sp_store_seal(&b);
        ok &= sp_store_take(&b, sizeof b) && b.cur.root == 84u && b.cur.formant == 12 && b.sent < SP_NSENT;
        b = a;                                       /* a record of version 1 (DAD JOKES 1 was bank 1): moved on */
        b.ver = 1;
        b.sent = 5;
        b.groove = 3;
        b.drums = 0;
        b.cur.tempo = 0;
        b.sum = sp_fnv(&b, (uint32_t)((const uint8_t *)&b.sum - (const uint8_t *)&b));
        ok &= sp_store_take(&b, sizeof b) && b.sent == 5u * 16u + 5u && b.ver == SP_VER && b.cur.voice == SP_LABAI &&
              b.cur.tempo == 100u && b.drums == 100u && b.groove == 0u;
        b = a;                                       /* version 2: PERCUSSION was bank 3, ROBOT 6, NUMBERS 8 */
        b.ver = 2;
        b.sent = 2u * 16u + 3u;
        b.sum = sp_fnv(&b, (uint32_t)((const uint8_t *)&b.sum - (const uint8_t *)&b));
        ok &= sp_store_take(&b, sizeof b) && b.sent == 4u * 16u + 3u;
        b = a;
        b.ver = 2;
        b.sent = 7u * 16u + 1u;
        b.sum = sp_fnv(&b, (uint32_t)((const uint8_t *)&b.sum - (const uint8_t *)&b));
        ok &= sp_store_take(&b, sizeof b) && b.sent == 11u * 16u + 1u && !strcmp(SP_BANK_NAME[11], "NUMBERS");
        check("the record: read back the same; damaged or short refused; values in range; version 1's, 2's pad moved on",
              ok);
    }
    /* ---- the screens */
    {
        struct timespec t0, t1;
        uint32_t p;
        double us;
        sp.snd = SP_SOUND_DEF;
        sp_apply();
        sp.req_sent = 0;
        su.page = 0;
        su.setup = 0;
        loop_ms(60);
        for (k = 0; k < 4u; k++) {                  /* the joke: four words spoken */
#if T_FM1
            fm1_in.notes = 1u << (k * 2u);
#else
            fm1_in.keys = 1ull << (12u + k * 2u);
#endif
            loop_ms(260);
#if T_FM1
            fm1_in.notes = 0;
#else
            fm1_in.keys = 0;
#endif
            loop_ms(60);
        }
#if T_FM1
        fm1_in.notes = 1u << 9;
#else
        fm1_in.keys = 1ull << 19;                   /* the fifth word held: speaking */
#endif
        loop_ms(150);
        draw_all();
        save("main");
        check("the home screen: the sentence, its fifth word bright, the mouth, the knob block", sp.wlast == 4 &&
              lint_bad == 0u);
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (k = 0; k < 50u; k++)
            draw_all();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / 50.0;
        printf("  a whole frame: %.0f us on this host\n", us);
#if T_FM1
        fm1_in.notes = 0;
#else
        fm1_in.keys = 0;
#endif
        loop_ms(1000);
        for (p = 0; p < SP_NPAGE; p++) {
            char n[32];
            su.page = (uint8_t)p;
            su.t_touch = fm1_ms | 1u;
            su.touch = 4u + p % 4u;
            draw_all();
            snprintf(n, sizeof n, "page_%s", SP_PAGE_NAME[p]);
            save(n);
        }
        su.page = 0;
        sp_voice_set(SP_LABAI);
        draw_all();
        save("popup");
        fm1_ms += 2000;
        sp_boot_frame(500);
        save("boot");
        check("every page, SETUP, PATCH, LETTERS, a popup, the faders: every text inside the screen and its cell",
              lint_bad == 0u);
    }
    {   /* the display: the word spoken always on the first line (the alphabet: 26 words on 3 lines) */
        sv_word_t w[40];
        uint32_t n = sv_layout(SP_SENT[10u * 16u + 12u], w, 40), i, top_ok = 1;
        for (i = 0; i < n; i++)
            top_ok &= w[i].line <= 2u;
        sp.sent = 10u * 16u + 12u;
        sp.wlast = 25;
        draw_all();
        save("alphabet_z");
        check("the display: the alphabet in 3 lines, Z spoken: its line on top (the screen scrolls)", n == 26u &&
              top_ok && w[25].line == 2u && !strcmp(SP_BANK_NAME[10], "SPELLING"));
    }
    {   /* every bank's header: its name's colour against the background, at least 4.5 : 1 */
        uint32_t b, worst_b = 0;
        double worst = 99.0;
        for (b = 0; b < SP_NBANKS; b++) {
            double lt = 0, lb, c;
            int32_t x, y, w = text_w(&AF_T, sp_bank_name(b));
            sp.sent = (uint16_t)(b * 16u);
            draw_all();
            lb = lum(fb[2u * 240u + 236u - 4u * 0u - 200u]);
            for (y = 1; y < 22; y++)
                for (x = 4; x < 4 + w && x < 150; x++) {
                    double l = lum(fb[(uint32_t)y * 240u + (uint32_t)x]);
                    lt = l > lt ? l : lt;
                }
            c = (lt + 0.05) / (lb + 0.05);
            if (c < worst)
                worst = c, worst_b = b;
            if (b == 8u)
                save("header_robot");
        }
        printf("  headers: the lowest contrast %.1f : 1 (%s)\n", worst, sp_bank_name(worst_b));
        check("every bank's header: its name readable (contrast at least 4.5 : 1, ROBOT's cyan too)", worst >= 4.5);
    }
    /* ---- USER: the editor */
    {
        uint32_t u0 = t_usaves, i, hello = 0xFFFFu;
        sp.mode = SP_MODE_SENT;
        su.page = 0, su.setup = 0, su.mempage = 0;
        sp.auto_on = 0;
        vst.sent = sp.sent = (uint16_t)SP_USENT;
        sp.req_sent = (uint16_t)SP_USENT;
        loop_ms(30);
        ok = sp_bank_of(sp.sent) == SP_BANK_USER && !strcmp(sp_sent_text(sp.sent), UE_HINT);
        draw_all();
        save("user_empty");
#if T_FM1
        fm1_in.buttons |= 1u << SF_REC;              /* REC held: the editor */
        t_press |= 1u << SF_REC;
        loop_ms(300);
        ok &= !ue.open;
        loop_ms(400);
        ok &= ue.open && sp.edit;
        fm1_in.buttons &= ~(1u << SF_REC);
        t_rel |= 1u << SF_REC;
        loop_ms(30);
#define UKEY(k) (fm1_in.notes = 1u << (k), loop_ms(30), fm1_in.notes = 0, loop_ms(30))
#define UACT(a) press(BTN_OF[a])
        static const uint8_t BTN_OF[UE_N] = {SF_REC, SF_HOME, SF_ENV, SF_LFO, SF_FX, SF_PLAY, SF_EDIT, SF_GLO, SF_SAVE};
#else
        fm1_in.pad_st[0] = 0x80;
        loop_ms(300);
        ok &= !ue.open;
        loop_ms(400);
        ok &= ue.open && sp.edit;
        fm1_in.pad_st[0] = 0;
        loop_ms(30);
#define UKEY(k) (fm1_in.keys = 1ull << (k), loop_ms(30), fm1_in.keys = 0, loop_ms(30))
        static uint8_t PAD_OF[UE_N];
        for (i = 0; i < 16u; i++)
            if (UE_PAD[i] != UE_NONE)
                PAD_OF[UE_PAD[i]] = (uint8_t)i;
#define UACT(a) (fm1_in.pad_st[PAD_OF[a]] = 0x80, loop_ms(30), fm1_in.pad_st[PAD_OF[a]] = 0, loop_ms(30))
#endif
        check("USER: an empty pad shows " UE_HINT "; held 0.6 s it opens the editor", ok);
        UKEY('H' - 'A'); UACT(UE_ADD);
        UKEY('E' - 'A'); UACT(UE_ADD);
        UKEY('L' - 'A'); UACT(UE_ADD);
        ok = ue.nt == 3u && !strcmp(ue.typed, "HEL") && ue.nm > 0u;
        for (i = 0; i < ue.nm; i++) {
            uint32_t n;
            const uint16_t *d = ue_dict(&n);
            ok &= !strncmp(SP_WNAME[d[ue.m0 + i]], "HEL", 3);
            if (!strcmp(SP_WNAME[d[ue.m0 + i]], "HELLO"))
                hello = i;
        }
        ok &= hello != 0xFFFFu;
        draw_all();
        save("editor_typing");
        UKEY('L' - 'A');                               /* (a key: the list jumps to HEL + L, HELLO) */
        ok &= ue.sel == hello;
        draw_all();
        save("editor_list");
        UACT(UE_PICK);
        ok &= ue.n == 1u && !strcmp(SP_WNAME[ue.w[0]], "HELLO") && ue.nt == 0u;
        check("the editor: keys pick letters, ADD types them, the list the words they begin, PICK one", ok);
        {
            uint32_t n, e1 = 0xFFFFu, w1 = 0xFFFFu, s0;
            const uint16_t *d = ue_dict(&n);
            for (i = 0; i < n; i++) {
                if (e1 == 0xFFFFu && !strncmp(SP_WNAME[d[i]], "EL", 2))
                    e1 = i;
                if (w1 == 0xFFFFu && SP_WNAME[d[i]][0] == 'W')
                    w1 = i;
            }
            loop_ms(1600);                             /* (the popup gone: the list on the glass) */
            UKEY('E' - 'A');
            UKEY('L' - 'A');                           /* (quickly after it: one prefix, EL) */
            ok = e1 != 0xFFFFu && ue.m0 == 0u && ue.sel == e1 && ue.njp == 2u && !strcmp(ue.jp, "EL");
            draw_all();
            save("editor_jump");
            s0 = ue.sel;
#if T_FM1
            press(SF_SEQ);
#else
            press(SB_RIGHT);
#endif
            ok &= ue.sel == s0 + 1u;
            loop_ms(UE_JUMP_MS + 100u);                /* a pause: a new prefix */
            UKEY('W' - 'A');
            ok &= w1 != 0xFFFFu && ue.sel == w1 && ue.njp == 1u;
            loop_ms(UE_JUMP_MS + 100u);
            check("the editor: a key jumps the list to its letter's first word, two quick ones to their prefix (EL), a pause starts anew", ok);
        }
        UKEY('Z' - 'A'); UACT(UE_ADD);
        UKEY('Q' - 'A'); UACT(UE_ADD);
        UKEY('X' - 'A'); UACT(UE_ADD);
        ok = ue.nm == 0u;
        UACT(UE_NEXT);
        ok &= ue.n == 4u && ue.w[1] == 25u && ue.w[2] == 16u && ue.w[3] == 23u;
        UACT(UE_DEL_W);
        ok &= ue.n == 3u;
        check("the editor: a word not in the vocabulary spelled (Z Q X), DEL WORD the last away", ok);
        UACT(UE_LANG);
        UKEY('H' - 'A'); UACT(UE_ADD);
        UKEY('A' - 'A'); UACT(UE_ADD);
        UKEY('U' - 'A'); UACT(UE_ADD);
        UKEY('S' - 'A'); UACT(UE_ADD);
        UACT(UE_NEXT);
        ok = ue.lang == 1u && ue.n == 4u && !strcmp(SP_WNAME[ue.w[3]], "HAUS") && (SP_WORD[ue.w[3]].flags & SP_W_DE);
        loop_ms(3000);
        UACT(UE_PREVIEW);
        ok &= (uint8_t)(sp.say_w - sp.say_r) >= 3u;
        draw_all();
        save("editor_sentence");
#if !T_FM1
        sp_ui_leds();
        ok &= t_pad[PAD_OF[UE_CANCEL]][0] > 80u && t_pad[PAD_OF[UE_CANCEL]][1] == 0u &&
              t_pad[PAD_OF[UE_OK]][1] > 80u && t_pad[PAD_OF[UE_OK]][0] == 0u;
#endif
        check("the editor: EN / DE (HAUS a German word), PREVIEW says the sentence; CANCEL red, OK green", ok);
        loop_ms(4000);
        UACT(UE_OK);
        ok = !ue.open && !sp.edit && t_usaves == u0 + 1u && sp.user_n[0] == 4u &&
             !strcmp(sp_sent_text(SP_USENT), "HELLO Z Q HAUS") && sp.sayq[(uint8_t)(sp.say_w - 1u) % 64u] == SP_WID_SAVED;
        loop_ms(1500);
        draw_all();
        save("user_saved");
        check("the editor: OK keeps the sentence (saved, SAVED said), the pad shows it", ok);
        check("the editor's screens (typing, the list, the sentence, its legend): every text inside its column",
              lint_bad == 0u);
        /* the sentence played: the keys speak its words */
        UKEY(12);
        ok = sp.wlast_id == ue.w[0];
        /* CANCEL keeps nothing */
#if T_FM1
        turn(SF_E_PRESET, 1);
        fm1_in.buttons |= 1u << SF_REC;
        t_press |= 1u << SF_REC;
        loop_ms(700);
        fm1_in.buttons &= ~(1u << SF_REC);
        t_rel |= 1u << SF_REC;
        loop_ms(30);
#else
        fm1_in.pad_st[1] = 0x80;
        loop_ms(700);
        fm1_in.pad_st[1] = 0;
        loop_ms(30);
#endif
        ok &= ue.open && ue.slot == 1u;
        UKEY('A' - 'A'); UACT(UE_ADD); UACT(UE_NEXT);
        UACT(UE_CANCEL);
        ok &= !ue.open && sp.user_n[1] == 0u && t_usaves == u0 + 1u;
        check("USER: a key speaks the sentence's words; CANCEL keeps nothing", ok);
        {   /* the record: the same back; under another vocabulary found again by the text; damaged: refused */
            sp_user_rec_t r, r2;
            uint16_t w0 = sp.user_w[0][0];
            sp_user_pack(&r);
            r2 = r;
            ok = sp_user_take(&r2, sizeof r2) && sp.user_n[0] == 4u && sp.user_w[0][0] == w0;
            r2 = r;
            r2.vocab ^= 1u;
            r2.w[0][0].id = (uint16_t)((w0 + 7u) % SP_NWORD);   /* (another build: the word elsewhere) */
            r2.sum = sp_fnv(&r2, (uint32_t)((const uint8_t *)&r2.sum - (const uint8_t *)&r2));
            ok &= sp_user_take(&r2, sizeof r2) && sp.user_n[0] == 4u && sp.user_w[0][0] == w0;
            r2 = r;
            r2.w[0][1].h ^= 0x5555u;                 /* (a word that is no longer there: left out) */
            r2.vocab ^= 1u;
            r2.sum = sp_fnv(&r2, (uint32_t)((const uint8_t *)&r2.sum - (const uint8_t *)&r2));
            ok &= sp_user_take(&r2, sizeof r2) && sp.user_n[0] == 3u;
            r2 = r;
            ((uint8_t *)&r2)[100] ^= 1u;
            ok &= !sp_user_take(&r2, sizeof r2) && sp.user_n[0] == 0u;
            ok &= sp_user_take(&r, sizeof r) && sp.user_n[0] == 4u;
            check("the USER record: read back; another vocabulary: found by the text (a lost word left out); damaged: "
                  "refused", ok);
        }
    }
    /* ---- RHYTHM */
    {
#if T_FM1
        su.page = 0;
        for (k = 0; k < SP_PG_RHYTHM; k++)
            press(SF_SEL);
        ok = su.page == SP_PG_RHYTHM;
        turn(SF_E_K1 + 3u, 1);                       /* KNOB 4: GROOVE BOSSA NOVA */
        loop_ms(500);
        ok &= sp.groove == 1u && sp.snd.tempo == RH_GROOVE[0].tempo;
        draw_all();
        save("rhythm");
        turn(SF_E_K1 + 3u, -1);                      /* OFF */
        loop_ms(100);
        ok &= sp.groove == 0u;
        check("RHYTHM: the page (SELECT), KNOB 4 a groove (its tempo), OFF stops it", ok);
#else
        press(SB_SEQ);
        ok = su.page == SP_PG_RHYTHM;
        fm1_in.pad_st[3] = 0x80;                     /* DUB HOUSE */
        loop_ms(30);
        fm1_in.pad_st[3] = 0;
        loop_ms(700);
        ok &= sp.groove == 4u && fm1_led[SB_SEQ] && sp.snd.tempo == RH_GROOVE[3].tempo;
        {
            uint32_t hi = 0, lo = 255;
            for (k = 0; k < 60u; k++) {              /* its pad pulses on the beat */
                loop_ms(15);
                sp_ui_leds();
                hi = t_pad[3][1] > hi ? t_pad[3][1] : hi;
                lo = t_pad[3][1] < lo ? t_pad[3][1] : lo;
            }
            ok &= hi == RH_RGB[3][1] && lo < hi && t_pad[5][1] == (uint8_t)(RH_RGB[5][1] * 10u / 100u);
        }
        draw_all();
        save("rhythm");
        press(SB_PARA);
        ok &= sp.fill_req || sp.fill_now;
        fm1_in.pad_st[3] = 0x80;                     /* the same pad again: stop */
        loop_ms(30);
        fm1_in.pad_st[3] = 0;
        loop_ms(30);
        ok &= sp.groove == 0u;
        press(SB_SEQ);
        ok &= su.page != SP_PG_RHYTHM;
        check("RHYTHM: SEQ the page, a pad a groove (its tempo, its pad pulsing), PARA FILL, again: stop", ok);
#endif
    }
    mope_screens();
    printf("%s\n", fails ? "SPEAK SCREEN TEST FAILED" : "speak screen test passed");
    return fails != 0;
}
