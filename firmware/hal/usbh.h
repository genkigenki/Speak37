/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (makkuro) */
/* USB0 (FUSB, 0x11800) as a full-speed HOST, register level only. The JieLi AC79 SDK keeps its host HAL
 * in a binary library (cpu.a: usb.c.o, gpio.c.o); each routine here is translated from that object's
 * LLVM IR, id 0 (USB0) only, and names the SDK function it follows. The protocol (enumeration,
 * descriptors) is src/usbhost.c; the device side stays fm1_usb.h + src/usb.c. Polled: nothing here
 * enables an interrupt (the SDK's usb_sie_enable, CON0 bit 11, is left out).
 *
 *   usbh_sie_init()          pads to host (15k pull-downs on D+ / D-), PHY on, SIE out of reset, host mode
 *   usbh_sie_close()         controller off, pads back to inputs (fm1_usb_reset's state)
 *   usbh_force_reset()       D+ / D- driven low as GPIO (SE0 on the bus); usbh_sie_close undoes it
 *   usbh_se0()               no device: D+ and D- both low
 *   usbh_wr / usbh_rd        the SIE through CON1 (MUSB byte map, as usb.c's sie_wr / sie_rd)
 *   usbh_bus_reset(on)       POWER.Reset (bit 3) set / cleared
 *   usbh_ep0_*               EP0 through its DMA buffer (EP0_ADR / EP0_CNT) and CSR0 (index 0)
 *   usbh_ep_rx_config / usbh_ep_rx_*   an IN endpoint (not used by milestone 1: the iso stream later)
 *
 * USB_IO_CON0 (0x51000), the SDK's names (include_lib/driver/cpu/wl82/asm/usb.h) and gpio.c's usb_* helpers
 * (pin 148 = D+, 149 = D-): 0 / 1 DPOUT / DMOUT, 2 / 3 DPIE / DMIE (1 input), 4 / 5 DPPD / DMPD (15k pull-down),
 *   6 / 7 DPPU / DMPU, 8 IO_PU_MODE, 9 / 10 DPDIE / DMDIE (digital input), 11 IO_MODE (GPIO), 12 SR (slew rate),
 *   13 / 14 DPDIEH / DMDIEH.
 * CON0 (usb.h): 0 PHY_ON, 1 LOW_SPEED, 2 USB_NRST (SIE out of reset), 3 TM1, 4 CID (the ID pin as the core sees
 * it: 0 = A device, the host), 5 VBUS (VBUS as the core sees it, set by the CPU), 6 USB_TEST, 9 PDCHKDP, 10 SOFIE,
 * 11 SIEIE, 12 CLR_SOF_PND, 13 SOF_PND, 14 SIE_PND, 15 CHKDPO, 16 / 17 DM_SE / DP_SE (line state). The host init
 * sets 0x25: PHY_ON, USB_NRST and VBUS already forced; the device attach 0x3D (also TM1, CID). The SDK has no other
 * session / A-valid override for USB0 (HUSB_CPU_AVALID / SESSEND / VBUSVALID belong to USB1's PHY). */
#pragma once
#include <stdint.h>
#include "fm1_usb.h"
#include "fm1_time.h"

#define USBH_IO_CON1 (*(volatile uint32_t *)0x51004u)

enum { UH_FADDR = 0, UH_POWER = 1, UH_INTRUSB = 6, UH_FRAME1 = 12, UH_FRAME2 = 13, UH_INTRTX1E = 7, UH_INTRTX2E = 8, UH_INTRRX1E = 9,
       UH_INTRRX2E = 10, UH_INTRUSBE = 11, UH_INDEX = 14, UH_DEVCTL = 15, UH_TXMAXP = 16, UH_CSR0 = 17,
       UH_RXMAXP = 19, UH_RXCSR1 = 20, UH_RXCSR2 = 21, UH_COUNT0 = 22, UH_RXCOUNT1 = 22, UH_RXCOUNT2 = 23,
       UH_TXTYPE = 24, UH_TXINTERVAL = 25, UH_RXTYPE = 26, UH_RXINTERVAL = 27 };
/* CSR0 in host mode (SDK usb.h CSR0H_*; DISPING, bit 11, is lost on USB0: CON1 writes 8 bits) */
enum { UH_C0_RXPKTRDY = 0x01, UH_C0_TXPKTRDY = 0x02, UH_C0_RXSTALL = 0x04, UH_C0_SETUPPKT = 0x08,
       UH_C0_ERROR = 0x10, UH_C0_REQPKT = 0x20, UH_C0_STATUSPKT = 0x40, UH_C0_NAKTIMEOUT = 0x80 };
/* DEVCTL: 0 session, 1 host request, 2 host mode, 5 low-speed device, 6 full-speed device */
enum { UH_DC_SESSION = 0x01, UH_DC_HOSTREQ = 0x02, UH_DC_HOSTMODE = 0x04, UH_DC_LSDEV = 0x20,
       UH_DC_FSDEV = 0x40 };
/* RXCSR in host mode (SDK RXCSRH_*, low byte; 0x20 ReqPkt, 0x40 RxStall as usb0_h_ep_read uses them) */
enum { UH_RX_RXPKTRDY = 0x01, UH_RX_ERROR = 0x04, UH_RX_DATAERR = 0x08, UH_RX_FLUSH = 0x10,
       UH_RX_REQPKT = 0x20, UH_RX_STALL = 0x40, UH_RX_CLRTOG = 0x80 };

static uint32_t usbh_sie_ot;                   /* SIE accesses that timed out (the log shows it) */

/* musb_write_usb (id 0): the 20000-iteration wait of the SDK, then give up */
static void usbh_wr(uint32_t r, uint32_t v)
{
    uint32_t n = 20000;
    if (!fm1_usb_sie_on())
        return;
    fm1_usb_sie_wr_start(r, v);
    while (!fm1_usb_sie_done())
        if (!--n) {
            usbh_sie_ot++;
            return;
        }
}

/* musb_read_usb (id 0): 0 on a timeout */
static uint32_t usbh_rd(uint32_t r)
{
    uint32_t n = 20000;
    if (!fm1_usb_sie_on())
        return 0;
    fm1_usb_sie_rd_start(r);
    while (!fm1_usb_sie_done())
        if (!--n) {
            usbh_sie_ot++;
            return 0;
        }
    return fm1_usb_sie_data();
}

/* usb_h_sie_init (id 0), with gpio.c's usb_set_die / usb_set_pull_up / usb_set_pull_down for pins 149, 148
 * inlined in the SDK's order. Its delay(1000) is a busy loop of unknown length: 20 us here */
static void usbh_sie_init(void)
{
    FM1_CLK_CON1 &= ~3u;                        /* USB clock = PLL48M (fm1_usb_attach; the SDK's boot does it) */
    FM1_USB_IO_CON0 = 0;
    USBH_IO_CON1 = 0;
    fm1_delay_us(20);
    FM1_USB_IO_CON0 |= 1u << 10;                /* D- digital input */
    FM1_USB_IO_CON0 |= 1u << 9;                 /* D+ digital input */
    FM1_USB_IO_CON0 &= ~(1u << 7);              /* no pull-ups */
    FM1_USB_IO_CON0 &= ~(1u << 6);
    FM1_USB_IO_CON0 |= 1u << 5;                 /* 15k pull-downs: the host side */
    FM1_USB_IO_CON0 |= 1u << 4;
    FM1_USB_IO_CON0 &= ~(1u << 8);              /* USB function, not GPIO */
    FM1_USB_IO_CON0 &= ~(1u << 11);
    FM1_USB_CON0 |= 0x25u;                      /* PHY on, SIE out of reset, host */
    fm1_delay_us(20);
}

/* USB_IO_CON0 SR (slew rate, bit 12): usb_g_sie_init sets it for the device, usb_h_sie_init leaves it 0 (hostopt) */
static void usbh_io_slew(uint32_t on)
{
    if (on)
        FM1_USB_IO_CON0 |= 1u << 12;
    else
        FM1_USB_IO_CON0 &= ~(1u << 12);
}

/* usb_host_init: no USB / TX / RX interrupt sources (write_intr_usbe 0, clr_intr_txe / rxe all) */
static void usbh_intr_off(void)
{
    usbh_wr(UH_INTRUSBE, 0);
    usbh_wr(UH_INTRTX1E, 0);
    usbh_wr(UH_INTRTX2E, 0);
    usbh_wr(UH_INTRRX1E, 0);
    usbh_wr(UH_INTRRX2E, 0);
}

/* usb_h_dev_status (id 0): 1 = D+ and D- both low (SE0: nothing attached, or a reset) */
static uint32_t usbh_se0(void) { return (FM1_USB_CON0 & (3u << 16)) == 0u; }
static uint32_t usbh_dp(void) { return (FM1_USB_CON0 >> 17) & 1u; }      /* usb_read_dp_se */
static uint32_t usbh_dm(void) { return (FM1_USB_CON0 >> 16) & 1u; }      /* usb_read_dm_se */

static uint32_t usbh_con0(void) { return FM1_USB_CON0; }                 /* (for the log) */
static uint32_t usbh_io_con0(void) { return FM1_USB_IO_CON0; }

/* usb_set_low_speed (id 0) */
static void usbh_low_speed(uint32_t on)
{
    if (on)
        FM1_USB_CON0 |= 2u;
    else
        FM1_USB_CON0 &= ~2u;
}

/* usb_h_chirp_and_reset (id 0), its parts: DEVCTL = 3 starts the session; the speed from DEVCTL
 * (FSDev / LSDev); POWER.Reset set (the SDK rewrites it until a read shows it), held reset_delay ms, cleared */
static void usbh_session(void) { usbh_wr(UH_DEVCTL, UH_DC_SESSION | UH_DC_HOSTREQ); }
static uint32_t usbh_devctl(void) { return usbh_rd(UH_DEVCTL); }
static void usbh_bus_reset(uint32_t on)
{
    uint32_t p, n = 0;
    if (!on) {
        usbh_wr(UH_POWER, usbh_rd(UH_POWER) & 0xF7u);
        return;
    }
    do {
        p = usbh_rd(UH_POWER);
        usbh_wr(UH_POWER, (p & 0xF7u) | 0x08u);
    } while (!(p & 0x08u) && ++n < 100u);
}

/* usb_write_faddr / usb_write_power (id 0); txfuncaddr is a no-op on USB0: FADDR addresses the device */
static void usbh_faddr(uint32_t a) { usbh_wr(UH_FADDR, a); }
static void usbh_power(uint32_t v) { usbh_wr(UH_POWER, v); }

/* usb_set_dma_taddr (id 0, ep 0): EP0's DMA buffer, both directions */
static void usbh_ep0_buf(void *p)
{
    FM1_USB_EP0_ADR = (uint32_t)(uintptr_t)p;
    __asm__ volatile("csync" ::: "memory");
}

/* usb_write_ep0 (id 0): the bytes into EP0's DMA buffer, EP0_CNT = len */
static void usbh_ep0_write(uint8_t *buf, const uint8_t *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        buf[i] = s[i];
    __asm__ volatile("csync" ::: "memory");
    FM1_USB_EP_CNT(0) = n;
}

/* usb_read_ep0 (id 0): out of EP0's DMA buffer (ssync first, as fm1_usb_rx_sync) */
static void usbh_ep0_read(uint8_t *d, const uint8_t *buf, uint32_t n)
{
    uint32_t i;
    __asm__ volatile("ssync" ::: "memory");
    for (i = 0; i < n; i++)
        d[i] = buf[i];
}

/* usb_write_csr0 / usb_read_csr0 / usb_read_count0 (id 0): INDEX 0 first */
static void usbh_csr0_wr(uint32_t v)
{
    usbh_wr(UH_INDEX, 0);
    usbh_wr(UH_CSR0, v);
}
static uint32_t usbh_csr0(void)
{
    usbh_wr(UH_INDEX, 0);
    return usbh_rd(UH_CSR0);
}
static uint32_t usbh_count0(void)
{
    usbh_wr(UH_INDEX, 0);
    return usbh_rd(UH_COUNT0);
}

/* usb_h_ep_config (id 0, an IN endpoint, ie = 0): DMA address, RxMaxP 1023, RxCSR ClrDataTog + FlushFIFO
 * (0x90), then for interrupt / iso the interval; dual buffering (the SDK's bulk path) left out. type: 1 iso,
 * 2 bulk, 3 interrupt (the SDK's numbering). EP1..3 (EP4 has its own address register) */
static void usbh_ep_rx_config(uint32_t ep, uint32_t type, uint32_t interval, void *dma)
{
    if (ep == 4u)
        FM1_USB_EP4_RADR = (uint32_t)(uintptr_t)dma;
    else
        FM1_USB_EP_RADR(ep) = (uint32_t)(uintptr_t)dma;
    __asm__ volatile("csync" ::: "memory");
    usbh_wr(UH_INDEX, ep);
    usbh_wr(UH_RXMAXP, 1023u & 0xFFu);          /* (CON1 carries 8 bits: as the SDK's musb_write_usb) */
    usbh_wr(UH_INDEX, ep);
    usbh_wr(UH_RXCSR1, 0x90u);
    usbh_wr(UH_RXCSR2, 0);
    if (type == 1u || type == 3u) {
        usbh_wr(UH_INDEX, ep);
        usbh_wr(UH_RXINTERVAL, interval);
    }
    fm1_usb_ep_enable(1u << ep);                /* usb_enable_ep, as usb.c's ep1_config does (not in the
                                                 * SDK's usb_h_ep_config: a guess that host mode needs it too) */
}

/* usb0_h_ep_read, one packet and not blocking: usbh_ep_rx_req asks for it (RxType = xfer << 4 | target
 * endpoint, RxCSR ReqPkt); usbh_ep_rx_poll then returns its length (the bytes are in the DMA buffer),
 * -1 still waiting, -2 stall, -3 data error / error (each cleared as the SDK does) */
static void usbh_ep_rx_req(uint32_t ep, uint32_t xfer, uint32_t target)
{
    usbh_wr(UH_INDEX, ep);
    usbh_wr(UH_RXTYPE, xfer << 4 | target);
    usbh_wr(UH_RXCSR1, UH_RX_REQPKT);
}
static int32_t usbh_ep_rx_poll(uint32_t ep)
{
    uint32_t c, n;
    usbh_wr(UH_INDEX, ep);
    c = usbh_rd(UH_RXCSR1);
    if (c & UH_RX_RXPKTRDY) {
        n = usbh_rd(UH_RXCOUNT1) | usbh_rd(UH_RXCOUNT2) << 8;
        __asm__ volatile("ssync" ::: "memory");
        usbh_wr(UH_RXCSR1, UH_RX_FLUSH);         /* the SDK writes 0x10 once the packet is taken */
        return (int32_t)n;
    }
    if (c & UH_RX_STALL) {                      /* (the SDK logs it and keeps waiting; here the caller decides) */
        usbh_wr(UH_RXCSR1, c & 0xBFu);
        return -2;
    }
    if (c & (UH_RX_DATAERR | UH_RX_ERROR)) {
        usbh_wr(UH_RXCSR1, UH_RX_CLRTOG);       /* (the SDK: reg = 128, written & 191) */
        return -3;
    }
    return -1;
}

/* usb_h_force_reset -> usb_io_reset (id 0): D+ / D- as GPIO outputs, low, no pulls, no input buffers */
static void usbh_force_reset(void)
{
    FM1_USB_IO_CON0 &= ~((1u << 9) | (1u << 10) | (1u << 13) | (1u << 14) | 0xF0u);
    FM1_USB_IO_CON0 &= ~0x0Fu;                  /* direction out (bits 2 / 3 clear), level low (bits 0 / 1) */
}

/* usb_h_sie_close then usb_sie_close (id 0): pull-downs / pull-ups off, CON0 cleared in the SDK's two
 * steps, then CON0 = CON1 = 0 and the pads inputs (0x0C) */
static void usbh_sie_close(void)
{
    FM1_USB_IO_CON0 &= ~0x30u;
    FM1_USB_IO_CON0 &= ~0xC0u;
    FM1_USB_CON0 &= 0xFFFF00u;
    FM1_USB_CON0 |= 0x15u;
    FM1_USB_CON0 = 0;
    FM1_USB_CON1 = 0;
    FM1_USB_IO_CON0 = 0x0Cu;
}
