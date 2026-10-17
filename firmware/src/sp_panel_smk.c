/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37's panel on the SMK-37: the knobs, pads, faders, buttons, the button LEDs and the pads' colours (sp_ui.c has
 * the shared actions and the screen).
 *   K1 MASTER (sp_main.c)  K2 PAGE  K3 VOICE  K4 BANK  /  K5..K8 the page's four (PLAY: TEMPO SPEED INTONE ROOT,
 *   VOICE: FORMANT BREATH LOFI METAL, ENVELOPE: ATTACK RELEASE HOLD GLIDE, MOD: VIB RATE VIB DEPTH WOB RATE WOB DEPTH,
 *   DELAY: TYPE TIME FEEDBACK MIX, SPACE: REVERB SIZE SPREAD TONE; SETUP: BEND CHORD VELO TRS OUT)
 *   pads         the bank's 16 sentences (top row 1..8, bottom 9..16): a tap selects one, from its first word; the
 *                USER bank's: held 0.6 s, the editor (sp_user.c: then the pads are its actions, K5 and < > the list)
 *   PATCH        tap: MOPE on / off (a world of its own: every control only talks to it, sp_mope_ui.c; LED lit);
 *                held 0.6 s: the pads are the 16 memories (a tap loads, held 1 s saves), held again: the sentences
 *   < >          the word before / the next word (the cursor)
 *   OCTAVE- / +  the keys' octave, -2..+2 (LED lit when shifted); both held 5 s: the update mode
 *   GLOBE        SETUP (LED lit)            FX          the DELAY page, again: SPACE (LED lit: a send is on)
 *   KNOB BANK    the next page              FADER BANK  the page before
 *   SCALE        the keys: SENTENCE, LETTERS (A..Z, ZERO..NINE), KIT (16 speech-percussion hits; the pads too, by
 *                their pressure) (LED lit: LETTERS or KIT)
 *   NOTE REPEAT  REPEAT: every key the same word (LED lit)
 *   CHORD        CHORD on / off: keys together speak one word (LED lit: on)
 *   ARP          HOLD: ONE SHOT, GATE, HOLD, LOOP (LED lit: HOLD or LOOP)
 *   SEQ PLAY     PLAY: the sentence by itself in time, the keys choose its pitch (LED lit)   SEQ REC  from the start
 *   SEQ          RHYTHM (LED lit while a groove plays): the page TEMPO DRUMS SWING GROOVE, the pads the 16 grooves (a
 *                tap starts one, on the next bar while PLAY runs; the one playing again stops it); SEQ again: back
 *   PARA         FILL: the next bar's last beat a roll
 *   faders       SPEED, FORMANT, DELAY's send, REVERB (each takes over once moved)
 *   wheels       pitch: bend (SETUP BEND, +-2 by default); mod: vibrato
 * The keys and wheels as played also go to the 3.5 mm jack (sp_trs.c: TRS MIDI OUT). */
enum { SB_KNOB_BANK = 0, SB_FADER_BANK = 1, SB_LEFT = 2, SB_RIGHT = 3, SB_ARP = 6, SB_REPEAT = 7, SB_SCALE = 8,
       SB_CHORD = 9, SB_GLOBE = 10, SB_PATCH = 12, SB_PARA = 13, SB_FX = 14, SB_SEQ = 15, SB_SEQPLAY = 16,
       SB_SEQREC = 17 };

static const char *sp_panel_tag(uint32_t cell)
{
    static const char *const T[8] = {"K1", "K2", "K3", "K4", "K5", "K6", "K7", "K8"};
    return T[cell & 7u];
}
static const char *ue_tag(uint32_t a) { (void)a; return ""; }   /* (the pads: their colours say it) */
static void sp_panel_top(uint8_t *id)
{
    id[0] = SC_MASTER;
    id[1] = SC_PAGE;
    id[2] = SC_VOICE;
    id[3] = SC_BANK;
}

/* the main loop: the controls (a frame, and while it waits) */
static void sp_ui_input(void)
{
    uint32_t rel, pressed = fm1_input_edges(&rel), k;
    sp_ui_sync();
    sp_trs_keys();                                         /* TRS MIDI OUT: the keys as played */
    if (ue.open) {                                         /* the editor: the pads its actions, K5 and < > the list */
        ue_keys();
        for (k = 1; k < FM1_NENC; k++) {
            int32_t n = fm1_enc_take(k);
            if (n && k >= 4u)
                ue_scroll(n > 0 ? 1 : -1);
        }
        if (pressed & 1u << SB_LEFT)
            ue_scroll(-1);
        if (pressed & 1u << SB_RIGHT)
            ue_scroll(1);
        for (k = 0; k < 16u; k++) {
            uint32_t down = fm1_in.pad_st[k] == 0x80u;
            if (down && !su.pdown[k] && UE_PAD[k] != UE_NONE)
                ue_act(UE_PAD[k]);
            su.pdown[k] = (uint8_t)down;
            ue.pdown[k] = (uint8_t)down;
            ue.pdone[k] = 1;                               /* (a pad held over the close: not a long press) */
        }
        (void)rel;
        return;
    }
    if (pressed & 1u << SB_PATCH)                          /* PATCH: a tap MOPE, held 0.6 s the memories */
        su.t_mtog = fm1_ms | 1u, su.mtog_done = 0;
    if (su.t_mtog && (fm1_in.buttons >> SB_PATCH & 1u) && !su.mtog_done &&
        (int32_t)(fm1_ms - su.t_mtog) >= (int32_t)UE_LONG_MS && sp.mode != SP_MODE_MOPE) {
        su.mtog_done = 1;
        sp_mempage_toggle();
    }
    if (rel & 1u << SB_PATCH) {
        if (su.t_mtog && !su.mtog_done)
            mo_toggle();
        su.t_mtog = 0;
    }
    if (sp.mode == SP_MODE_MOPE) {                         /* MOPE: every control only talks to it */
        static const uint8_t B[18] = {MO_CT_PAGEBTN, MO_CT_PAGEBTN, MO_CT_ARROW, MO_CT_ARROW, MO_CT_OCTAVE,
                                      MO_CT_OCTAVE, MO_CT_HOLD, MO_CT_REPEAT, MO_CT_SCALE, MO_CT_CHORD, MO_CT_SETUP,
                                      0xFF, 0xFF, MO_CT_FILL, MO_CT_FX, MO_CT_RHYTHM, MO_CT_PLAY, MO_CT_REC};
        for (k = 1; k < FM1_NENC; k++) {                   /* K2..K8 (K1: MASTER, still the volume: sp_main.c) */
            int32_t n = fm1_enc_take(k);
            if (n)
                mo_ui_ctl_dir(k, n > 0 ? 1 : -1);
        }
        for (k = 0; k < 18u; k++)
            if (pressed >> k & 1u && B[k] != 0xFFu)
                mo_ui_ctl(B[k]);
        for (k = 0; k < 16u; k++) {
            uint32_t down = fm1_in.pad_st[k] == 0x80u;
            if (down && !su.pdown[k])
                mo_pad(k);
            su.pdown[k] = (uint8_t)down;
        }
        for (k = 0; k < 4u; k++) {                         /* (a fader: moved, not its jitter: mo_move) */
            static mo_mv_t fm[4];
            int32_t f = fm1_in.fader[k], d;
            su.fad_last[k] = (uint8_t)f;
            if (fm1_in.frames < 200u || !su.fad_ok)
                fm[k].init = 0;
            else if ((d = mo_move(&fm[k], f, 3, fm1_ms)) != 0)
                mo_ui_ctl_dir(MO_CT_F1 + k, d);
        }
        if (fm1_in.frames >= 200u)
            su.fad_ok = 1;
        return;
    }
    for (k = 1; k < FM1_NENC; k++) {                       /* K2..K8 (K1: MASTER, sp_main.c) */
        int32_t n = fm1_enc_take(k);
        if (!n)
            continue;
        if (k == 1u) {
            sp_touch(1);
            sp_page_step(n > 0 ? 1 : -1);
        } else if (k == 2u) {
            sp_touch(2);
            sp_voice_set((uint32_t)sp_clampi((int32_t)sp.snd.voice + (n > 0 ? 1 : -1), 0, SP_NVOICE - 1));
        } else if (k == 3u) {
            sp_touch(3);
            sp_bank_step(n > 0 ? 1 : -1);
        } else
            sp_param_turn(k, n);
    }
    if (pressed & 1u << SB_KNOB_BANK)
        sp_page_step(1);
    if (pressed & 1u << SB_FADER_BANK)
        sp_page_step(-1);
    if (pressed & 1u << SB_GLOBE)
        sp_setup_toggle();
    if (pressed & 1u << SB_FX)
        sp_page_set(su.page == SP_PG_DELAY && !su.setup ? SP_PG_SPACE : SP_PG_DELAY);
    if (pressed & 1u << SB_SCALE)
        sp_mode_toggle();
    if (pressed & 1u << SB_REPEAT)
        sp_lock_toggle();
    if (pressed & 1u << SB_CHORD)
        sp_chord_toggle();
    if (pressed & 1u << SB_ARP)
        sp_hold_cycle();
    if (pressed & 1u << SB_SEQPLAY)
        sp_auto_toggle();
    if (pressed & 1u << SB_SEQREC)
        sp_restart();
    if (pressed & 1u << SB_SEQ)
        sp_rhythm_toggle();
    if (pressed & 1u << SB_PARA)
        sp_fill();
    if (pressed & 1u << SB_LEFT)
        sp_step(-1);
    if (pressed & 1u << SB_RIGHT)
        sp_step(1);
    if (pressed & 1u << FM1_BTN_OCT_DOWN)
        sp_octave(-1);
    if (pressed & 1u << FM1_BTN_OCT_UP)
        sp_octave(1);
    (void)rel;
    for (k = 0; k < 16u; k++) {                            /* the pads: one pad a press (debounced by the HAL) */
        uint32_t down = fm1_in.pad_st[k] == 0x80u;
        if (su.page == SP_PG_RHYTHM && !su.setup && !su.mempage) {   /* RHYTHM: a pad a groove */
            if (down && !su.pdown[k])
                sp_rhythm_pad(k);
        } else if (sp.mode == SP_MODE_KIT && !su.mempage) {      /* KIT: a pad a hit, by its pressure */
            if (down && !su.pdown[k])
                sp_hit(k, 40u + fm1_in.pad_press[k] * 87u / 127u);
        } else if (su.mempage) {
            uint32_t a = sp_slot_key(&su.sk[k], down, fm1_ms);
            if (a == SS_LOAD)
                sp_mem_load(k);
            else if (a == SS_SAVE)
                sp_mem_save(k);
        } else {
            if (down && !su.pdown[k])
                sp_sent_set(sp_bank_of(vst.sent) * 16u + k);
            if (ue_long(k, down) && sp_bank_of(vst.sent) == SP_BANK_USER)
                ue_open(k);                                /* (a USER pad held: the editor) */
        }
        su.pdown[k] = (uint8_t)down;
    }
    for (k = 0; k < 4u; k++) {   /* the faders: SPEED, FORMANT, DELAY, REVERB; each takes over once it moves */
        uint32_t f = fm1_in.fader[k];
        if (fm1_in.frames < 200u || !su.fad_ok)
            su.fad_last[k] = (uint8_t)f;
        else if (f != su.fad_last[k]) {
            su.fad_last[k] = (uint8_t)f;
            sp_fader_set(k, f > 127u ? 127u : f);
        }
    }
    if (fm1_in.frames >= 200u)
        su.fad_ok = 1;
}

/* the button LEDs and the pads */
static void sp_ui_leds(void)
{
    sp_padview_t pv;
    uint32_t k;
    uint8_t c[3];
    for (k = 0; k < SMK_NLED; k++)
        fm1_led[k] = 0;
    if (sp.mode == SP_MODE_MOPE) {                         /* MOPE: its LED, its shows */
        for (k = 0; k < SMK_NLED; k++)
            fm1_led[k] = (uint8_t)mo_led(k, k == SB_PATCH);
        for (k = 0; k < 16u; k++) {
            mo_pad_rgb(k, c);
            padled_set(k, c[0], c[1], c[2]);
        }
        return;
    }
    fm1_led[SB_GLOBE] = su.setup;
    fm1_led[SB_FX] = sp.snd.dmix || sp.snd.reverb;
    fm1_led[SB_SCALE] = sp.mode != SP_MODE_SENT;
    fm1_led[SB_PATCH] = su.mempage;
    fm1_led[SB_REPEAT] = sp.lock;
    fm1_led[SB_CHORD] = sp.chord_ms != 0u;
    fm1_led[SB_ARP] = sp.snd.hold == SP_HOLD_HOLD || sp.snd.hold == SP_HOLD_LOOP;
    fm1_led[SB_SEQPLAY] = sp.auto_on;
    fm1_led[SB_SEQ] = sp.groove != 0u;
    fm1_led[SB_PARA] = sp.fill_req || sp.fill_now;
    fm1_led[FM1_BTN_OCT_DOWN] = sp.snd.oct < 0;
    fm1_led[FM1_BTN_OCT_UP] = sp.snd.oct > 0;
    if (su.blink && fm1_ms - su.t_blink > 600u)
        su.blink = 0;
    pv.mempage = su.mempage;
    pv.kit = sp.mode == SP_MODE_KIT;
    for (k = 0; k < 16u; k++)
        if (su.hit >> k & 1u && fm1_ms - su.t_hitpad[k] > 120u)
            su.hit &= (uint16_t)~(1u << k);
    pv.hit = su.hit;
    pv.bank = (uint8_t)sp_bank_of(sp.sent);
    pv.pad = (uint8_t)(sp.mode == SP_MODE_SENT ? sp.sent % 16u : 0xFFu);
    pv.speaking = sp.speaking != 0u && sp.mode == SP_MODE_SENT;
    pv.used = vst.used;
    pv.loaded = vst.loaded;
    pv.blink = su.blink;
    pv.held = 0;
    pv.edit = ue.open;
    pv.mope = 0;
    pv.rhythm = su.page == SP_PG_RHYTHM && !su.setup;
    pv.groove = sp.groove;
    pv.pending = sp.groove_req != 0xFFu ? sp.groove_req : 0u;
    pv.beat = sp.tr_on && sp.tr_pos % sp_beat() == 0u && sp.tr_t < sp_steplen(0) * 3u / 4u;
    pv.empty = 0;
    if (pv.bank == SP_BANK_USER)
        for (k = 0; k < 16u; k++)
            pv.empty |= (uint16_t)(!sp.user_n[k] << k);
    for (k = 0; k < SP_MEMS; k++)
        pv.held |= (uint16_t)(su.sk[k].down << k);
    if ((int32_t)(fm1_ms - padled_hold_until) < 0)
        return;                                            /* (console padtest) */
    for (k = 0; k < 16u; k++) {
        sp_pad_rgb(k, &pv, c);
        padled_set(k, c[0], c[1], c[2]);
    }
}
