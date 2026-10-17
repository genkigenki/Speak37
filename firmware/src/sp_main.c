/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (the boot, the ISRs' frame and the update
 * paths, from main.c and audio.c)
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37's boot, ISRs and main loop. Boot: WDT first, the boot-loop guard (two crashes in the first 30 s: UBOOT),
 * the fatal vectors, the guards; then the store, the LCD, the input scan (TIMER5, 10 kHz), the audio (ALNK0).
 *   audio ISR   per half buffer (128 frames), blocks of SP_BLK: USB-MIDI in, then sp_render (sp_dsp.c: the keys, the
 *               voices, the bus) -> MASTER, a soft limiter -> the USB audio input (a computer hears it) -> 24 bit.
 *               Its load is measured; above 85 % the voices are cut down (sp.vmax, 8 .. 3), then RHYTHM's ringing lanes
 *               (rh_lmax, 8 .. 4); below 60 % they come back, the lanes first.
 *   TIMER5      the key / button / pad / fader scan, the milliseconds, the USB device poll (2 kHz)
 *   main loop   the update paths (M-UPGRADE, UBOOT), the console, the panel (sp_ui.c), the wheels, MASTER, the store */
#define HALF_WORDS (HALF_FRAMES * 2u)
#define OUT_SHIFT 7                                 /* Q15 -> 24 bit, -6 dBFS ceiling */
#define SP_LOAD_HI ((HALF_FRAMES * 1000000u / FS) * 85u / 100u)   /* us of a half: cut a voice */
#define SP_LOAD_LO ((HALF_FRAMES * 1000000u / FS) * 60u / 100u)   /* .. give one back */

static volatile uint32_t t5_nested_ticks;
static volatile int32_t sp_master_q12 = 2047;
static uint32_t sp_over, sp_under;

void fm1_timer5_irq(void)
{
    static uint32_t sub, owed;
    uint32_t t0 = fm1_ticks(), usb_due = sub % 5u == 0u;
    fm1_timer5_ack();
    fm1_input_tick();
#if BOARD_SMK37
    smo_tick();                                     /* TRS MIDI OUT: a byte when the last has gone (also nested) */
#endif
    {
        static uint32_t last, acc;
        acc += t0 - last;
        last = t0;
        while (acc >= 1000u * FM1_TICKS_PER_US) {
            acc -= 1000u * FM1_TICKS_PER_US;
            fm1_ms++;
        }
    }
    if (usb_due)
        owed |= 1u;
    if (++sub == 10u)
        sub = 0;
    if (sp_dbg.in_audio) {                          /* nested in the render: only what cannot wait */
        sp_dbg.nested++;
        if (usb_due)
            uac_service();
        t5_nested_ticks += fm1_ticks() - t0;
        return;
    }
    if (owed & 1u)
        usb_poll();
    owed = 0;
}
extern void isr_timer5(void);

/* USB-MIDI in (usb.c's queue): notes, bend, CC1, program change */
static void sp_midi_drain(void)
{
    if (midi_in_overflow) {                         /* a lost note-off must never leave a note held */
        mi_r = mi_w;
        sp.mheld[0] = sp.mheld[1] = sp.mheld[2] = sp.mheld[3] = 0;
        midi_in_overflow = 0;
        return;
    }
    while (mi_r != mi_w) {
        uint32_t pkt = midi_in_q[mi_r % MQ];
        RING_PUBLISH();
        mi_r++;
        sp_midi(pkt);
    }
}

/* the soft limiter: linear to 24576, then a knee up to the ceiling 32767 */
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

static void sp_audio_block(int32_t *out, uint32_t n)
{
    uint32_t i;
    int32_t g = sp_master_q12;
    sp_midi_drain();
    sp_render(out, n);
    for (i = 0; i < 2u * n; i++)
        out[i] = sp_limit((int32_t)((int64_t)out[i] * g >> 12));
    uac_tap(out, n);                                /* the USB audio input: what the speaker plays */
    for (i = 0; i < 2u * n; i++)
        out[i] *= 1 << OUT_SHIFT;
}

void fm1_alnk0_irq(void)
{
    uint8_t p;
    uint32_t t0;
    sp_dbg.in_audio = 1;
    p = fm1_audio_pending();
    t0 = fm1_ticks();
    t5_nested_ticks = 0;
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half(), b, us;
        int32_t *o = &abuf[half * HALF_WORDS];
        uac_render_start();
        for (b = 0; b < HALF_FRAMES; b += SP_BLK)
            sp_audio_block(o + 2u * b, SP_BLK);
        fm1_audio_ack_half();
        sp_dbg.halves++;
        us = (fm1_ticks() - t0 - t5_nested_ticks) / FM1_TICKS_PER_US;
        if (us > audio_max_us)
            audio_max_us = us;
        sp_cpu_q8 = (sp_cpu_q8 * 31u + us * 256u / (HALF_FRAMES * 1000000u / FS)) / 32u;
        if (us + t5_nested_ticks / FM1_TICKS_PER_US > SP_LOAD_HI) {   /* too slow: one voice fewer */
            sp_under = 0;
            if (++sp_over >= 2u && sp.vmax > 3u) {
                sp.vmax--;
                sp_over = 0;
            } else if (sp_over >= 2u && rh_lmax > 4u) {   /* (the speech at its least: the drums' lanes) */
                rh_lmax--;
                sp_over = 0;
            }
        } else if (us < SP_LOAD_LO) {
            sp_over = 0;
            if (++sp_under >= 2000u && rh_lmax < DV_NLANE) {   /* (~6 s: the drums' lanes first, then the voices) */
                rh_lmax++;
                sp_under = 0;
            } else if (sp_under >= 2000u && sp.vmax < SP_NV) {
                sp.vmax++;
                sp_under = 0;
            }
        }
        if (fm1_audio_free_half() != half)
            sp_dbg.late++;
    }
    sp_dbg.in_audio = 0;
}
extern void isr_alnk0(void);

/* ------------------------------------------------------------ the store --- */
static void sp_store_save(void)
{
    sp_store_seal(&vst);
    if (!flash_ok || st_save(OBJ_SETTINGS, &vst, sizeof vst)) {
        sp_store_fail = 1;
        sp_popup("SAVE", "FAILED");
    }
}

/* the USER sentences (sp_user.c's OK): their own record (OBJ_PROJECT0; the SMK-37: FL_DATA 0x9D000 / 0x9E000) */
static sp_user_rec_t sp_urec;
static void sp_user_save(void)
{
    sp_user_pack(&sp_urec);
    if (!flash_ok || st_save(OBJ_PROJECT0, &sp_urec, sizeof sp_urec)) {
        sp_store_fail = 1;
        sp_popup("SAVE", "FAILED");
    }
}

static void sp_store_boot(void)
{
    uint32_t f = irq_save();
    int n = -1;
    flash_ok = FL_JEDEC_OK(FL_FAR(fl_jedec_ram)());      /* the expected 1 MiB part, else RAM only */
    irq_restore(f);
    sp_store_defaults(&vst);
    if (flash_ok) {
        fl_plain_window_init();                          /* (USRTRIM through XIP: the key curves) */
        st_owner_check();                                /* never another firmware's store (storage.c) */
        n = st_load(OBJ_SETTINGS, &vst, sizeof vst);
        if (!sp_store_take(&vst, n))
            sp_store_defaults(&vst);
        sp_user_take(&sp_urec, st_load(OBJ_PROJECT0, &sp_urec, sizeof sp_urec));
    }
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
#if BOARD_SMK37
    smk_master = vst.master;                            /* (the SMK's K1: a virtual pot; the FM-1's is a real one) */
    sp_glob.trs_on = (uint8_t)!vst.trs_off;
    trs_out = sp_glob.trs_on;
#endif
    sp_apply();
}

#if BOARD_SMK37
/* ----------------------------------------------------------- the wheels --- */
/* self-calibrating (main.c's smk_wheels): the pitch wheel's rest is its centre (taken at power-on, followed slowly
 * while it rests), each range grows to what it has reached. Pitch: -8192..8191 with a dead band at the centre (exactly
 * no bend), SETUP BEND semitones at the ends; mod: 0..127 (sp_dsp.c: vibrato). The state line shows a wheel moved */
#define SP_WHEEL_BAND 8                                 /* raw steps: the wheels' jitter is a few (mod ~474 .. 629) */
static void sp_wheels(void)
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
    if (sp.mode == SP_MODE_MOPE) {                       /* MOPE: a wheel moved, not its noise or drift (mo_move: */
        static mo_mv_t mvp, mvm;                         /*  the raw readings, a band beyond their jitter) */
        int32_t dp = mo_move(&mvp, p, SP_WHEEL_BAND, fm1_ms), dm = mo_move(&mvm, m, SP_WHEEL_BAND, fm1_ms);
        if (dp)
            mo_ui_ctl_dir(MO_CT_BEND, dp);
        if (dm)
            mo_ui_ctl_dir(MO_CT_MODW, -dm);              /* (the mod wheel reads lower as it goes up) */
    }
    sp_trs_bend(b);                                      /* TRS MIDI OUT: as played (sp_trs.c) */
    sp_trs_modw(mw);
    if (b != lastb) {
        int32_t was = lastb;
        lastb = b;
        sp.bend = sp.mode == SP_MODE_MOPE ? 0 : (int16_t)(b * (int32_t)sp.bend_range / 128);   /* (1/64 semitones;
                                                                                         MOPE: only its comment) */
        if (was != 99999) {                              /* the state line: BEND +1.5 */
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
#endif

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    /* the whole record: the vector, where the last interrupt came back to (reti), rets, the EMU and DBG causes
     * (stack limit; write limits, bus, PC), sp, the uptime and the last four branches (ETM) */
    static const char *const L[8] = {"VEC", "RETI", "RETS", "EMU", "DBG", "SP", "ETM0", "ETM1"};
    const uint32_t V[8] = {c->vec, c->pc, c->rets, c->emu, c->dbg, c->sp, c->etm[0], c->etm[1]};
    char b[24];
    uint32_t t0, i;
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, UI_CRASH_BG);
    draw_text_line(0, 4, 240, &AF_M, "SPEAK37 CRASH", UI_CRASH_INK, UI_CRASH_BG, 1);
    for (i = 0; i < 8u; i++) {
        str_cpy(b, L[i], sizeof b);
        b[str_len(b)] = ' ';
        b[str_len(L[i]) + 1u] = 0;
        hexs(b + str_len(b), V[i]);
        draw_text_line(10, 30 + i * 20u, 220, &AF_S, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    }
    hexs(b, c->etm[2]);
    b[8] = ' ';
    hexs(b + 9, c->etm[3]);
    draw_text_line(10, 30 + 8u * 20u, 220, &AF_S, b, UI_CRASH_INK, UI_CRASH_BG, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 8000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

static void sp_uboot(const char *why)
{
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, SV_BG);
    draw_text_line(0, 110, 240, &AF_M, why, SV_TEXT, SV_BG, 1);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;                              /* intentional: not a failed boot */
    fm1_enter_uboot();
}

static void fm1_main(void)
{
    uint32_t ms;
    sp_init();
    sp_store_boot();
    palette_set(SP_PALETTE);                            /* (text in colour: GREY would blend it on red alone) */
    lcd_init();
    lcd_fill(0, 0, 240, 240, SV_BG);
    sp_dbg.boots++;
    sp_mhz = fm1_cpu_mhz();                             /* (the interrupts still off: the boot screen's number) */
    fm1_input_init();
    fm1_adc_init();
#if BOARD_SMK37
    if (flash_ok)
        smk_trim_load(fm1_xip_ptr(SMK_TRIM_FLASH));      /* the factory key / pad curves (read only) */
#endif
#if FELUCCA_OTA
    if (flash_ok)
        ota_boot_cleanup();
#endif
    for (ms = 0; ms < 2u * HALF_WORDS; ms++)
        abuf[ms] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
    usb_start();
#if BOARD_SMK37
    smk_uart1_tx_init();                                /* TRS MIDI OUT (PORTC, IOMAP RMW: before TIMER5) */
#endif
    fm1_timer5_start(isr_timer5, 4);                    /* above ALNK0 (3): the scan nests into the render */
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    {   /* the boot screen (the sound and USB already run) */
        uint32_t t0 = fm1_ms;
        while (fm1_ms - t0 < SP_BOOT_MS) {
            fm1_wdt_feed();
            sp_boot_frame(fm1_ms - t0);
        }
    }
    lcd_fill(0, 0, 240, 240, SV_BG);
    su.redraw = 1;
#if BOARD_SMK37
    if (st_owner_cleared)
        sp_popup("MEMORY", "NEW STORE");
#endif

    for (;;) {
        uint32_t m = fm1_ms;
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {     /* a crash or hang in the first 30 s counts */
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        {   /* MASTER, the level pot^2 / 256: the SMK's K1 (a virtual pot, smk_input.h), the FM-1's pot (smoothed) */
            static int32_t last = -1, sm = -1;
            int32_t pot = fm1_adc_read(FM1_ADC_MASTER);
#if !BOARD_SMK37
            if (pot < 0)
                pot = (int32_t)sp_pot;
            sm = sm < 0 ? pot * 16 : sm + (pot * 16 - sm) / 8;
            pot = sm / 16;
            pot = last >= 0 && pot - last < 6 && last - pot < 6 ? last : pot;   /* (the ADC's last bits: no flicker) */
#endif
            (void)sm;
            sp_pot = (uint32_t)(pot < 0 ? 0 : pot > 1023 ? 1023 : pot);
            sp_master_q12 = (int32_t)(sp_pot * sp_pot / 256u);
            if (last >= 0 && (int32_t)sp_pot != last)
                sp_touch(0);                            /* (its cell) */
            last = (int32_t)sp_pot;
        }
        {   /* OCT- + OCT+ held 5 s: UBOOT (the update path) */
            static uint32_t t0;
            if ((fm1_in.buttons & FM1_OCT_BOTH) != FM1_OCT_BOTH)
                t0 = fm1_ms;
            else if (fm1_ms - t0 > 5000u)
                sp_uboot("UBOOT");
            else if (fm1_ms - t0 > 2000u)
                sp_popup("UPDATE MODE IN", fm1_ms - t0 > 4000u ? "1" : fm1_ms - t0 > 3000u ? "2" : "3");
        }
#if FELUCCA_OTA
        ota_service();                                  /* M-UPGRADE handshake (the installers) */
        if (usb.ota_req) {
            usb.ota_req = 0;
            sp.mheld[0] = sp.mheld[1] = sp.mheld[2] = sp.mheld[3] = 0;
            if (flash_ok)
                ota_session();                          /* returns only if nothing was committed */
            lcd_fill(0, 0, 240, 240, SV_BG);
            su.redraw = 1;
        }
#endif
        if (usb.uboot_req)                              /* SysEx F0 22 24 35 7D F7 from the host */
            sp_uboot("UBOOT (USB)");
#if FELUCCA_CDC
        cdc_task();
#endif
        sp_ui_input();
#if BOARD_SMK37
        sp_wheels();
#endif
        sp_ui_autosave(sp_pot);
        sp_ui_leds();
#if BOARD_SMK37
        padled_flush();                                 /* the pads' colours, between LCD transfers */
#endif
        sp_ui_draw();
        while (fm1_ms - m < 15u) {
            sp_ui_input();
#if BOARD_SMK37
            sp_wheels();
#endif
        }
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, p3, src, wdt;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 2u) {
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    fm1_main();
    for (;;)
        ;
}
