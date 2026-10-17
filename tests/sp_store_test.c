/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37's record in the flash (sp_state.c's record through storage.c, as sp_main.c saves and loads it), on a
 * simulated 1 MiB NOR. Built for the SMK-37 (-DBOARD_SMK37=1) and the FM-1 (tests/run_tests.sh):
 *   - every erase and every program goes through the allow-lists of fm1_flash_map.h: inside the store's windows
 *     (SMK-37: FL_DATA 0x9D000..0xBAFFF only) and never into a protected range (FL_PROTECTED: the head, BTIF, USRTRIM,
 *     USRFLASH, the gap 0xEB000..0xF3FFF, USR, key_mac); only 4 KiB erases (no 64 KiB block erase exists in the
 *     driver: fm1_flash.h has the 0x20 command only)
 *   - after 300 saves, the owner check and loads, every byte outside the store's windows is as it was
 *   - the record's two copies sit where the map says (SMK-37 0xAD000 / 0xAE000), the newest is read back exactly; a
 *     copy torn halfway leaves the older one in charge */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include "fm1_cc.h"
#include "fm1_flash_map.h"

static uint8_t nor[0x100000], ref[0x100000];
static uint32_t n_erase, n_prog, bad_erase, bad_prog, cut_after = 0xFFFFFFFFu;
static uint32_t lo_seen = 0xFFFFFFFFu, hi_seen;
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    if (off + n > sizeof nor)
        return -1;
    memcpy(dst, nor + off, n);
    return 0;
}
static int allowed(uint32_t off, uint32_t n)
{
#if BOARD_SMK37
    return FL_STORE_OK(off, n) && FL_GUARD(off, n) && !FL_PROTECTED(off, n);
#else
    return FL_STORE_OK(off, n) && FL_GUARD(off, n);
#endif
}
static int st_erase(uint32_t off)
{
    n_erase++;
    if ((off & 0xFFFu) || !allowed(off, 0x1000u)) {
        bad_erase++;
        printf("  erase outside the windows: 0x%05X\n", off);
        return -8;
    }
    lo_seen = off < lo_seen ? off : lo_seen;
    hi_seen = off + 0x1000u > hi_seen ? off + 0x1000u : hi_seen;
    memset(nor + off, 0xFF, 0x1000);
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    uint32_t i;
    n_prog++;
    if (!allowed(off, n)) {
        bad_prog++;
        printf("  program outside the windows: 0x%05X + %u\n", off, n);
        return -8;
    }
    for (i = 0; i < n; i++) {
        if (n_prog > cut_after)                    /* (a power cut: the rest never arrives) */
            return -1;
        nor[off + i] &= s[i];
    }
    return 0;
}
#include "../firmware/src/storage.c"

/* the sound's types and the record (sp_dsp.c, sp_state.c) */
static uint64_t t_keys;
#define SP_KEYS() t_keys
#define SP_VEL(k) 100u
#include "../firmware/src/sp_dsp.c"
#include "../firmware/src/sp_state.c"
static void sp_edit_pad_rgb(uint32_t pad, uint8_t *rgb) { (void)pad, rgb[0] = rgb[1] = rgb[2] = 0; }   /* (sp_user.c) */

static int bad;
static void check(const char *what, int ok)
{
    printf("speak store %s: %-74s %s\n", BOARD_SMK37 ? "smk" : "fm1", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

/* as sp_main.c: sp_store_save and sp_store_boot's load */
static int save(sp_store_t *st)
{
    sp_store_seal(st);
    return st_save(OBJ_SETTINGS, st, sizeof *st);
}
static int load(sp_store_t *st)
{
    int n = st_load(OBJ_SETTINGS, st, sizeof *st);
    return sp_store_take(st, n);
}

static int outside_kept(void)
{
    uint32_t i;
    for (i = 0; i < sizeof nor; i++) {
#if BOARD_SMK37
        int store = FL_IN(i, 1u, FL_DATA_LO, FL_DATA_HI);
#else
        int store = FL_STORE_OK(i, 1u);
#endif
        if (!store && nor[i] != ref[i]) {
            printf("  byte 0x%05X changed\n", i);
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    sp_store_t a, b;
    uint32_t i, k;
    for (i = 0; i < sizeof nor; i++)               /* a full flash: every byte something, the store erased */
        nor[i] = (uint8_t)(0x5A ^ (i >> 12) ^ i);
#if BOARD_SMK37
    memset(nor + FL_DATA_LO, 0xFF, FL_DATA_HI - FL_DATA_LO);
    memcpy(nor + 0xEAFF8, "usrdata", 8);           /* (the stock mapping's marker in USRFLASH) */
#else
    memset(nor + FL_DATA_LO, 0xFF, FL_DATA_HI - FL_DATA_LO);
    memset(nor + FL_GLOB_LO, 0xFF, FL_GLOB_HI - FL_GLOB_LO);
#endif
    memcpy(ref, nor, sizeof nor);
    st_owner_check();                              /* (SMK-37: the store's owner tag; the FM-1: nothing) */
    sp_store_defaults(&a);
    for (k = 0; k < 300u; k++) {                   /* 300 saves, each different */
        a.cur.speed = (uint8_t)(k & 127u);
        a.cur.voice = (uint8_t)(k % SP_NVOICE);
        a.mem[k % SP_MEMS].release = (uint8_t)(k & 127u);
        a.used |= (uint16_t)(1u << (k % SP_MEMS));
        a.sent = (uint16_t)(k % (SP_NBANK * 16u));
        if (save(&a))
            break;
        if (!load(&b) || memcmp(&a, &b, sizeof a))
            break;
    }
    check("300 saves, each read back exactly", k == 300u);
    check("every erase and program inside the store's windows (FL_STORE_OK, FL_GUARD, never FL_PROTECTED)",
          !bad_erase && !bad_prog && n_erase >= 300u && n_prog > 300u);
#if BOARD_SMK37
    printf("        %u erases, %u programs, all in 0x%05X..0x%05X\n", n_erase, n_prog, lo_seen, hi_seen - 1u);
    check("the record's copies at 0xAD000 / 0xAE000; the owner tag 0xAF000 (FL_DATA)",
          st_sector(OBJ_SETTINGS, 0) == 0xAD000u && st_sector(OBJ_SETTINGS, 1) == 0xAE000u &&
          lo_seen >= FL_DATA_LO && hi_seen <= FL_DATA_HI && !memcmp(nor + 0xAF000, "SMK37DATA", 9));
    check("USRFLASH (the stock mapping, its marker) and the gap 0xEB000..0xF3FFF untouched",
          !memcmp(nor + 0xEAFF8, "usrdata", 8) && !memcmp(nor + 0xC2000, ref + 0xC2000, 0xF4000 - 0xC2000));
#else
    printf("        %u erases, %u programs, all in 0x%05X..0x%05X\n", n_erase, n_prog, lo_seen, hi_seen - 1u);
    check("the record's copies in the FM-1's settings sectors (FL_GLOB 0xFC000..)", lo_seen >= FL_GLOB_LO &&
          hi_seen <= FL_GLOB_HI);
#endif
    {   /* the USER sentences: their own record (OBJ_PROJECT0), 100 saves, each read back */
        static sp_user_rec_t r, r2;
        uint32_t e0 = bad_erase, p0 = bad_prog, j;
        for (k = 0; k < 100u; k++) {
            for (j = 0; j < 16u; j++) {
                sp.user_n[j] = (uint8_t)((k + j) % (SP_UMAX + 1u));
                for (i = 0; i < sp.user_n[j]; i++)
                    sp.user_w[j][i] = (uint16_t)((k * 7u + j * 3u + i) % SP_NWORD);
            }
            sp_user_pack(&r);
            if (st_save(OBJ_PROJECT0, &r, sizeof r))
                break;
            memset(sp.user_n, 0, sizeof sp.user_n);
            if (!sp_user_take(&r2, st_load(OBJ_PROJECT0, &r2, sizeof r2)) || memcmp(&r, &r2, sizeof r))
                break;
        }
        check("the USER record: 100 saves, each read back exactly, every write inside the windows",
              k == 100u && bad_erase == e0 && bad_prog == p0);
#if BOARD_SMK37
        check("the USER record's copies at 0x9D000 / 0x9E000 (FL_DATA)", st_sector(OBJ_PROJECT0, 0) == 0x9D000u &&
              st_sector(OBJ_PROJECT0, 1) == 0x9E000u && lo_seen >= FL_DATA_LO && hi_seen <= FL_DATA_HI);
#else
        check("the USER record's copies at 0x97000 / 0x98000 (the FM-1's FL_DATA)",
              st_sector(OBJ_PROJECT0, 0) == 0x97000u && st_sector(OBJ_PROJECT0, 1) == 0x98000u &&
              FL_STORE_OK(0x97000u, 0x2000u));
#endif
        cut_after = n_prog + 1u;                   /* a save cut short: the one before it read back */
        sp.user_n[0] = 1;
        sp_user_pack(&r2);
        st_save(OBJ_PROJECT0, &r2, sizeof r2);
        cut_after = 0xFFFFFFFFu;
        check("the USER record: a save cut short leaves the one before it",
              sp_user_take(&r2, st_load(OBJ_PROJECT0, &r2, sizeof r2)) && !memcmp(&r, &r2, sizeof r));
    }
    check("every byte outside the store's windows as it was", outside_kept());
    {   /* a power cut halfway through a save: the older copy stays in charge */
        sp_store_t c = a;
        c.cur.speed = 1;
        cut_after = n_prog + 1u;                   /* (the first payload page written, the rest and the commit record not) */
        save(&c);
        cut_after = 0xFFFFFFFFu;
        check("a save cut short: the copy before it is read back", load(&b) && !memcmp(&a, &b, sizeof a));
    }
    {   /* every byte of the record damaged once: never taken */
        sp_store_t c;
        uint32_t taken = 0;
        sp_store_seal(&a);
        for (i = 0; i < sizeof a; i++) {
            c = a;
            ((uint8_t *)&c)[i] ^= 0x41u;
            taken += sp_store_take(&c, sizeof c);
        }
        check("a record damaged in any byte is refused (the defaults)", taken == 0u);
    }
    (void)t_keys;
    printf("%s\n", bad ? "SPEAK STORE TEST FAILED" : "speak store test passed");
    return bad != 0;
}
