/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* SPEAK37: speech, played musically. One compilation unit (build.py: BOARD=smk37, the SMK-37 Pro / Elite / MKE-P37;
 * BOARD=fm1speak, the FM-1; felucca.c stays the FM-1's Felucca).
 * Of Felucca it keeps the platform (the HAL, the LCD and its fonts, USB device mode with MIDI, the serial console and
 * the USB audio input, the flash store, the M-UPGRADE update entry) and adds the speaking instrument (sp_*.c): an LPC
 * speech engine in the style of the TI TMS5220 with its own words (tools/gen_vocab.py), a word a key at the key's
 * pitch, sentences on the pads. Each file uses what the files above it define. */

/* ---------------------------------------------------- build options --- */
#ifndef BOARD_SMK37
#define BOARD_SMK37 0            /* (0: the FM-1: the panel sp_panel_fm1.c) */
#endif
#define SPEAK_BUILD 1            /* (usb.c: the USB names are Speak37's) */
#ifndef FELUCCA_FLASH
#define FELUCCA_FLASH 1          /* the store: the memories, the settings */
#endif
#ifndef FELUCCA_OTA
#define FELUCCA_OTA 1            /* M-UPGRADE update entry (the installers) */
#endif
#ifndef FELUCCA_OTA_DRYRUN
#define FELUCCA_OTA_DRYRUN 0
#endif
#ifndef FELUCCA_CDC
#define FELUCCA_CDC 1            /* the serial console */
#endif
#ifndef FELUCCA_CDC_DEFAULT
#define FELUCCA_CDC_DEFAULT 1
#endif
#ifndef FELUCCA_UAC
#define FELUCCA_UAC 1            /* USB audio input: a computer records Speak37 */
#endif
#ifndef FELUCCA_UAC_TONE
#define FELUCCA_UAC_TONE 0
#endif
#define FELUCCA_UART 0           /* (PH8 is a key-matrix row on the SMK; the FM-1: no TRS MIDI IN here) */
#ifndef FELUCCA_ID
#if BOARD_SMK37
#define FELUCCA_ID SMK_MODEL "_500"   /* the package identity (build.py) */
#else
#define FELUCCA_ID "FM-1_500"
#endif
#endif
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "v1.0"
#endif
#define SP_VERSION FELUCCA_VERSION
#define FELUCCA_USBHOST 0
#if FELUCCA_OTA && !FELUCCA_FLASH
#error "FELUCCA_OTA needs FELUCCA_FLASH"
#endif

/* ------------------------------------------------------------- HAL --- */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#if BOARD_SMK37
#include "smk_input.h"           /* keys, buttons, encoders, LEDs, pads, faders, wheels, battery */
#include "smk_compat.h"
#else
#include "fm1_input.h"           /* the FM-1: 27 keys, 14 buttons, 7 encoders, the key and button LEDs */
#endif
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_lcd_hw.h"
#include "fm1_xip.h"
#if !BOARD_SMK37
#include "fm1_adc.h"             /* MASTER (a pot), the battery */
#endif

/* ---------------------------------------------------------- system --- */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
static volatile uint32_t fm1_ms;
#define BOOTGUARD_MAGIC 0x42475244u
struct { uint32_t magic, failed, pending; } bootguard __attribute__((section(".noinit")));
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];
#define HALF_FRAMES 128u         /* I2S half buffer: 2.9 ms */
#define FS 44118u                /* 24 MHz / 544 */
static int32_t abuf[2u * 2u * HALF_FRAMES] __attribute__((aligned(4)));

/* ----------------------------------------------------- base, display --- */
#include "libc.c"
#if BOARD_SMK37 && !defined(LCD_BAUD)
#define LCD_BAUD 2u              /* the LCD's SPI: 60 MHz / 3 = 20 MHz (Felucca: 12 MHz) for the frame rate */
#endif
#define GFX_DOUBLE 1             /* two canvases: one drawn while the other goes out */
#include "lcd.c"
#include "gfx.c"

/* ------------------------------------------------------------ sound --- */
#if BOARD_SMK37
#define SP_KEYS() fm1_in.keys
#define SP_VEL(k) fm1_in.key_vel[k]
#else
#define SP_KEYS() ((uint64_t)fm1_in.notes << 5)   /* (the FM-1's 27 keys F3..G5: the SMK's keys 5..31) */
#define SP_VEL(k) 100u                           /* (no velocity) */
#endif
#include "sp_dsp.c"              /* the speech engine, the instrument */
#if BOARD_SMK37
#include "smk_uart_tx.h"
#include "smk_midi_out.h"        /* TRS MIDI OUT (UART1 TX, PC7): the keyboard as played (sp_trs.c) */
#define SP_TRS 1
#endif
#include "usb.c"                 /* device mode: MIDI, the console, the USB audio input, the update SysEx */
#include "sp_state.c"            /* the record, the pads, the knobs' parameters */
#if BOARD_SMK37
#include "sp_trs.c"              /* TRS MIDI OUT: the keys and wheels to the 3.5 mm jack */
#endif
#include "sp_view.c"             /* the pictures: the display, the mouth */
#include "sp_ui.c"               /* the screen, the shared actions */
#if BOARD_SMK37
#include "sp_panel_smk.c"        /* the knobs, pads, faders, buttons, LEDs */
#else
#include "sp_panel_fm1.c"
#endif

/* --------------------------------------------------- storage, update --- */
#include "fm1_flash.h"
#include "storage_hw.c"
#include "storage.c"
#if FELUCCA_OTA
#include "ota.c"
#include "ota_hw.c"
#endif
#if FELUCCA_CDC
#include "sp_console.c"
#endif
#include "sp_main.c"
