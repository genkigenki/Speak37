/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (makkuro) */
/* USB host mode (SMK-37 Pro, milestone 1): USB0 leaves device mode, becomes a full-speed host, enumerates
 * what is plugged in (a USB microphone) and shows it: VID:PID, the strings, the speed, the interfaces,
 * endpoints and audio formats. Every step and every descriptor field goes to the RAM log (usbhost_desc.c;
 * console `logdump` once back in device mode). Nothing streams yet; SET_CONFIGURATION is the last request.
 *
 * Never at boot: only on request, console `host` or < and > held together 3 s (both seen released since
 * power-on). usbhost_session() owns the main loop while it sets the device up (as ota_session does; any other
 * button, 60 s without a device or 120 s in all: back to device mode). Once a microphone streams, host mode goes on
 * in the background (milestone 4): the normal UI is back, the header shows MIC / VOX, FADER BANK opens the VOCODER
 * page (usbhost_page), the main loop runs uh_poll (usbhost_bg: a lost stream is set up again). < and > held 3 s
 * again, or 60 s without a device, end it: usb_start, the update path and the console. The audio and the input scan
 * run from their ISRs throughout, usb_poll is idle (usb.up = 0).
 * The enumeration is a polled state machine (uh_poll): one CSR0 look per call, every wait by UH_NOW, so
 * the screen and the buttons stay live. Registers: hal/usbh.h (the SDK's host HAL, translated).
 * VBUS: nothing here switches a supply, and the SMK-37 Pro gives none on its port: a self-powered hub between
 * the two, or a Y cable. A hub (class 9) gets a minimal hub driver (milestone 1b): its ports powered, polled
 * with GetPortStatus, the first full-speed device on one reset and enumerated as address 2 like a direct one
 * (a full-speed hub, full-speed device: no split transactions; a low-speed one would need PRE: not supported).
 * Self-contained, the USB host core: the state machine, the control transfers, the stream, the hub, the RAM log;
 * the platform comes in through a few macros the includer defines: UH_NOW (milliseconds, a variable or an
 * expression), UH_DELAY_MS(n), UH_KEYS_TO_VOCODER(on) (the keys play the vocoder's carrier, not the instrument).
 * The screens, the session loop, the VOCODER page and the buttons are the instrument's: usbhost_ui.c (Felucca). */
#include "usbh.h"
#if !defined(UH_NOW) || !defined(UH_KEYS_TO_VOCODER)
#error "usbhost.c: the includer defines UH_NOW, UH_DELAY_MS and UH_KEYS_TO_VOCODER (the platform glue)"
#endif

#define UH_ADDR 1u                         /* the address SET_ADDRESS gives (the SDK: random 1..16); a hub's */
#define UH_DADDR 2u                        /* the device behind a hub */
#define UH_NPORT 7u                        /* hub ports handled */
/* USB 2.0 timing (9.1.2, 7.1.7.5): connect debounce >= 100 ms (UH_WAIT), bus reset >= 10 ms (50 as root ports
 * drive it; the SDK's mount +10 a retry), reset recovery >= 10 ms, SET_ADDRESS recovery >= 2 ms */
#define UH_RESET_MS 50u
#define UH_RECOVERY_MS 20u
#define UH_ADDR_MS 20u
#define UH_TRIES 3u                        /* enumeration attempts (the SDK's mount retries) */
#define UH_XFER_MS 500u                    /* one stage of a control transfer (the SDK: 500 jiffies); NAKs are
                                            * retried by the controller meanwhile */
#define UH_NODEV_MS 60000u                 /* no device this long: back to device mode */
#define UH_TOTAL_MS 120000u                /* host mode at most this long */

enum { UH_IDLE, UH_WAIT, UH_ATTACH, UH_RESET, UH_ENUM, UH_DONE, UH_FAIL, UH_HUB, UH_STREAM };
static const char *const UH_STATE[] = {"OFF", "WAITING", "ATTACHED", "BUS RESET", "ENUMERATING", "DONE", "FAILED",
                                       "HUB", "STREAMING"};
/* the hub's steps: one request each; H_WAIT a pause, then uh.hwait */
enum { H_DESC, H_POWER, H_SCAN, H_RESET, H_RSTAT, H_CLRRST, H_CLRCONN, H_WATCH, H_WAIT, H_ENUM };
static const char *const UH_HSTEP[] = {"HUB DESCRIPTOR", "PORT POWER", "PORT STATUS", "PORT RESET", "PORT STATUS",
                                       "CLEAR C_PORT_RESET", "CLEAR C_PORT_CONNECTION", "PORT STATUS"};
enum { E_DEV8, E_ADDR, E_DEV18, E_CFG9, E_CFG, E_LANG, E_MAN, E_PROD, E_SER, E_SETCFG,
       E_SETIF, E_SETRATE, E_GETRATE, E_GETMUTE, E_UNMUTE, E_GETVOL, E_GETVMIN, E_GETVMAX, E_GETVRES, E_STREAM, E_END };
static const char *const UH_STEP[] = {"GET DEVICE (8)", "SET ADDRESS", "GET DEVICE", "GET CONFIG (9)", "GET CONFIG",
                                      "GET LANGUAGES", "GET MANUFACTURER", "GET PRODUCT", "GET SERIAL",
                                      "SET CONFIGURATION", "SET INTERFACE", "SET_CUR RATE", "GET_CUR RATE",
                                      "GET_CUR MUTE", "SET_CUR UNMUTE", "GET_CUR VOLUME", "GET_MIN VOLUME",
                                      "GET_MAX VOLUME", "GET_RES VOLUME"};
#define UH_STREAM_MS 600000u               /* streaming at most this long (for now) */
enum { CT_IDLE, CT_SETUP, CT_IN, CT_STATUS_IN, CT_STATUS_OUT, CT_RECOVER, CT_OUT };   /* the control transfer's stage */
/* `hostopt N` (console): experiments switchable at run time, 0 = the default (the e09d257 behaviour, which
 * enumerated the hub); the set in use is logged at the start of each session */
enum { UHO_POWER60 = 1u,                   /* POWER 0x60 (the SDK usb.h's FUSB_MODE 0) instead of 0x40 */
       UHO_NO_SESSION = 2u,                /* never write DEVCTL's session bit again after host init (the SDK) */
       UHO_NO_PORT_POWER = 4u,             /* hub: no SET_FEATURE PORT_POWER, scan the ports at once */
       UHO_POWER_SLOW = 8u,                /* hub: one port powered at a time, bPwrOn2PwrGood after each */
       UHO_SLEW = 16u,                     /* USB_IO_CON0 SR (slew rate, bit 12) set, as device mode has it */
       UHO_RETRY_RESET = 32u,              /* enumeration retries from the bus reset, not from a new host init */
       UHO_STREAM_KICK = 64u };            /* streaming: DEVCTL's session asked for again when the device bit drops */
static const char *const UHO_NAME[] = {"POWER60", "NO_SESSION", "NO_PORT_POWER", "POWER_SLOW", "SLEW", "RETRY_RESET",
                                       "STREAM_KICK"};
#define UH_RECOVERIES 3u                   /* session recoveries in one transfer, then the device counts as gone */
#define UH_SESSION_MS 500u                 /* a session asked for again: this long for DEVCTL to show the device */

static uint8_t uh_ep0[64 + 4] __attribute__((aligned(4)));          /* EP0's DMA buffer (SETUP out, data in) */
static uint8_t uh_iso[512 + 4] __attribute__((aligned(4)));         /* host EP1 RX's DMA buffer: the iso IN packets */
static uint8_t uh_odata[4];                                          /* an OUT data stage (SET_CUR) */
static uint8_t uh_devd[18], uh_cfgd[512] UH_POOL, uh_strd[64];
static char uh_ln[56][UH_LW] UH_POOL;          /* the summary lines and the log's tail (screen, console) */
static uint16_t uh_cfg_n;                                           /* bytes of uh_cfgd received */

static struct {
    uint8_t st, step, ctl, addr, speed, tries, phase, req, scroll;
    uint16_t want, got;                    /* the data stage: bytes asked for, received */
    uint8_t setup[8];
    uint8_t *dst;
    uint32_t t;                            /* the current stage / phase started (UH_NOW) */
    uint32_t t_conn, t_se0, t_nodev, t_start, t_poke, reset_ms;
    uint32_t sessions, resets, xfers, xfer_errs, pokes;
    uint8_t csr, dc, recov;                /* the last CSR0 / DEVCTL a transfer saw; recov: session recoveries in it */
    uint8_t lost_stage;                    /* the stage the session was lost in (the transfer restarts from SETUP) */
    uint32_t t_kick, recoveries;           /* the session last asked for again; recoveries in all */
    uint8_t opt, link0;                    /* opt: UHO_* (`hostopt`); link0: the transfer began with DEVCTL's device bit */
    uint8_t out, view;                     /* out: the transfer has an OUT data stage; view: streaming 0 meters, 1 list */
    /* the stream picked from the descriptors (uh_pick_stream): interface, alt, endpoint, its max packet and interval,
     * the format; the rate asked for */
    uint8_t s_ok, s_if, s_alt, s_ep, s_int, s_ch, s_sb, s_bits;
    uint16_t s_maxp;
    uint32_t s_rate, t_stream, t_sec, pkts_sec, pkts_last;
    uint32_t mpk[2], mrms[2];              /* the meters as last taken (the screen) */
    uint8_t logged_first, logged_drop;
    int16_t s_vol;                         /* the device's volume, GET_CUR (1/256 dB) */
    uint8_t vc_touch;                      /* the vocoder control turned last (lit on the screen) */
    uint8_t bg, vox_off;                   /* bg: host mode in the background (the normal UI); vox_off: VOX off */
    uint8_t quiet;                         /* an automatic probe: no log lines until a device connects */
    uint32_t t_bgfail;
    uint32_t t_touch;
    uint8_t eaddr, naddr, mps, hubx;       /* the device being enumerated: its address now, the one it gets; EP0's
                                            * max packet of this transfer; hubx: a request to the hub */
    uint8_t hstep, hport, hwait;           /* the hub: its step, the port scanned, the step after a pause */
    uint32_t hwait_ms, t_hreset;
    char msg[UH_LW];                       /* the last thing that happened */
    char fail[UH_LW];                      /* the last failure, as logged */
    char result[UH_LW];                    /* the outcome (OK / FAILED + why): on the screen until host mode ends */
    uint8_t res_ok, follow;                /* follow: the screen's list kept at its end (the newest log line) */
} uh;

static struct {
    uint8_t present, nports, pwr2good, mps0, port, bad, cand;   /* port: the device's (0 none); bad: a port left
                                                                 * alone until it is empty again */
    uint16_t vid, pid, chars;
    uint16_t pst[UH_NPORT + 1u], pch[UH_NPORT + 1u];  /* wPortStatus / wPortChange as last read, by port 1.. */
    uint32_t t_cand;
} uh_hub;

/* a log line with the time: "[12.345] text" */
static void uh_note(const char *s)
{
    char l[100], *p = uh_s(l, "[");
    uint32_t i;
    p = uh_d(p, UH_NOW / 1000u);
    p = uh_s(p, ".");
    p = uh_d(p, UH_NOW / 100u % 10u);
    p = uh_d(p, UH_NOW / 10u % 10u);
    p = uh_d(p, UH_NOW % 10u);
    p = uh_s(p, "] ");
    for (i = 0; s[i] && p < l + sizeof l - 1u; i++)
        *p++ = s[i];
    *p = 0;
    if (!uh.quiet)
        uh_log(l);
    for (i = 0; s[i] && i + 1u < UH_LW; i++)
        uh.msg[i] = s[i];
    uh.msg[i] = 0;
}

/* "<what> <value hex>" into the log, e.g. the registers */
static void uh_note_x(const char *s, uint32_t v, uint32_t digits)
{
    char l[60], *p = uh_s(l, s);
    p = uh_s(p, " ");
    uh_x(p, v, digits);
    uh_note(l);
}

static void uh_regs(const char *why)
{
    char l[90], *p = uh_s(l, why);
    p = uh_s(p, ": devctl ");
    p = uh_x(p, usbh_devctl(), 2);
    p = uh_s(p, " power ");
    p = uh_x(p, usbh_rd(UH_POWER), 2);
    p = uh_s(p, " intrusb ");
    p = uh_x(p, usbh_rd(UH_INTRUSB), 2);              /* (read clears it: what came since the last look) */
    p = uh_s(p, " con0 ");
    p = uh_x(p, usbh_con0(), 8);
    p = uh_s(p, " io ");
    uh_x(p, usbh_io_con0(), 8);
    uh_note(l);
}

/* ------------------------------------------------ control transfers --- */
/* usb_ctlXfer / usb_control_transfers (SDK usb_ctrl_transfer.c), one stage at a time: SETUP (SetupPkt +
 * TxPktRdy), the IN data packets (ReqPkt each; a short one ends it), the status stage (StatusPkt + TxPktRdy
 * after IN data, StatusPkt + ReqPkt without data). Only requests without OUT data are needed. */
/* the link as DEVCTL shows it: host mode, and a full- or low-speed device on the session */
static uint32_t uh_link_ok(uint32_t dc) { return (dc & UH_DC_HOSTMODE) && (dc & (UH_DC_FSDEV | UH_DC_LSDEV)); }

/* DEVCTL's session asked for again (nothing else: no CON0, PHY or pad bit), only when host mode has gone, at most
 * every 200 ms (writing it while a session comes up keeps it from coming up: hardware test 3) */
static void uh_kick(uint32_t dc)
{
    if ((dc & UH_DC_HOSTMODE) || (uh.opt & UHO_NO_SESSION) || UH_NOW - uh.t_kick < 200u)
        return;
    usbh_session();
    uh.t_kick = UH_NOW;
    uh.pokes++;
    uh_note_x("session asked for again; devctl", dc, 2);
}

/* SETUP out of uh.setup (a new transfer, or the same one again after a session recovery) */
static void uh_ctl_issue(void)
{
    uh.got = 0;
    uh.link0 = (uint8_t)uh_link_ok(usbh_devctl());
    usbh_faddr(uh.addr);
    usbh_ep0_write(uh_ep0, uh.setup, 8);
    usbh_csr0_wr(UH_C0_SETUPPKT | UH_C0_TXPKTRDY);
    uh.ctl = CT_SETUP;
    uh.t = UH_NOW;
}

static void uh_ctl_start(uint32_t rt, uint32_t req, uint32_t val, uint32_t idx, uint32_t len, uint8_t *dst)
{
    uh.setup[0] = (uint8_t)rt;
    uh.setup[1] = (uint8_t)req;
    uh.setup[2] = (uint8_t)val;
    uh.setup[3] = (uint8_t)(val >> 8);
    uh.setup[4] = (uint8_t)idx;
    uh.setup[5] = (uint8_t)(idx >> 8);
    uh.setup[6] = (uint8_t)len;
    uh.setup[7] = (uint8_t)(len >> 8);
    uh.want = (uint16_t)len;
    uh.dst = dst;
    uh.out = !(rt & 0x80u) && len;
    uh.xfers++;
    uh.recov = 0;
    uh.mps = uh.hubx ? uh_hub.mps0 : uh_dev.mps0;
    uh_ctl_issue();
}

static const char *const UH_CT[] = {"idle", "setup", "data in", "status in", "status out", "recover", "data out"};

/* the session was lost under a transfer (DEVCTL: no host mode, or no device): ask for it again, keep the address and
 * the state; uh_ctl_poll restarts the transfer once DEVCTL shows the device. The keyboard gives no VBUS: the
 * controller's session logic can end the session while the device is still attached (hardware test 2) */
static void uh_recover_start(void)
{
    char l[90], *p = uh_s(l, "session lost in ");
    p = uh_s(p, UH_CT[uh.ctl]);
    p = uh_s(p, ": devctl ");
    p = uh_x(p, uh.dc, 2);
    p = uh_s(p, " intrusb ");
    p = uh_x(p, usbh_rd(UH_INTRUSB), 2);
    p = uh_s(p, " D+ ");
    p = uh_d(p, usbh_dp());
    p = uh_s(p, " D- ");
    p = uh_d(p, usbh_dm());
    p = uh_s(p, ", recovery ");
    uh_d(p, uh.recov + 1u);
    uh_note(l);
    uh.recov++;
    uh.recoveries++;
    uh.lost_stage = uh.ctl;
    uh.ctl = CT_RECOVER;
    uh.t = UH_NOW;
    uh_kick(uh.dc);
}

/* 0 busy, 1 done, < 0 failed: -1 timeout, -2 device gone (the lines low 100 ms with the session up, or the
 * session not back), -3 stall, -4 bus error (no handshake after the controller's retries) (uh_enum_fail: -5 bad
 * descriptor). The SDK takes a DEVCTL without host mode, or SE0 for 1000 polls, as offline at once */
static int uh_ctl_poll(void)
{
    uint32_t c, n, mps = uh.mps;
    uh.dc = (uint8_t)usbh_devctl();
    if (uh.ctl == CT_RECOVER) {
        if (uh_link_ok(uh.dc) && !usbh_se0()) {
            char l[60], *p = uh_s(l, "session back after ");
            p = uh_d(p, UH_NOW - uh.t_kick);
            p = uh_s(p, " ms (devctl ");
            p = uh_x(p, uh.dc, 2);
            uh_s(p, "): the transfer again");
            uh_note(l);
            uh.t_se0 = 0;
            uh_ctl_issue();
            return 0;
        }
        if (UH_NOW - uh.t > UH_SESSION_MS) {
            uh_note_x("session not back; devctl", uh.dc, 2);
            return -2;
        }
        uh_kick(uh.dc);
        return 0;
    }
    if (uh.link0 && !uh_link_ok(uh.dc) && (usbh_dp() || usbh_dm())) {
        /* DEVCTL lost the device under the transfer, the device's line still high: wait for it (the session asked
         * for again only if host mode went), then the transfer again; unless that keeps happening */
        if (uh.recov >= UH_RECOVERIES)
            return -2;
        uh_recover_start();
        return 0;
    }
    if (usbh_se0()) {                                   /* the session up and both lines low: unplugged */
        if (!uh.t_se0)
            uh.t_se0 = UH_NOW | 1u;
        else if (UH_NOW - uh.t_se0 > 100u)
            return -2;
    } else {
        uh.t_se0 = 0;
    }
    if (UH_NOW - uh.t > UH_XFER_MS)
        return -1;
    c = usbh_csr0();
    uh.csr = (uint8_t)c;
    if (c & UH_C0_RXSTALL) {
        usbh_csr0_wr(0);
        return -3;
    }
    if (c & UH_C0_ERROR) {
        usbh_csr0_wr(0);
        return -4;
    }
    switch (uh.ctl) {
    case CT_SETUP:
        if (c & UH_C0_TXPKTRDY)
            return 0;
        if (uh.out && uh.want) {                        /* the OUT data (one packet: SET_CUR) */
            usbh_faddr(uh.addr);
            usbh_ep0_write(uh_ep0, uh_odata, uh.want > sizeof uh_odata ? sizeof uh_odata : uh.want);
            usbh_csr0_wr(UH_C0_TXPKTRDY);
            uh.ctl = CT_OUT;
            uh.t = UH_NOW;
            return 0;
        }
        usbh_csr0_wr(uh.want ? UH_C0_REQPKT : UH_C0_STATUSPKT | UH_C0_REQPKT);
        uh.ctl = uh.want ? CT_IN : CT_STATUS_IN;
        uh.t = UH_NOW;
        return 0;
    case CT_OUT:                                        /* sent: the status stage, IN (StatusPkt + ReqPkt) */
        if (c & UH_C0_TXPKTRDY)
            return 0;
        uh.got = uh.want;
        usbh_faddr(uh.addr);
        usbh_csr0_wr(UH_C0_STATUSPKT | UH_C0_REQPKT);
        uh.ctl = CT_STATUS_IN;
        uh.t = UH_NOW;
        return 0;
    case CT_IN:
        if (!(c & UH_C0_RXPKTRDY))
            return 0;
        n = usbh_count0();
        if (mps < 8u)
            mps = 8u;
        {
            uint32_t m = n < (uint32_t)(uh.want - uh.got) ? n : (uint32_t)(uh.want - uh.got);
            if (m > 64u)
                m = 64u;
            usbh_ep0_read(uh.dst + uh.got, uh_ep0, m);
            uh.got = (uint16_t)(uh.got + m);
        }
        if (n < mps || uh.got >= uh.want) {             /* short packet or all of it: the status stage */
            usbh_csr0_wr(0);
            usbh_faddr(uh.addr);
            usbh_csr0_wr(UH_C0_STATUSPKT | UH_C0_TXPKTRDY);
            uh.ctl = CT_STATUS_OUT;
        } else {
            usbh_csr0_wr(UH_C0_REQPKT);
        }
        uh.t = UH_NOW;
        return 0;
    case CT_STATUS_IN:
        if (!(c & UH_C0_RXPKTRDY))
            return 0;
        usbh_csr0_wr(0);
        return 1;
    case CT_STATUS_OUT:
        return (c & UH_C0_TXPKTRDY) ? 0 : 1;
    default:
        return -1;
    }
}

/* INTRUSB (polled, read clears it) into the log while a device is handled: 7 VBus error, 6 session request,
 * 5 disconnect, 4 connect, 2 reset / babble, 1 resume, 0 suspend (3, SOF, left out) */
static void uh_watch_intr(void)
{
    uint32_t iu = usbh_rd(UH_INTRUSB) & ~0x08u;
    char l[40], *p;
    if (!iu)
        return;
    p = uh_s(l, "intrusb ");
    p = uh_x(p, iu, 2);
    p = uh_s(p, " devctl ");
    uh_x(p, usbh_devctl(), 2);
    uh_note(l);
}

/* the failure of a request as text: the step, why, CSR0 and DEVCTL as the transfer last saw them */
static void uh_note_fail(const char *step, int r, const char *then)
{
    char l[90], *p = uh_s(l, step);
    p = uh_s(p, r == -1 ? ": timeout" : r == -2 ? ": device gone" : r == -3 ? ": stall" :
                r == -4 ? ": bus error" : ": bad descriptor");
    p = uh_s(p, " csr0 ");
    p = uh_x(p, uh.csr, 2);
    p = uh_s(p, " devctl ");
    p = uh_x(p, uh.dc, 2);
    uh_s(p, then);
    uh.xfer_errs++;
    uh_note(l);
    for (p = l; *p && p - l < (int32_t)UH_LW - 1; p++)
        uh.fail[p - l] = *p;
    uh.fail[p - l] = 0;
}

/* the outcome on the screen (kept until host mode ends, or the next outcome): "FAILED <why>" / "OK ..." */
static void uh_result(int ok, const char *why)
{
    char *p = uh_s(uh.result, ok ? "OK " : "FAILED ");
    uint32_t i;
    for (i = 0; why[i] && p < uh.result + UH_LW - 1u; i++)
        *p++ = why[i];
    *p = 0;
    uh.res_ok = (uint8_t)ok;
    uh.follow = !ok;                                    /* OK: the list from its top (the device), else its end (the log) */
    uh.scroll = 0;
}

/* OK and the device in a line: VID:PID, interfaces, power, (the hub port) */
static void uh_result_ok(void)
{
    char l[UH_LW], *p = uh_x(l, uh_dev.vid, 4);
    p = uh_s(p, ":");
    p = uh_x(p, uh_dev.pid, 4);
    p = uh_s(p, " ");
    p = uh_d(p, uh_dev.cfg_nif);
    p = uh_s(p, "IF ");
    p = uh_d(p, uh_dev.max_ma2 * 2u);
    p = uh_s(p, "mA");
    if (uh_hub.present && uh_hub.port) {
        p = uh_s(p, " HUB P");
        uh_d(p, uh_hub.port);
    }
    uh_result(1, l);
}

/* ---------------------------------------------------- the device --- */
static void uh_to_wait(void)
{
    usbh_sie_init();
    usbh_power(uh.opt & UHO_POWER60 ? 0x60 : 0x40);    /* the SDK's mount: 0x40 FUSB_MODE 1, 0x60 FUSB_MODE 0 */
    usbh_io_slew(uh.opt & UHO_SLEW);
    usbh_intr_off();
    usbh_ep0_buf(uh_ep0);
    usbh_session();
    usbh_low_speed(0);
    uh.st = UH_WAIT;
    uh.ctl = CT_IDLE;
    uh.addr = 0;
    uh_hub.present = 0;
    uh.t_conn = 0;
    uh.t_nodev = UH_NOW;
    uh.t_poke = UH_NOW;
    uh_regs("host init");
    uh_note("waiting for a device");
}

static void uh_reset_start(void)
{
    uh.st = UH_RESET;
    uh.phase = 0;
    uh.addr = uh.eaddr = 0;
    uh.naddr = UH_ADDR;
    uh.ctl = CT_IDLE;
    uh.resets++;
    uh.t_se0 = 0;
    usbh_bus_reset(1);
    uh.t = UH_NOW;
}

/* a failed request of the enumeration: again from a fresh host init (the SDK's mount loop: usb_h_sie_init,
 * the reset 10 ms longer), UH_TRIES in all, then FAILED */
static void uh_hub_go(uint32_t step);
static void uh_enum_fail(int r)
{
    uh.ctl = CT_IDLE;
    if (r == -2) {
        uh_note_fail(UH_STEP[uh.step], r, "");
        uh_to_wait();
        return;
    }
    if (uh_hub.present) {                               /* behind the hub: its port reset again, not the bus */
        uh.st = UH_HUB;
        if (++uh.tries < UH_TRIES) {
            uh_note_fail(UH_STEP[uh.step], r, ", retry from the port reset");
            uh_hub_go(H_RESET);
            return;
        }
        uh_note_fail(UH_STEP[uh.step], r, "");
        uh_note("enumeration failed (the port is left alone until empty)");
        uh_result(0, uh.fail);
        uh_hub.bad = uh_hub.port;
        uh_hub.port = 0;
        uh.hport = 1;
        uh_hub_go(H_SCAN);
        return;
    }
    if (++uh.tries < UH_TRIES && (uh.opt & UHO_RETRY_RESET)) {
        uh_note_fail(UH_STEP[uh.step], r, ", retry from the bus reset");
        uh.reset_ms += 10u;
        uh_reset_start();
        return;
    }
    if (uh.tries < UH_TRIES) {
        uh_note_fail(UH_STEP[uh.step], r, ", retry from host init");
        uh.reset_ms += 10u;
        usbh_sie_close();
        UH_DELAY_MS(10);
        usbh_sie_init();
        usbh_power(uh.opt & UHO_POWER60 ? 0x60 : 0x40);
        usbh_io_slew(uh.opt & UHO_SLEW);
        usbh_intr_off();
        usbh_ep0_buf(uh_ep0);
        usbh_session();
        usbh_low_speed(0);
        uh.st = UH_ATTACH;
        uh.t = uh.t_kick = UH_NOW;
        return;
    }
    uh_note_fail(UH_STEP[uh.step], r, "");
    uh.st = UH_FAIL;
    uh_note("enumeration failed");
    uh_result(0, uh.fail);
}

static void uh_hub_start(void);

/* the stream to play: the first audio streaming alt setting with a type I PCM format (1 / 2 channels, 16 / 24 bit)
 * and an isochronous IN endpoint that fits uh_iso; 48 kHz if it has it, else its first rate */
static void uh_pick_stream(void)
{
    uint32_t i, k;
    char l[90], *p;
    uh.s_ok = 0;
    for (i = 0; i < uh_dev.nif && !uh.s_ok; i++) {
        const uh_if_t *it = &uh_dev.itf[i];
        if (it->cls != 1u || it->sub != 2u || it->fmt != 1u || it->proto == 0x20u || it->ch < 1u || it->ch > 2u ||
            (it->sub_bytes != 2u && it->sub_bytes != 3u))
            continue;
        for (k = 0; k < uh_dev.nep; k++) {
            const uh_ep_t *e = &uh_dev.ep[k];
            if (e->itf != i || (e->attr & 3u) != 1u || !(e->addr & 0x80u) || (e->maxp & 0x7FFu) > sizeof uh_iso - 4u)
                continue;
            uh.s_ok = 1;
            uh.s_if = it->num;
            uh.s_alt = it->alt;
            uh.s_ep = e->addr;
            uh.s_maxp = (uint16_t)(e->maxp & 0x7FFu);
            uh.s_int = e->interval ? e->interval : 1u;
            uh.s_ch = it->ch;
            uh.s_sb = it->sub_bytes;
            uh.s_bits = it->bits;
            uh.s_rate = it->nrates == 0xFFu ? (it->rate[0] <= 48000u && it->rate[1] >= 48000u ? 48000u : it->rate[0])
                                            : it->rate[0];
            if (it->nrates != 0xFFu) {
                uint32_t r;
                for (r = 0; r < it->nrates; r++)
                    if (it->rate[r] == 48000u)
                        uh.s_rate = 48000u;
            }
            break;
        }
    }
    if (!uh.s_ok) {
        uh_note("no PCM stream to play (type I, 16 / 24 bit, 1 / 2 ch, iso IN)");
        return;
    }
    p = uh_s(l, "stream: IF ");
    p = uh_d(p, uh.s_if);
    p = uh_s(p, " alt ");
    p = uh_d(p, uh.s_alt);
    p = uh_s(p, " EP ");
    p = uh_x(p, uh.s_ep, 2);
    p = uh_s(p, " maxp ");
    p = uh_d(p, uh.s_maxp);
    p = uh_s(p, " ");
    p = uh_d(p, uh.s_rate);
    p = uh_s(p, " Hz ");
    p = uh_d(p, uh.s_ch);
    p = uh_s(p, "ch ");
    p = uh_d(p, uh.s_bits);
    p = uh_s(p, "bit, feature unit ");
    uh_d(p, uh_dev.fu_id);
    uh_note(l);
}

static void uh_stream_start(void);
static void uh_enum_start(uint32_t step)
{
    uint32_t v = 0, len = 0, idx = 0;
    uint8_t *dst = uh_strd;
    uh.hubx = 0;
    uh.addr = uh.eaddr;
    for (;; step++) {
        uh.step = (uint8_t)step;
        switch (step) {
        case E_DEV8: v = 0x0100; len = 8; dst = uh_devd; break;
        case E_ADDR: uh_ctl_start(0x00, 5, uh.naddr, 0, 0, 0); return;
        case E_DEV18: v = 0x0100; len = 18; dst = uh_devd; break;
        case E_CFG9: v = 0x0200; len = 9; dst = uh_cfgd; break;
        case E_CFG:
            v = 0x0200;
            len = uh_dev.total > sizeof uh_cfgd ? sizeof uh_cfgd : uh_dev.total;
            dst = uh_cfgd;
            break;
        case E_LANG: v = 0x0300; len = sizeof uh_strd; break;
        case E_MAN: case E_PROD: case E_SER: {
            uint32_t i = step == E_MAN ? uh_dev.i_man : step == E_PROD ? uh_dev.i_prod : uh_dev.i_ser;
            if (!i)
                continue;                               /* no such string: the next step */
            v = 0x0300 | i;
            idx = 0x0409;
            len = sizeof uh_strd;
            break;
        }
        case E_SETCFG: uh_ctl_start(0x00, 9, uh_dev.cfg_val, 0, 0, 0); return;
        case E_SETIF:                                   /* the stream (milestone 2): its alt setting */
            uh_pick_stream();
            if (!uh.s_ok)
                continue;
            uh_ctl_start(0x01, 11, uh.s_alt, uh.s_if, 0, 0);
            return;
        case E_SETRATE:                                 /* UAC1 SET_CUR sampling frequency, to the endpoint */
            if (!uh.s_ok)
                continue;
            uh_odata[0] = (uint8_t)uh.s_rate;
            uh_odata[1] = (uint8_t)(uh.s_rate >> 8);
            uh_odata[2] = (uint8_t)(uh.s_rate >> 16);
            uh_ctl_start(0x22, 0x01, 0x0100, uh.s_ep, 3, 0);
            return;
        case E_GETRATE:
            if (!uh.s_ok)
                continue;
            uh_ctl_start(0xA2, 0x81, 0x0100, uh.s_ep, 3, uh_strd);
            return;
        case E_GETMUTE:                                 /* the feature unit: mute (master channel) */
            if (!uh.s_ok || !uh_dev.fu_id)
                continue;
            uh_ctl_start(0xA1, 0x81, 0x0100, (uint32_t)uh_dev.fu_id << 8 | uh_dev.ac_if, 1, uh_strd);
            return;
        case E_UNMUTE:                                  /* (only when GET_CUR said muted) */
            if (!uh.s_ok || !uh_dev.fu_id || uh_strd[0] != 1u)
                continue;
            uh_odata[0] = 0;
            uh_ctl_start(0x21, 0x01, 0x0100, (uint32_t)uh_dev.fu_id << 8 | uh_dev.ac_if, 1, 0);
            return;
        case E_GETVOL: case E_GETVMIN: case E_GETVMAX: case E_GETVRES:
            /* the volume, channel 1: GET_CUR / MIN / MAX / RES (1/256 dB), logged only: the device's own setting
             * stays (forcing 0 dB made the receiver clip: hardware test 5) */
            if (!uh.s_ok || !uh_dev.fu_id)
                continue;
            uh_strd[0] = uh_strd[1] = 0;
            uh_ctl_start(0xA1, 0x81u + (step - E_GETVOL), 0x0201, (uint32_t)uh_dev.fu_id << 8 | uh_dev.ac_if, 2, uh_strd);
            return;
        case E_STREAM:
            if (!uh.s_ok || uh_hub.present)             /* (behind the hub: its port watch needs the SIE) */
                continue;
            uh_stream_start();
            return;
        default:
            uh.st = UH_DONE;
            uh.t_se0 = 0;
            uh.t = UH_NOW;
            uh_regs("enumerated");
            if (uh_dev.cls == 9u || (uh_dev.nif && uh_dev.itf[0].cls == 9u)) {
                if (!uh_hub.present) {
                    uh_hub_start();
                    return;
                }
                uh_note("a hub behind the hub: not supported");
            }
            uh_note("done");
            uh_result_ok();
            return;
        }
        uh_ctl_start(0x80, 6, v, idx, len, dst);
        return;
    }
}

/* one request finished (r = uh_ctl_poll's 1 or < 0): take its data, start the next */
static void uh_enum_done(int r)
{
    char l[70], *p;
    uint32_t s = uh.step;
    uh.ctl = CT_IDLE;
    if (r < 0 && s <= E_CFG) {                          /* (the SDK goes on after a failed first GET DEVICE) */
        uh_enum_fail(r);
        return;
    }
    if (r < 0 && s == E_SETIF && r != -2) {             /* no alt setting: no stream (the rest stands) */
        uh_note_fail(UH_STEP[s], r, ": no stream");
        uh.s_ok = 0;
        uh_enum_start(E_END);
        return;
    }
    if (r < 0) {                                        /* strings, SET_CONFIGURATION, the audio requests: noted, not fatal */
        uh_note_fail(UH_STEP[s], r, r == -2 ? "" : " (skipped)");
        if (r == -2) {
            uh_to_wait();
            return;
        }
        uh_enum_start(s + 1u);
        return;
    }
    p = uh_s(l, UH_STEP[s]);
    p = uh_s(p, " ok, ");
    p = uh_d(p, uh.got);
    uh_s(p, " bytes");
    uh_note(l);
    switch (s) {
    case E_DEV8:
        uh_parse_dev(uh_devd, uh.got);
        if (uh_dev.mps0 != 8u && uh_dev.mps0 != 16u && uh_dev.mps0 != 32u && uh_dev.mps0 != 64u) {
            uh_note_x("bad mps0, using 8:", uh_dev.mps0, 2);
            uh_dev.mps0 = 8;
        }
        break;
    case E_ADDR:
        uh.addr = uh.eaddr = uh.naddr;
        uh.t = UH_NOW;
        uh.phase = 1;                                   /* UH_ADDR_MS before the next request */
        return;
    case E_DEV18:
        if (uh_parse_dev(uh_devd, uh.got) || !uh_dev.dev_ok) {
            uh_enum_fail(-5);
            return;
        }
        break;
    case E_CFG9:
        if (uh.got < 9u || uh_cfgd[1] != 2u) {
            uh_enum_fail(-5);
            return;
        }
        uh_dev.total = (uint16_t)uh_le16(uh_cfgd + 2);
        if (uh_dev.total < 9u) {
            uh_enum_fail(-5);
            return;
        }
        if (uh_dev.total > sizeof uh_cfgd)
            uh_note_x("config longer than the buffer: cut, wTotalLength", uh_dev.total, 4);
        break;
    case E_CFG:
        uh_cfg_n = uh.got;
        uh_parse_cfg(uh_cfgd, uh.got);
        break;
    case E_LANG:
        p = uh_s(l, "languages");
        {
            uint32_t i;
            for (i = 2; i + 1u < uh.got && i < 10u; i += 2) {
                p = uh_s(p, " ");
                p = uh_x(p, uh_le16(uh_strd + i), 4);
            }
        }
        uh_note(l);
        break;
    case E_MAN: case E_PROD: case E_SER: {
        char *d = s == E_MAN ? uh_dev.man : s == E_PROD ? uh_dev.prod : uh_dev.ser;
        uh_str(d, UH_SLEN, uh_strd, uh.got);
        p = uh_s(l, s == E_MAN ? "manufacturer: " : s == E_PROD ? "product: " : "serial: ");
        uh_s(p, d);
        uh_note(l);
        break;
    }
    case E_SETCFG:
        uh_note_x("configured: value", uh_dev.cfg_val, 2);
        break;
    case E_SETIF:
        p = uh_s(l, "interface ");
        p = uh_d(p, uh.s_if);
        p = uh_s(p, " alt ");
        uh_d(p, uh.s_alt);
        uh_note(l);
        break;
    case E_GETRATE:
        p = uh_s(l, "rate now ");
        p = uh_d(p, uh.got >= 3u ? uh_le24(uh_strd) : 0u);
        uh_s(p, " Hz");
        uh_note(l);
        break;
    case E_GETMUTE:
        uh_note(uh.got && uh_strd[0] ? "muted: unmute" : "not muted");
        break;
    case E_GETVOL: case E_GETVMIN: case E_GETVMAX: case E_GETVRES:
        {
            static const char *const VN[4] = {"volume ch1 cur ", "volume ch1 min ", "volume ch1 max ", "volume ch1 res "};
            int32_t v = uh.got >= 2u ? (int16_t)uh_le16(uh_strd) : 0, t;
            if (s == E_GETVOL)
                uh.s_vol = (int16_t)v;
            p = uh_s(l, VN[s - E_GETVOL]);
            if (v < 0) {
                p = uh_s(p, "-");
                v = -v;
            }
            t = v * 10 / 256;                           /* 0.1 dB */
            p = uh_d(p, (uint32_t)t / 10u);
            p = uh_s(p, ".");
            p = uh_d(p, (uint32_t)t % 10u);
            uh_s(p, " dB");
            uh_note(l);
        }
        break;
    }
    uh_enum_start(s + 1u);
}

/* ------------------------------------------------------- the stream --- */
/* Milestone 2: the isochronous IN endpoint on host EP1 RX (usb_h_ep_config for iso, usb0_h_ep_read per packet),
 * polled from TIMER5 at 2 kHz, also nested in the render (the frames must not wait for it): one ReqPkt per packet,
 * the packet taken (uhs_push, usbhost_audio.c) and the next one asked for. While it runs the TIMER5 ISR owns the
 * SIE (INDEX and the CON1 port): the main loop touches no SIE register in UH_STREAM. The audio ISR plays the ring
 * (uhs_mix). */
#define UHS_HEP 1u                         /* the host endpoint used */
static volatile uint8_t uhs_svc_on;        /* uhs_service polls (TIMER5) */
static struct {
    uint32_t pkts, dataerr, errs, rearms, missed, zero, odd;   /* odd: not the size the rate asks for (+-1 frame) */
    uint16_t lastf, expect, fb;
    uint8_t fvalid, nfirst;
    uint16_t first[8];                     /* the first packets' sizes (logged by the main loop) */
    uint32_t t_pkt;                        /* UH_NOW of the last packet */
    uint8_t tick, dc, iu, dc_drop, iu_drop;   /* DEVCTL / INTRUSB (accumulated) as last seen; at the device bit's drop */
    uint32_t t_drop, pk_drop, kicks;       /* the drop: when (0 none), the packets until then; sessions asked for */
} uhsv;

static void uhs_rearm(void)
{
    usbh_wr(UH_RXTYPE, 0x10u | (uh.s_ep & 15u));      /* (usb0_h_ep_read: xfer 1 = iso << 4 | the target ep) */
    usbh_wr(UH_RXCSR1, UH_RX_REQPKT);
}

/* TIMER5 (main.c), every 0.5 ms */
static void uhs_service(void)
{
    uint32_t c, n, f, d;
    if (!uhs_svc_on)
        return;
    usbh_wr(UH_INDEX, UHS_HEP);
    c = usbh_rd(UH_RXCSR1);
    if (c & UH_RX_RXPKTRDY) {
        n = usbh_rd(UH_RXCOUNT1) | usbh_rd(UH_RXCOUNT2) << 8;
        if (n > sizeof uh_iso - 4u)
            n = sizeof uh_iso - 4u;
        f = usbh_rd(UH_FRAME1) | (usbh_rd(UH_FRAME2) & 7u) << 8;
        if (uhsv.fvalid) {
            d = (f - uhsv.lastf) & 0x7FFu;
            if (d > 1u)
                uhsv.missed += d - 1u;
        }
        uhsv.lastf = (uint16_t)f;
        uhsv.fvalid = 1;
        fm1_usb_rx_sync();                              /* (the DMA wrote uh_iso) */
        if (c & UH_RX_DATAERR)
            uhsv.dataerr++;                             /* a CRC error: the packet left out */
        else if (n)
            uhs_push(uh_iso, n);
        else
            uhs_push_silence(uh.s_rate / 1000u);        /* empty: a frame of silence, no fade, no new prime */
        uhsv.pkts++;
        uhsv.t_pkt = UH_NOW;
        if (!n)
            uhsv.zero++;
        else if (n + uhsv.fb < uhsv.expect || n > uhsv.expect + uhsv.fb)
            uhsv.odd++;
        if (uhsv.nfirst < 8u)
            uhsv.first[uhsv.nfirst++] = (uint16_t)n;
        usbh_wr(UH_INDEX, UHS_HEP);
        usbh_wr(UH_RXCSR1, UH_RX_FLUSH);                /* (the SDK, once the packet is taken) */
        uhs_rearm();
    } else if (c & UH_RX_ERROR) {
        uhsv.errs++;
        uhs_rearm();
    } else if (!(c & UH_RX_REQPKT)) {
        uhsv.rearms++;                                  /* (none asked for: the first, or lost) */
        uhs_rearm();
    }
    if (++uhsv.tick >= 10u) {                           /* every 5 ms: DEVCTL and INTRUSB (read clears it) */
        uint32_t dc = usbh_rd(UH_DEVCTL), iu = usbh_rd(UH_INTRUSB) & ~0x08u;
        uhsv.tick = 0;
        uhsv.dc = (uint8_t)dc;
        uhsv.iu |= (uint8_t)iu;
        if (!uhsv.t_drop && !(dc & (UH_DC_FSDEV | UH_DC_LSDEV))) {   /* the device bit went: note it once */
            uhsv.t_drop = UH_NOW | 1u;
            uhsv.dc_drop = (uint8_t)dc;
            uhsv.iu_drop = uhsv.iu;
            uhsv.pk_drop = uhsv.pkts;
        }
        if ((uh.opt & UHO_STREAM_KICK) && !(dc & (UH_DC_FSDEV | UH_DC_LSDEV)) && uhsv.kicks < 1000u) {
            usbh_session();
            uhsv.kicks++;
        }
    }
}

static void uh_stream_start(void)
{
    uint32_t k;
    char l[60], *p;
    for (k = 0; k < sizeof uhsv; k++)
        ((uint8_t *)&uhsv)[k] = 0;
    uhsv.fb = (uint16_t)(uh.s_ch * uh.s_sb);
    uhsv.t_pkt = UH_NOW;
    uh.logged_drop = 0;
    uhsv.expect = (uint16_t)(uh.s_rate / 1000u * uhsv.fb);
    usbh_faddr(uh.eaddr);
    usbh_ep_rx_config(UHS_HEP, 1, uh.s_int, uh_iso);   /* usb_h_ep_config, iso */
    uhs_start(uh.s_rate, uh.s_ch, uh.s_sb);
#ifdef UH_STREAM_HOOK
    UH_STREAM_HOOK();                                   /* (the includer's own vocoder: it reads the stream itself) */
#else
    if (!vc.ready)
        vc_init();                                      /* (the controls kept from one stream to the next) */
    UH_KEYS_TO_VOCODER(!uh.vox_off);                    /* VOX: the keys play the vocoder's carrier, not the track */
    uhs.vc = !uh.vox_off;
#endif
    usbh_wr(UH_INDEX, UHS_HEP);
    uhs_rearm();
    RING_PUBLISH();
    uhs_svc_on = 1;                                     /* from here TIMER5 owns the SIE */
    uh.st = UH_STREAM;
    uh.t_stream = uh.t_sec = UH_NOW;
    uh.pkts_last = uh.pkts_sec = 0;
    uh.logged_first = 0;
    uh.view = 0;
    p = uh_s(l, "streaming: expect ");
    p = uh_d(p, uhsv.expect);
    uh_s(p, " bytes a frame");
    uh_note(l);
    uh_result(1, "streaming");
}

/* the stream ends (unplugged, a button, a limit): TIMER5 lets the SIE go, the audio fades out */
static void uh_stream_stop(const char *why)
{
    char l[100], *p;
    uhs_svc_on = 0;
    RING_PUBLISH();
    uhs_stop();
    uhs.vc = 0;
    UH_KEYS_TO_VOCODER(0);
    p = uh_s(l, "stream stopped (");
    p = uh_s(p, why);
    p = uh_s(p, "): packets ");
    p = uh_d(p, uhsv.pkts);
    p = uh_s(p, " missed ");
    p = uh_d(p, uhsv.missed);
    p = uh_s(p, " err ");
    p = uh_d(p, uhsv.errs + uhsv.dataerr);
    p = uh_s(p, " odd ");
    p = uh_d(p, uhsv.odd);
    p = uh_s(p, " under ");
    p = uh_d(p, uhs.underruns);
    p = uh_s(p, " over ");
    p = uh_d(p, uhs.overruns);
    p = uh_s(p, " L!=R pkts ");
    uh_d(p, uhs.lrdiff);
    uh_note(l);
    uh.t_start = UH_NOW;                                /* (the 120 s from here) */
}

/* the main loop's part while streaming: no SIE (TIMER5 owns it), the lines, the rate per second, the log */
static void uh_stream_poll(void)
{
    char l[90], *p;
    uint32_t k;
    if (!uh.logged_first && (uhsv.nfirst >= 8u || UH_NOW - uh.t_stream > 1000u)) {
        uh.logged_first = 1;
        p = uh_s(l, "first packets:");
        for (k = 0; k < uhsv.nfirst; k++) {
            p = uh_s(p, " ");
            p = uh_d(p, uhsv.first[k]);
        }
        if (!uhsv.nfirst)
            uh_s(p, " none");
        uh_note(l);
    }
    if (UH_NOW - uh.t_sec >= 1000u) {
        uint32_t n = uhsv.pkts;
        uh.pkts_sec = n - uh.pkts_last;
        uh.pkts_last = n;
        uh.t_sec = UH_NOW;
        if ((UH_NOW - uh.t_stream) / 1000u % 10u == 0u) {   /* every 10 s a line */
            p = uh_s(l, "stream: pkts/s ");
            p = uh_d(p, uh.pkts_sec);
            p = uh_s(p, " missed ");
            p = uh_d(p, uhsv.missed);
            p = uh_s(p, " err ");
            p = uh_d(p, uhsv.errs + uhsv.dataerr);
            p = uh_s(p, " fill ");
            p = uh_d(p, uhs.w - uhs.r);
            p = uh_s(p, " under ");
            p = uh_d(p, uhs.underruns);
            p = uh_s(p, " over ");
            uh_d(p, uhs.overruns);
            uh_note(l);
        }
    }
    if (uhsv.t_drop && !uh.logged_drop) {              /* the device bit dropped: what the ISR saw */
        uh.logged_drop = 1;
        p = uh_s(l, "stream: devctl ");
        p = uh_x(p, uhsv.dc_drop, 2);
        p = uh_s(p, " (device bit gone) after ");
        p = uh_d(p, uhsv.t_drop - uh.t_stream);
        p = uh_s(p, " ms, packets ");
        p = uh_d(p, uhsv.pk_drop);
        p = uh_s(p, ", intrusb so far ");
        p = uh_x(p, uhsv.iu_drop, 2);
        p = uh_s(p, ", D+ ");
        p = uh_d(p, usbh_dp());
        p = uh_s(p, " D- ");
        uh_d(p, usbh_dm());
        uh_note(l);
    }
    /* unplugged: no packet for 50 ms and both lines low (the lines alone read low once the session is gone, and the
     * device bit alone drops while the packets go on: hardware test 4) */
    k = UH_NOW - uhsv.t_pkt;
    if (k > 50u && usbh_se0()) {
        if (!uh.t_se0)
            uh.t_se0 = UH_NOW | 1u;
    } else {
        uh.t_se0 = 0;
    }
    if ((uh.t_se0 && UH_NOW - uh.t_se0 > 20u) || k > 500u) {
        p = uh_s(l, k > 500u && !uh.t_se0 ? "no packets for 500 ms" : "no packets, lines low");
        p = uh_s(p, ": devctl ");
        p = uh_x(p, uhsv.dc, 2);
        p = uh_s(p, " intrusb ");
        uh_x(p, uhsv.iu, 2);
        uh_note(l);
        uh.t_se0 = 0;
        uh_stream_stop("packets stopped");
        uh_to_wait();
    }
}

/* --------------------------------------------------------- the hub --- */
/* USB 2.0 chapter 11, the least of it: the hub descriptor, PORT_POWER on every port, GetPortStatus polled (not
 * the status change endpoint), PORT_RESET on the first port with a device (debounced 100 ms), C_PORT_RESET and
 * C_PORT_CONNECTION cleared, then the device enumerated from address 0 to UH_DADDR through the hub, as a direct
 * one. While it is in use its port is read once a second (unplugged: scan again). */
enum { HF_PORT_RESET = 4, HF_PORT_POWER = 8, HF_C_PORT_CONNECTION = 16, HF_C_PORT_RESET = 20 };

static void uh_dev_clear(void)
{
    uint32_t k;
    for (k = 0; k < sizeof uh_dev; k++)
        ((uint8_t *)&uh_dev)[k] = 0;
    uh_cfg_n = 0;
    uh.follow = 1;
}

static void uh_hub_req(uint32_t rt, uint32_t req, uint32_t val, uint32_t idx, uint32_t len)
{
    uh.hubx = 1;
    uh.addr = UH_ADDR;
    uh_ctl_start(rt, req, val, idx, len, uh_strd);
}

static void uh_hub_pause(uint32_t ms, uint32_t next)
{
    uh.ctl = CT_IDLE;
    uh.hstep = H_WAIT;
    uh.hwait = (uint8_t)next;
    uh.hwait_ms = ms;
    uh.t = UH_NOW;
}

static void uh_hub_go(uint32_t step)
{
    uh.hstep = (uint8_t)step;
    switch (step) {
    case H_DESC: uh_hub_req(0xA0, 6, 0x2900, 0, 15); break;                    /* GET_DESCRIPTOR hub */
    case H_POWER: uh_hub_req(0x23, 3, HF_PORT_POWER, uh.hport, 0); break;     /* SET_FEATURE PORT_POWER */
    case H_SCAN: uh_hub_req(0xA3, 0, 0, uh.hport, 4); break;                  /* GET_STATUS port */
    case H_RESET:
        uh.t_hreset = UH_NOW;
        uh_hub_req(0x23, 3, HF_PORT_RESET, uh_hub.port, 0);
        break;
    case H_RSTAT: case H_WATCH: uh_hub_req(0xA3, 0, 0, uh_hub.port, 4); break;
    case H_CLRRST: uh_hub_req(0x23, 1, HF_C_PORT_RESET, uh_hub.port, 0); break;
    case H_CLRCONN: uh_hub_req(0x23, 1, HF_C_PORT_CONNECTION, uh_hub.port, 0); break;
    case H_ENUM:                                        /* the device behind the hub, as a direct one */
        uh_dev_clear();
        uh_dev.mps0 = 8;
        uh.eaddr = 0;
        uh.naddr = UH_DADDR;
        uh.phase = 0;
        uh.st = UH_ENUM;
        uh_note("enumerating the device behind the hub");
        uh_enum_start(E_DEV8);
        break;
    default:
        break;
    }
}

static void uh_hub_start(void)
{
    uint32_t k;
    uh_hub.present = 1;
    uh_hub.vid = uh_dev.vid;
    uh_hub.pid = uh_dev.pid;
    uh_hub.mps0 = uh_dev.mps0;
    uh_hub.port = uh_hub.bad = uh_hub.cand = 0;
    for (k = 0; k <= UH_NPORT; k++)
        uh_hub.pst[k] = uh_hub.pch[k] = 0;
    uh.st = UH_HUB;
    uh_note("a hub: its ports");
    uh_hub_go(H_DESC);
}

/* a port's status into the log when it changed */
static void uh_hub_port_note(uint32_t port, uint32_t st, uint32_t ch)
{
    char l[90], *p;
    if (port > UH_NPORT || (st == uh_hub.pst[port] && ch == uh_hub.pch[port]))
        return;
    uh_hub.pst[port] = (uint16_t)st;
    uh_hub.pch[port] = (uint16_t)ch;
    p = uh_s(l, "port ");
    p = uh_d(p, port);
    p = uh_s(p, " status ");
    p = uh_x(p, st, 4);
    p = uh_s(p, " change ");
    p = uh_x(p, ch, 4);
    p = uh_s(p, st & 0x100u ? " power" : " no power");
    if (st & 1u)
        p = uh_s(p, st & 0x200u ? " device LS" : st & 0x400u ? " device HS" : " device FS");
    if (st & 2u)
        p = uh_s(p, " enabled");
    if (st & 8u)
        p = uh_s(p, " overcurrent");
    if (st & 0x10u)
        uh_s(p, " resetting");
    uh_note(l);
}

/* back to scanning from port 1 (the port in hand given up) */
static void uh_hub_rescan(uint32_t pause)
{
    uh_hub.port = 0;
    uh_hub.cand = 0;
    uh.hport = 1;
    uh_hub_pause(pause, H_SCAN);
}

static uint32_t uh_hub_pwr_ms(void) { return uh_hub.pwr2good * 2u < 100u ? 100u : uh_hub.pwr2good * 2u; }

/* one hub request finished (uh_ctl_poll's 1 or < 0) */
static void uh_hub_done(int r)
{
    uint32_t st = uh_le16(uh_strd), ch = uh_le16(uh_strd + 2), s = uh.hstep;
    char l[90], *p;
    uh.ctl = CT_IDLE;
    if (r < 0) {
        uh_note_fail(UH_HSTEP[s], r, "");
        if (r == -2) {
            uh_to_wait();
        } else if (s == H_DESC) {                       /* not used as a hub */
            uh_hub.present = 0;
            uh.st = UH_DONE;
            uh_note("done");
            uh_result_ok();
        } else if (s == H_POWER) {
            r = 1;                                      /* (noted; the next port all the same) */
        } else {
            uh_hub_rescan(200);
        }
        if (r < 0)
            return;
    }
    switch (s) {
    case H_DESC:
        if (uh.got < 7u || uh_strd[1] != 0x29u) {
            uh_note_x("hub descriptor: bad, type", uh.got > 1u ? uh_strd[1] : 0u, 2);
            uh_hub.present = 0;
            uh.st = UH_DONE;
            return;
        }
        uh_hub.nports = uh_strd[2] > UH_NPORT ? (uint8_t)UH_NPORT : uh_strd[2];
        uh_hub.chars = (uint16_t)uh_le16(uh_strd + 3);
        uh_hub.pwr2good = uh_strd[5];
        p = uh_s(l, "hub: ports ");
        p = uh_d(p, uh_strd[2]);
        p = uh_s(p, " chars ");
        p = uh_x(p, uh_hub.chars, 4);
        p = uh_s(p, (uh_hub.chars & 3u) == 0u ? " (ganged power)" : (uh_hub.chars & 3u) == 1u ? " (per-port power)" : "");
        p = uh_s(p, " pwr2good ");
        p = uh_d(p, uh_strd[5] * 2u);
        p = uh_s(p, " ms, ");
        p = uh_d(p, uh_strd[6]);
        uh_s(p, " mA");
        uh_note(l);
        if (!uh_hub.nports) {
            uh.st = UH_DONE;
            return;
        }
        uh.hport = 1;
        if (uh.opt & UHO_NO_PORT_POWER) {
            uh_note("PORT_POWER left out (hostopt)");
            uh_hub_go(H_SCAN);
            return;
        }
        uh_hub_go(H_POWER);
        return;
    case H_POWER:
        if (++uh.hport <= uh_hub.nports) {
            if (uh.opt & UHO_POWER_SLOW)
                uh_hub_pause(uh_hub_pwr_ms(), H_POWER);
            else
                uh_hub_go(H_POWER);
            return;
        }
        uh_note("ports powered");
        uh.hport = 1;
        uh_hub_pause(uh_hub_pwr_ms(), H_SCAN);
        return;
    case H_SCAN:
        uh_hub_port_note(uh.hport, st, ch);
        if (uh.hport == uh_hub.bad && !(st & 1u)) {
            uh_hub.bad = 0;                             /* the device that failed is gone: the port again */
            uh_note("port free again");
        }
        if ((st & 1u) && uh.hport != uh_hub.bad) {
            if (uh_hub.cand != uh.hport) {               /* connected: debounced 100 ms (11.8.2) */
                uh_hub.cand = uh.hport;
                uh_hub.t_cand = UH_NOW;
            } else if (UH_NOW - uh_hub.t_cand >= 100u) {
                uh_hub.cand = 0;
                uh_hub.port = uh.hport;
                uh.tries = 0;
                p = uh_s(l, "device on port ");
                uh_d(p, uh_hub.port);
                uh_note(l);
                uh_hub_go(H_RESET);
                return;
            }
        } else if (uh_hub.cand == uh.hport) {
            uh_hub.cand = 0;
        }
        if (++uh.hport > uh_hub.nports) {
            uh.hport = 1;
            uh_hub_pause(50, H_SCAN);
        } else {
            uh_hub_go(H_SCAN);
        }
        return;
    case H_RESET:
        uh_hub_pause(20, H_RSTAT);                      /* the hub drives it 10..20 ms */
        return;
    case H_RSTAT:
        uh_hub_port_note(uh_hub.port, st, ch);
        if (!(st & 1u)) {
            uh_note("device gone during the port reset");
            uh_hub_rescan(0);
        } else if ((ch & 0x10u) || !(st & 0x10u)) {      /* C_PORT_RESET, or no longer resetting */
            uh_hub_go(H_CLRRST);
        } else if (UH_NOW - uh.t_hreset > 1000u) {
            uh_note("port reset does not end");
            uh_result(0, "hub port reset does not end");
            uh_hub.bad = uh_hub.port;
            uh_hub_rescan(0);
        } else {
            uh_hub_pause(10, H_RSTAT);
        }
        return;
    case H_CLRRST:
        uh_hub_go(H_CLRCONN);
        return;
    case H_CLRCONN:
        st = uh_hub.pst[uh_hub.port];
        if (st & 0x200u) {
            p = uh_s(l, "low-speed device on port ");
            p = uh_d(p, uh_hub.port);
            uh_s(p, ": needs PRE preambles, not supported");
            uh_note(l);
            uh_result(0, "low-speed device behind the hub");
            uh_hub.bad = uh_hub.port;
            uh_hub_rescan(0);
            return;
        }
        if (!(st & 2u))
            uh_note("port not enabled after the reset: trying anyway");
        uh_hub_pause(UH_RECOVERY_MS, H_ENUM);           /* reset recovery (>= 10 ms) */
        return;
    case H_WATCH:
        uh_hub_port_note(uh_hub.port, st, ch);
        if ((st & 1u) && !(ch & 1u)) {
            uh.st = UH_DONE;
            uh.t = UH_NOW;
            return;
        }
        uh_note("device behind the hub unplugged");
        uh_dev_clear();
        uh_hub_rescan(0);
        return;
    default:
        return;
    }
}

/* the hub and its ports as lines (the screen, `hoststate`) */
static uint32_t uh_hub_lines(char (*ln)[UH_LW], uint32_t max)
{
    uint32_t n = 0, k;
    char *p;
    if (!uh_hub.present || !max)
        return 0;
    p = uh_s(ln[n], "HUB ");
    p = uh_x(p, uh_hub.vid, 4);
    p = uh_s(p, ":");
    p = uh_x(p, uh_hub.pid, 4);
    p = uh_s(p, "  ");
    p = uh_d(p, uh_hub.nports);
    uh_s(p, " PORTS");
    n++;
    for (k = 1; k <= uh_hub.nports && n < max; k++, n++) {
        uint32_t st = uh_hub.pst[k];
        p = uh_s(ln[n], "  P");
        p = uh_d(p, k);
        p = uh_s(p, st & 1u ? (st & 0x200u ? " DEVICE LS" : " DEVICE FS") : st & 0x100u ? " EMPTY" : " NO POWER");
        if (st & 2u)
            p = uh_s(p, " ENABLED");
        if (k == uh_hub.port)
            p = uh_s(p, " IN USE");
        if (k == uh_hub.bad)
            uh_s(p, " FAILED");
    }
    return n;
}

/* the state machine, called as often as it can be (usbhost_session) */
static void uh_poll(void)
{
    uint32_t dc;
    int r;
    switch (uh.st) {
    case UH_WAIT:                                        /* a device pulls D+ (full speed) or D- (low) up */
        if (usbh_se0()) {
            uh.t_conn = 0;
        } else if (!uh.t_conn) {
            uh.t_conn = UH_NOW | 1u;
        } else if (UH_NOW - uh.t_conn >= 100u) {        /* steady for 100 ms (USB: >= 100 ms before the reset) */
            char l[40], *p = uh_s(l, "connect: D+ ");
            uh.quiet = 0;                               /* (a probe found something: from here, all of it) */
            p = uh_d(p, usbh_dp());
            p = uh_s(p, " D- ");
            uh_d(p, usbh_dm());
            uh_note(l);
            {
                uint32_t k;
                for (k = 0; k < sizeof uh_dev; k++)
                    ((uint8_t *)&uh_dev)[k] = 0;
            }
            uh_cfg_n = 0;
            uh.follow = 1;
            uh.tries = 0;
            uh.reset_ms = UH_RESET_MS;
            uh.st = UH_ATTACH;
            uh.t = uh.t_kick = UH_NOW;
            break;
        }
        if (UH_NOW - uh.t_poke >= 500u) {               /* the session dropped (no device): start it again */
            uh.t_poke = UH_NOW;
            dc = usbh_devctl();
            if (!(dc & UH_DC_HOSTMODE) && !(uh.opt & UHO_NO_SESSION)) {
                usbh_session();                         /* (quietly: this repeats while nothing is plugged in) */
                uh.pokes++;
            }
        }
        break;
    case UH_ATTACH:                                      /* usb_h_chirp_and_reset: the speed from DEVCTL */
        uh_watch_intr();
        dc = usbh_devctl();
        uh_kick(dc);
        if (uh_link_ok(dc) || UH_NOW - uh.t > UH_SESSION_MS) {
            uint32_t ls = dc & UH_DC_FSDEV ? 0u : dc & UH_DC_LSDEV ? 1u : usbh_dm() && !usbh_dp();
            if (!(dc & (UH_DC_FSDEV | UH_DC_LSDEV)))
                uh_note_x("devctl shows no device, speed from the lines; devctl", dc, 2);
            uh.speed = ls ? 1u : 2u;
            usbh_low_speed(ls);
            uh_note(ls ? "low speed" : "full speed");
            uh_reset_start();
        }
        break;
    case UH_RESET:
        if (uh.phase == 0 && UH_NOW - uh.t >= uh.reset_ms) {
            usbh_bus_reset(0);
            uh.phase = 1;
            uh.t = uh.t_kick = UH_NOW;
            uh_regs("bus reset done");
        } else if (uh.phase == 1 && UH_NOW - uh.t >= UH_RECOVERY_MS) {
            dc = usbh_devctl();                         /* the first SETUP: once DEVCTL shows the device, or 500 ms */
            if (!uh_link_ok(dc) && UH_NOW - uh.t < UH_RECOVERY_MS + UH_SESSION_MS) {
                uh_kick(dc);                            /* (only if host mode went) */
                break;
            }
            if (!uh_link_ok(dc))
                uh_note_x("after the reset devctl still (going on)", dc, 2);
            else if (UH_NOW - uh.t > UH_RECOVERY_MS + 5u)
                uh_note_x("devctl ready after the reset:", dc, 2);
            uh.st = UH_ENUM;
            uh.phase = 0;
            uh_dev.mps0 = 8;
            uh_enum_start(E_DEV8);
        }
        break;
    case UH_ENUM:
        uh_watch_intr();
        if (uh.ctl == CT_IDLE) {                         /* the wait after SET_ADDRESS */
            if (uh.phase && UH_NOW - uh.t >= UH_ADDR_MS) {
                uh.phase = 0;
                uh_enum_start(E_DEV18);
            }
            break;
        }
        r = uh_ctl_poll();
        if (r)
            uh_enum_done(r);
        break;
    case UH_STREAM:
        uh_stream_poll();
        break;
    case UH_HUB:
        uh_watch_intr();
        if (uh.ctl == CT_IDLE) {
            if (uh.hstep == H_WAIT && UH_NOW - uh.t >= uh.hwait_ms)
                uh_hub_go(uh.hwait);
            break;
        }
        r = uh_ctl_poll();
        if (r)
            uh_hub_done(r);
        break;
    case UH_DONE:
        if (uh_hub.present && uh_hub.port && UH_NOW - uh.t >= 1000u) {
            uh.st = UH_HUB;                              /* the device behind the hub: still there? */
            uh_hub_go(H_WATCH);
            break;
        }
        /* fall through */
    case UH_FAIL:                                        /* unplugged: wait for the next one */
        dc = usbh_devctl();
        uh_kick(dc);                                    /* (idle: only if host mode went) */
        if (!usbh_se0()) {
            uh.t_se0 = 0;
        } else if (!uh.t_se0) {
            uh.t_se0 = UH_NOW | 1u;
        } else if (UH_NOW - uh.t_se0 > 200u) {
            uh.t_se0 = 0;
            uh_note("device unplugged");
            uh_to_wait();
        }
        break;
    default:
        break;
    }
}

