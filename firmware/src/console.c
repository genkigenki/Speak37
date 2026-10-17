/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Serial console on the CDC-ACM function (FELUCCA_CDC=1): read-only
 * diagnostics. Runs in the main loop (cdc_task); usb_poll moves the bytes.
 * The baud rate is ignored. Nothing here writes memory or flash; `uboot`
 * does what the SysEx soft key does, `host` (SMK-37 Pro) what < + > held does: USB host mode (usbhost.c). */
#define CON_LINE 64u

static struct {
    char line[CON_LINE];
    uint32_t len;
    uint8_t dtr_seen, stalled;   /* stalled: the host stopped reading, drop the rest of this reply */
} con;

static void con_putc(char c)
{
    uint32_t t0 = fm1_ms;
    while (co_w - co_r >= CO_N) {                      /* full: wait for usb_poll, briefly */
        if (!cdc.dtr || con.stalled || fm1_ms - t0 > 20u) {
            con.stalled = 1;
            return;
        }
        fm1_wdt_feed();
    }
    cdc_out[co_w % CO_N] = (uint8_t)c;
    RING_PUBLISH();
    co_w++;
}

static void con_puts(const char *s)
{
    while (*s)
        con_putc(*s++);
}

static void con_hex(uint32_t v, uint32_t digits)
{
    while (digits--)
        con_putc("0123456789ABCDEF"[(v >> (digits * 4u)) & 15u]);
}

static void con_dec(int32_t v)
{
    char b[12];
    uint32_t n = 0, u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    if (v < 0)
        con_putc('-');
    do
        b[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) != 0 && n < sizeof b);
    while (n)
        con_putc(b[--n]);
}

static void con_kv(const char *k, int32_t v)            /* "key value\r\n" */
{
    con_puts(k);
    con_putc(' ');
    con_dec(v);
    con_puts("\r\n");
}

static void con_kx(const char *k, uint32_t v)
{
    con_puts(k);
    con_puts(" 0x");
    con_hex(v, 8);
    con_puts("\r\n");
}

static uint32_t con_num(const char **p, int *ok)        /* decimal or 0x hex */
{
    const char *s = *p;
    uint32_t v = 0, base = 10, d, n = 0;
    while (*s == ' ')
        s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2, base = 16;
    for (;; s++, n++) {
        char c = *s;
        if (c >= '0' && c <= '9')
            d = (uint32_t)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f')
            d = (uint32_t)(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F')
            d = (uint32_t)(c - 'A' + 10);
        else
            break;
        v = v * base + d;
    }
    *ok = n > 0;
    *p = s;
    return v;
}

static int con_word(const char **p, const char *w)       /* match a whole word */
{
    const char *s = *p;
    while (*s == ' ')
        s++;
    while (*w && *s == *w)
        s++, w++;
    if (*w || (*s && *s != ' '))
        return 0;
    *p = s;
    return 1;
}

static void con_memr(const char *p)
{
    int ok, ok2;
    uint32_t a = con_num(&p, &ok), n = con_num(&p, &ok2), i;
    if (!ok) {
        con_puts("usage: memr ADDR [LEN<=256]\r\n");
        return;
    }
    if (!ok2 || !n)
        n = 64;
    if (n > 256u)
        n = 256;
    if (!fm1_mem_readable(a, n)) {
        con_puts("only RAM 01C00000-01C80000 and XIP 02000000-02100000\r\n");
        return;
    }
    for (i = 0; i < n; i++) {
        if (i % 16u == 0) {
            con_hex(a + i, 8);
            con_putc(':');
        }
        con_putc(' ');
        con_hex(fm1_peek8(a + i), 2);
        if (i % 16u == 15u || i + 1u == n)
            con_puts("\r\n");
    }
}

#if FELUCCA_FLASH
static void con_flr(const char *p)                  /* flash read over SPI (no XIP decryption) */
{
    static uint8_t b[256];
    int ok, ok2;
    uint32_t a = con_num(&p, &ok), n = con_num(&p, &ok2), i;
#if BOARD_SMK37
    if (!ok || a < 0x9D000u || a >= 0x100000u) {
        con_puts("usage: flr OFFSET [LEN<=256]   (0x9D000..0xFFFFF)\r\n");
#else
    if (!ok || a < 0x93000u || a >= 0x100000u) {
        con_puts("usage: flr OFFSET [LEN<=256]   (0x93000..0xFFFFF)\r\n");
#endif
        return;
    }
    if (!ok2 || !n)
        n = 64;
    if (n > 256u)
        n = 256;
    if (a + n > 0x100000u)
        n = 0x100000u - a;
    if (!flash_ok || st_read(a, b, n)) {
        con_puts("flash not available\r\n");
        return;
    }
    for (i = 0; i < n; i++) {
        if (i % 16u == 0) {
            con_hex(a + i, 6);
            con_putc(':');
        }
        con_putc(' ');
        con_hex(b[i], 2);
        if (i % 16u == 15u || i + 1u == n)
            con_puts("\r\n");
    }
}
#endif

#if FELUCCA_UAC
static void con_uac(void)                              /* USB audio input: stream state and glitches */
{
    uint32_t p = uac.pkts, base = uac.r48 ? 48u : 44u;
    con_kv("uac_alt", uac.alt);
    con_kv("uac_hz", uac.r48 ? 48000 : 44100);      /* the rate the host set (SET_CUR; 44100 until it does) */
    con_kv("uac_rate_sets", (int32_t)uac.rate_sets);
    con_kv("uac_starts", (int32_t)uac.starts);
    con_kv("uac_pkts", (int32_t)p);
    con_kv("uac_rate_hz", p ? (int32_t)base * 1000 + (int32_t)(uac.frames - base * p) * 1000 / (int32_t)p : 0);   /* < 5 h */
    con_kv("uac_underruns", (int32_t)uac.underruns);
    con_kv("uac_overruns", (int32_t)uac.overruns);
    con_kv("uac_missed", (int32_t)uac.missed);
    con_kv("uac_stalls", (int32_t)uac.stalls);
    con_kv("uac_adj_up", (int32_t)uac.adj_up);
    con_kv("uac_adj_down", (int32_t)uac.adj_down);
    con_kv("uac_fill", (int32_t)uac.fill_min);
    con_kv("uac_fill_lo", uac.fill_lo == 0xFFFFFFFFu ? -1 : (int32_t)uac.fill_lo);
    con_kv("uac_fill_hi", (int32_t)uac.fill_hi);
}
#endif

static void con_status(void)
{
    const engine_t *e = ENGINES[TSEL->eng_req % NENGINES];
    con_puts("felucca ");
    con_puts(FELUCCA_VERSION);
    con_puts("\r\n");
    con_kv("uptime_ms", (int32_t)fm1_ms);
    con_kv("cpu_pct", (int32_t)(song.cpu_q8 * 100u / 256u));
    con_kv("audio_max_us", (int32_t)felucca_dbg.max_us);
    con_kv("audio_late", (int32_t)felucca_dbg.late);
    con_kv("voices_shed", (int32_t)shed_count);
    con_kv("voices_given_up", (int32_t)voice_kills);
    con_kv("track", (int32_t)song.sel + 1);
    con_kv("batt_raw", song.batt_raw);
    con_puts("engine ");
    con_puts(e->name);
    con_puts("\r\n");
    con_puts("preset ");
    con_puts(TSEL->preset < e->npresets ? e->presets[TSEL->preset].name : "-");
    con_puts("\r\n");
    con_kv("bpm", song.g[G_BPM]);
    con_kv("playing", song.playing);
    con_kv("boots", (int32_t)felucca_dbg.boots);
    con_kv("usb_resets", (int32_t)usb.resets);
    con_kv("usb_sof", (int32_t)usb.sof_seen);
    con_kv("usb_suspends", (int32_t)usb.suspends);
    con_kv("usb_retries", (int32_t)usb.retries);
    con_kv("usb_frame", (int32_t)usb.frame);
    con_kv("usb_frame_stalls", (int32_t)usb.frame_stalls);
    con_kv("usb_max_gap_polls", (int32_t)usb.max_gap);
    con_kv("midi_rx_pkts", (int32_t)usb.rx_pkts);
    con_kv("midi_tx_pkts", (int32_t)usb.tx_pkts);
#if FELUCCA_UAC
    con_uac();
#endif
    con_kv("uart_enabled", FELUCCA_UART);
#if FELUCCA_UART
    con_kv("uart_rx_bytes", (int32_t)um.bytes);
    con_kv("uart_rx_msgs", (int32_t)um.msgs);
    con_kv("uart_rx_drops", (int32_t)um.drops);
#endif
#if FELUCCA_FLASH
    con_kv("flash", flash_ok);
#endif
}

static void con_dbg(void)
{
    const uint32_t *w = (const uint32_t *)&felucca_dbg;
    static const char *const NAMES[] = {"magic", "halves", "max_us", "nested", "in_audio", "late",
                                        "timer_irqs", "ui_frames", "last_us", "cpu_q8", "boots",
                                        "stage", "page", "home", "prev_stage", "prev_page",
                                        "prev_home", "prev_rst", "prev_frames"};
    uint32_t i;
    for (i = 0; i < sizeof NAMES / sizeof NAMES[0] && i < sizeof felucca_dbg / 4u; i++)
        con_kx(NAMES[i], w[i]);
#if FELUCCA_UAC
    con_uac();
#endif
}

/* input scan (hal/fm1_input.h fm1_in_stat) since the last `inp`, then cleared: tick gaps (= LED
 * on-time of a column), per-column LED share in 1/1000, the tick cost, the dim pulse (its average width and the
 * bits of the 595 shift it spans), encoder transitions */
static void con_inp(void)
{
#if BOARD_SMK37
    con_kv("timer_irqs", (int32_t)felucca_dbg.timer_irqs);      /* (SMK-37 Pro: no scan statistics; `smk`) */
    con_kv("nested", (int32_t)felucca_dbg.nested);
#else
    static uint32_t t_last;
    uint32_t now = fm1_ticks(), win = now - t_last, i, k;
    uint32_t win_us = win / FM1_TICKS_PER_US;
    t_last = now;
    con_kv("window_ms", (int32_t)(win_us / 1000u));
    con_kv("ticks_per_s", (int32_t)(win_us >= 1000u ? fm1_in_stat.ticks * 1000u / (win_us / 1000u) : 0u));
    con_kv("gap_max_us", (int32_t)(fm1_in_stat.gap_max / FM1_TICKS_PER_US));
    con_puts("gap_hist <.15 .3 .6 1.2 2.4 4.8 9.6 more ms:");
    for (i = 0; i < FM1_GAP_BINS; i++) {
        con_putc(' ');
        con_dec((int32_t)fm1_in_stat.gap_hist[i]);
    }
    con_puts("\r\n");
    con_kv("tick_cost_avg_ns", (int32_t)(fm1_in_stat.ticks ? fm1_in_stat.cost_sum / fm1_in_stat.ticks * 1000u / FM1_TICKS_PER_US : 0u));
    con_kv("tick_cost_max_us", (int32_t)(fm1_in_stat.cost_max / FM1_TICKS_PER_US));
    con_kv("tick_cpu_permille", (int32_t)(win >= 1000u ? fm1_in_stat.cost_sum / (win / 1000u) : 0u));
    con_kv("dim_pulse_ns", (int32_t)(fm1_in_stat.dim_n ? fm1_in_stat.dim_sum / fm1_in_stat.dim_n * 1000u / FM1_TICKS_PER_US : 0u));
    con_kv("dim_bits", (int32_t)fm1__dim_k);
    con_puts("led_on_permille:");
    for (i = 0; i < FM1_NCOL; i++) {
        con_putc(' ');
        con_dec((int32_t)(win >= 1000u ? fm1_in_stat.on[i] / (win / 1000u) : 0u));
    }
    con_puts("\r\nled_on_max_us:");
    for (i = 0; i < FM1_NCOL; i++) {
        con_putc(' ');
        con_dec((int32_t)(fm1_in_stat.on_max[i] / FM1_TICKS_PER_US));
    }
    con_puts("\r\nenc moves/lost/detent state:");
    for (k = 0; k < FM1_NENC; k++) {
        con_putc(' ');
        con_dec((int32_t)fm1_in_stat.enc_moves[k]);
        con_putc('/');
        con_dec((int32_t)fm1_in_stat.enc_lost[k]);
        con_putc('/');
        con_dec(fm1_in.enc_rest[k]);
    }
    con_puts("\r\n");
    {   /* note keys, from the first scan that saw the contact (us): debounced press, note-on, DAC out */
        uint32_t n = fm1_in_stat.press_n, m = fm1_in_stat.kb_n;
        con_kv("key_presses", (int32_t)n);
        con_kv("key_press_avg_us", (int32_t)(n ? fm1_in_stat.press_sum / n / FM1_TICKS_PER_US : 0u));
        con_kv("key_press_max_us", (int32_t)(fm1_in_stat.press_max / FM1_TICKS_PER_US));
        con_kv("key_notes", (int32_t)m);
        con_kv("key_noteon_avg_us", (int32_t)(m ? fm1_in_stat.kb_sum / m / FM1_TICKS_PER_US : 0u));
        con_kv("key_noteon_max_us", (int32_t)(fm1_in_stat.kb_max / FM1_TICKS_PER_US));
        con_kv("key_dac_avg_us", (int32_t)(m ? fm1_in_stat.dac_sum / m / FM1_TICKS_PER_US : 0u));
        con_kv("key_dac_max_us", (int32_t)(fm1_in_stat.dac_max / FM1_TICKS_PER_US));
    }
    con_kv("timer_irqs", (int32_t)felucca_dbg.timer_irqs);
    con_kv("nested", (int32_t)felucca_dbg.nested);
    {
        uint32_t k2 = fm1__lock();
        memset((void *)&fm1_in_stat.gap_max, 0, sizeof fm1_in_stat - sizeof fm1_in_stat.last);
        fm1__unlock(k2);
    }
#endif
}

static void con_crash(void)
{
    if (fm1_crash.magic != FM1_CRASH_MAGIC) {
        con_puts("no crash record\r\n");
        return;
    }
    con_kv("count", (int32_t)fm1_crash.count);
    con_kx("vec", fm1_crash.vec);
    con_kx("pc", fm1_crash.pc);
    con_kx("rets", fm1_crash.rets);
    con_kx("emu", fm1_crash.emu);
    con_kx("sp", fm1_crash.sp);
    con_kx("psr", fm1_crash.psr);
    con_kv("uptime_ms", (int32_t)fm1_crash.uptime_ms);
    con_kv("early", (int32_t)fm1_crash.early);
}

static void con_params(void)
{
    uint32_t i;
    for (i = 0; i < P_COUNT; i++) {
        con_dec((int32_t)i);
        con_putc('=');
        con_dec(TSEL->p[i]);
        con_puts(i % 8u == 7u || i + 1u == P_COUNT ? "\r\n" : " ");
    }
}

#if BOARD_SMK37
static void con_smk(void)                               /* the SMK-37 Pro inputs (smk_input.h) */
{
    uint32_t i;
    con_kv("adc_ok", (int32_t)fm1_in.adc_ok);
    con_kv("adc_miss", (int32_t)fm1_in.adc_miss);
    con_kv("frames", (int32_t)fm1_in.frames);
    con_puts("pad raw ");
    for (i = 0; i < SMK_NPAD; i++) { con_dec(fm1_in.pad_raw[i]); con_putc(' '); }
    con_puts("\r\npad base ");
    for (i = 0; i < SMK_NPAD; i++) { con_dec(fm1_in.pad_base[i]); con_putc(' '); }
    con_puts("\r\npad st ");
    for (i = 0; i < SMK_NPAD; i++) { con_dec(fm1_in.pad_st[i]); con_putc(' '); }
    con_puts("\r\nfader raw ");
    for (i = 0; i < 4u; i++) { con_dec(fm1_in.fad_raw[i]); con_putc(' '); }
    con_puts("\r\nfader ");
    for (i = 0; i < 4u; i++) { con_dec(fm1_in.fader[i]); con_putc(' '); }
    con_puts("\r\n");
    con_kv("wheel0", fm1_in.wheel[0]);
    con_kv("wheel1", fm1_in.wheel[1]);
    con_kv("bend", smk_bend14);
    con_kv("modw", smk_modw);
    con_kv("pedal", fm1_in.pedal);
    con_kv("batt", fm1_in.batt);
    con_kx("buttons", fm1_in.buttons);
    con_kx("notes", fm1_in.notes);
    for (i = 0; i < FM1_NENC; i++) {
        con_puts("enc");
        con_dec((int32_t)i);
        con_putc('=');
        con_dec(fm1_in.enc_steps[i]);
        con_putc(i + 1u == FM1_NENC ? '\r' : ' ');
    }
    con_puts("\r\n");
}

/* what the firmware sends the pads: GRB words by hardware pad (0..7 top row, 8..15 bottom row) */
static void con_leds(void)
{
    uint32_t i;
    for (i = 0; i < SMK_NPAD; i++) {
        con_puts("pad");
        con_dec((int32_t)i);
        con_puts(" grb ");
        con_hex(padled_rgb[i], 6);
        con_puts(i % 4u == 3u ? "\r\n" : "  ");
    }
}

/* the pad LEDs' colour order: hardware pad 8 (bottom left) red, 9 green, 10 blue, 11 white, for 20 s */
static void con_padtest(void)
{
    uint32_t i;
    for (i = 0; i < SMK_NPAD; i++)
        padled_set(i, 0, 0, 0);
    padled_set(8, 40, 0, 0);
    padled_set(9, 0, 40, 0);
    padled_set(10, 0, 0, 40);
    padled_set(11, 40, 40, 40);
    padled_hold_until = fm1_ms + 20000u;
    con_puts("top row from the left: red green blue white\r\n");
}
#endif

#if FELUCCA_USBHOST
static void con_logc(char c)
{
    if (c == '\n')
        con_putc('\r');
    con_putc(c);
}

/* USB host mode (usbhost.c): what the last session found, and its raw descriptors */
static void con_hoststate(void)
{
    char (*ln)[UH_LW] = uh_ln;
    uint32_t n = uh_hub_lines(ln, sizeof uh_ln / sizeof uh_ln[0]), i;
    n += uh_lines(ln + n, sizeof uh_ln / sizeof uh_ln[0] - n);
    con_puts("state ");
    con_puts(UH_STATE[uh.st]);
    con_puts("\r\nresult ");
    con_puts(uh.result);
    con_puts("\r\nlast ");
    con_puts(uh.msg);
    con_puts("\r\n");
    con_puts("usb_mic ");                              /* MENU > USB MIC, what it found */
    con_puts(UHM_NAME[VOX_MODE() % 3u]);
    con_putc(' ');
    con_puts(uh_status_text());
    con_puts("\r\n");
    con_kv("auto_probes", (int32_t)ua.probes);
    con_kv("sessions", (int32_t)uh.sessions);
    con_kv("transfers", (int32_t)uh.xfers);
    con_kv("transfer_errors", (int32_t)uh.xfer_errs);
    con_kv("bus_resets", (int32_t)uh.resets);
    con_kv("session_restarts", (int32_t)uh.pokes);
    con_kv("sie_timeouts", (int32_t)usbh_sie_ot);
    if (uh.s_ok) {                                      /* the stream (milestone 2) */
        con_kv("stream_rate", (int32_t)uh.s_rate);
        con_kv("stream_ch", uh.s_ch);
        con_kv("stream_bits", uh.s_bits);
        con_kx("stream_ep", uh.s_ep);
        con_kv("stream_maxp", uh.s_maxp);
        con_kv("stream_packets", (int32_t)uhsv.pkts);
        con_kv("stream_pkts_per_s", (int32_t)uh.pkts_sec);
        con_kv("stream_missed_frames", (int32_t)uhsv.missed);
        con_kv("stream_errors", (int32_t)uhsv.errs);
        con_kv("stream_crc_errors", (int32_t)uhsv.dataerr);
        con_kv("stream_rearms", (int32_t)uhsv.rearms);
        con_kv("stream_zero_len", (int32_t)uhsv.zero);
        con_kv("stream_odd_len", (int32_t)uhsv.odd);
        con_kv("stream_frames", (int32_t)uhs.frames);
        con_kv("stream_fill", (int32_t)(uhs.w - uhs.r));
        con_kv("stream_underruns", (int32_t)uhs.underruns);
        con_kv("stream_overruns", (int32_t)uhs.overruns);
        con_kv("stream_primes", (int32_t)uhs.primes);
        con_kv("stream_limited", (int32_t)uhs.limited);
        con_kv("stream_step_q16", (int32_t)uhs.step);
        con_kv("stream_lr_differ_pkts", (int32_t)uhs.lrdiff);   /* 0: the device sends L = R (a mono source) */
        con_kv("stream_silence_frames", (int32_t)uhs.silent);
        con_kx("stream_devctl", uhsv.dc);
        con_kx("stream_intrusb_seen", uhsv.iu);
        con_kv("stream_device_bit_drop_ms", uhsv.t_drop ? (int32_t)(uhsv.t_drop - uh.t_stream) : -1);
        con_kv("stream_session_kicks", (int32_t)uhsv.kicks);
        con_kv("stream_volume_cur_db256", uh.s_vol);
        con_kv("cpu_pct", (int32_t)VOX_CPU_PCT());
        {
            uint32_t k;
            con_puts("vocoder");
            for (k = 0; k < VCP_N; k++) {
                con_putc(' ');
                con_puts(VCP_NAME[k]);
                con_putc('=');
                con_dec(vc.p[k]);
            }
            con_puts(" OCT=");
            con_dec(vc.oct);
            con_puts("\r\n");
        }
    }
    if (uh_dev.dev_ok) {
        con_puts("vid:pid ");
        con_hex(uh_dev.vid, 4);
        con_putc(':');
        con_hex(uh_dev.pid, 4);
        con_puts(uh.speed == 1u ? " low speed\r\n" : " full speed\r\n");
        con_puts("manufacturer ");
        con_puts(uh_dev.man);
        con_puts("\r\nproduct ");
        con_puts(uh_dev.prod);
        con_puts("\r\nserial ");
        con_puts(uh_dev.ser);
        con_puts("\r\n");
    }
    for (i = 0; i < n; i++) {
        con_puts(ln[i]);
        con_puts("\r\n");
    }
    con_puts("device descriptor:");
    for (i = 0; i < (uh_dev.dev_ok ? 18u : 0u); i++) {
        con_putc(' ');
        con_hex(uh_devd[i], 2);
    }
    con_puts("\r\nconfiguration (");
    con_dec(uh_cfg_n);
    con_puts(" bytes):");
    for (i = 0; i < uh_cfg_n; i++) {
        con_puts(i % 16u ? " " : "\r\n ");
        con_hex(uh_cfgd[i], 2);
    }
    con_puts("\r\n");
}
#endif

static void con_exec(const char *p)
{
    if (con_word(&p, "help") || con_word(&p, "?")) {
        con_puts("status  dbg  inp  crash  params  memr ADDR [LEN]  flr OFF [LEN]  uboot yes\r\n");
#if FELUCCA_USBHOST
        con_puts("host  hoststate  logdump  logclear  hostopt [N]  vocdump   (USB host mode: < + > held 3 s on / off)\r\n");
#endif
    } else if (con_word(&p, "status"))
        con_status();
    else if (con_word(&p, "dbg"))
        con_dbg();
    else if (con_word(&p, "crash"))
        con_crash();
    else if (con_word(&p, "inp"))
        con_inp();
    else if (con_word(&p, "params"))
        con_params();
#if BOARD_SMK37
    else if (con_word(&p, "smk"))
        con_smk();
    else if (con_word(&p, "padtest"))
        con_padtest();
    else if (con_word(&p, "leds"))
        con_leds();
#endif
#if FELUCCA_USBHOST
    else if (con_word(&p, "host")) {
        con_puts("USB host mode: the console goes now. Unplug the computer, plug the device in.\r\n");
        con_puts("Back to device mode on any button but < >, after 60 s without a device, after 120 s.\r\n");
        con_puts("Then 'logdump' and 'hoststate'.\r\n");
        uh.req = 1;                                    /* main loop (usbhost_service), after this reply */
    } else if (con_word(&p, "hoststate"))
        con_hoststate();
    else if (con_word(&p, "logdump"))
        uh_log_each(con_logc);
    else if (con_word(&p, "vocdump")) {                /* the vocoder's input and bands since the last vocdump */
        uint32_t b;
        con_kv("input_peak", vc.inpk);                  /* (as the device sends it: 32767 = full scale) */
        con_kv("input_clips", (int32_t)vc.clips);
        con_kv("after_gain_comp_peak", vc.cpk);
        con_kv("comp_gain_q12", vc.gc);
        con_kv("input_gain_q8", vc.ig);
        uh_cpu_take();
        con_kv("cpu_pct_audio_isr", (int32_t)uh_cpu[0]);   /* (over the last second; the page refreshes them too) */
        con_kv("cpu_pct_vocoder", (int32_t)uh_cpu[1]);
        con_kv("cpu_pct_usb_iso", (int32_t)uh_cpu[2]);
        con_kv("cpu_pct_engines", (int32_t)uh_cpu[3]);
        con_kv("unvoiced_pct", vc.blocks ? (int32_t)(vc.uvblocks * 100u / vc.blocks) : 0);
        vc.uvblocks = vc.blocks = 0;
        con_puts("band_peaks_db");
        for (b = 0; b < VC_NB; b++) {
            con_putc(' ');
            con_dec(uhs_db((uint32_t)(vc.envpk[b] > 32767 ? 32767 : vc.envpk[b])));
            vc.envpk[b] = 0;
        }
        con_puts("\r\n");
        vc.inpk = vc.cpk = 0;
        vc.clips = 0;
    } else if (con_word(&p, "logclear")) {
        uh_log_clear();
        con_puts("log cleared\r\n");
    } else if (con_word(&p, "hostopt")) {
        int ok;
        uint32_t v = con_num(&p, &ok), k;
        if (ok)
            uh.opt = (uint8_t)(v & 0x7Fu);
        con_kv("hostopt", uh.opt);
        for (k = 0; k < sizeof UHO_NAME / sizeof UHO_NAME[0]; k++) {
            con_puts(uh.opt & 1u << k ? "  [x] " : "  [ ] ");
            con_dec((int32_t)(1u << k));
            con_putc(' ');
            con_puts(UHO_NAME[k]);
            con_puts("\r\n");
        }
    }
#endif
    else if (con_word(&p, "memr"))
        con_memr(p);
#if FELUCCA_FLASH
    else if (con_word(&p, "flr"))
        con_flr(p);
#endif
    else if (con_word(&p, "uboot")) {
        if (con_word(&p, "yes")) {
            con_puts("entering UBOOT\r\n");
            usb.uboot_req = 1;                         /* main loop: same path as the SysEx key */
        } else {
            con_puts("type 'uboot yes'\r\n");
        }
    } else if (*p)
        con_puts("? (help)\r\n");
}

static void cdc_task(void)                              /* main loop */
{
    if (cdc.dtr && !con.dtr_seen) {
        con_puts("\r\nFelucca ");
        con_puts(FELUCCA_VERSION);
        con_puts(" console - 'help'\r\n> ");
    }
    con.dtr_seen = cdc.dtr;
    while (ci_r != ci_w) {
        char c;
        RING_PUBLISH();                                /* usb_poll (producer) can preempt us */
        c = (char)cdc_in[ci_r % CI_N];
        RING_PUBLISH();
        ci_r++;
        if (c == '\r' || c == '\n') {
            if (c == '\n' && con.len == 0)
                continue;                              /* the LF of a CR LF */
            con_puts("\r\n");
            con.line[con.len] = 0;
            con.stalled = 0;
            con_exec(con.line);
            con.len = 0;
            con_puts("> ");
        } else if (c == 0x7F || c == 0x08) {
            if (con.len) {
                con.len--;
                con_puts("\b \b");
            }
        } else if (c >= ' ' && c < 0x7F && con.len + 1u < CON_LINE) {
            con.line[con.len++] = c;
            con_putc(c);
        }
    }
}
