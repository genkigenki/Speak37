/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37's panel: the shared actions of the knobs, buttons and pads (sp_panel_smk.c, sp_panel_fm1.c map the
 * hardware onto them) and the screen (main loop). The sound is sp_dsp.c, the record and the pages sp_state.c, the
 * record's flash side sp_main.c (sp_store_save).
 *
 * Screen (240 x 240), grouped by what it shows, the knob block at the bottom:
 *   0..23     the header: the bank (in its colour), the voice
 *   24..99    the display: the sentence on a Speak & Spell glass, two lines of 20 starburst characters, the word spoken
 *             brightest (LETTERS: the letters typed; PATCH: the memories; SETUP: what it sets)
 *   100..159  the pad and the word (WORD 3 / 14), the sentence's progress, the state (SPEAK, REPEAT, LETTERS, the
 *             octave, a wheel), the voices sounding; on the right the mouth: the speaking voice's filter as a spectrum
 *   160..239  the knob block: the upper row K1..K4 (MASTER, PAGE, VOICE, BANK; the FM-1: MASTER, SELECT, ALGORITHM,
 *             PRESETS), the lower row the page's four (SMK K5..K8, the FM-1's KNOB 1..4), the knob turned last
 *             framed. A popup (a voice, a memory, a page) sits over the display for 1.5 s.
 * The keys mode MOPE: the display and the info strip are MOPE's picture, the robot and what it says (sp_mope_ui.c). */
static sp_store_t vst;                     /* the record (sp_main.c reads and writes it) */
static void sp_store_save(void);
/* the panel (sp_panel_smk.c / sp_panel_fm1.c) */
static const char *sp_panel_tag(uint32_t cell);           /* a knob block cell's knob: "K5" / "1" */
static void sp_panel_top(uint8_t *id);                    /* the upper row's four */
static uint32_t sp_pot;                                    /* MASTER, 0..1023 (sp_main.c) */
static volatile uint32_t sp_cpu_q8, audio_max_us;          /* the audio ISR's load, its longest half (sp_main.c) */
static struct { uint32_t in_audio, halves, late, nested, boots; } sp_dbg __attribute__((unused));   /* (sp_main.c, the console) */
static uint8_t sp_store_fail __attribute__((unused));                          /* a save failed (sp_main.c) */
static void sp_user_save(void);                            /* the USER sentences into the flash (sp_main.c) */
#define MO_RGB RGB(150, 205, 255)                          /* MOPE's name: the popups' light blue */
static void mo_toggle(void);                               /* (sp_mope_ui.c) */
static void mo_ui_ctl(uint32_t id);
static void mo_ui_ctl_dir(uint32_t id, int32_t dir);
static void mo_draw_legend(void);
static int mo_dark(void);
static void mo_black(uint32_t y, uint32_t h);
static int mo_screen(void);
static void mo_draw_display(void);
static void mo_draw_info(void);
static const char *mo_header_note(void);
static const char *sp_user_text(uint32_t slot);            /* (sp_user.c) */
#if BOARD_SMK37
#define RH_PICK_HINT "PADS PICK A GROOVE"                   /* RHYTHM, nothing playing */
#define UE_HINT "LONG CLICK TO RECORD"                     /* an empty USER pad */
#define UE_SCROLL_HINT "THE LIST: K5 OR < >"
#else
#define RH_PICK_HINT "KNOB 4 PICKS A GROOVE"
#define UE_HINT "HOLD REC TO RECORD"
#define UE_SCROLL_HINT "THE LIST: KNOB 1 OR ARP / SEQ"
#endif

enum { SC_MASTER = 100, SC_PAGE, SC_VOICE, SC_BANK, SC_PAD, SC_MEM };   /* the upper row's cells */

static struct {
    uint8_t page;                          /* SP_PG_* (the pages of the knobs) */
    uint8_t setup;                         /* SETUP open (GLOBE / GLO) */
    uint8_t mempage;                       /* PATCH: the pads are the memories */
    uint32_t t_pop;
    char pop_t[24], pop_v[24];
    uint8_t touch;                         /* the knob turned last: 0..7 (its cell), t_touch: when (0 none) */
    uint32_t t_touch;
    char note[24];                         /* the state line's note (sp_hnote), since t_note */
    uint32_t t_note;
    uint8_t redraw, snd_dirty, glob_dirty;
    uint32_t t_change;
    sp_slotkey_t sk[SP_MEMS];
    uint8_t pdown[16];
    uint8_t blink;
    uint32_t t_blink;
    uint8_t fad_last[4], fad_ok, fader;
    uint32_t t_fader;
    uint8_t msel;                          /* the memory selected (FM-1: SAVE + PRESETS) */
    uint8_t chord_was;                     /* CHORD's value before it was switched off */
    uint16_t hit;                          /* KIT pads: lit (sp_hit), each for 120 ms */
    uint32_t t_hitpad[16];
    uint8_t mouth[24];
    uint32_t t_frame, frames, fps, t_fps, n;
    uint32_t t_hit_ui;                     /* KIT: when the screen saw the last hit */
    uint8_t rh_back;                       /* RHYTHM (SEQ): the page to go back to */
    uint32_t hits_seen;
    uint32_t t_mtog;                       /* MOPE's button (SMK PATCH, FM-1 EDIT): down since, its hold done */
    uint8_t mtog_done;
} su;

static void sp_popup(const char *t, const char *v)
{
    str_cpy(su.pop_t, t, sizeof su.pop_t);
    str_cpy(su.pop_v, v, sizeof su.pop_v);
    su.t_pop = fm1_ms | 1u;
}
static void sp_hnote(const char *s)
{
    str_cpy(su.note, s, sizeof su.note);
    su.t_note = fm1_ms | 1u;
}
static void sp_changed(void)
{
    su.snd_dirty = 1;
    su.redraw = 1;
    su.t_change = fm1_ms;
    sp_apply();
}
static void sp_touch(uint32_t k)
{
#if BOARD_SMK37
    mo_ui_ctl(k & 7u);                                     /* (MOPE: K1..K8) */
#else
    mo_ui_ctl((k & 7u) < 4u ? k & 7u : MO_CT_FK1 + (k & 3u));   /* (POT SELECT ALGORITHM PRESETS, KNOB 1..4) */
#endif
    su.touch = (uint8_t)k;
    su.t_touch = fm1_ms | 1u;
    su.redraw = 1;
}

/* ------------------------------------------------------------- actions --- */
static uint32_t sp_bank_of(uint32_t s) { return s / 16u; }
static const char *sp_bank_name(uint32_t b) { return b < SP_NBANK ? SP_BANK_NAME[b] : "USER"; }
static const char *sp_bank_short(uint32_t b) { return b < SP_NBANK ? SP_BANK_SHORT[b] : "USER"; }
/* a sentence's text as shown (a USER pad's: its words', or the hint) */
static const char *sp_sent_text(uint32_t s)
{
    s %= SP_NSENT;
    return s >= SP_USENT ? sp_user_text(s - SP_USENT) : SP_SENT[s];
}
/* the pad in use as the sound has it (a MIDI program change moves it too), once no request is pending */
static void sp_ui_sync(void)
{
    mo_drive();                                            /* MOPE (sp_mope.c): listening, talking, sulking */
    if (sp.req_sent == 0xFFFFu && vst.sent != sp.sent)
        vst.sent = sp.sent;
    if (sp.groove_req == 0xFFu && sp_glob.groove != sp.groove) {   /* (GROOVE shows the groove playing) */
        sp_glob.groove = sp.groove;
        su.redraw = 1;
    }
}
/* a pad: its sentence from its first word (and the keys back to the sentences) */
static void sp_sent_set(uint32_t s)
{
    char t[16];
    s %= SP_NSENT;
    sp.req_sent = (uint16_t)s;
    vst.sent = (uint16_t)s;
    if (sp.mode != SP_MODE_SENT) {
        sp.mode = SP_MODE_SENT;
        vst.mode = SP_MODE_SENT;
    }
    su.glob_dirty = 1;
    su.t_change = fm1_ms;
    su.redraw = 1;
    sp_d(sp_s(t, "PAD "), (int32_t)(s % 16u) + 1, 0);
    sp_hnote(t);
}
static void sp_voice_set(uint32_t v)
{
    v %= SP_NVOICE;
    if (sp.snd.voice == v)
        return;
    sp_voice_load(&sp.snd, v);
    sp_changed();
    sp_popup("VOICE", SP_VOICE_NAME[v]);
}
/* a bank chosen: its own voice when it has one (ROBOT, LAB AI, the songs' CHOIR), else the voice stays */
static void sp_bank_voice(uint32_t b)
{
    if (b < SP_NBANK && SP_BANK_VOICE[b] >= 0)
        sp_voice_set((uint32_t)SP_BANK_VOICE[b]);
}
static void sp_bank_step(int32_t d)
{
    int32_t b = sp_clampi((int32_t)sp_bank_of(vst.sent) + d, 0, (int32_t)SP_NBANKS - 1);
    if (sp.mode == SP_MODE_MOPE)
        return;                                            /* (MOPE has its own words: the knob only gets a comment) */
    if ((uint32_t)b != sp_bank_of(vst.sent)) {
        sp_sent_set((uint32_t)b * 16u + vst.sent % 16u);
        sp_bank_voice((uint32_t)b);
        sp_popup("BANK", sp_bank_name((uint32_t)b));
    }
}
static void sp_page_set(uint32_t p)
{
    su.setup = 0;
    su.page = (uint8_t)(p % SP_NPAGE);
    su.redraw = 1;
    sp_popup("PAGE", SP_PAGE_NAME[su.page]);
}
static void sp_page_step(int32_t d)
{
    if (su.setup) {
        su.setup = 0;
        su.redraw = 1;
    }
    sp_page_set((uint32_t)sp_clampi((int32_t)su.page + d, 0, SP_NPAGE - 1));
}
static void sp_setup_toggle(void)
{
    su.setup = !su.setup;
    su.redraw = 1;
    if (!su.setup && su.glob_dirty)
        sp_popup("SETUP", "KEPT");
}
/* the keys: SENTENCE -> LETTERS -> KIT -> SENTENCE (MOPE has its own button) */
static void sp_mode_set(uint32_t m)
{
    sp.mode = (uint8_t)(m % SP_MODE_MOPE);                 /* (MOPE: its own button, mo_toggle) */
    sp.ntyped = 0;
    sp.typed[0] = 0;
    vst.mode = sp.mode;
    su.glob_dirty = 1;
    su.t_change = fm1_ms;
    su.redraw = 1;
    sp_popup("KEYS", SP_MODE_NAME[sp.mode]);
}
static void sp_mode_toggle(void) { sp_mode_set(sp.mode + 1u); }
/* a KIT pad: hit k at velocity v (the audio ISR plays it at its next block) */
static void sp_hit(uint32_t k, uint32_t v)
{
    uint8_t w = sp.hit_w;
    if ((uint8_t)(w - sp.hit_r) >= 16u)
        return;
    sp.hitq[w % 16u] = (uint8_t)k;
    sp.hitv[w % 16u] = (uint8_t)(v > 127u ? 127u : v < 1u ? 1u : v);
    RING_PUBLISH();
    sp.hit_w = (uint8_t)(w + 1u);
    su.hit |= (uint16_t)(1u << (k & 15u));
    su.t_hitpad[k & 15u] = fm1_ms | 1u;
}
static void sp_auto_toggle(void)
{
    sp.req_auto = sp.auto_on ? 2u : 1u;
    sp_hnote(sp.auto_on ? "STOP" : "PLAY");
}
/* RHYTHM: a groove picked (1..RH_N: its tempo and swing with it; it starts now, or on the bar while PLAY runs), or
 * 0: the drums stop */
static void sp_groove_pick(uint32_t g)
{
    if (!g || g > RH_N) {
        sp.groove_req = 0;
        sp_glob.groove = 0;
        sp_hnote("RHYTHM OFF");
    } else {
        const rh_groove_t *G = &RH_GROOVE[g - 1u];
        sp.groove_req = (uint8_t)g;
        sp_glob.groove = (uint8_t)g;
        vst.groove = (uint8_t)g;
        sp.snd.tempo = G->tempo;
        sp.swing = sp_glob.swing = vst.swing = G->swing;
        sp_changed();
        sp_popup("RHYTHM", G->name);
    }
    su.glob_dirty = 1;
    su.t_change = fm1_ms;
    su.redraw = 1;
}
/* a RHYTHM pad: its groove; the one playing again: stop */
static void sp_rhythm_pad(uint32_t k)
{
    sp_groove_pick(sp.groove == k + 1u && sp.groove_req == 0xFFu ? 0u : k + 1u);
}
static void sp_fill(void)
{
    if (!sp.groove)
        return;
    sp.fill_req = 1;
    sp_hnote("FILL");
}
/* SEQ: the RHYTHM page (the pads the grooves), again: the page before */
static void sp_rhythm_toggle(void)
{
    if (su.page == SP_PG_RHYTHM && !su.setup && !su.mempage)
        sp_page_set(su.rh_back != SP_PG_RHYTHM ? su.rh_back : SP_PG_PLAY);
    else {
        su.rh_back = su.page;
        su.mempage = 0;
        sp_page_set(SP_PG_RHYTHM);
    }
}
static void sp_restart(void)
{
    sp.req_restart = 1;
    sp_hnote("FROM THE START");
}
static void sp_step(int32_t d)
{
    sp.req_step = (int8_t)d;
    sp_hnote(d > 0 ? "NEXT WORD" : "WORD BACK");
}
static void sp_lock_toggle(void)
{
    sp.lock = !sp.lock;
    sp_glob.lock = sp.lock;
    sp_hnote(sp.lock ? "REPEAT WORD" : "REPEAT OFF");
}
static void sp_chord_toggle(void)
{
    char t[20];
    if (sp_glob.chord) {
        su.chord_was = sp_glob.chord;
        sp_glob.chord = 0;
    } else
        sp_glob.chord = su.chord_was ? su.chord_was : 40u;
    sp.chord_ms = sp_glob.chord;
    vst.chord = sp_glob.chord;
    su.glob_dirty = 1;
    su.t_change = fm1_ms;
    su.redraw = 1;
    sp_glob.chord ? (void)sp_s(sp_d(sp_s(t, "CHORD "), sp_glob.chord, 0), " MS") : (void)sp_s(t, "CHORD OFF");
    sp_hnote(t);
}
static void sp_hold_cycle(void)
{
    char t[20];
    sp.snd.hold = (uint8_t)((sp.snd.hold + 1u) % SP_NHOLD);
    sp_changed();
    sp_s(t, SP_HOLD_NAME[sp.snd.hold]);
    sp_hnote(t);
}
static void sp_octave(int32_t d)
{
    char t[20];
    int32_t o = sp_clampi(sp.snd.oct + d, -2, 2);
    if (o != sp.snd.oct) {
        sp.snd.oct = (int8_t)o;
        sp_changed();
    }
    sp_d(sp_s(t, "OCT "), sp.snd.oct, 1);
    sp_hnote(t);
}
static void sp_mem_load(uint32_t k)
{
    char t[16];
    if (!(vst.used >> k & 1u)) {
        sp_d(sp_s(t, "EMPTY "), (int32_t)k + 1, 0);
        sp_popup("MEMORY", t);
        return;
    }
    sp.snd = vst.mem[k];
    vst.loaded = (uint8_t)k;
    su.msel = (uint8_t)k;
    sp_changed();
    sp_d(sp_s(t, "MEMORY "), (int32_t)k + 1, 0);
    sp_popup(t, SP_VOICE_NAME[sp.snd.voice % SP_NVOICE]);
}
static void sp_mem_save(uint32_t k)
{
    char t[16];
    vst.mem[k] = sp.snd;
    vst.used |= (uint16_t)(1u << k);
    vst.loaded = (uint8_t)k;
    vst.cur = sp.snd;
    su.msel = (uint8_t)k;
    sp_store_save();
    su.snd_dirty = 0;
    su.blink = (uint8_t)(k + 1u);
    su.t_blink = fm1_ms;
    sp_d(sp_s(t, "SAVED "), (int32_t)k + 1, 0);
    sp_popup("MEMORY", t);
}
static void sp_mempage_toggle(void)
{
    su.mempage = !su.mempage;
    su.redraw = 1;
    sp_popup("PADS", su.mempage ? "MEMORIES" : "SENTENCES");
}
/* a knob of the page (cell 4..7 = SMK K5..K8, FM-1 KNOB 1..4) turned d detents */
static void sp_param_turn(uint32_t cell, int32_t d)
{
    uint32_t k = SP_PAGE[su.setup ? SP_PG_SETUP : su.page % SP_NPAGE][cell & 3u];
    if (k == SK_NONE)
        return;
    sp_touch(cell);
    if (!sk_turn(&sp.snd, k, d))
        return;
    if (k == SK_GROOVE) {                                  /* (RHYTHM: a groove picked by its knob) */
        sp_groove_pick(sp_glob.groove);
        return;
    }
    if (k >= SK_BEND) {                                    /* SETUP, RHYTHM: global */
        sp.drums = vst.drums = sp_glob.drums;
        sp.swing = vst.swing = sp_glob.swing;
        sp.bend_range = sp_glob.bend;
        sp.chord_ms = sp_glob.chord;
        sp.lock = sp_glob.lock;
        vst.bend = sp_glob.bend;
        vst.chord = sp_glob.chord;
#ifdef SP_TRS
        if (k == SK_TRS)
            sp_trs_set(sp_glob.trs_on);
        vst.trs_off = (uint8_t)!sp_glob.trs_on;
#endif
        su.glob_dirty = 1;
        su.t_change = fm1_ms;
        su.redraw = 1;
        return;
    }
    sp_changed();
}
/* a fader moved (SMK): SPEED, FORMANT, DELAY's send, REVERB; v 0..127 */
static const uint8_t SP_FADER_SK[4] = {SK_SPEED, SK_FORMANT, SK_DMIX, SK_REVERB};
static void sp_fader_set(uint32_t f, uint32_t v)
{
    int32_t lo, hi, st;
    uint8_t *p = sk_ptr(&sp.snd, SP_FADER_SK[f & 3u], &lo, &hi, &st);
    int32_t x = lo + (int32_t)v * (hi - lo + 1) / 128;
    x = sp_clampi(x, lo, hi);
    su.fader = (uint8_t)f;
    su.t_fader = fm1_ms | 1u;
    mo_ui_ctl(MO_CT_F1 + (f & 3u));
    if ((lo < 0 ? (int32_t)(int8_t)*p : (int32_t)*p) != x) {
        *p = (uint8_t)x;
        sp_changed();
    }
}

/* ------------------------------------------------------------------- screen --- */
#define SP_DISP_Y 24
#define SP_INFO_Y 100
#define SP_KNOB_Y 160
static uint16_t sp_bank_c(uint32_t b)
{
    const uint8_t *c = SP_BANK_RGB[b % SP_NBANK_RGB];
    return RGB(c[0], c[1], c[2]);
}

/* text cut to fit w px (the cell's) */
static void sp_fit(char *t, int32_t w)
{
    uint32_t n = str_len(t);
    while (n > 1u && text_w(&AF_S, t) > w)
        t[--n] = 0;
}

static void sp_draw_header(void)
{
    const uint32_t b = sp_bank_of(sp.sent), mope = sp.mode == SP_MODE_MOPE;
    const char *v = SP_VOICE_NAME[sp.snd.voice % SP_NVOICE];
    if (mope && mo_header_note())                          /* (MOPE: what it heard) */
        v = mo_header_note();
    if (mope && mo_dark()) {
        mo_black(0, SP_DISP_Y);
        return;
    }
    sv_begin(0, SP_DISP_Y);
    sv_text(4, (23 - AF_T_CAP_H) / 2 - AF_T_CAP_Y, &AF_T, mope ? "MOPE" : sp_bank_name(b), mope ? MO_RGB : sp_bank_c(b));
    sv_text(236 - text_w(&AF_S, v), 6, &AF_S, v, SV_TEXT);
    sv_rect(0, 23, 240, 1, SV_GRID);
    cv_blit(0, 0);
}

/* the display's two lines: the sentence, the letters typed, or a page's words */
/* a popup (1.5 s) over the glass: black, a light-blue rim, its two lines in the display's segments (the name
 * dimmer, the value bright) */
#define SV_POP_RIM RGB(120, 200, 255)
static void sp_popup_line(int32_t y, const char *s, uint16_t c, uint32_t glow)
{
    int32_t n = 0, x, k;
    while (s[n] && n < SV_COLS)
        n++;
    x = 120 - (n * SV_CW) / 2;
    for (k = 0; k < n; k++)
        sv_char(x + k * SV_CW, y, s[k], 0, c, glow);
}
static void sp_popup_draw(void)
{
    if (!su.t_pop || fm1_ms - su.t_pop >= 1500u) {
        su.t_pop = 0;
        return;
    }
    sv_rbox(4, SP_DISP_Y + 4, 232, 68, RGB(0, 0, 0), SV_POP_RIM);
    sv_rbox(5, SP_DISP_Y + 5, 230, 66, RGB(0, 0, 0), SV_POP_RIM);
    sp_popup_line(SP_DISP_Y + 12, su.pop_t, SV_MID, 0);
    sp_popup_line(SP_DISP_Y + 12 + SV_LH, su.pop_v, SV_ON, 1);
}

static void sp_draw_display(void)
{
    if (sp.tag_words != su.hits_seen) {                    /* (a word started: KIT lights its name) */
        su.hits_seen = sp.tag_words;
        su.t_hit_ui = fm1_ms;
    }
    const int32_t gx = 4, gy = SP_DISP_Y + 4, x0 = 11, y0 = SP_DISP_Y + 12;
    if (mo_screen()) {
        mo_draw_display();
        return;
    }
    sv_begin(SP_DISP_Y, SP_INFO_Y - SP_DISP_Y);
    sv_rbox(gx, gy, 232, 68, SV_GLASS, SV_RIM);
    if (su.mempage) {
        sv_display(x0, y0, "MEMORIES: TAP LOADS, HOLD 1 S SAVES", -1, fm1_ms);
    } else if (su.setup) {
        sv_display(x0, y0, "SETUP: THE WHEEL, CHORDS, THE JACK", -1, fm1_ms);
    } else if (su.page == SP_PG_RHYTHM) {                  /* RHYTHM: the groove, its bar: a step a character */
        uint32_t g = sp.groove_req != 0xFFu && sp.groove_req ? sp.groove_req : sp.groove, k, n;
        char l1[SV_COLS + 1];
        const char *nm = g ? RH_GROOVE[g - 1u].name : RH_PICK_HINT;
        for (k = 0; k < SV_COLS; k++)
            l1[k] = *nm ? *nm++ : ' ';
        l1[SV_COLS] = 0;
        n = g ? RH_GROOVE[g - 1u].steps : 16u;
        for (k = 0; k < SV_COLS; k++) {
            char c = ' ';
            uint32_t lit = 0;
            if (k < n && g) {
                const rh_groove_t *G = &RH_GROOVE[g - 1u];
                c = G->lane[RH_K] && G->lane[RH_K][k] != '.' ? 'K' :
                    (G->lane[RH_S] && G->lane[RH_S][k] != '.') || (G->lane[RH_C] && G->lane[RH_C][k] != '.') ? 'S' :
                    (G->lane[RH_H] && G->lane[RH_H][k] != '.') || (G->lane[RH_O] && G->lane[RH_O][k] != '.') ? '-' :
                    '.';
                lit = sp.groove == g && sp.tr_on && sp.tr_pos == k;
            }
            sv_char(x0 + (int32_t)k * SV_CW, y0, l1[k], 0, g && sp.groove == g ? SV_ON : SV_MID, 0);
            if (lit) {
                sv_rect(x0 + (int32_t)k * SV_CW - 1, y0 + SV_LH + 25, SV_CW + 1, 4, SV_GLOW);
                sv_rect(x0 + (int32_t)k * SV_CW, y0 + SV_LH + 26, SV_CW - 1, 2, SV_ON);
            }
            sv_char(x0 + (int32_t)k * SV_CW, y0 + SV_LH, c, 0, lit ? SV_ON : (k % (n == 12u &&
                    RH_GROOVE[(g ? g : 1u) - 1u].beat == 3u ? 3u : 4u)) == 0u ? SV_MID : SV_DIM, lit);
        }
    } else if (sp.mode == SP_MODE_KIT) {                   /* the hits played, the newest at the right end; KIT */
        char t[SV_COLS + 1];
        uint32_t n = sp.ntyped, k;
        for (k = 0; k < SV_COLS; k++) {
            int32_t at = (int32_t)n - SV_COLS + (int32_t)k;
            t[k] = at >= 0 ? sp.typed[at] : ' ';
        }
        t[SV_COLS] = 0;
        for (k = 0; k < SV_COLS; k++) {
            static const char L2[SV_COLS + 1] = "  KIT  16 HITS      ";
            uint32_t last = n && (int32_t)k >= SV_COLS - (int32_t)str_len(SP_WNAME[sp.wlast_id % SP_NWORD]);
            sv_char(x0 + (int32_t)k * SV_CW, y0, t[k], 0, last && fm1_ms - su.t_hit_ui < 400u ? SV_ON : SV_MID,
                    last && fm1_ms - su.t_hit_ui < 400u);
            sv_char(x0 + (int32_t)k * SV_CW, y0 + SV_LH, L2[k], 0, SV_DIM, 0);
        }
    } else if (sp.mode == SP_MODE_LETTERS) {
        char t[24];
        uint32_t n = sp.ntyped, i, k;
        for (i = 0; i < 2u; i++)
            for (k = 0; k < SV_COLS; k++) {
                char c = ' ';
                uint32_t lit = 0;
                if (i == 0u && n) {                        /* the letters typed, the newest at the right end */
                    int32_t at = (int32_t)n - SV_COLS + (int32_t)k;
                    if (at >= 0) {
                        c = sp.typed[at];
                        lit = (uint32_t)at == n - 1u ? 2u : 1u;
                    }
                }
                if (i == 1u) {
                    static const char L2[SV_COLS + 1] = "    LETTERS  A TO 9 ";
                    c = L2[k];
                    lit = 0;
                }
                sv_char(x0 + (int32_t)k * SV_CW, y0 + (int32_t)i * SV_LH, c, 0, lit == 2u ? SV_ON :
                        lit ? SV_MID : SV_DIM, lit == 2u);
            }
        (void)t;
    } else {
        sv_display(x0, y0, sp_sent_text(sp.sent), sp_sent_len(sp.sent) ? sp.wlast : -1, fm1_ms);
    }
    sp_popup_draw();
    cv_blit(0, SP_DISP_Y);
}

/* the state line: a note (1.5 s), else what is on */
static void sp_state_text(char *t)
{
    char *p = t;
    t[0] = 0;
    if (su.t_note && fm1_ms - su.t_note < 1500u) {
        str_cpy(t, su.note, 24);
        return;
    }
    if (sp.auto_on || sp.play_wait)
        p = sp_s(sp_d(sp_s(p, "PLAY "), sp.snd.tempo, 0), " ");
    else if (sp.groove)
        p = sp_s(sp_d(p, sp.snd.tempo, 0), " BPM ");
    if (sp.lock)
        p = sp_s(p, "REPEAT ");
    if (sp.snd.oct)
        p = sp_s(sp_d(sp_s(p, "OCT"), sp.snd.oct, 1), " ");
    if (vst.loaded < SP_MEMS)
        p = sp_s(sp_d(sp_s(p, "M"), vst.loaded + 1, 0), " ");
    if (p == t)
        sp_s(p, SP_HOLD_NAME[sp.snd.hold % SP_NHOLD]);
}

static void sp_draw_info(void)
{
    const int32_t y = SP_INFO_Y;
    char t[32];
    uint32_t i, n = sp_sent_len(sp.sent);
    if (mo_screen() && !(su.t_fader && fm1_ms - su.t_fader < 1500u)) {
        mo_draw_info();
        return;
    }
    sv_begin(y, SP_KNOB_Y - y);
    if (su.t_fader && fm1_ms - su.t_fader < 1500u) {       /* a fader moved: the four */
        static const char *const L[4] = {"SPEED", "FORMANT", "DELAY", "REVERB"};
        for (i = 0; i < 4u; i++) {
            const int32_t x = (int32_t)i * 60;
            uint16_t c = i == su.fader ? SV_HOT : SV_TEXT;
            if (i == su.fader)
                sv_frame(x + 1, y + 6, 58, 48, SV_LAB);
            sv_text(x + 5, y + 9, &AF_S, L[i], i == su.fader ? SV_HOT : SV_LAB);
            sk_text(&sp.snd, SP_FADER_SK[i], t);
            sp_fit(t, 52);
            sv_text(x + 5, y + 25, &AF_S, t, c);
            sv_bar(x + 5, y + 42, 50, 7, sk_bar(&sp.snd, SP_FADER_SK[i]), c);
        }
        cv_blit(0, (uint32_t)y);
        return;
    }
    su.t_fader = 0;
    if (sp.mode == SP_MODE_KIT) {
        sv_text(4, y + 2, &AF_M, "KIT", SV_LAB);
        sp_s(sp_s(t, "HIT "), sp.ntyped ? SP_WNAME[sp.wlast_id % SP_NWORD] : "-");
        sv_text(4, y + 21, &AF_S, t, SV_TEXT);
    } else if (sp.mode == SP_MODE_LETTERS) {
        sv_text(4, y + 2, &AF_M, "LETTERS", SV_LAB);
        sp_s(sp_note_text(sp_s(t, "AT "), sp.snd.root + 12 * sp.snd.oct), "");
        sv_text(4, y + 21, &AF_S, t, SV_TEXT);
    } else {
        int32_t cur = sp.wlast, x = 4, wd;
        sp_d(sp_s(t, "PAD "), (int32_t)(sp.sent % 16u) + 1, 0);
        x = sv_text(4, y + 2, &AF_M, t, SV_LAB) + 8;
        sp_d(sp_s(sp_d(sp_s(t, "WORD "), cur < 0 ? 0 : cur + 1, 0), " / "), (int32_t)n, 0);
        sv_text(x, y + 4, &AF_S, t, SV_TEXT);
        wd = n ? 112 / (int32_t)n : 112;                   /* the progress: a dash a word */
        wd = wd < 3 ? 3 : wd;
        for (i = 0; i < n && (int32_t)i * wd < 114; i++)
            sv_rect(4 + (int32_t)i * wd, y + 24, wd - 1, 4, (int32_t)i < cur ? SV_DIM : (int32_t)i == cur ? SV_ON :
                    SV_GHOST);
    }
    sp_state_text(t);
    i = (uint32_t)sv_text(4, y + 34, &AF_S, t, su.t_note && fm1_ms - su.t_note < 1500u ? SV_HOT : SV_MID);
    if (sp.tr_on) {                                        /* PLAY, RHYTHM: the beats of the bar, the one now lit */
        uint32_t b, nb = sp_bar() / sp_beat(), now = sp.tr_pos / sp_beat();
        for (b = 0; b < nb; b++)
            sv_rect((int32_t)i + 6 + (int32_t)b * 9, y + 39, 6, 6, now == b ? (b ? SV_ON : SV_HOT) : SV_GHOST);
    }
    for (i = 0; i < SP_NV; i++)                            /* the voices sounding */
        sv_rect(4 + (int32_t)i * 8, y + 52, 5, 4, sp.speaking >> i & 1u ? SV_ON : i < sp.vmax ? SV_GHOST : SV_BG);
    {   /* the mouth */
        int32_t k[10];
        uint32_t f = fm1__lock(), j;
        for (j = 0; j < 10u; j++)
            k[j] = sp.vis_k[j];
        j = (uint32_t)sp.vis_e;
        fm1__unlock(f);
        sv_frame(122, y + 3, 114, 54, SV_GRID);
        sv_mouth(126, y + 6, 108, 48, k, (int32_t)j, su.mouth);
    }
    cv_blit(0, (uint32_t)y);
}

/* a cell of the knob block: its label (and its knob's tag), its value, a bar */
static void sp_cell(uint32_t k, uint32_t cell, int32_t x, int32_t y, uint32_t hot)
{
    uint16_t c = hot ? SV_HOT : SV_TEXT, lc = SV_LAB;
    const char *lab = "", *tag = sp_panel_tag(cell);
    char t[24];
    int32_t bar = -1;
    t[0] = 0;
    if (k == SK_NONE)
        return;
    if (hot)
        sv_frame(x + 1, y + 1, 58, 38, SV_LAB);
    switch (k) {
    case SC_MASTER:
        lab = "MASTER";
        sp_d(t, (int32_t)(sp_pot * 100u / 1023u), 0);
        bar = (int32_t)(sp_pot * 127u / 1023u);
        break;
    case SC_PAGE:
        lab = "PAGE";
        sp_s(t, su.setup ? "SETUP" : SP_PAGE_NAME[su.page % SP_NPAGE]);
        break;
    case SC_VOICE:
        lab = "VOICE";
        sp_s(t, SP_VOICE_NAME[sp.snd.voice % SP_NVOICE]);
        break;
    case SC_BANK: {
        lab = "BANK";
        sp_s(t, sp_bank_short(sp_bank_of(sp.sent)));
        lc = sp_bank_c(sp_bank_of(sp.sent));
        break;
    }
    case SC_PAD:                                           /* (FM-1: PRESETS, the sentences of every bank) */
        lab = "PAD";
        sp_d(sp_s(sp_d(sp_s(t, "B"), (int32_t)sp_bank_of(sp.sent) + 1, 0), " P"), (int32_t)(sp.sent % 16u) + 1, 0);
        lc = sp_bank_c(sp_bank_of(sp.sent));
        break;
    case SC_MEM:                                           /* (FM-1: SAVE held + PRESETS) */
        lab = "MEMORY";
        sp_d(sp_s(t, "M"), (int32_t)su.msel + 1, 0);
        if (!(vst.used >> su.msel & 1u))
            sp_s(t + str_len(t), " -");
        break;
    default:
        lab = SK_NAME[k];
        sk_text(&sp.snd, k, t);
        bar = sk_bar(&sp.snd, k);
    }
    {
        char l[16];
        str_cpy(l, lab, sizeof l);
        sp_fit(l, 54);
        sv_text(x + 4, y + 3, &AF_S, l, lc);
    }
    sp_fit(t, 52);
    sv_text(x + 4, y + 17, &AF_S, t, c);
    {   /* the bar (when the value has one), and the knob's tag at its end: K5 / 1 */
        int32_t tw = tag[0] ? text_w(&AF_S, tag) + 4 : 0, bw = 52 - tw;
        if (bar >= 0) {
            sv_rect(x + 4, y + 34, bw, 2, SV_GRID);
            sv_rect(x + 4, y + 34, bw * bar / 127, 2, hot ? SV_HOT : SV_MID);
        }
        if (tag[0])
            sv_text(x + 56 - tw + 4, y + 27, &AF_S, tag, SV_TAG);
    }
}

static void sp_draw_knobs(void)
{
    uint8_t id[8];
    uint32_t k, hot = su.t_touch && fm1_ms - su.t_touch < 2000u;
    const uint8_t *pg = SP_PAGE[su.setup ? SP_PG_SETUP : su.page % SP_NPAGE];
    if (sp.mode == SP_MODE_MOPE) {                         /* (MOPE: its legend) */
        mo_draw_legend();
        return;
    }
    sp_panel_top(id);
    for (k = 0; k < 4u; k++)
        id[4u + k] = pg[k];
    sv_begin(SP_KNOB_Y, 240 - SP_KNOB_Y);
    sv_rect(0, SP_KNOB_Y, 240, 1, SV_GRID);
    for (k = 0; k < 8u; k++) {
        int32_t x = (int32_t)(k & 3u) * 60, y = SP_KNOB_Y + (int32_t)(k >> 2) * 40;
        if (k & 3u)
            sv_rect(x, y + 4, 1, 32, SV_GRID);
        sp_cell(id[k], k, x, y, hot && su.touch == k);
    }
    sv_rect(0, SP_KNOB_Y + 40, 240, 1, SV_GRID);
    cv_blit(0, SP_KNOB_Y);
}

#include "sp_user.c"                       /* the editor (the USER bank) */
#include "sp_mope_ui.c"                    /* MOPE's picture, its pads */

static void sp_ui_draw(void)
{
    uint32_t per = sp_cpu_q8 > 225u ? 125u : sp_cpu_q8 > 192u ? 66u : 33u;
    if (fm1_ms - su.t_frame < per)
        return;
    su.t_frame = fm1_ms;
    su.frames++;
    if (fm1_ms - su.t_fps >= 1000u) {
        su.fps = su.frames * 1000u / (fm1_ms - su.t_fps);
        su.frames = 0;
        su.t_fps = fm1_ms;
    }
    if (ue.open) {
        ue_draw();
        return;
    }
    if (su.n++ % 4u == 0u || su.redraw)
        sp_draw_header();
    sp_draw_display();
    sp_draw_info();
    if (su.redraw || (su.t_touch && fm1_ms - su.t_touch < 2200u) || su.n % 8u == 0u || sp.mode == SP_MODE_MOPE) {
        su.redraw = 0;
        sp_draw_knobs();
    }
}

/* the boot (1.2 s; the sound and USB already run): the glass, SPEAK37 typed onto it, then READY */
#define SP_BOOT_MS 1200u
static uint32_t sp_mhz;
static void sp_boot_frame(uint32_t t)
{
    static const char *const L1 = "      SPEAK37       ";
    char l1[SV_COLS + 1], l2[SV_COLS + 1];
    uint32_t k, shown = t * 22u / 700u;
    for (k = 0; k < SV_COLS; k++) {
        l1[k] = k < shown ? L1[k] : ' ';
        l2[k] = ' ';
    }
    l1[SV_COLS] = l2[SV_COLS] = 0;
    if (t >= 800u) {
        static const char *const L2 = "       READY        ";
        for (k = 0; k < SV_COLS; k++)
            l2[k] = L2[k];
    }
    for (k = 0; k < 2u; k++) {
        uint32_t i;
        char m[24];
        sv_begin((int32_t)k * 120, 120);
        sv_text(120 - text_w(&AF_T, "SPEAK37") / 2, 14, &AF_T, "SPEAK37", SV_LAB);
        sp_s(sp_d(sp_s(m, "AC791N "), (int32_t)sp_mhz, 0), " MHZ");
        sv_text(120 - text_w(&AF_S, m) / 2, 40, &AF_S, m, SV_MID);
        sv_rbox(4, 64, 232, 68, SV_GLASS, SV_RIM);
        for (i = 0; i < SV_COLS; i++) {
            sv_char(11 + (int32_t)i * SV_CW, 72, l1[i], 0, SV_ON, l1[i] != ' ');
            sv_char(11 + (int32_t)i * SV_CW, 102, l2[i], 0, SV_MID, 0);
        }
        cv_blit(0, k * 120u);
    }
}

/* the sound in use, MASTER, the pad and the settings saved a while after the last change, while nothing sounds (a
 * flash write halts the audio for a moment) */
static void sp_ui_autosave(uint32_t master)
{
    if (master != vst.master && !su.snd_dirty) {
        su.snd_dirty = 1;
        su.t_change = fm1_ms;
    }
    if ((!su.snd_dirty && !su.glob_dirty) || fm1_ms - su.t_change < 4000u || sp.speaking || sp.idle < 44118u)
        return;
    vst.cur = sp.mode == SP_MODE_MOPE && mu.saved ? mu.snd : sp.snd;   /* (MOPE is never kept: what was before it) */
    vst.master = (uint16_t)master;
    vst.sent = sp.sent;
    vst.mode = sp.mode == SP_MODE_MOPE ? mu.prev : sp.mode;
    su.snd_dirty = 0;
    su.glob_dirty = 0;
    sp_store_save();
}
