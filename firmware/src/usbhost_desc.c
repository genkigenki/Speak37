/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (makkuro) */
/* USB host, the part without hardware (host tested: tests/usbhost_test.c):
 *   the RAM log    text lines, for `logdump`: the first UH_LOG_HEAD bytes of a session kept apart, the rest in
 *                  a ring (the newest UH_LOG_N bytes); in .pool (NOINIT has no room, a reset clears it)
 *   uh_parse_dev   the device descriptor -> uh_dev
 *   uh_parse_cfg   the configuration descriptor set -> uh_dev's interfaces / endpoints / audio formats,
 *                  every descriptor and field into the log
 *   uh_str         a string descriptor (UTF-16LE) -> ASCII
 *   uh_lines       the summary as short text lines (the screen, `hoststate`)
 * The hardware side (enumeration, screen, console) is usbhost.c. */
#ifndef UH_POOL
#define UH_POOL __attribute__((section(".pool")))
#endif
#define UH_LOG_N 6144u                     /* bytes of log kept: the ring */
#define UH_LOG_HEAD 2048u                  /* bytes of log kept: a session's start */
#define UH_LOG_MAGIC 0x55484C47u           /* "UHLG" */
#define UH_NIF 16u                         /* interface alt settings kept */
#define UH_NEP 24u                         /* endpoints kept */
#define UH_NRATE 6u                        /* sample rates kept per alt setting */
#define UH_SLEN 32u                        /* string descriptors: characters kept + 1 */
#define UH_LW 44u                          /* a summary line: characters + 1 */

static struct {
    uint32_t magic, w;                     /* w: bytes ever written (the ring index is w % UH_LOG_N) */
    char b[UH_LOG_N];
} uh_log_ring UH_POOL;

static struct {
    uint32_t n, w_end;                     /* bytes held; the ring's w after the last line held here */
    char b[UH_LOG_HEAD];
} uh_log_head UH_POOL;

typedef struct {
    uint8_t addr, attr, interval, itf;     /* itf: index into uh_dev.itf */
    uint16_t maxp;                         /* wMaxPacketSize (bits 11..12: extra transactions, high speed) */
} uh_ep_t;

typedef struct {
    uint8_t num, alt, cls, sub, proto, neps;
    uint8_t fmt, ch, sub_bytes, bits;      /* audio streaming: format type (1 = type I), channels, bytes / sample, bits */
    uint8_t nrates;                        /* discrete rates in rate[]; 0xFF: continuous, rate[0] .. rate[1] */
    uint8_t link;                          /* AS_GENERAL bTerminalLink */
    uint16_t tag;                          /* UAC1 wFormatTag (1 = PCM); UAC2: bmFormats' low bits */
    uint32_t rate[UH_NRATE];
} uh_if_t;

typedef struct {
    uint16_t vid, pid, bcd_usb, bcd_dev, total;
    uint8_t cls, sub, proto, mps0, i_man, i_prod, i_ser, ncfg;
    uint8_t cfg_val, cfg_attr, cfg_nif, max_ma2;   /* max_ma2: bMaxPower, 2 mA units */
    uint8_t nif, nep, lost;                 /* lost: interfaces / endpoints / rates beyond the tables */
    uint8_t dev_ok, cfg_ok;
    uint8_t ac_if, fu_id;                   /* audio: the control interface, its first feature unit (0 none) */
    uh_if_t itf[UH_NIF];
    uh_ep_t ep[UH_NEP];
    char man[UH_SLEN], prod[UH_SLEN], ser[UH_SLEN];
} uh_dev_t;

static uh_dev_t uh_dev;

/* ------------------------------------------------------------- the log --- */
static void uh_log_reset_if_bad(void)
{
    if (uh_log_ring.magic != UH_LOG_MAGIC) {
        uh_log_ring.magic = UH_LOG_MAGIC;
        uh_log_ring.w = 0;
        uh_log_head.n = uh_log_head.w_end = 0;
    }
}

/* a session starts: its first lines are kept from here (uh_log_head) */
static void uh_log_session(void)
{
    uh_log_reset_if_bad();
    uh_log_head.n = 0;
    uh_log_head.w_end = uh_log_ring.w;
}

static void uh_log_clear(void) { uh_log_ring.magic = 0; uh_log_reset_if_bad(); }

static void uh_log_c(char c)
{
    uh_log_ring.b[uh_log_ring.w % UH_LOG_N] = c;
    uh_log_ring.w++;
}

/* one line (no newline in s) */
static void uh_log(const char *s)
{
    uint32_t n = 0, i;
    uh_log_reset_if_bad();
    while (s[n])
        uh_log_c(s[n++]);
    uh_log_c('\n');
    if (uh_log_head.n + n + 1u <= UH_LOG_HEAD && uh_log_head.w_end + n + 1u == uh_log_ring.w) {
        for (i = 0; i < n; i++)                         /* (whole lines, only while nothing was left out) */
            uh_log_head.b[uh_log_head.n++] = s[i];
        uh_log_head.b[uh_log_head.n++] = '\n';
        uh_log_head.w_end = uh_log_ring.w;
    }
}

/* the log oldest first: the session's start (uh_log_head), then the ring after it, whole lines (a gap between the
 * two is marked; a wrapped ring drops its first, cut line) */
static void uh_log_each(void (*put)(char))
{
    uint32_t w, i, k;
    uh_log_reset_if_bad();
    w = uh_log_ring.w;
    for (k = 0; k < uh_log_head.n; k++)
        put(uh_log_head.b[k]);
    i = uh_log_head.n ? uh_log_head.w_end : 0u;
    if (w - i > UH_LOG_N) {
        const char *m = "... (the middle of the log is lost) ...\n";
        i = w - UH_LOG_N;
        while (i < w && uh_log_ring.b[i++ % UH_LOG_N] != '\n')
            ;
        while (*m)
            put(*m++);
    }
    for (; i < w; i++)
        put(uh_log_ring.b[i % UH_LOG_N]);
}

/* the newest max lines of the log, oldest first, each cut to UH_LW - 1 characters and without its "[time] "
 * (the screen's history); returns the count */
static uint32_t uh_log_tail(char (*ln)[UH_LW], uint32_t max)
{
    uint32_t st[32], en[32], n = 0, w, start, end, j, k, i;
    uh_log_reset_if_bad();
    w = uh_log_ring.w;
    start = w > UH_LOG_N ? w - UH_LOG_N : 0u;
    if (max > 32u)
        max = 32u;
    end = w ? w - 1u : 0u;                              /* the last line's newline */
    while (n < max && end > start) {
        j = end;
        while (j > start && uh_log_ring.b[(j - 1u) % UH_LOG_N] != '\n')
            j--;
        if (j == start && start)                        /* cut by the ring's wrap */
            break;
        st[n] = j;
        en[n++] = end;
        end = j ? j - 1u : 0u;
        if (!j)
            break;
    }
    for (k = 0; k < n; k++) {
        uint32_t a = st[n - 1u - k], b = en[n - 1u - k];
        if (uh_log_ring.b[a % UH_LOG_N] == '[')         /* "[12.345] " off */
            for (i = a; i < b && i < a + 16u; i++)
                if (uh_log_ring.b[i % UH_LOG_N] == ']') {
                    a = i + 2u <= b ? i + 2u : b;
                    break;
                }
        for (i = 0; a + i < b && i + 1u < UH_LW; i++)
            ln[k][i] = uh_log_ring.b[(a + i) % UH_LOG_N];
        ln[k][i] = 0;
    }
    return n;
}

/* ------------------------------------------------- text, no printf --- */
static char *uh_s(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}

static char *uh_x(char *d, uint32_t v, uint32_t digits)
{
    while (digits--)
        *d++ = "0123456789ABCDEF"[(v >> (digits * 4u)) & 15u];
    *d = 0;
    return d;
}

static char *uh_d(char *d, uint32_t v)
{
    char b[10];
    uint32_t n = 0;
    do
        b[n++] = (char)('0' + v % 10u);
    while ((v /= 10u) != 0 && n < sizeof b);
    while (n)
        *d++ = b[--n];
    *d = 0;
    return d;
}

static char *uh_bcd(char *d, uint32_t v)                 /* 0x0110 -> "1.10" */
{
    d = uh_d(d, (v >> 12 & 15u) * 10u + (v >> 8 & 15u));
    *d++ = '.';
    return uh_x(d, v & 0xFFu, 2);
}

static uint32_t uh_le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t uh_le24(const uint8_t *p) { return uh_le16(p) | (uint32_t)p[2] << 16; }

static const char *uh_class_name(uint32_t c, uint32_t s)
{
    switch (c) {
    case 0x01: return s == 1u ? "AUDIO CTRL" : s == 2u ? "AUDIO STREAM" : s == 3u ? "MIDI" : "AUDIO";
    case 0x02: return "CDC";
    case 0x03: return "HID";
    case 0x08: return "MASS STORAGE";
    case 0x09: return "HUB";
    case 0x0A: return "CDC DATA";
    case 0x0E: return "VIDEO";
    case 0xFE: return "APP SPECIFIC";
    case 0xFF: return "VENDOR";
    default: return "?";
    }
}

static const char *uh_ep_type(uint32_t attr)
{
    static const char *const T[4] = {"CTRL", "ISO", "BULK", "INT"};
    return T[attr & 3u];
}

static const char *uh_term_name(uint32_t t)
{
    switch (t) {
    case 0x0101: return "USB STREAM";
    case 0x0201: return "MICROPHONE";
    case 0x0202: return "DESKTOP MIC";
    case 0x0203: return "PERSONAL MIC";
    case 0x0204: return "OMNI MIC";
    case 0x0205: return "MIC ARRAY";
    case 0x0301: return "SPEAKER";
    case 0x0302: return "HEADPHONES";
    case 0x0402: return "HEADSET";
    case 0x0603: return "LINE";
    default: return "";
    }
}

/* ------------------------------------------------------ descriptors --- */
/* a string descriptor (UTF-16LE) -> ASCII, '?' for the rest; the length its own bLength (at most n) */
static void uh_str(char *dst, uint32_t cap, const uint8_t *d, uint32_t n)
{
    uint32_t i, k = 0, len = n >= 2u && d[1] == 3u ? (d[0] < n ? d[0] : n) : 0u;
    for (i = 2; i + 1u < len && k + 1u < cap; i += 2) {
        uint32_t c = uh_le16(d + i);
        dst[k++] = (char)(c >= 0x20u && c < 0x7Fu ? c : '?');
    }
    dst[k] = 0;
}

/* the device descriptor (18 bytes, or the first 8 of it: then only the class and bMaxPacketSize0) */
static int uh_parse_dev(const uint8_t *d, uint32_t n)
{
    char l[80], *p;
    if (n < 8u || d[1] != 1u || d[0] < 18u) {
        uh_log("device descriptor: bad");
        return -1;
    }
    uh_dev.bcd_usb = (uint16_t)uh_le16(d + 2);
    uh_dev.cls = d[4];
    uh_dev.sub = d[5];
    uh_dev.proto = d[6];
    uh_dev.mps0 = d[7];
    p = uh_s(l, "dev: usb ");
    p = uh_bcd(p, uh_dev.bcd_usb);
    p = uh_s(p, " class ");
    p = uh_x(p, d[4], 2);
    p = uh_s(p, "/");
    p = uh_x(p, d[5], 2);
    p = uh_s(p, "/");
    p = uh_x(p, d[6], 2);
    p = uh_s(p, " mps0 ");
    uh_d(p, d[7]);
    uh_log(l);
    if (n < 18u)
        return 0;
    uh_dev.vid = (uint16_t)uh_le16(d + 8);
    uh_dev.pid = (uint16_t)uh_le16(d + 10);
    uh_dev.bcd_dev = (uint16_t)uh_le16(d + 12);
    uh_dev.i_man = d[14];
    uh_dev.i_prod = d[15];
    uh_dev.i_ser = d[16];
    uh_dev.ncfg = d[17];
    uh_dev.dev_ok = 1;
    p = uh_s(l, "dev: vid ");
    p = uh_x(p, uh_dev.vid, 4);
    p = uh_s(p, " pid ");
    p = uh_x(p, uh_dev.pid, 4);
    p = uh_s(p, " rel ");
    p = uh_bcd(p, uh_dev.bcd_dev);
    p = uh_s(p, " str ");
    p = uh_d(p, d[14]);
    p = uh_s(p, "/");
    p = uh_d(p, d[15]);
    p = uh_s(p, "/");
    p = uh_d(p, d[16]);
    p = uh_s(p, " ncfg ");
    uh_d(p, d[17]);
    uh_log(l);
    return 0;
}

/* audio class-specific interface descriptors (bDescriptorType 0x24) of the current interface it */
static void uh_parse_cs_audio(uh_if_t *it, const uint8_t *d, uint32_t len)
{
    char l[80], *p = uh_s(l, "  cs ");
    uint32_t st = len > 2u ? d[2] : 0u, uac2 = it->proto == 0x20u, i;
    if (it->sub == 1u) {                                     /* audio control */
        if (st == 1u && len >= 8u) {
            p = uh_s(p, "AC HEADER adc ");
            p = uh_bcd(p, uh_le16(d + 3));
            if (!uac2 && len >= 8u) {
                p = uh_s(p, " streams");
                for (i = 0; i < d[7] && i < 8u && 8u + i < len; i++) {
                    p = uh_s(p, " ");
                    p = uh_d(p, d[8 + i]);
                }
            }
        } else if (st == 2u && len >= 8u) {
            p = uh_s(p, "INPUT TERM ");
            p = uh_d(p, d[3]);
            p = uh_s(p, " type ");
            p = uh_x(p, uh_le16(d + 4), 4);
            p = uh_s(p, " ");
            p = uh_s(p, uh_term_name(uh_le16(d + 4)));
            if (!uac2 && len >= 12u) {
                p = uh_s(p, " ch ");
                p = uh_d(p, d[7]);
            } else if (uac2 && len >= 17u) {
                p = uh_s(p, " ch ");
                p = uh_d(p, d[8]);
            }
        } else if (st == 3u && len >= 9u) {
            p = uh_s(p, "OUTPUT TERM ");
            p = uh_d(p, d[3]);
            p = uh_s(p, " type ");
            p = uh_x(p, uh_le16(d + 4), 4);
            p = uh_s(p, " ");
            p = uh_s(p, uh_term_name(uh_le16(d + 4)));
            p = uh_s(p, " src ");
            p = uh_d(p, d[7]);
        } else if (st == 6u && len >= 7u) {
            if (!uh_dev.fu_id) {
                uh_dev.fu_id = d[3];
                uh_dev.ac_if = it->num;
            }
            p = uh_s(p, "FEATURE UNIT ");
            p = uh_d(p, d[3]);
            p = uh_s(p, " src ");
            p = uh_d(p, d[4]);
            if (!uac2 && len >= 7u && d[5] >= 1u && 6u + d[5] <= len) {
                p = uh_s(p, " ctl0 ");
                p = uh_x(p, d[6], 2);              /* bit 0 mute, 1 volume */
            }
        } else if (uac2 && st == 0x0Au && len >= 8u) {
            p = uh_s(p, "CLOCK SOURCE ");
            p = uh_d(p, d[3]);
            p = uh_s(p, " attr ");
            p = uh_x(p, d[4], 2);
        } else {
            p = uh_s(p, "AC subtype ");
            p = uh_x(p, st, 2);
            p = uh_s(p, " len ");
            p = uh_d(p, len);
        }
    } else if (it->sub == 2u) {                              /* audio streaming */
        if (st == 1u && len >= 7u) {
            it->link = d[3];
            p = uh_s(p, "AS GENERAL link ");
            p = uh_d(p, d[3]);
            if (!uac2) {
                it->tag = (uint16_t)uh_le16(d + 5);
                p = uh_s(p, " delay ");
                p = uh_d(p, d[4]);
                p = uh_s(p, " format ");
                p = uh_x(p, it->tag, 4);
                p = uh_s(p, it->tag == 1u ? " PCM" : it->tag == 2u ? " PCM8" : "");
            } else if (len >= 16u) {
                it->fmt = d[5];
                it->tag = (uint16_t)uh_le16(d + 6);
                it->ch = d[10];
                p = uh_s(p, " UAC2 type ");
                p = uh_d(p, d[5]);
                p = uh_s(p, " formats ");
                p = uh_x(p, uh_le24(d + 6) | (uint32_t)d[9] << 24, 8);
                p = uh_s(p, " ch ");
                p = uh_d(p, d[10]);
            }
        } else if (st == 2u && len >= 6u) {
            it->fmt = d[3];
            if (!uac2 && d[3] == 1u && len >= 8u) {          /* UAC1 type I */
                uint32_t nr = d[7], k;
                it->ch = d[4];
                it->sub_bytes = d[5];
                it->bits = d[6];
                p = uh_s(p, "FORMAT I ch ");
                p = uh_d(p, d[4]);
                p = uh_s(p, " ");
                p = uh_d(p, d[5]);
                p = uh_s(p, "B ");
                p = uh_d(p, d[6]);
                p = uh_s(p, "bit rates");
                if (nr == 0u) {                              /* continuous: lower, upper */
                    it->nrates = 0xFF;
                    if (len >= 14u) {
                        it->rate[0] = uh_le24(d + 8);
                        it->rate[1] = uh_le24(d + 11);
                        p = uh_s(p, " ");
                        p = uh_d(p, it->rate[0]);
                        p = uh_s(p, "..");
                        p = uh_d(p, it->rate[1]);
                    }
                } else {
                    for (k = 0; k < nr && 8u + 3u * k + 3u <= len; k++) {
                        if (k < UH_NRATE)
                            it->rate[it->nrates++] = uh_le24(d + 8 + 3u * k);
                        else
                            uh_dev.lost++;
                        if (k < 4u) {                       /* (the log line stays short; all are kept) */
                            p = uh_s(p, " ");
                            p = uh_d(p, uh_le24(d + 8 + 3u * k));
                        }
                    }
                    if (nr > 4u)
                        p = uh_s(p, " ..");
                }
            } else if (uac2 && d[3] == 1u) {                 /* UAC2 type I: the rates come from the clock */
                it->sub_bytes = d[4];
                it->bits = d[5];
                p = uh_s(p, "FORMAT I (UAC2) ");
                p = uh_d(p, d[4]);
                p = uh_s(p, "B ");
                p = uh_d(p, d[5]);
                p = uh_s(p, "bit, rates: clock source");
            } else {
                p = uh_s(p, "FORMAT type ");
                p = uh_d(p, d[3]);
            }
        } else {
            p = uh_s(p, "AS subtype ");
            p = uh_x(p, st, 2);
            p = uh_s(p, " len ");
            p = uh_d(p, len);
        }
    } else {
        p = uh_s(p, "subtype ");
        p = uh_x(p, st, 2);
        p = uh_s(p, " len ");
        p = uh_d(p, len);
    }
    (void)p;
    uh_log(l);
}

/* the whole configuration descriptor set (n bytes of it): interfaces, endpoints, audio formats */
static int uh_parse_cfg(const uint8_t *d, uint32_t n)
{
    char l[80], *p;
    uint32_t i = 0, cur = 0xFFu;            /* cur: the interface the descriptors belong to */
    uh_dev.nif = uh_dev.nep = 0;
    if (n < 9u || d[1] != 2u || d[0] < 9u) {
        uh_log("config descriptor: bad");
        return -1;
    }
    while (i + 2u <= n) {
        uint32_t len = d[i], ty = d[i + 1u];
        const uint8_t *q = d + i;
        if (len < 2u || i + len > n) {
            p = uh_s(l, "descriptor at ");
            p = uh_d(p, i);
            p = uh_s(p, len < 2u ? ": bad length" : ": cut off (buffer / wTotalLength)");
            uh_log(l);
            break;
        }
        if (ty == 2u && len >= 9u) {
            uh_dev.total = (uint16_t)uh_le16(q + 2);
            uh_dev.cfg_nif = q[4];
            uh_dev.cfg_val = q[5];
            uh_dev.cfg_attr = q[7];
            uh_dev.max_ma2 = q[8];
            p = uh_s(l, "cfg ");
            p = uh_d(p, q[5]);
            p = uh_s(p, " total ");
            p = uh_d(p, uh_dev.total);
            p = uh_s(p, " ifs ");
            p = uh_d(p, q[4]);
            p = uh_s(p, q[7] & 0x40u ? " self" : " bus");
            p = uh_s(p, "-powered ");
            p = uh_d(p, q[8] * 2u);
            uh_s(p, " mA");
            uh_log(l);
        } else if (ty == 4u && len >= 9u) {
            p = uh_s(l, "IF ");
            p = uh_d(p, q[2]);
            p = uh_s(p, " alt ");
            p = uh_d(p, q[3]);
            p = uh_s(p, " eps ");
            p = uh_d(p, q[4]);
            p = uh_s(p, " class ");
            p = uh_x(p, q[5], 2);
            p = uh_s(p, "/");
            p = uh_x(p, q[6], 2);
            p = uh_s(p, "/");
            p = uh_x(p, q[7], 2);
            p = uh_s(p, " ");
            p = uh_s(p, uh_class_name(q[5], q[6]));
            if (q[5] == 1u && q[7] == 0x20u)
                p = uh_s(p, " UAC2");
            uh_log(l);
            if (uh_dev.nif < UH_NIF) {
                uh_if_t *it = &uh_dev.itf[uh_dev.nif];
                uint32_t k;
                for (k = 0; k < sizeof *it; k++)
                    ((uint8_t *)it)[k] = 0;
                it->num = q[2];
                it->alt = q[3];
                it->cls = q[5];
                it->sub = q[6];
                it->proto = q[7];
                cur = uh_dev.nif++;
            } else {
                cur = 0xFFu;
                uh_dev.lost++;
            }
        } else if (ty == 5u && len >= 7u) {
            uint32_t mp = uh_le16(q + 4);
            p = uh_s(l, "  EP ");
            p = uh_x(p, q[2], 2);
            p = uh_s(p, q[2] & 0x80u ? " IN " : " OUT ");
            p = uh_s(p, uh_ep_type(q[3]));
            if ((q[3] & 3u) == 1u) {
                static const char *const SY[4] = {"", " async", " adaptive", " sync"};
                p = uh_s(p, SY[q[3] >> 2 & 3u]);
                p = uh_s(p, (q[3] >> 4 & 3u) == 1u ? " feedback" : (q[3] >> 4 & 3u) == 2u ? " implicit" : "");
            }
            p = uh_s(p, " maxp ");
            p = uh_d(p, mp & 0x7FFu);
            if (mp >> 11 & 3u) {
                p = uh_s(p, "x");
                p = uh_d(p, (mp >> 11 & 3u) + 1u);
            }
            p = uh_s(p, " int ");
            p = uh_d(p, q[6]);
            if (len >= 9u && q[8]) {                          /* audio endpoints: bSynchAddress */
                p = uh_s(p, " synch ");
                p = uh_x(p, q[8], 2);
            }
            uh_log(l);
            if (uh_dev.nep < UH_NEP && cur != 0xFFu) {
                uh_ep_t *e = &uh_dev.ep[uh_dev.nep++];
                e->addr = q[2];
                e->attr = q[3];
                e->maxp = (uint16_t)mp;
                e->interval = q[6];
                e->itf = (uint8_t)cur;
                uh_dev.itf[cur].neps++;
            } else {
                uh_dev.lost++;
            }
        } else if (ty == 0x0Bu && len >= 8u) {
            p = uh_s(l, "IAD first ");
            p = uh_d(p, q[2]);
            p = uh_s(p, " count ");
            p = uh_d(p, q[3]);
            p = uh_s(p, " class ");
            p = uh_x(p, q[4], 2);
            p = uh_s(p, "/");
            uh_x(p, q[5], 2);
            uh_log(l);
        } else if (ty == 0x24u && cur != 0xFFu && uh_dev.itf[cur].cls == 1u) {
            uh_parse_cs_audio(&uh_dev.itf[cur], q, len);
        } else if (ty == 0x25u && len >= 4u) {
            p = uh_s(l, "  cs EP subtype ");
            p = uh_x(p, q[2], 2);
            p = uh_s(p, " attr ");
            p = uh_x(p, q[3], 2);
            if (q[2] == 1u && cur != 0xFFu && uh_dev.itf[cur].proto != 0x20u)
                p = uh_s(p, q[3] & 1u ? " (rate control)" : " (no rate control)");
            uh_log(l);
        } else if (ty == 0x21u && len >= 9u) {
            p = uh_s(l, "  HID ");
            p = uh_bcd(p, uh_le16(q + 2));
            p = uh_s(p, " report len ");
            uh_d(p, uh_le16(q + 7));
            uh_log(l);
        } else {
            p = uh_s(l, "  desc type ");
            p = uh_x(p, ty, 2);
            p = uh_s(p, " len ");
            uh_d(p, len);
            uh_log(l);
        }
        i += len;
    }
    if (uh_dev.lost) {
        p = uh_s(l, "not kept (tables full): ");
        uh_d(p, uh_dev.lost);
        uh_log(l);
    }
    uh_dev.cfg_ok = 1;
    return 0;
}

/* ------------------------------------------------------- the summary --- */
/* the interfaces as lines: "IF1.1 AUDIO STREAM", its format and rates, its endpoints; returns the count */
static uint32_t uh_lines(char (*ln)[UH_LW], uint32_t max)
{
    uint32_t n = 0, i, k;
    char *p;
    for (i = 0; i < uh_dev.nif && n < max; i++) {
        const uh_if_t *it = &uh_dev.itf[i];
        p = uh_s(ln[n], "IF");
        p = uh_d(p, it->num);
        p = uh_s(p, ".");
        p = uh_d(p, it->alt);
        p = uh_s(p, " ");
        p = uh_s(p, uh_class_name(it->cls, it->sub));
        if (it->cls == 1u && it->proto == 0x20u)
            uh_s(p, " UAC2");
        n++;
        if (it->cls == 1u && it->sub == 2u && it->fmt && n < max) {
            p = uh_s(ln[n], "  ");
            p = uh_s(p, it->tag == 1u || it->proto == 0x20u ? "PCM " : "FMT ");
            p = uh_d(p, it->ch);
            p = uh_s(p, "ch ");
            p = uh_d(p, it->bits);
            p = uh_s(p, "bit");
            n++;
            if (it->nrates && n < max) {
                p = uh_s(ln[n], "  ");
                if (it->nrates == 0xFFu) {
                    p = uh_d(p, it->rate[0]);
                    p = uh_s(p, "..");
                    p = uh_d(p, it->rate[1]);
                } else {
                    for (k = 0; k < it->nrates && k < 4u; k++) {
                        p = uh_d(p, it->rate[k]);
                        p = uh_s(p, " ");
                    }
                }
                uh_s(p, "Hz");
                n++;
            }
        }
        for (k = 0; k < uh_dev.nep && n < max; k++) {
            const uh_ep_t *e = &uh_dev.ep[k];
            if (e->itf != i)
                continue;
            p = uh_s(ln[n], "  EP ");
            p = uh_x(p, e->addr, 2);
            p = uh_s(p, " ");
            p = uh_s(p, uh_ep_type(e->attr));
            p = uh_s(p, e->addr & 0x80u ? " IN " : " OUT ");
            p = uh_d(p, e->maxp & 0x7FFu);
            p = uh_s(p, "B /");
            uh_d(p, e->interval);
            n++;
        }
    }
    return n;
}
