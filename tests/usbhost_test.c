/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (makkuro) */
/* Host test of src/usbhost_desc.c (the USB host's descriptor parser and RAM log, SMK-37 Pro):
 * a canned UAC1 USB microphone (device, configuration with AC / AS / HID interfaces, strings) parsed as
 * usbhost.c hands it over: VID / PID, mps0, the interfaces and alt settings, the endpoints, the type I
 * format (channels, bits, discrete rates), the summary lines; a continuous-rate format; a configuration cut
 * short (wTotalLength beyond the buffer), bad lengths and a zero length (no endless loop); the string
 * descriptor decoder; the log ring wrapping (whole lines out, oldest first). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define UH_POOL
#include "../firmware/src/usbhost_desc.c"

static int fails;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (!(c)) {                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                 \
            fails++;                                                            \
        }                                                                       \
    } while (0)

/* a class-compliant UAC1 microphone: mono 16 bit, 48000 / 44100 Hz, EP 0x81 iso async; a HID interface */
static const uint8_t DEV[18] = {18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0x00, 0x01, 1, 2, 3, 1};
static const uint8_t CFG[] = {
    9, 2, 0, 0, 3, 1, 0, 0x80, 50,                               /* wTotalLength patched in main */
    9, 4, 0, 0, 0, 1, 1, 0, 0,                                   /* IF 0: audio control */
    9, 0x24, 1, 0x00, 0x01, 30, 0, 1, 1,                         /* AC header 1.00, total 30, stream IF 1 */
    12, 0x24, 2, 1, 0x01, 0x02, 0, 1, 0, 0, 0, 0,                /* input terminal 1: microphone, 1 ch */
    9, 0x24, 3, 3, 0x01, 0x01, 0, 2, 0,                          /* output terminal 3: USB stream, src 2 */
    9, 4, 1, 0, 0, 1, 2, 0, 0,                                   /* IF 1 alt 0: zero bandwidth */
    9, 4, 1, 1, 1, 1, 2, 0, 0,                                   /* IF 1 alt 1 */
    7, 0x24, 1, 3, 1, 0x01, 0x00,                                /* AS general: link 3, PCM */
    14, 0x24, 2, 1, 1, 2, 16, 2, 0x80, 0xBB, 0x00, 0x44, 0xAC, 0x00,   /* type I: 1 ch, 2 B, 16 bit, 48000 44100 */
    9, 5, 0x81, 0x05, 96, 0, 1, 0, 0,                            /* EP 0x81 iso async, 96 B, every frame */
    7, 0x25, 1, 1, 0, 0, 0,                                      /* cs EP general: rate control */
    9, 4, 2, 0, 1, 3, 0, 0, 0,                                   /* IF 2: HID */
    9, 0x21, 0x11, 0x01, 0, 1, 0x22, 33, 0,                      /* HID 1.11, report 33 */
    7, 5, 0x83, 0x03, 4, 0, 10,                                  /* EP 0x83 interrupt, 4 B, 10 ms */
};
static const uint8_t STR_PROD[] = {18, 3, 'U', 0, 'S', 0, 'B', 0, ' ', 0, 'M', 0, 'i', 0, 'c', 0, 0xE9, 0};

static char dump[UH_LOG_N + UH_LOG_HEAD + 64];
static uint32_t dump_n;
static void put(char c)
{
    if (dump_n + 1u < sizeof dump)
        dump[dump_n++] = c;
    dump[dump_n] = 0;
}

static int log_has(const char *s)
{
    dump_n = 0;
    dump[0] = 0;
    uh_log_each(put);
    return strstr(dump, s) != NULL;
}

int main(void)
{
    uint8_t cfg[sizeof CFG], cut[200];
    char ln[24][UH_LW], s[UH_SLEN];
    uint32_t n, i;

    memcpy(cfg, CFG, sizeof cfg);
    cfg[2] = (uint8_t)sizeof cfg;
    cfg[3] = (uint8_t)(sizeof cfg >> 8);
    memset(&uh_log_ring, 0xA5, sizeof uh_log_ring);              /* garbage: the log starts afresh */

    /* the first 8 bytes, then all 18 */
    CHECK(uh_parse_dev(DEV, 8) == 0);
    CHECK(uh_dev.mps0 == 64 && !uh_dev.dev_ok);
    CHECK(uh_parse_dev(DEV, 18) == 0);
    CHECK(uh_dev.dev_ok && uh_dev.vid == 0x1234 && uh_dev.pid == 0x5678 && uh_dev.bcd_usb == 0x0200);
    CHECK(uh_dev.i_man == 1 && uh_dev.i_prod == 2 && uh_dev.i_ser == 3 && uh_dev.ncfg == 1);
    CHECK(log_has("dev: vid 1234 pid 5678 rel 1.00 str 1/2/3 ncfg 1"));
    CHECK(uh_parse_dev(STR_PROD, sizeof STR_PROD) < 0);          /* not a device descriptor */

    /* the configuration */
    CHECK(uh_parse_cfg(cfg, sizeof cfg) == 0);
    CHECK(uh_dev.cfg_ok && uh_dev.cfg_val == 1 && uh_dev.cfg_nif == 3 && uh_dev.max_ma2 == 50);
    CHECK(uh_dev.total == sizeof cfg && uh_dev.lost == 0);
    CHECK(uh_dev.nif == 4 && uh_dev.nep == 2);
    CHECK(uh_dev.itf[0].num == 0 && uh_dev.itf[0].cls == 1 && uh_dev.itf[0].sub == 1);
    CHECK(uh_dev.itf[1].num == 1 && uh_dev.itf[1].alt == 0 && uh_dev.itf[1].neps == 0 && !uh_dev.itf[1].fmt);
    CHECK(uh_dev.itf[2].num == 1 && uh_dev.itf[2].alt == 1 && uh_dev.itf[2].sub == 2 && uh_dev.itf[2].neps == 1);
    CHECK(uh_dev.itf[2].fmt == 1 && uh_dev.itf[2].tag == 1 && uh_dev.itf[2].link == 3);
    CHECK(uh_dev.itf[2].ch == 1 && uh_dev.itf[2].sub_bytes == 2 && uh_dev.itf[2].bits == 16);
    CHECK(uh_dev.itf[2].nrates == 2 && uh_dev.itf[2].rate[0] == 48000 && uh_dev.itf[2].rate[1] == 44100);
    CHECK(uh_dev.itf[3].cls == 3 && uh_dev.itf[3].neps == 1);
    CHECK(uh_dev.fu_id == 0 && uh_dev.ac_if == 0);               /* (this one has no feature unit) */
    CHECK(uh_dev.ep[0].addr == 0x81 && (uh_dev.ep[0].attr & 3) == 1 && uh_dev.ep[0].maxp == 96 &&
          uh_dev.ep[0].interval == 1 && uh_dev.ep[0].itf == 2);
    CHECK(uh_dev.ep[1].addr == 0x83 && (uh_dev.ep[1].attr & 3) == 3 && uh_dev.ep[1].maxp == 4 &&
          uh_dev.ep[1].interval == 10 && uh_dev.ep[1].itf == 3);
    CHECK(log_has("cfg 1 total 128 ifs 3 bus-powered 100 mA"));
    CHECK(log_has("  cs INPUT TERM 1 type 0201 MICROPHONE ch 1"));
    CHECK(log_has("  cs OUTPUT TERM 3 type 0101 USB STREAM src 2"));
    CHECK(log_has("  cs AS GENERAL link 3 delay 1 format 0001 PCM"));
    CHECK(log_has("  cs FORMAT I ch 1 2B 16bit rates 48000 44100"));
    CHECK(log_has("  EP 81 IN ISO async maxp 96 int 1"));
    CHECK(log_has("  cs EP subtype 01 attr 01 (rate control)"));
    CHECK(log_has("  HID 1.11 report len 33"));
    CHECK(log_has("IF 2 alt 0 eps 1 class 03/00/00 HID"));

    n = uh_lines(ln, 24);
    CHECK(n == 8);
    if (n == 8) {
        CHECK(!strcmp(ln[0], "IF0.0 AUDIO CTRL"));
        CHECK(!strcmp(ln[1], "IF1.0 AUDIO STREAM"));
        CHECK(!strcmp(ln[2], "IF1.1 AUDIO STREAM"));
        CHECK(!strcmp(ln[3], "  PCM 1ch 16bit"));
        CHECK(!strcmp(ln[4], "  48000 44100 Hz"));
        CHECK(!strcmp(ln[5], "  EP 81 ISO IN 96B /1"));
        CHECK(!strcmp(ln[6], "IF2.0 HID"));
        CHECK(!strcmp(ln[7], "  EP 83 INT IN 4B /10"));
    }
    CHECK(uh_lines(ln, 3) == 3);                                 /* the screen's rows bound it */
    for (i = 0; i < n; i++)
        CHECK(strlen(ln[i]) < UH_LW);

    /* continuous rates (bSamFreqType 0) */
    {
        uint8_t c2[sizeof CFG];
        memcpy(c2, cfg, sizeof c2);
        for (i = 0; i + 1u < sizeof c2; i++)
            if (c2[i] == 14 && c2[i + 1u] == 0x24 && c2[i + 2u] == 2) {
                c2[i + 7u] = 0;                                  /* 0: lower 48000, upper 44100 as the bytes stand */
                break;
            }
        CHECK(uh_parse_cfg(c2, sizeof c2) == 0);
        CHECK(uh_dev.itf[2].nrates == 0xFF && uh_dev.itf[2].rate[0] == 48000 && uh_dev.itf[2].rate[1] == 44100);
        n = uh_lines(ln, 24);
        CHECK(n == 8 && !strcmp(ln[4], "  48000..44100Hz"));
    }

    /* cut short: what fits is kept, the cut is logged */
    memcpy(cut, cfg, 100);
    CHECK(uh_parse_cfg(cut, 100) == 0);
    CHECK(uh_dev.nif == 3 && log_has(": cut off (buffer / wTotalLength)"));
    /* a zero length stops the walk */
    memcpy(cut, cfg, sizeof cfg);
    cut[9] = 0;
    CHECK(uh_parse_cfg(cut, sizeof cfg) == 0 && uh_dev.nif == 0 && log_has("descriptor at 9: bad length"));
    CHECK(uh_parse_cfg(DEV, 18) < 0);                            /* not a configuration */

    /* strings: UTF-16LE -> ASCII, '?' beyond, the descriptor's own length, the cap */
    uh_str(s, sizeof s, STR_PROD, sizeof STR_PROD);
    CHECK(!strcmp(s, "USB Mic?"));
    uh_str(s, 4, STR_PROD, sizeof STR_PROD);
    CHECK(!strcmp(s, "USB"));
    uh_str(s, sizeof s, STR_PROD, 6);
    CHECK(!strcmp(s, "US"));
    uh_str(s, sizeof s, DEV, sizeof DEV);
    CHECK(!strcmp(s, ""));

    /* the ring wraps: the start kept apart (whole lines), the gap marked, then the newest, oldest first */
    for (i = 0; i < 2000u; i++) {
        char l[16], *p = uh_s(l, "line ");
        uh_d(p, i);
        uh_log(l);
    }
    CHECK(uh_log_ring.w > UH_LOG_N);
    CHECK(log_has("line 1999\n"));
    CHECK(!strncmp(dump, "dev: usb 2.00", 13));                  /* the first line ever: in the head */
    CHECK(strstr(dump, "\n... (the middle of the log is lost) ...\nline ") != NULL);
    CHECK(dump_n <= UH_LOG_N + UH_LOG_HEAD + 64u && dump[dump_n - 1u] == '\n');
    CHECK(uh_log_head.n > UH_LOG_HEAD - 20u && uh_log_head.b[uh_log_head.n - 1u] == '\n');

    /* a session: its start kept from there; logclear */
    uh_log_session();
    uh_log("== session start");
    for (i = 0; i < 1000u; i++)
        uh_log("filler filler filler");
    CHECK(log_has("== session start\nfiller"));
    CHECK(!strstr(dump, "dev: usb"));
    uh_log_clear();
    CHECK(!log_has("filler") && dump_n == 0);
    uh_log("after clear");
    CHECK(log_has("after clear\n") && dump_n == 12);
    for (i = 0; i < 2000u; i++) {
        char l[16], *p = uh_s(l, "line ");
        uh_d(p, i);
        uh_log(l);
    }

    /* the screen's history: the newest lines, oldest first, the time stamp off, cut to the row */
    {
        char t[8][UH_LW];
        CHECK(uh_log_tail(t, 3) == 3);
        CHECK(!strcmp(t[0], "line 1997") && !strcmp(t[1], "line 1998") && !strcmp(t[2], "line 1999"));
        uh_log("[12.345] connect: D+ 1 D- 0");
        uh_log("[12.400] a line much longer than one row of the screen can show, cut there");
        CHECK(uh_log_tail(t, 2) == 2);
        CHECK(!strcmp(t[0], "connect: D+ 1 D- 0"));
        CHECK(strlen(t[1]) == UH_LW - 1u && !strncmp(t[1], "a line much", 11));
        uh_log_ring.magic = 0;                                    /* an empty log */
        CHECK(uh_log_tail(t, 8) == 0);
        uh_log("only");
        CHECK(uh_log_tail(t, 8) == 1 && !strcmp(t[0], "only"));
    }

    if (fails) {
        printf("usbhost_test: %d FAILED\n", fails);
        return 1;
    }
    printf("usbhost_test: ok\n");
    return 0;
}
