/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (the line editor and its output, from
 * console.c)
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37's serial console on the CDC-ACM function (a computer on the port): diagnostics of the sound and the panel,
 * and SAY: a word spoken by its name. Runs in the main loop (cdc_task). */
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

/* ------------------------------------------------------------- Speak37 --- */
static void con_status(void)
{
    uint32_t i;
    con_puts("voice ");
    con_puts(SP_VOICE_NAME[sp.snd.voice % SP_NVOICE]);
    con_puts("\r\nbank ");
    con_puts(SP_BANK_NAME[sp.sent / 16u % SP_NBANK]);
    con_puts("\r\nsentence ");
    con_puts(sp_sent_text(sp.sent));
    con_puts("\r\n");
    con_kv("pad", (int32_t)(sp.sent % 16u) + 1);
    con_kv("word", sp.wlast + 1);
    con_kv("keys_mode", sp.mode);                         /* 0 SENTENCE, 1 LETTERS, 2 KIT, 3 MOPE */
    con_kv("mope_gloom", mo.gloom);                       /* (MOPE: its mood 0..120, its state, lines said) */
    con_kv("mope_state", mo.state);
    con_kv("mope_lines", mo.lines);
    con_kv("speaking_mask", (int32_t)sp.speaking);
    con_kv("voices_max", sp.vmax);
    con_kv("drum_lanes", rh_lmax);
    con_kv("words_started", (int32_t)sp.tag_words);
    con_kv("master_pot", (int32_t)sp_pot);
    con_kv("cpu_pct", (int32_t)(sp_cpu_q8 * 100u / 256u));
    con_kv("cpu_max_us", (int32_t)audio_max_us);
    con_kv("audio_late", (int32_t)sp_dbg.late);
    con_kv("out_peak", sp.peak);
    con_kv("screen_fps", (int32_t)su.fps);
    con_kv("store_fail", sp_store_fail);
    con_kv("vocabulary_words", (int32_t)SP_NWORD);
    con_kv("lpc_bytes", (int32_t)SP_LPC_BYTES);
    for (i = 0; i < SK_N; i++) {
        char t[24];
        con_puts(SK_NAME[i]);
        con_putc(' ');
        sk_text(&sp.snd, i, t);
        con_puts(t);
        con_puts(i + 1u == SK_N ? "\r\n" : "  ");
    }
    sp.peak = 0;
    audio_max_us = 0;
}

/* SAY WORD: the word by its name, at ROOT (a test of the vocabulary from a computer) */
static void con_say(const char *p)
{
    uint32_t i, n;
    char w[24];
    while (*p == ' ')
        p++;
    for (n = 0; p[n] && p[n] != ' ' && n + 1u < sizeof w; n++)
        w[n] = (char)(p[n] >= 'a' && p[n] <= 'z' ? p[n] - 32 : p[n]);
    w[n] = 0;
    for (i = 0; i < SP_NWORD; i++)
        if (str_eq(SP_WNAME[i], w)) {
            uint32_t f = fm1__lock();
            sp_speak(i, (int32_t)sp.snd.root * 64, 100, SP_TAG_AUTO);
            sp_release(SP_TAG_AUTO);
            fm1__unlock(f);
            con_puts("said ");
            con_puts(w);
            con_puts("\r\n");
            return;
        }
    con_puts("not in the vocabulary\r\n");
}

#if BOARD_SMK37
static void con_smk(void)
{
    uint32_t i;
    con_puts("pad st ");
    for (i = 0; i < SMK_NPAD; i++) { con_dec(fm1_in.pad_st[i]); con_putc(' '); }
    con_puts("\r\nfader ");
    for (i = 0; i < 4u; i++) { con_dec(fm1_in.fader[i]); con_putc(' '); }
    con_puts("\r\n");
    con_kv("wheel0", fm1_in.wheel[0]);
    con_kv("wheel1", fm1_in.wheel[1]);
    con_kv("bend", sp.bend);
    con_kv("modw", sp.modw);
    con_kx("buttons", fm1_in.buttons);
    for (i = 0; i < FM1_NENC; i++) {
        con_puts("enc");
        con_dec((int32_t)i);
        con_putc('=');
        con_dec(fm1_in.enc_steps[i]);
        con_putc(' ');
    }
    con_puts("\r\n");
}

static void con_padtest(void)
{
    uint32_t i;
    for (i = 0; i < SMK_NPAD; i++)
        padled_set(i, 0, 0, 0);
    padled_set(0, 40, 0, 0);
    padled_set(1, 0, 40, 0);
    padled_set(2, 0, 0, 40);
    padled_set(3, 40, 40, 40);
    padled_hold_until = fm1_ms + 20000u;
    con_puts("top row from the left: red green blue white (20 s)\r\n");
}
#endif

/* MOPE's insides (on the device: why it says nothing) */
static void con_mope(void)
{
    con_kv("on", mo.on);
    con_kv("state", mo.state);                           /* 0 LISTEN, 1 ANNOUNCE, 2 SULK */
    con_kv("activity", mo.act);                          /* 0 none, 1 LIGHTS, 2 DARK, 3 ASK, 4 GAME */
    con_kv("activity_phase", mo.aphase);
    con_kv("gloom", mo.gloom);
    con_kv("keys_held", mo.nheld);
    con_kv("phrase_notes", mo.np);
    con_kv("phrase_pending", mo.pend);
    con_kv("control_pending", mo.ctl);
    con_kv("words_busy", (int32_t)mo_busy());
    con_kv("reply_playing", mo.reply_on);
    con_kv("piece_playing", mos.seq_on);
    con_kv("lines", mo.lines);
    con_kv("pose", mo.pose);
    con_kv("ms_since_key", (int32_t)(sp.now / 44u - mo.t_last));
    con_kv("ms_since_line_end", (int32_t)(sp.now / 44u - mo.t_quiet));
}

static void con_exec(const char *p)
{
    if (con_word(&p, "help") || con_word(&p, "?")) {
        con_puts("status  mope  say WORD  smk  padtest  uboot yes\r\n");
    } else if (con_word(&p, "status"))
        con_status();
    else if (con_word(&p, "mope"))
        con_mope();
    else if (con_word(&p, "say"))
        con_say(p);
#if BOARD_SMK37
    else if (con_word(&p, "smk"))
        con_smk();
    else if (con_word(&p, "padtest"))
        con_padtest();
#endif
    else if (con_word(&p, "uboot")) {
        if (con_word(&p, "yes")) {
            con_puts("entering UBOOT\r\n");
            usb.uboot_req = 1;
        } else {
            con_puts("type 'uboot yes'\r\n");
        }
    } else if (*p)
        con_puts("? (help)\r\n");
}

static void cdc_task(void)                              /* main loop */
{
    if (cdc.dtr && !con.dtr_seen) {
        con_puts("\r\nSpeak37 ");
        con_puts(SP_VERSION);
        con_puts(" console - 'help'\r\n> ");
    }
    con.dtr_seen = cdc.dtr;
    while (ci_r != ci_w) {
        char c;
        RING_PUBLISH();
        c = (char)cdc_in[ci_r % CI_N];
        RING_PUBLISH();
        ci_r++;
        if (c == '\r' || c == '\n') {
            if (c == '\n' && con.len == 0)
                continue;
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
