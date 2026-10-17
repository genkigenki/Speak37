/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The flash map: which ranges the RAM flash driver (fm1_flash.h) may erase / program, per board
 * (BOARD_SMK37, fm1_cc.h). Macros and compile-time checks only, so host tests can include it. */
#pragma once
#include <stdint.h>
#include "fm1_cc.h"

/* [off, off + n) inside [lo, hi), without wrapping: off + n can overflow, and
 * the 1 MiB part ignores the high address bits, so a wrapped range lands low. */
#define FL_IN(off, n, lo, hi) ((uint32_t)(off) >= (lo) && (uint32_t)(off) <= (hi) && \
                               (uint32_t)(n) <= (hi) - (uint32_t)(off))
/* The flash chip Felucca's storage accepts (JEDEC id from 0x9F, project.c); extend it here for another part. */
#if BOARD_SMK37
/* SMK-37 Pro: 0xEB6014, 1 MiB, read from a unit over mask-ROM UBOOT (2026-10-09; jl-uboot-tool "Online device:
 * ID: 0xeb6014"). The driver uses only generic SPI NOR commands (9F, 05/35, 06, 20, 02, 0B). */
#define FL_JEDEC_OK(id) ((id) == 0xEB6014u || (id) == 0x856014u)
#else
#define FL_JEDEC_OK(id) ((id) == 0x856014u)
#endif

#if BOARD_SMK37
/* ---- SMK-37 flash map (Speak37: strict; see the fm1-to-smk37 guide) ----
 *   0x00000-0x03FFF  head (SPL, isd_config)         NEVER
 *   0x04000-0x9CFFF  app area (stock PRCT end)      update loader only (FL_APP)
 *   0x9D000-0xBAFFF  VM: the store (storage.c: Speak37's record in OBJ_SETTINGS 0xAD000 / 0xAE000, the owner tag
 *                    0xAF000)                                                        FL_DATA
 *   0xBB000-0xBFFFF  VM: M-UPGRADE loader staging + update record (ota.c)          FL_OTA
 *   0xC0000-0xC0FFF  BTIF (BT pairing)              NEVER
 *   0xC1000-0xC1FFF  USRTRIM (key/pad/wheel calibration, "trimdat")   NEVER (read only: the key curves)
 *   0xC2000-0xEAFFF  USRFLASH: the stock firmware's controller mapping (global settings, 8 presets, the end marker
 *                    "usrdata" at 0xEAFF8)          NEVER (kept as the stock firmware left it)
 *   0xEB000-0xF3FFF  free (unlisted)                NEVER (unused)
 *   0xF4000-0xFDFFF  USR (stock resources; kept for a rollback)   NEVER
 *   0xFE000-0xFEFFF  free                           NEVER (unused)
 *   0xFF000-0xFFFFF  key_mac                        NEVER
 * So the app writes only FL_DATA and FL_OTA, the update loader only the app area and its records below BTIF:
 * nothing at or above 0xC0000 is ever erased or programmed (FL_PROTECTED, the RAM driver's own net).
 * Everything below is an explicit allow-list; the asserts keep each allowed window out of the protected ranges at
 * compile time. */
#define FL_HEAD_HI      0x00004000u                /* protected: [0, FL_HEAD_HI) */
#define FL_CAL_LO       0x000C0000u                /* protected: BTIF + USRTRIM */
#define FL_CAL_HI       0x000C2000u
#define FL_STOCKMAP_LO  0x000C2000u                /* protected: the stock controller mapping (USRFLASH) */
#define FL_STOCKMAP_HI  0x000EB000u
#define FL_GAP_LO       0x000EB000u                /* protected: the unlisted gap before USR (unused) */
#define FL_GAP_HI       0x000F4000u
#define FL_USRRES_LO    0x000F4000u                /* protected: stock USR resources */
#define FL_USRRES_HI    0x000FE000u
#define FL_KEYMAC_LO    0x000FF000u                /* protected: key_mac and above */
#define FL_TOP_LO       0x000C0000u                /* protected as a whole: everything from BTIF up */
#define FL_APP_LO       0x00004000u                /* the update loader: app area */
#define FL_APP_HI       0x0009D000u
#define FL_REC_LO       0x0009D000u                /* the update loader: where it may drop records (VM only) */
#define FL_REC_HI       0x000C0000u
#define FL_PLAIN_LO     0x0009D000u                /* plain XIP window from the VM start */
#define FL_DATA_LO      0x0009D000u                /* the store */
#define FL_DATA_HI      0x000BB000u
#define FL_OTA_LO       0x000BB000u                /* M-UPGRADE loader staging, ota.c */
#define FL_OTA_HI       0x000C0000u
/* a window [lo, hi) is allowed: sector aligned, non-empty, and below everything protected at the top, above the head */
#define FL_WIN_SAFE(lo, hi) ((lo) % 0x1000u == 0u && (hi) % 0x1000u == 0u && (lo) < (hi) && \
                             (lo) >= FL_HEAD_HI && (hi) <= FL_TOP_LO)
_Static_assert(FL_CAL_LO == FL_TOP_LO && FL_CAL_HI == FL_STOCKMAP_LO && FL_STOCKMAP_HI == FL_GAP_LO &&
               FL_GAP_HI == FL_USRRES_LO && FL_USRRES_HI < FL_KEYMAC_LO, "SMK: the protected ranges, in order");
_Static_assert(FL_WIN_SAFE(FL_DATA_LO, FL_DATA_HI), "SMK: FL_DATA overlaps a protected range");
_Static_assert(FL_WIN_SAFE(FL_OTA_LO, FL_OTA_HI), "SMK: FL_OTA overlaps a protected range");
_Static_assert(FL_WIN_SAFE(FL_APP_LO, FL_APP_HI), "SMK: FL_APP overlaps a protected range");
_Static_assert(FL_WIN_SAFE(FL_REC_LO, FL_REC_HI), "SMK: FL_REC overlaps a protected range");
_Static_assert(FL_REC_HI == FL_CAL_LO && FL_OTA_HI <= FL_REC_HI && FL_OTA_LO >= FL_REC_LO,
               "SMK: the update record (OTA area) must lie in the loader's record window, below BTIF");
_Static_assert(FL_DATA_HI <= FL_OTA_LO && FL_APP_HI <= FL_DATA_LO, "SMK: windows overlap");
/* the update loader (loader.c): the app area, and records only in the VM below BTIF */
#define FL_LDR_RANGE_OK(off, n) (FL_IN(off, n, FL_APP_LO, FL_APP_HI) || FL_IN(off, n, FL_REC_LO, FL_REC_HI))
/* the app's own store: FL_DATA only */
#define FL_STORE_OK(off, n) FL_IN(off, n, FL_DATA_LO, FL_DATA_HI)
#ifndef FL_RANGE_OK
#define FL_RANGE_OK(off, n) (FL_STORE_OK(off, n) || FL_IN(off, n, FL_OTA_LO, FL_OTA_HI))
#endif
/* second, independent net in the RAM driver: [off, off + n) touches a protected range (also when a
 * caller supplies its own FL_RANGE_OK, as the update loader does): the head, or anything from 0xC0000 up */
#define FL_HITS(off, n, lo, hi) ((uint32_t)(off) < (hi) && \
                                 ((uint32_t)(off) >= (lo) || (uint32_t)(n) > (lo) - (uint32_t)(off)))
#define FL_PROTECTED(off, n) (FL_HITS(off, n, 0u, FL_HEAD_HI) || (uint32_t)(off) >= FL_TOP_LO || \
                              (uint32_t)(n) > FL_TOP_LO - (uint32_t)(off))
#define FL_GUARD(off, n) (FL_RANGE_OK(off, n) && !FL_PROTECTED(off, n))
#else /* FM-1 */
#define FL_PLAIN_LO     0x00093000u                /* plain XIP window from the FM-1 VM start */
#define FL_DATA_LO      0x00097000u                /* Felucca main store */
#define FL_DATA_HI      0x000E0000u
#define FL_GLOB_LO      0x000FC000u                /* Felucca superblock / globals */
#define FL_GLOB_HI      0x000FF000u
#define FL_OTA_LO       0x000E0000u                /* M-UPGRADE loader staging, ota.c */
#define FL_OTA_HI       0x000E5000u
#define FL_AUTO_LO      0x000E5000u                /* 1.2: the autosave (storage.c OBJ_AUTOSAVE), A/B, just above */
#define FL_AUTO_HI      0x000E7000u                /* the OTA staging */
#define FL_VBANK_LO     0x000E7000u                /* 1.4.1: the FM6 voice bank (fm6_vbank.c), the sector above */
#define FL_VBANK_HI     0x000E8000u                /* (below the stock OTA's VM backup, 0xE8000: never written) */
/* Felucca's own store (projects, user samples; settings; the autosave and the FM6 voice bank: one range) */
#define FL_STORE_OK(off, n) (FL_IN(off, n, FL_DATA_LO, FL_DATA_HI) || FL_IN(off, n, FL_GLOB_LO, FL_GLOB_HI) ||                              FL_IN(off, n, FL_AUTO_LO, FL_VBANK_HI))
/* Where the RAM driver may erase / program. The app build allows only its own
 * data regions; the update loader (firmware/loader) defines its own window. */
#ifndef FL_RANGE_OK
#define FL_RANGE_OK(off, n) (FL_STORE_OK(off, n) || FL_IN(off, n, FL_OTA_LO, FL_OTA_HI))
#endif
#define FL_GUARD(off, n) FL_RANGE_OK(off, n)
#endif /* BOARD_SMK37 */
