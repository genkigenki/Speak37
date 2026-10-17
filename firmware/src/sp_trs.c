/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37 on the SMK-37: TRS MIDI OUT, the keyboard as played on the 3.5 mm jack (smk_midi_out.h, the driver), on
 * MIDI channel 1: a note on with the key's velocity, its note off (the note it was sent with, whatever the octave
 * since), the pitch wheel (pitch bend, the centre at rest) and the mod wheel (CC 1). A held chord plays an external
 * synth while Speak37 speaks over it. Not the notes from USB MIDI in. SETUP (GLOBE), K8: TRS OUT ON / OFF, kept in
 * the record (vst.trs_off). Main loop only: sp_trs_keys (sp_panel_smk.c), sp_trs_bend / sp_trs_modw (sp_main.c
 * sp_wheels); the driver's TIMER5 tick sends the bytes. (As Vox37's vo_trs.c.) */
#define SP_TRS_CH 0u                              /* channel 1 */
#define SP_TRS_GAP 4u                             /* ms between two bends / two mod values (the line's pace) */
#define SP_TRS_PKT(cin, st, d1, d2) ((uint32_t)(cin) | (uint32_t)(st) << 8 | (uint32_t)(d1) << 16 | (uint32_t)(d2) << 24)

static struct {
    uint64_t keys;                                /* the keys as last seen */
    uint64_t on;                                  /* keys whose note on went out */
    uint8_t note[37];                             /* ... with this note */
    int32_t bend, modw;                           /* as last sent (modw -1: not yet) */
    uint32_t bend_t, modw_t;
} strs = {0, 0, {0}, 8192, -1, 0, 0};

static void sp_trs_note(uint32_t k, uint32_t n, uint32_t vel)   /* vel 0: the key let go */
{
    if (vel) {
        strs.on |= 1ull << k;
        strs.note[k] = (uint8_t)n;
        smo_put(SP_TRS_PKT(9u, 0x90u | SP_TRS_CH, n, vel));
    } else if (strs.on >> k & 1u) {
        strs.on &= ~(1ull << k);
        smo_put(SP_TRS_PKT(8u, 0x80u | SP_TRS_CH, strs.note[k], 0u));
    }
}

/* the keys (SP_KEYS, SP_VEL: as the sound reads them), the octave as Speak37 plays them */
static void sp_trs_keys(void)
{
    uint64_t now = SP_KEYS(), ch = now ^ strs.keys;
    uint32_t k;
    strs.keys = now;
    for (k = 0; ch && k < 37u; k++)
        if (ch >> k & 1u) {
            int32_t n = 48 + 12 * sp.snd.oct + (int32_t)k;
            uint32_t vel = SP_VEL(k);
            vel = vel ? (vel > 127u ? 127u : vel) : 100u;
            if (!(now >> k & 1u))
                sp_trs_note(k, 0, 0);
            else if (n >= 0 && n < 128)
                sp_trs_note(k, (uint32_t)n, vel);
        }
}

static void sp_trs_bend(int32_t b)                /* the pitch wheel, -8192..8191 (0 at rest) */
{
    int32_t v = sp_clampi(b + 8192, 0, 16383), d = v - strs.bend;
    if (v == strs.bend || (v != 8192 && (fm1_ms - strs.bend_t < SP_TRS_GAP || (d < 16 && d > -16 && v && v != 16383))))
        return;                                   /* (the centre and the ends at once; a small wobble not sent) */
    strs.bend = v;
    strs.bend_t = fm1_ms;
    smo_put(SP_TRS_PKT(14u, 0xE0u | SP_TRS_CH, (uint32_t)v & 127u, (uint32_t)v >> 7));
}

static void sp_trs_modw(int32_t m)                /* the mod wheel, 0..127 */
{
    int32_t v = sp_clampi(m, 0, 127), d = v - strs.modw;
    if (v == strs.modw || fm1_ms - strs.modw_t < SP_TRS_GAP || (strs.modw >= 0 && d < 2 && d > -2 && v && v != 127))
        return;                                   /* (a wobble of one step at rest: not sent; the ends always) */
    strs.modw = v;
    strs.modw_t = fm1_ms;
    smo_put(SP_TRS_PKT(11u, 0xB0u | SP_TRS_CH, 1u, (uint32_t)v));
}

/* ON / OFF (ON tries the UART again); going OFF lets go of the notes held and the bend first */
static void sp_trs_set(uint32_t on)
{
    uint32_t k;
    if (!on && trs_out) {
        for (k = 0; k < 37u; k++)
            sp_trs_note(k, 0, 0);
        if (strs.bend != 8192) {
            strs.bend = 8192;
            smo_put(SP_TRS_PKT(14u, 0xE0u | SP_TRS_CH, 0u, 64u));
        }
    }
    smo_on(on != 0u);
}
