/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37's panel on the FM-1: no pads, faders or wheels, so their jobs move to the encoders and the buttons (sp_ui.c
 * has the shared actions and the screen; the screen is the SMK's, 240 x 240 on both).
 *   MASTER (pot)   the volume
 *   SELECT         the page (PLAY, VOICE, ENVELOPE, MOD, DELAY, SPACE, RHYTHM: TEMPO DRUMS SWING GROOVE; KNOB 4
 *                  picks a groove, OFF stops it; while PLAY runs it starts on the next bar)
 *   ALGORITHM      the voice (CLASSIC .. GIANT)
 *   PRESETS        the sentence: every pad of every bank in a row (B1 P1 .. B21 P16), from its first word;
 *                  SAVE held + PRESETS: a memory (M1 .. M16), loaded when SAVE is let go
 *   KNOB 1..4      the page's four (the SMK's K5..K8)
 *   SAVE           held 1 s (PRESETS not turned): the sound into the memory shown (M); let go after SAVE + PRESETS:
 *                  that memory loaded
 *   SEL            the next page     HOME   PLAY      ENV   ENVELOPE      LFO   MOD      FX    DELAY, again SPACE
 *   GLO            SETUP (KNOB 1..4: BEND, CHORD, VELO, REPEAT)  EDIT   the keys: SENTENCE, LETTERS, KIT; held
 *                  0.6 s: MOPE on / off (a world of its own: every control only talks to it; REC praises it, PLAY
 *                  pokes it, SEQ asks for a joke, the game on the first 16 keys' lights)
 *   ARP            the word before   SEQ    the next word   PLAY  PLAY (the sentence by itself in time, the keys
 *                  choose its pitch)   REC  the first word; on a USER pad held 0.6 s: the editor (sp_user.c)
 *   OCT- / OCT+    the keys' octave; both held 5 s: the update mode
 * The 27 keys (F3..G5, no velocity) speak; their LEDs light the keys held. Button LEDs: GLO (SETUP), EDIT (LETTERS),
 * PLAY (PLAY on), SAVE (held), FX (a send on), SEL (a page other than PLAY), OCT- / OCT+ (shifted).
 * The ids: the FM-1's matrix (Felucca's panel.c PANEL_DEFAULT): buttons OCT- 0, OCT+ 1, FX 2, SEL 3, ENV 4, LFO 5,
 * EDIT 6, GLO 7, HOME 8, SAVE 9, ARP 10, SEQ 11, PLAY 12, REC 13; encoders SELECT 0, ALGORITHM 1, KNOB 1..4 2..5,
 * PRESETS 6, every one clockwise + */
enum { SF_FX = 2, SF_SEL = 3, SF_ENV = 4, SF_LFO = 5, SF_EDIT = 6, SF_GLO = 7, SF_HOME = 8, SF_SAVE = 9, SF_ARP = 10,
       SF_SEQ = 11, SF_PLAY = 12, SF_REC = 13 };
enum { SF_E_SELECT = 0, SF_E_ALGO = 1, SF_E_K1 = 2, SF_E_PRESET = 6 };

static struct { uint8_t memsel, turned, rec_done; uint32_t t_rec; } sf;
static void mo_input_fm1(uint32_t pressed);

static const char *sp_panel_tag(uint32_t cell)
{
    static const char *const T[8] = {"POT", "SEL", "ALG", "PRE", "1", "2", "3", "4"};
    return T[cell & 7u];
}
/* the editor's buttons (sp_user.c): REC ADD, HOME PICK, ENV NEXT, LFO DEL LTR, FX DEL WORD, PLAY PREVIEW, EDIT
 * EN / DE, GLO CANCEL, SAVE OK; KNOB 1, PRESETS, ARP / SEQ the list */
static const char *ue_tag(uint32_t a)
{
    static const char *const T[UE_N] = {"REC ", "HOME ", "ENV ", "LFO ", "FX ", "PLAY ", "EDIT ", "GLO ", "SAVE "};
    return a < UE_N ? T[a] : "";
}
static void sp_panel_top(uint8_t *id)
{
    id[0] = SC_MASTER;
    id[1] = SC_PAGE;
    id[2] = SC_VOICE;
    id[3] = sf.memsel ? SC_MEM : SC_PAD;
}

/* the main loop: the controls (a frame, and while it waits) */
static void sp_ui_input(void)
{
    uint32_t rel, pressed = fm1_input_edges(&rel), k, save = (fm1_in.buttons >> SF_SAVE) & 1u;
    int32_t n;
    sp_ui_sync();
    if (ue.open) {                                         /* the editor */
        static const uint8_t BTN[UE_N] = {SF_REC, SF_HOME, SF_ENV, SF_LFO, SF_FX, SF_PLAY, SF_EDIT, SF_GLO, SF_SAVE};
        ue_keys();
        if ((n = fm1_enc_take(SF_E_K1)) != 0 || (n = fm1_enc_take(SF_E_PRESET)) != 0)
            ue_scroll(n > 0 ? 1 : -1);
        for (k = 0; k < 7u; k++)
            (void)fm1_enc_take(k);
        if (pressed & 1u << SF_ARP)
            ue_scroll(-1);
        if (pressed & 1u << SF_SEQ)
            ue_scroll(1);
        for (k = 0; k < UE_N; k++)
            if (pressed & 1u << BTN[k]) {
                ue_act(k);
                if (!ue.open)
                    break;
            }
        sf.rec_done = 1;
        sf.turned = 1;                                     /* (SAVE closed it: no memory saved as it is held on) */
        return;
    }
    if (pressed & 1u << SF_EDIT)                           /* EDIT: a tap the keys' mode, held 0.6 s MOPE */
        su.t_mtog = fm1_ms | 1u, su.mtog_done = 0;
    if (su.t_mtog && (fm1_in.buttons >> SF_EDIT & 1u) && !su.mtog_done &&
        (int32_t)(fm1_ms - su.t_mtog) >= (int32_t)UE_LONG_MS) {
        su.mtog_done = 1;
        mo_toggle();
    }
    if (rel & 1u << SF_EDIT) {
        if (su.t_mtog && !su.mtog_done)
            sp.mode == SP_MODE_MOPE ? mo_ui_ctl(MO_CT_EDIT) : sp_mode_toggle();
        su.t_mtog = 0;
    }
    if (sp.mode == SP_MODE_MOPE) {
        mo_input_fm1(pressed);
        return;
    }
    if ((fm1_in.buttons >> SF_REC & 1u) && sp_bank_of(vst.sent) == SP_BANK_USER && sp.mode != SP_MODE_MOPE) {
        /* REC held: the editor */
        if (pressed & 1u << SF_REC)
            sf.t_rec = fm1_ms, sf.rec_done = 0;
        if (!sf.rec_done && fm1_ms - sf.t_rec >= UE_LONG_MS) {
            sf.rec_done = 1;
            ue_open(vst.sent % 16u);
            return;
        }
    }
    if ((n = fm1_enc_take(SF_E_SELECT)) != 0) {
        sp_touch(1);
        sp_page_step(n > 0 ? 1 : -1);
    }
    if ((n = fm1_enc_take(SF_E_ALGO)) != 0) {
        sp_touch(2);
        sp_voice_set((uint32_t)sp_clampi((int32_t)sp.snd.voice + (n > 0 ? 1 : -1), 0, SP_NVOICE - 1));
    }
    if ((n = fm1_enc_take(SF_E_PRESET)) != 0) {
        sp_touch(3);
        if (save) {                                        /* SAVE held: a memory */
            char t[16];
            sf.memsel = 1;
            sf.turned = 1;
            su.msel = (uint8_t)(((int32_t)su.msel + (n > 0 ? 1 : -1) + (int32_t)SP_MEMS) % (int32_t)SP_MEMS);
            sp_d(sp_s(t, "MEMORY "), su.msel + 1, 0);
            sp_hnote(t);
        } else if (sp.mode != SP_MODE_MOPE) {             /* (MOPE: PRESETS only gets a comment) */
            int32_t s = ((int32_t)vst.sent + (n > 0 ? 1 : -1) + (int32_t)SP_NSENT) % (int32_t)SP_NSENT;
            uint32_t b = sp_bank_of(vst.sent);
            sp_sent_set((uint32_t)s);
            if (sp_bank_of((uint32_t)s) != b) {
                sp_bank_voice(sp_bank_of((uint32_t)s));
                sp_popup("BANK", sp_bank_name(sp_bank_of((uint32_t)s)));
            }
        }
    }
    for (k = 0; k < 4u; k++)                               /* KNOB 1..4: the page's four */
        if ((n = fm1_enc_take(SF_E_K1 + k)) != 0)
            sp_param_turn(4u + k, n);
    {   /* SAVE: held 1 s without PRESETS: save; let go after SAVE + PRESETS: load */
        uint32_t a = sp_slot_key(&su.sk[0], save && !sf.turned, fm1_ms);
        if (a == SS_SAVE)
            sp_mem_save(su.msel % SP_MEMS);
        if (pressed & 1u << SF_SAVE) {
            sf.turned = 0;
            sf.memsel = 1;
            su.redraw = 1;
        }
        if (rel & 1u << SF_SAVE) {
            if (sf.turned)
                sp_mem_load(su.msel % SP_MEMS);
            sf.turned = 0;
            sf.memsel = 0;
            su.redraw = 1;
        }
    }
    if (pressed & 1u << SF_SEL)
        sp_page_step(su.page + 1u < SP_NPAGE ? 1 : -(int32_t)SP_NPAGE);
    if (pressed & 1u << SF_HOME)
        sp_page_set(SP_PG_PLAY);
    if (pressed & 1u << SF_ENV)
        sp_page_set(SP_PG_ENV);
    if (pressed & 1u << SF_LFO)
        sp_page_set(SP_PG_MOD);
    if (pressed & 1u << SF_FX)
        sp_page_set(su.page == SP_PG_DELAY && !su.setup ? SP_PG_SPACE : SP_PG_DELAY);
    if (pressed & 1u << SF_GLO)
        sp_setup_toggle();
    if (pressed & 1u << SF_ARP)
        sp_step(-1);
    if (pressed & 1u << SF_SEQ)
        sp_step(1);

    if (pressed & 1u << SF_PLAY)
        sp_auto_toggle();
    if (pressed & 1u << SF_REC)
        sp_restart();
    if (pressed & 1u << FM1_BTN_OCT_DOWN)
        sp_octave(-1);
    if (pressed & 1u << FM1_BTN_OCT_UP)
        sp_octave(1);
}

/* MOPE: every control only talks to it (EDIT: sp_ui_input) */
static void mo_input_fm1(uint32_t pressed)
{
    static const uint8_t E[7] = {MO_CT_PAGE, MO_CT_VOICE, MO_CT_FK1, MO_CT_FK2, MO_CT_FK3, MO_CT_FK4, MO_CT_BANK};
    static const uint8_t B[14] = {MO_CT_OCTAVE, MO_CT_OCTAVE, MO_CT_FX, MO_CT_PAGEBTN, MO_CT_PAGEBTN, MO_CT_PAGEBTN,
                                  0xFF, MO_CT_SETUP, MO_CT_PAGEBTN, MO_CT_SAVE, MO_CT_ARROW, 0xFF, 0xFF, 0xFF};
    uint32_t k;
    for (k = 0; k < 7u; k++) {
        int32_t n = fm1_enc_take(k);
        if (n)
            mo_ui_ctl_dir(E[k], n > 0 ? 1 : -1);
    }
    for (k = 0; k < 14u; k++)
        if (pressed >> k & 1u && B[k] != 0xFFu)
            mo_ui_ctl(B[k]);
    if (pressed & 1u << SF_PLAY)
        mo_ctl_pose(), mo_poke();
    if (pressed & 1u << SF_REC)
        mo_ctl_pose(), mo_praise();
    if (pressed & 1u << SF_SEQ)
        mo_ctl_pose(), mo_joke();
}

/* the LEDs: the buttons by their state, the keys by the notes held */
static void sp_ui_leds(void)
{
    uint32_t k, notes = fm1_in.notes;
    if (sp.mode == SP_MODE_MOPE) {                         /* MOPE: EDIT lit, the game's key, its shows */
        for (k = 0; k < 14u; k++)
            fm1_led_key(k, mo_led(k, k == SF_EDIT));
        for (k = 0; k < 27u; k++)
            fm1_led_key(14u + k, mo_dark() ? 0 : mo_gaming() ? k == mo.gpos : mo_lights() ?
                        mo_led(k + 14u, 0) : (int)(notes >> k & 1u));
        return;
    }
    if (su.blink && fm1_ms - su.t_blink > 600u)
        su.blink = 0;
    fm1_led_key(SF_GLO, su.setup);
    fm1_led_key(SF_EDIT, sp.mode != SP_MODE_SENT);
    fm1_led_key(SF_PLAY, sp.auto_on);
    fm1_led_key(SF_SAVE, su.sk[0].down || su.blink);
    fm1_led_key(SF_FX, sp.snd.dmix || sp.snd.reverb);
    fm1_led_key(SF_SEL, su.page != SP_PG_PLAY && !su.setup);
    fm1_led_key(FM1_BTN_OCT_DOWN, sp.snd.oct < 0);
    fm1_led_key(FM1_BTN_OCT_UP, sp.snd.oct > 0);
    for (k = 0; k < 27u; k++)
        fm1_led_key(14u + k, (int)(notes >> k & 1u));
}
