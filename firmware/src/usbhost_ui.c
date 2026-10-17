/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (makkuro) */
/* USB host mode, the instrument's side (the glue to usbhost.c, the self-contained core): the setup screen and its
 * session loop, the stream / vocoder screen, the VOCODER page, the header's MIC / VOX level, the < + > combo, USB MIC
 * AUTO / OFF / ON, the way back to device mode (usb_start). Uses the UI (lcd, canvas, fonts, colours, ui_message),
 * the panel (buttons, knobs), the input HAL and usb.c (usb.up / config / suspended / resets / detached).
 * The firmware's own parts come through these macros (the includer defines them; the defaults: none of it):
 *   VOX_CPU_PCT()       the audio ISR's load in %                                         default 0
 *   VOX_HAS_CDC         1: the CDC console (co_r, co_w, cdc) is there                       default 0
 *   VOX_MODE()          USB MIC: UHM_AUTO, UHM_OFF or UHM_ON (the includer's setting; it      default UHM_OFF: host
 *                       defines the UHM_* enum too)                                         mode on request only
 *   VOX_NTRK            tracks that can be the vocoder's carrier (VOX TRACK; the firmware     default 0: the keys play
 *                       calls vc_track on that track's sound); VOX_TRACK_NAME(t) its sound    the module's own synth
 *   VOX_STORE(k)        a byte of the settings: k 0..7 the controls K1..K8 + 1 (0: the        default: not kept
 *                       default), 8 VOX TRACK (255: OFF); VOX_STORE_SAVE() writes them */
#ifndef VOX_CPU_PCT
#define VOX_CPU_PCT() 0u
#endif
#ifndef VOX_HAS_CDC
#define VOX_HAS_CDC 0
#endif
#ifndef VOX_MODE
enum { UHM_AUTO, UHM_OFF, UHM_ON };
#define VOX_MODE() UHM_OFF
#endif
#ifndef VOX_NTRK
#define VOX_NTRK 0u
#define VOX_TRACK_NAME(t) ""
#endif
static const char *const UHM_NAME[3] = {"AUTO", "OFF", "ON"};
#ifndef VOX_VC
#define VOX_VC 1                           /* 1: vocoder.c's VOCODER page and stream screen; 0: the includer has its own
                                            * vocoder and screens (the session's setup screen and USB MIC AUTO stay) */
#endif

/* ------------------------------------------------------- the screen --- */
/* a level bar: -60..0 dBFS, the RMS filled, the peak a line */
static void uh_bar(int32_t y, uint32_t pk, uint32_t rms)
{
    const int32_t x0 = 40, w = 192, h = 14;
    int32_t dp = uhs_db(pk) + 60, dr = uhs_db(rms) + 60;
    dp = dp < 0 ? 0 : dp;
    dr = dr < 0 ? 0 : dr;
    cv_rect(x0, y, w, h, T_LINE);
    cv_rect(x0, y, dr * w / 60, h, T_THEME);
    if (dp)
        cv_rect(x0 + dp * w / 60 - 2, y, 2, h, pk >= 32000u ? T_REC : T_ACCENT);
}


/* the CPU in %: [0] the audio ISR's load (VOX_CPU_PCT), [1] the vocoder (uhs_mix), [2] the iso service (TIMER5),
 * [3] the rest of the audio ISR (the engines, the effects); measured over the last second or more */
static uint32_t uh_cpu[4];
static void uh_cpu_take(void)
{
    static uint32_t t;
    uint32_t dt = fm1_ms - t, m, v;
    if (dt < 1000u)
        return;
    t = fm1_ms;
    m = uhs.cyc_mix;
    v = uhs.cyc_svc;
    uhs.cyc_mix -= m;
    uhs.cyc_svc -= v;
    uh_cpu[0] = VOX_CPU_PCT();
    uh_cpu[1] = m / (FM1_TICKS_PER_US * 10u * dt);       /* ticks / (ticks a ms * dt) * 100 */
    uh_cpu[2] = v / (FM1_TICKS_PER_US * 10u * dt);
    uh_cpu[3] = uh_cpu[0] > uh_cpu[1] ? uh_cpu[0] - uh_cpu[1] : 0u;
}

#if VOX_VC
/* streaming: the vocoder's screen. Top: the state, the device, the format and packets, the microphone L / R, the notes
 * held; bottom: the 16 bands' envelopes (the vocoder's spectrum), the eight controls, the hint */
static void uh_draw_stream(char (*hd)[UH_LW], uint32_t left)
{
    static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    char t[UH_LW], *p;
    uint32_t lh = AF_S.h + 1u, i, b, k = fm1__lock();
    uhs_meter_take(uh.mpk, uh.mrms);
    fm1__unlock(k);
    cv_begin(240, 120, T_BG);
    cv_text(4, 0, &AF_S, hd[0], T_THEME);
    cv_text(4, (int32_t)lh, &AF_S, uh_dev.prod[0] ? uh_dev.prod : hd[1], T_TEXT);
    uh_cpu_take();
    p = uh_s(t, "CPU ");                                /* the audio ISR's load, and its parts */
    p = uh_d(p, uh_cpu[0]);
    p = uh_s(p, "%  VOC ");
    p = uh_d(p, uh_cpu[1]);
    p = uh_s(p, "  USB ");
    p = uh_d(p, uh_cpu[2]);
    p = uh_s(p, "  ENG ");
    uh_d(p, uh_cpu[3]);
    cv_text(4, (int32_t)(2u * lh), &AF_S, t, uh_cpu[0] > 85u ? T_REC : T_MID);
    for (i = 0; i < 2u; i++) {                          /* the microphone */
        cv_text(4, (int32_t)((3u + i) * lh), &AF_S, i ? "R" : "L", T_TEXT);
        uh_bar((int32_t)((3u + i) * lh + 1u), uh.mpk[i], uh.mrms[i]);
    }
    if (uhs.vtrack) {                                   /* the carrier: VOX TRACK and its sound */
        p = uh_s(t, "CARRIER TRACK ");
        p = uh_d(p, uhs.vtrack);
        p = uh_s(p, "  ");
        uh_s(p, VOX_TRACK_NAME(uhs.vtrack));
    } else if (!VOX_NTRK && uhs.vc) {                   /* the module's own synth: the notes held */
        p = uh_s(t, "NOTES");
        for (i = 0, k = 0; i < 37u && k < 6u; i++)
            if (vc.keys >> i & 1u) {
                uint32_t n = (uint32_t)(48 + 12 * vc.oct) + i;
                p = uh_s(p, " ");
                p = uh_s(p, NN[n % 12u]);
                p = uh_d(p, n / 12u - 1u);
                k++;
            }
        if (!k)
            uh_s(p, " -");
    } else {
        uh_s(t, "CARRIER OFF (the microphone dry)");
    }
    cv_text(4, (int32_t)(5u * lh), &AF_S, t, T_TEXT);
    p = uh_s(t, uhsv.t_drop ? "DROP " : "");
    p = uh_d(p, uh.pkts_sec);
    p = uh_s(p, "pk/s miss ");
    p = uh_d(p, uhsv.missed);
    p = uh_s(p, " err ");
    p = uh_d(p, uhsv.errs + uhsv.dataerr);
    p = uh_s(p, " ring ");
    p = uh_d(p, uhs.w - uhs.r);
    p = uh_s(p, " u");
    uh_d(p, uhs.underruns);
    if (6u * lh + lh <= 120u)
        cv_text(4, (int32_t)(6u * lh), &AF_S, t, uhsv.t_drop ? T_REC : T_DIM);
    cv_blit(0, 0);

    cv_begin(240, 120, T_BG);
    for (b = 0; b < VC_NB; b++) {                       /* the bands: -60..0 dB of their envelopes */
        int32_t w = 224 / (int32_t)VC_NB, x = 8 + (int32_t)b * w, db = uhs_db((uint32_t)(vc.env[b] > 32767 ? 32767 : vc.env[b])) + 60, h;
        db = db < 0 ? 0 : db;
        h = db * 54 / 60;
        cv_rect(x, 2, w - 2, 54, T_LINE);
        if (h)
            cv_rect(x, 56 - h, w - 2, h, T_THEME);
    }
    for (i = 0; i < 2u; i++) {                          /* the controls, four a row; the one turned last lit */
        uint32_t j, x = 4;
        for (j = 0; j < 4u; j++) {
            uint32_t c = i * 4u + j;
            vc_ctl_text(c, t);
            cv_text((int32_t)x, (int32_t)(60u + i * lh), &AF_S, t,
                    c == uh.vc_touch && fm1_ms - uh.t_touch < 2000u ? T_ACCENT : T_TEXT);
            x += 60u;
        }
    }
    if (uh.bg) {
        uh_s(t, VOX_NTRK ? "< > TRACK   KNOB BANK: BACK" : "> VOX  OCT-/+  KNOB BANK: BACK");
    } else {
        p = uh_s(t, "< > LIST  OCT-/+  OTHER: EXIT ");
        p = uh_d(p, left);
        uh_s(p, "s");
    }
    cv_text(4, (int32_t)(120u - lh), &AF_S, t, T_DIM);
    cv_blit(0, 120);
    lcd_sync();
}

#endif

/* rows of AF_S: a header (state, VID:PID and speed, manufacturer, product, the outcome: kept until host mode
 * ends, the last event until there is one), then a list: the hub's and the device's lines, then the log's newest
 * lines (newest at the bottom). While a device is handled the list follows its end; once it is done it shows its
 * top; < / > scroll it (> past the end: follow again). The hint with the seconds left at the bottom. */
static void uh_draw(void)
{
    char (*ln)[UH_LW] = uh_ln, hd[5][UH_LW], ft[UH_LW], *p;
    const uint32_t cap = sizeof uh_ln / sizeof uh_ln[0];
    uint32_t lh = AF_S.h + 1u, per = 120u / lh, rows = 2u * per, body = rows - 6u, nl, i, half, left;
    uint32_t now = fm1_ms, t_end = uh.t_start + UH_TOTAL_MS;
    if (uh.st == UH_WAIT && uh.t_nodev + UH_NODEV_MS < t_end)
        t_end = uh.t_nodev + UH_NODEV_MS;
    left = (int32_t)(t_end - now) > 0 ? (t_end - now + 999u) / 1000u : 0u;
    p = uh_s(hd[0], "USB HOST  ");
    uh_s(p, UH_STATE[uh.st == UH_HUB && uh.hstep == H_WATCH ? UH_DONE : uh.st]);
    if (uh_dev.dev_ok) {
        p = uh_x(hd[1], uh_dev.vid, 4);
        p = uh_s(p, ":");
        p = uh_x(p, uh_dev.pid, 4);
        p = uh_s(p, uh.speed == 1u ? "  LOW SPEED" : "  FULL SPEED");
        p = uh_s(p, "  USB ");
        uh_bcd(p, uh_dev.bcd_usb);
    } else {
        uh_s(hd[1], uh.st == UH_WAIT ? "plug a device in" : uh.st == UH_HUB ? "hub: plug a device into it" : "-");
    }
    uh_s(hd[2], uh_dev.man[0] ? uh_dev.man : "-");
    uh_s(hd[3], uh_dev.prod[0] ? uh_dev.prod : "-");
    uh_s(hd[4], uh.result[0] ? uh.result : uh.msg);
    if (uh.st == UH_STREAM) {
        t_end = uh.t_stream + UH_STREAM_MS;
        left = (int32_t)(t_end - now) > 0 ? (t_end - now + 999u) / 1000u : 0u;
#if VOX_VC
        if (!uh.view) {
            uh_draw_stream(hd, left);
            return;
        }
#endif
    }
    nl = uh_hub_lines(ln, cap);
    nl += uh_lines(ln + nl, cap - nl);
    if (nl + 2u < cap) {
        uh_s(ln[nl++], "-- log --");
        nl += uh_log_tail(ln + nl, cap - nl < 20u ? cap - nl : 20u);
    }
    if (uh.follow || uh.scroll + body > nl)
        uh.scroll = (uint8_t)(nl > body ? nl - body : 0u);
    p = uh_s(ft, nl > body ? "< > SCROLL  " : "");
    p = uh_s(p, "OTHER KEY: EXIT  ");
    p = uh_d(p, left);
    uh_s(p, " s");
    for (half = 0; half < 2u; half++) {
        cv_begin(240, 120, T_BG);
        for (i = 0; i < per; i++) {
            uint32_t r = half * per + i, y = i * lh;
            if (r < 4u)
                cv_text(4, (int32_t)y, &AF_S, hd[r], r == 0u ? T_THEME : T_TEXT);
            else if (r == 4u)
                cv_text(4, (int32_t)y, &AF_S, hd[r], uh.result[0] ? (uh.res_ok ? T_THEME : T_REC) : T_MID);
            else if (r == rows - 1u)
                cv_text(4, (int32_t)y, &AF_S, ft, T_DIM);
            else if (r - 5u < body && uh.scroll + r - 5u < nl)
                cv_text(4, (int32_t)y, &AF_S, ln[uh.scroll + r - 5u], T_TEXT);
        }
        cv_blit(0, half * 120u);
    }
    lcd_sync();
}

/* ----------------------------------------------------------- session --- */
static void uh_device_mode(const char *out);
static void usbhost_session(const char *why)
{
    const uint32_t lr = 1u << SMK_BTN_LEFT | 1u << SMK_BTN_RIGHT;
    uint32_t t_draw = 0, pressed;
    const char *out = 0;
    char l[60], *p;
    uh_log_reset_if_bad();
    uh.sessions++;
    uh.t_start = fm1_ms;
    usbh_sie_ot = 0;
    uh.xfers = uh.xfer_errs = uh.resets = uh.pokes = uh.recoveries = 0;
    uh_log_session();                                   /* (the session's start is kept: logdump) */
    p = uh_s(l, "== host mode, session ");
    p = uh_d(p, uh.sessions);
    p = uh_s(p, " (");
    p = uh_s(p, why);
    uh_s(p, ")");
    uh_note(l);
    {
        char o[90];
        uint32_t k;
        p = uh_s(o, "hostopt ");
        p = uh_d(p, uh.opt);
        for (k = 0; k < sizeof UHO_NAME / sizeof UHO_NAME[0]; k++)
            if (uh.opt & 1u << k) {
                p = uh_s(p, " ");
                p = uh_s(p, UHO_NAME[k]);
            }
        uh_note(o);
    }
#if VOX_HAS_CDC
    {   /* the console's last words reach the computer before the port goes */
        uint32_t t0 = fm1_ms;
        while (co_r != co_w && cdc.dtr && fm1_ms - t0 < 300u)
            fm1_wdt_feed();
    }
#endif
    scr_wake_now();
    usb_detach();                                       /* usb_poll idle (usb.up = 0), no usb_retry */
    fm1_delay_ms(30);                                   /* the computer sees the device leave */
    {
        uint32_t k;
        for (k = 0; k < sizeof uh_dev; k++)
            ((uint8_t *)&uh_dev)[k] = 0;
    }
    uh.speed = 0;
    uh.scroll = 0;
    uh.follow = 1;
    uh.result[0] = uh.fail[0] = 0;
    uh_to_wait();
    fm1_input_edges(0);                                 /* (the combo's own presses) */
    lcd_fill(0, 0, 240, 240, T_BG);
    while (!out) {
        fm1_wdt_feed();
        uh_poll();
        pressed = fm1_input_edges(0);
        if (uh.st == UH_STREAM) {                       /* streaming: host mode goes on behind the normal UI */
            uh.bg = 1;
            uhs.head = 1;
            uh_note("streaming: back to the UI (< + > 3 s: host mode off)");
            fm1_input_edges(0);
            fm1_delay_ms(200);                          /* (the result row readable a moment) */
            lcd_fill(0, 0, 240, 240, T_BG);
            ui.force = 1;
            ui_message("USB MIC ON");
            return;
        }
        if (pressed & 1u << SMK_BTN_LEFT) {
            uh.follow = 0;
            if (uh.scroll)
                uh.scroll--;
        }
        if (pressed & 1u << SMK_BTN_RIGHT) {
            uint32_t was = uh.scroll;
            uh.scroll++;
            uh_draw();                                  /* (it clamps: no move = at the end, follow again) */
            if (uh.scroll == was)
                uh.follow = 1;
        }
        if (pressed & ~lr)
            out = "button";
        else if (uh.st == UH_WAIT && fm1_ms - uh.t_nodev > UH_NODEV_MS)
            out = "no device for 60 s";
        else if (uh.st == UH_STREAM && fm1_ms - uh.t_stream > UH_STREAM_MS)
            out = "10 min streaming limit";
        else if (uh.st != UH_STREAM && fm1_ms - uh.t_start > UH_TOTAL_MS)
            out = "120 s are up";
        if (fm1_ms - t_draw >= 100u || out) {
            uh_draw();
            t_draw = fm1_ms;
        }
    }
    uh_device_mode(out);
}

/* host mode ends (the session's limits, < + > again, no device): the stream stopped, the SIE closed, the device
 * presented again (the update path, the console), the UI redrawn */
static void uh_device_mode(const char *out)
{
    char l[60], *p = uh_s(l, "leaving host mode: ");
    uh_s(p, out);
    uh_note(l);
    p = uh_s(l, "transfers ");
    p = uh_d(p, uh.xfers);
    p = uh_s(p, " failed ");
    p = uh_d(p, uh.xfer_errs);
    p = uh_s(p, " resets ");
    p = uh_d(p, uh.resets);
    p = uh_s(p, " session recoveries ");
    p = uh_d(p, uh.recoveries);
    p = uh_s(p, " sie timeouts ");
    uh_d(p, usbh_sie_ot);
    uh_note(l);
    if (uh.st == UH_STREAM)
        uh_stream_stop(out);
    usbh_sie_close();
    uh.st = UH_IDLE;
    fm1_delay_ms(10);
    usb.config = 0;                                     /* (as usb_cdc_switch: nothing sent before the host configures) */
#if VOX_HAS_CDC
    cdc.dtr = 0;
    cdc.rx_pend = 0;
#endif
    usb.detached = 0;
    usb_start();                                        /* the device again: the update path, the console */
    uh_note(usb.up ? "device mode" : "device mode: no SIE yet (usb_retry)");
    uh.bg = 0;
    uhs.head = 0;
    uh.quiet = 0;
    scr_wake_now();                                     /* (MENU > SCREEN OFF: counted from here) */
    lcd_fill(0, 0, 240, 240, T_BG);
    ui.force = 1;
}

/* main loop: a request from the console (uh.req = 1), or < and > held together 3 s; never before both have
 * been seen released (a stuck reading at power-on starts nothing) */
/* the header's microphone level (ui_draw.c draw_head): 0 no stream, 1 quiet, 2 signal, 3 at full scale; looked at
 * every 150 ms (the peak in between) */
static uint32_t uhs_head_level(void)
{
    static uint32_t t, lv;
    if (fm1_ms - t >= 150u) {
        uint32_t pk = uhs.hpk;
        t = fm1_ms;
        uhs.hpk = 0;
        lv = uh.st != UH_STREAM ? 0u : pk >= 32000u ? 3u : pk >= 300u ? 2u : 1u;
    }
    return lv;
}

/* host mode in the background (main loop): the state machine (a lost stream set up again), its limits */
static void usbhost_bg(void)
{
    uh_poll();
    if (uh.st == UH_FAIL) {
        if (!uh.t_bgfail)
            uh.t_bgfail = fm1_ms | 1u;
        else if (fm1_ms - uh.t_bgfail > 10000u) {
            uh_device_mode("the device failed for 10 s");
            ui_message("USB HOST OFF");
            return;
        }
    } else {
        uh.t_bgfail = 0;
    }
    if (uh.st == UH_WAIT && fm1_ms - uh.t_nodev > UH_NODEV_MS) {
        uh_device_mode("no device for 60 s");
        ui_message("USB HOST OFF");
    }
}

#if VOX_VC
/* the vocoder's controls and its carrier in the settings (VOX_STORE): [0..7] the K1..K8 controls + 1 (0: the
 * default), [8] VOX TRACK: 0 track 1 (the default), 1..VOX_NTRK-1 the others, 255 OFF */
static void uh_vc_load(void)
{
#ifdef VOX_STORE
    uint32_t k;
    for (k = 0; k < VCP_KNOBS; k++)
        if (VOX_STORE(k) && VOX_STORE(k) <= 128u)
            vc.p[k] = (uint8_t)(VOX_STORE(k) - 1u);
    if (VOX_NTRK)
        uhs.vtrack = (uint8_t)(VOX_STORE(8u) == 255u ? 0u : VOX_STORE(8u) < VOX_NTRK ? VOX_STORE(8u) + 1u : 1u);
#endif
    vc_apply();
}
static void uh_vc_save(void)
{
#ifdef VOX_STORE
    uint32_t k;
    for (k = 0; k < VCP_KNOBS; k++)
        VOX_STORE(k) = (uint8_t)(vc.p[k] == VCP_DEF[k] ? 0u : vc.p[k] + 1u);
    VOX_STORE(8u) = (uint8_t)(uhs.vtrack ? uhs.vtrack - 1u : 255u);
    VOX_STORE_SAVE();
#endif
}

/* the VOCODER page (Vox37: MENU > SYSTEM > VOCODER; main loop, in place of ui_input): the knobs as on every page
 * (panel_enc: K2..K8 by their roles, the HARDWARE CALIBRATION's mapping and direction; K1 is the MASTER knob, encoder
 * 0): K1 MIX, K2 REL, K3 FMT, K4 SIBIL, K5 WAVE, K6 DET, K7 GAIN, K8 LEVEL; < > VOX TRACK (the carrier: OFF, a track;
 * its keys, steps and MIDI play it, its sound goes through the vocoder before its INSERT and FX); the mod wheel MIX;
 * KNOB BANK or FADER BANK closes it (the settings kept). WAVE and DET shape the module's own keys synth, used when
 * VOX_NTRK is 0: then > is VOX on / off (the keys play the synth), OCT- / OCT+ its octave, the pitch wheel its bend */
static void usbhost_page(void)
{
    static uint8_t modw_last = 0xFF, open;
    static const uint8_t ROLE[VCP_KNOBS] = {0xFF, EN_SELECT, EN_ALGO, EN_PRESET, EN_K1, EN_K1 + 1, EN_K1 + 2, EN_K1 + 3};
    uint32_t pressed = fm1_input_edges(0), e, re = 0;
    if (!open) {
        open = 1;
        lcd_fill(0, 0, 240, 240, T_BG);
        uh.t_touch = 0;
    }
    for (e = 0; e < VCP_KNOBS; e++) {
        int32_t d = ROLE[e] == 0xFF ? fm1_enc_take(SMK_ENC_MASTER) : panel_enc(ROLE[e]);
        if (!d)
            continue;
        re |= vc_turn(e, d);
        uh.vc_touch = (uint8_t)e;
        uh.t_touch = fm1_ms | 1u;
        {
            char t[16], l[24], *p = uh_s(l, "K");
            vc_ctl_text(e, t);
            p = uh_d(p, e + 1u);
            p = uh_s(p, " ");
            uh_s(p, t);
            uh_note(l);
        }
    }
    if (re)
        vc_apply();
    if (!VOX_NTRK) {                                    /* the module's own synth, played by the keys */
        if (pressed & 1u << SMK_BTN_RIGHT) {
            uh.vox_off ^= 1u;
            if (uh.st == UH_STREAM) {
                uhs.vc = !uh.vox_off;
                UH_KEYS_TO_VOCODER(!uh.vox_off);
            }
            uh_note(uh.vox_off ? "VOX off" : "VOX on");
        }
        if (pressed & 1u << FM1_BTN_OCT_DOWN && vc.oct > -3)
            vc.oct--;
        if (pressed & 1u << FM1_BTN_OCT_UP && vc.oct < 2)
            vc.oct++;
        vc.bend = smk_bend14;
    } else if (pressed & (1u << SMK_BTN_LEFT | 1u << SMK_BTN_RIGHT)) {   /* < >: the carrier, OFF or a track */
        uint32_t v = uhs.vtrack;
        v = pressed & 1u << SMK_BTN_RIGHT ? (v + 1u) % (VOX_NTRK + 1u) : (v + VOX_NTRK) % (VOX_NTRK + 1u);
        uhs.vtrack = (uint8_t)v;
        {
            char l[24], *p = uh_s(l, "VOX TRACK ");
            if (v)
                uh_d(p, v);
            else
                uh_s(p, "OFF");
            uh_note(l);
        }
    }
    if (modw_last != 0xFF && smk_modw != modw_last) {
        vc.p[VCP_MIX] = smk_modw;                       /* the mod wheel: MIX */
        uh.vc_touch = VCP_MIX;
        uh.t_touch = fm1_ms | 1u;
    }
    modw_last = smk_modw;
    if (pressed & (1u << SMK_BTN_FADER_BANK | 1u << SMK_BTN_KNOB_BANK)) {
        uhs.page = 0;                                   /* back to the UI; the controls kept (settings) */
        open = 0;
        uh_vc_save();
        lcd_fill(0, 0, 240, 240, T_BG);
        ui.force = 1;
    }
}

static void usbhost_page_draw(void)
{
    static uint32_t t;
    char hd[5][UH_LW], *p;
    if (fm1_ms - t < 100u)
        return;
    t = fm1_ms;
    p = uh_s(hd[0], "VOCODER  ");
    p = uh_s(p, (VOX_NTRK ? uhs.vtrack != 0 : !uh.vox_off) ? "VOX ON" : "VOX OFF");
    if (uh.st != UH_STREAM)
        uh_s(p, uh.st == UH_WAIT ? "  NO MIC" : "  SETUP");
    uh_s(hd[1], uh_dev.prod[0] ? uh_dev.prod : "-");
    hd[2][0] = hd[3][0] = hd[4][0] = 0;
    uh_draw_stream(hd, 0);
}

#endif

/* ------------------------------------------------------ USB MIC: AUTO --- */
/* MENU > SYSTEM > USB MIC AUTO (the default): a USB device whenever a computer is on the port, a microphone when none
 * is. The SMK-37 Pro gives no VBUS and no VBUS sense is known (DEVCTL's VBus bits move with nothing attached, PB1 is
 * the charger's status, read only and never driven), so "a computer is there" is what device mode sees of one: it
 * configured us and its frame counter runs (usb.config, usb.suspended: the frame number frozen 100 ms), or it reset
 * the bus (usb.resets). No computer for 3 s after power-on (10 s once one was there: a sleeping computer is not taken
 * for gone at once) -> a probe: host mode in the background, 1.5 s for a device to pull D+ / D- up. A microphone:
 * streamed (usbhost.c). Nothing, or a device that is not one (UAC2, a hub, anything else): device mode again, and the
 * next probe after 4 s (30 s after a device that is not a microphone), each 4 s long enough for a computer plugged in
 * meanwhile to enumerate us: it wins, and while it is there no probe runs. A microphone unplugged: device mode at
 * once. Every switch is logged with its reason; the probes themselves are quiet (uh.quiet) until a device connects.
 * OFF: never; ON: host mode from 3 s after power-on (or the change), in the background, until < + > held or 60 s
 * without a device. < + > held 3 s toggles in any mode (on: the setup screen). The update
 * path is never blocked: device mode between probes, OCT- + OCT+ held (UBOOT) works in host mode too, and a crash in
 * host mode within 30 s of power-on counts for the boot guard (UBOOT after two). */
static struct {
    uint32_t t_host, t_back, wait, probes, resets;
    uint8_t probing, seen;                 /* probing: this host mode is an automatic probe; seen: a computer was there */
    uint8_t on_done;                       /* ON: host mode started (once; again after a change of the setting) */
    char status[UH_LW];                    /* the menu's status: none, the microphone, why not */
} ua;

static const char *uh_status_text(void)
{
    if (uh.st == UH_STREAM)
        return uh_dev.prod[0] ? uh_dev.prod : "MIC";
    if (VOX_MODE() == UHM_AUTO && !uh.bg && ua.seen && fm1_ms - ua.t_host < 3000u)
        return "PC";
    return ua.status[0] ? ua.status : "";
}

static void ua_status(const char *s)
{
    uint32_t i;
    for (i = 0; s[i] && i + 1u < UH_LW; i++)
        ua.status[i] = s[i];
    ua.status[i] = 0;
}

/* host mode in the background without the setup screen (an automatic probe, MENU > USB MIC ON) */
static void uh_bg_start(const char *why, uint32_t quiet)
{
    char l[70], *p = uh_s(l, "== host mode (");
    uh_log_reset_if_bad();
    uh.sessions++;
    uh.t_start = fm1_ms;
    usbh_sie_ot = 0;
    uh.xfers = uh.xfer_errs = uh.resets = uh.pokes = uh.recoveries = 0;
    uh.quiet = (uint8_t)quiet;
    p = uh_s(p, why);
    uh_s(p, ")");
    uh_note(l);
    usb_detach();
    fm1_delay_ms(30);
    {
        uint32_t k;
        for (k = 0; k < sizeof uh_dev; k++)
            ((uint8_t *)&uh_dev)[k] = 0;
    }
    uh.speed = 0;
    uh.result[0] = uh.fail[0] = 0;
    uh_to_wait();
    uh.bg = 1;
    uhs.head = 1;
}

/* why a device is no microphone, for the menu */
static const char *ua_why(void)
{
    uint32_t i;
    if (uh_hub.present)
        return "HUB: NOT YET";
    for (i = 0; i < uh_dev.nif; i++)
        if (uh_dev.itf[i].cls == 1u && uh_dev.itf[i].proto == 0x20u)
            return "UAC2: NOT YET";
    for (i = 0; i < uh_dev.nif; i++)
        if (uh_dev.itf[i].cls == 1u && uh_dev.itf[i].sub == 2u)
            return "NO PCM STREAM";
    return uh.st == UH_FAIL ? "NO ANSWER" : "NOT A MIC";
}

/* main loop, AUTO: a probe when no computer is on the port; its end */
static void usbhost_auto(void)
{
    uint32_t now = fm1_ms;
    if (!uh.bg) {                                       /* device mode: is a computer there? */
        if ((usb.up && usb.config && !usb.suspended) || usb.resets != ua.resets) {
            ua.resets = usb.resets;
            ua.t_host = now;
            if (!ua.seen)
                uh_note("auto: a computer on the port");
            ua.seen = 1;
        }
        if (now - ua.t_host > (ua.seen ? 10000u : 3000u) && now - ua.t_back > ua.wait && now > 3000u) {
            ua.probing = 1;
            ua.probes++;
            if (ua.probes % 20u == 0u) {                /* (the quiet probes: a line now and then) */
                char l[48], *p = uh_s(l, "auto: ");
                p = uh_d(p, ua.probes);
                uh_s(p, " probes, no microphone");
                uh_note(l);
            }
            uh_bg_start(ua.probes == 1u ? "auto: no computer on the port, looking for a microphone" : "auto probe",
                        ua.probes > 1u);
        }
        return;
    }
    if (!ua.probing)
        return;
    if (uh.st == UH_STREAM) {                           /* a microphone: it stays */
        ua_status(uh_dev.prod[0] ? uh_dev.prod : "MIC");
        if (!uh.quiet)
            uh_note("auto: a microphone, streaming");
        ua.probing = 2;
        ua.probes = 0;
        return;
    }
    if (ua.probing == 2u && uh.st == UH_WAIT) {         /* it went: device mode at once (a computer may be next) */
        uh.quiet = 0;
        ua_status("NO MIC");
        uh_device_mode("auto: the microphone is gone");
        ua.probing = 0;
        ua.t_back = now;
        ua.wait = 4000u;
        return;
    }
    if (uh.st == UH_WAIT && now - uh.t_start > 1500u) {  /* nothing on the port: device mode, again in 4 s */
        ua_status("NO MIC");
        uh_device_mode("auto: no device");
        uh.quiet = 0;
        ua.probing = 0;
        ua.t_back = now;
        ua.wait = 4000u;
        return;
    }
    if ((uh.st == UH_DONE || uh.st == UH_FAIL || uh.st == UH_HUB) && now - uh.t_start > 2500u) {
        char l[48], *p = uh_s(l, "auto: no microphone: ");
        ua_status(ua_why());
        uh_s(p, ua.status);
        uh_note(l);
        uh_device_mode("auto: not a microphone");
        ua.probing = 0;
        ua.t_back = now;
        ua.wait = 30000u;                               /* (the same device: not every 4 s) */
        return;
    }
    if (now - uh.t_start > 8000u) {
        ua_status("NO ANSWER");
        uh_device_mode("auto: the probe took too long");
        ua.probing = 0;
        ua.t_back = now;
        ua.wait = 4000u;
    }
}

static void usbhost_service(void)
{
    static uint32_t t0;
    static uint8_t armed, mode_last = 0xFF;
#if VOX_VC
    if (!vc.ready) {                                    /* (power-on: the vocoder's controls from the settings) */
        vc_init();
        uh_vc_load();
        uh.vox_off = VOX_NTRK != 0;                     /* (a track the carrier: not the module's keys synth) */
    }
#endif
    const uint32_t lr = 1u << SMK_BTN_LEFT | 1u << SMK_BTN_RIGHT;
    uint32_t b = fm1_in.buttons & lr, r, mode = VOX_MODE();
    if (!b)
        armed = 1;
    if (b != lr || !armed)
        t0 = fm1_ms;
    else if (fm1_ms - t0 > 3000u) {
        armed = 0;
        if (uh.bg) {                                    /* in the background: < + > again ends host mode */
            uh_device_mode("< + > held");
            ui_message("USB HOST OFF");
            ua.probing = 0;
            ua.t_back = fm1_ms;
            ua.wait = 30000u;                           /* (AUTO: the next probe a while later) */
            return;
        }
        uh.req = 2;
    }
    if (mode != mode_last) {                            /* MENU > USB MIC changed (or power-on) */
        if (mode_last != 0xFF) {
            char l[24], *p = uh_s(l, "USB MIC ");
            uh_s(p, UHM_NAME[mode % 3u]);
            uh_note(l);
            ua.on_done = 0;                             /* (ON: host mode once more) */
            if (mode == UHM_OFF && uh.bg && ua.probing) {
                uh_device_mode("USB MIC OFF");
                ua.probing = 0;
            }
        }
        mode_last = (uint8_t)mode;
    }
    if (mode == UHM_AUTO && !uh.req)
        usbhost_auto();
    if (mode == UHM_ON && !uh.bg && !ua.on_done && fm1_ms > 3000u) {   /* ON: host mode, from 3 s after power-on */
        ua.on_done = 1;
        ua.probing = 0;
        uh_bg_start("USB MIC ON", 0);
    }
    if (uh.bg) {
        usbhost_bg();
        return;
    }
    if (!uh.req)
        return;
    r = uh.req;
    uh.req = 0;
    ua.probing = 0;
    usbhost_session(r == 1u ? "console" : "< + > held");
}
