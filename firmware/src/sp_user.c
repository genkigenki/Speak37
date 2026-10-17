/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 M. Geisler (genkigenki/makkuro) */
/* Speak37's sentence editor: the USER bank's 16 sentences, written on the instrument (sp_ui.c's part; the panels map
 * their buttons and pads onto ue_act, sp_main.c saves the record, sp_state.c packs it).
 *
 * An empty USER pad shows LONG CLICK TO RECORD; a pad of the USER bank held 0.6 s (the FM-1: REC held 0.6 s) opens
 * it. Then the keys pick a letter (A..Z from the lowest key): the list jumps to its first word beginning with it
 * (two or three letters within UE_JUMP_MS of each other: with them, E L: the first word with EL; a pause: anew), and
 *   ADD       the letter picked joins the word being typed (and is said)
 *   PICK      the word list's word selected joins the sentence (the list: the words of the language, EN or DE, that
 *             begin with what is typed; a knob and < > scroll it)
 *   NEXT      the word typed joins the sentence: the vocabulary's word of that name, or, unknown, spelled: its letters
 *   DEL LTR   the last letter typed away;  DEL WORD the sentence's last word away
 *   PREVIEW   the sentence said;  EN / DE   the word list's language
 *   CANCEL    closed, nothing kept;  OK     the sentence kept (saved to the flash, the record of sp_state.c) and SAVED
 * A word joining the sentence is said; the newest glows. The screen: the sentence and the word typed on the glass,
 * the list below it, what the pads (the FM-1: the buttons) do at the bottom, each in its colour. */
#define UE_LONG_MS 600u
#define UE_JUMP_MS 1200u                   /* the keys' letters: one prefix to jump to while they come this quickly */
enum { UE_ADD, UE_PICK, UE_NEXT, UE_DEL_L, UE_DEL_W, UE_PREVIEW, UE_LANG, UE_CANCEL, UE_OK, UE_N, UE_NONE = 255 };
/* the actions' colours (the pads, the legend): ADD yellow, PICK cyan, NEXT blue, DEL LTR orange, DEL WORD amber,
 * PREVIEW violet, EN / DE white, CANCEL red, OK green */
static const uint8_t UE_RGB[UE_N][3] = {{255, 220, 0}, {0, 220, 255}, {60, 110, 255}, {255, 120, 0}, {255, 60, 0},
                                        {190, 110, 255}, {230, 230, 230}, {255, 0, 0}, {0, 255, 60}};
static const char *const UE_NAME[UE_N] = {"ADD", "PICK", "NEXT", "DEL LTR", "DEL WORD", "PREVIEW", "EN / DE",
                                          "CANCEL", "OK"};
/* the SMK-37's pads: the top row ADD PICK NEXT DEL LTR DEL WORD PREVIEW EN/DE, the bottom row's last two CANCEL OK */
static const uint8_t UE_PAD[16] = {UE_ADD, UE_PICK, UE_NEXT, UE_DEL_L, UE_DEL_W, UE_PREVIEW, UE_LANG, UE_NONE,
                                   UE_NONE, UE_NONE, UE_NONE, UE_NONE, UE_NONE, UE_NONE, UE_CANCEL, UE_OK};
static const char *ue_tag(uint32_t a);    /* the panel: the pad or button an action is on ("" the pads) */

static struct {
    uint8_t open, slot, lang;              /* lang: 0 EN, 1 DE */
    char cand;                             /* the letter the keys picked (0 none) */
    char typed[16];
    uint8_t nt;
    uint16_t w[SP_UMAX];
    uint8_t n;
    uint16_t m0, nm, sel;                  /* the list: its first word in the dictionary, how many, the one selected */
    uint8_t kq_r;
    char jp[4];                            /* the keys' prefix (the list jumps to it), its length, its last letter's time */
    uint8_t njp;
    uint32_t t_jp;
    uint32_t t_glow, t_cand, t_act;        /* the newest word's glow, the letter picked, the last action (its pad) */
    uint8_t act;
    uint8_t pdown[16];                     /* (the pads: held since t_down, the long press done) */
    uint8_t pdone[16];
    uint32_t t_down[16];
} ue;

/* a word said at ROOT, after the ones before it (the audio's sp_say_run) */
static void sp_say(uint32_t w)
{
    uint8_t q = sp.say_w;
    if ((uint8_t)(q - sp.say_r) >= 64u)
        return;
    sp.sayq[q % 64u] = (uint16_t)(w % SP_NWORD);
    RING_PUBLISH();
    sp.say_w = (uint8_t)(q + 1u);
}
static void sp_say_clear(void) { sp.say_r = sp.say_w; }

static const uint16_t *ue_dict(uint32_t *n)
{
    *n = ue.lang ? SP_NDICT_DE : SP_NDICT_EN;
    return ue.lang ? SP_DICT_DE : SP_DICT_EN;
}
/* -1, 0, +1: a word's name against what is typed (0: it begins with it) */
static int32_t ue_cmp(const char *name)
{
    uint32_t i;
    for (i = 0; i < ue.nt; i++) {
        if (!name[i] || (uint8_t)name[i] < (uint8_t)ue.typed[i])
            return -1;
        if ((uint8_t)name[i] > (uint8_t)ue.typed[i])
            return 1;
    }
    return 0;
}
/* the list: the dictionary's words that begin with what is typed (it is sorted by name: they stand together) */
static void ue_match(void)
{
    uint32_t n, lo = 0, hi, e;
    const uint16_t *d = ue_dict(&n);
    hi = n;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2u;
        if (ue_cmp(SP_WNAME[d[m]]) < 0)
            lo = m + 1u;
        else
            hi = m;
    }
    for (e = lo; e < n && ue_cmp(SP_WNAME[d[e]]) == 0; e++)
        ;
    ue.m0 = (uint16_t)lo;
    ue.nm = (uint16_t)(e - lo);
    ue.sel = 0;
}
/* the list's selection: its first word beginning with the keys' prefix (after what is typed), the nearest after it
 * when none does */
static void ue_jump(void)
{
    uint32_t n, lo = 0, hi = ue.nm;
    const uint16_t *d = ue_dict(&n);
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2u, i;
        const char *nm = SP_WNAME[d[(ue.m0 + m) % n]] + ue.nt;
        int32_t c = 0;
        for (i = 0; i < ue.njp && !c; i++)
            c = !nm[i] || (uint8_t)nm[i] < (uint8_t)ue.jp[i] ? -1 : (uint8_t)nm[i] > (uint8_t)ue.jp[i] ? 1 : 0;
        if (c < 0)
            lo = m + 1u;
        else
            hi = m;
    }
    if (ue.nm)
        ue.sel = (uint16_t)(lo < ue.nm ? lo : ue.nm - 1u);
    su.redraw = 1;
}
static void ue_scroll(int32_t d)
{
    if (!ue.nm)
        return;
    ue.sel = (uint16_t)sp_clampi((int32_t)ue.sel + d, 0, (int32_t)ue.nm - 1);
    su.redraw = 1;
}

/* a word joins the sentence (and is said, and glows) */
static void ue_put(uint32_t w, uint32_t say)
{
    if (ue.n >= SP_UMAX)
        return;
    ue.w[ue.n++] = (uint16_t)w;
    if (say)
        sp_say(w);
    ue.t_glow = fm1_ms | 1u;
}
static void ue_typed_clear(void)
{
    ue.nt = 0;
    ue.typed[0] = 0;
    ue.njp = 0;
    ue_match();
}
/* the word typed: the vocabulary's word of that name (the list's language first), else spelled */
static void ue_next(void)
{
    uint32_t n, i, w = 0xFFFFu;
    const uint16_t *d = ue_dict(&n);
    if (!ue.nt)
        return;
    if (ue.nm && SP_WNAME[d[ue.m0]][ue.nt] == 0)
        w = d[ue.m0];
    for (i = 0; w == 0xFFFFu && i < SP_NWORD; i++)        /* (a bank's word: the same name, the same language) */
        if (!(SP_WORD[i].flags & (SP_W_PERC | SP_W_SING)) && !!(SP_WORD[i].flags & SP_W_DE) == ue.lang &&
            str_len(SP_WNAME[i]) == ue.nt && ue_cmp(SP_WNAME[i]) == 0)
            w = i;
    if (w != 0xFFFFu)
        ue_put(w, 1);
    else
        for (i = 0; i < ue.nt && ue.n < SP_UMAX; i++) {   /* unknown: spelled, a letter a word */
            uint32_t c = (uint8_t)ue.typed[i];
            if (c >= 'A' && c <= 'Z')
                ue_put(c - 'A', 1);
        }
    ue_typed_clear();
}

static void ue_close(void)
{
    ue.open = 0;
    sp.edit = 0;
    su.redraw = 1;
}
static void ue_open(uint32_t slot)
{
    uint32_t i;
    if (sp.auto_on)
        sp.req_auto = 2;
    ue.open = 1;
    ue.slot = (uint8_t)(slot % 16u);
    ue.n = sp.user_n[ue.slot];
    for (i = 0; i < ue.n; i++)
        ue.w[i] = sp.user_w[ue.slot][i];
    ue.cand = 0;
    ue.kq_r = sp.kq_w;
    ue.act = UE_NONE;
    ue.t_glow = 0;
    ue_typed_clear();
    sp.edit = 1;
    su.redraw = 1;
    {
        char t[16];
        sp_d(sp_s(t, "USER "), (int32_t)ue.slot + 1, 0);
        sp_popup("EDIT", t);
    }
}

/* an action (a pad, a button) */
static void ue_act(uint32_t a)
{
    if (!ue.open || a >= UE_N)
        return;
    ue.act = (uint8_t)a;
    ue.t_act = fm1_ms | 1u;
    su.redraw = 1;
    switch (a) {
    case UE_ADD:
        if (ue.cand && ue.nt < sizeof ue.typed - 1u) {
            ue.typed[ue.nt++] = ue.cand;
            ue.typed[ue.nt] = 0;
            sp_say((uint32_t)(ue.cand - 'A'));            /* (the letter's name) */
            ue.njp = 0;
            ue_match();
            ue.t_glow = fm1_ms | 1u;
        }
        break;
    case UE_PICK:
        if (ue.nm) {
            uint32_t n;
            const uint16_t *d = ue_dict(&n);
            ue_put(d[(ue.m0 + ue.sel) % n], 1);
            ue_typed_clear();
        }
        break;
    case UE_NEXT:
        ue_next();
        break;
    case UE_DEL_L:
        if (ue.nt) {
            ue.typed[--ue.nt] = 0;
            ue.njp = 0;
            ue_match();
        }
        break;
    case UE_DEL_W:
        if (ue.n) {
            ue.n--;
            sp_say(SP_WID_DELETED);
        }
        break;
    case UE_PREVIEW: {
        uint32_t i;
        sp_say_clear();
        for (i = 0; i < ue.n && i < 60u; i++)
            sp_say(ue.w[i]);
        break;
    }
    case UE_LANG:
        ue.lang = !ue.lang;
        ue_match();
        if (ue.njp)
            ue_jump();
        sp_popup("WORD LIST", ue.lang ? "DEUTSCH" : "ENGLISH");
        break;
    case UE_CANCEL:
        sp_say_clear();
        sp_say(SP_WID_CANCELLED);
        sp_popup("EDIT", "CANCELLED");
        ue_close();
        break;
    case UE_OK: {
        uint32_t i;
        if (ue.nt)
            ue_next();
        sp.user_n[ue.slot] = 0;                            /* (the audio reads it: the words first, then the count) */
        RING_PUBLISH();
        for (i = 0; i < ue.n; i++)
            sp.user_w[ue.slot][i] = ue.w[i];
        RING_PUBLISH();
        sp.user_n[ue.slot] = ue.n;
        sp_user_save();
        sp_say_clear();
        sp_say(SP_WID_SAVED);
        sp_popup("EDIT", ue.n ? "SAVED" : "EMPTY");
        sp.req_sent = (uint16_t)(SP_USENT + ue.slot);      /* (from its first word) */
        ue_close();
        break;
    }
    default:
        break;
    }
}

/* the keys (the audio queues them while the editor is open): a key picks a letter */
static void ue_keys(void)
{
    while (ue.kq_r != sp.kq_w) {
        uint32_t k = sp.kq[ue.kq_r % 16u];
        ue.kq_r++;
#if !BOARD_SMK37
        k -= 5u;                                           /* (the FM-1's lowest key, F3, is key 5: there A) */
#endif
        if (k < 26u) {
            ue.cand = (char)('A' + k);
            ue.t_cand = fm1_ms | 1u;
            if (!ue.njp || ue.njp >= 3u || fm1_ms - ue.t_jp >= UE_JUMP_MS)
                ue.njp = 0;                                /* (a pause, or three letters: a new prefix) */
            ue.jp[ue.njp++] = ue.cand;
            ue.jp[ue.njp] = 0;
            ue.t_jp = fm1_ms;
            ue_jump();
            su.redraw = 1;
        }
    }
}

/* a USER pad: held UE_LONG_MS opens the editor (the panels: down, now); 1 when it did */
static uint32_t ue_long(uint32_t pad, uint32_t down)
{
    pad &= 15u;
    if (down && !ue.pdown[pad]) {
        ue.t_down[pad] = fm1_ms;
        ue.pdone[pad] = 0;
    }
    ue.pdown[pad] = (uint8_t)down;
    if (down && !ue.pdone[pad] && fm1_ms - ue.t_down[pad] >= UE_LONG_MS) {
        ue.pdone[pad] = 1;
        return 1;
    }
    return 0;
}

/* the editor's pad colours: an action's pad in its colour (lit at full a moment after it was pressed), the rest dark */
static void sp_edit_pad_rgb(uint32_t pad, uint8_t *rgb)
{
    uint32_t a = UE_PAD[pad & 15u], k, pct;
    if (a == UE_NONE) {
        rgb[0] = rgb[1] = rgb[2] = 0;
        return;
    }
    pct = ue.act == a && ue.t_act && fm1_ms - ue.t_act < 200u ? 100u : 40u;
    if (a == UE_ADD && ue.cand && fm1_ms - ue.t_cand < 1500u)
        pct = 70u;                                         /* (a letter picked: ADD waits) */
    for (k = 0; k < 3u; k++)
        rgb[k] = (uint8_t)(UE_RGB[a][k] * pct / 100u);
}

/* the user sentence's text as the display shows it (its words' names; empty: the hint) */
static const char *sp_user_text(uint32_t slot)
{
    static char t[SP_UMAX * 16u];
    char *p = t;
    uint32_t i, n = sp.user_n[slot % 16u];
    if (!n)
        return UE_HINT;
    for (i = 0; i < n; i++) {
        if (i)
            *p++ = ' ';
        p = sp_s(p, SP_WNAME[sp.user_w[slot % 16u][i] % SP_NWORD]);
    }
    return t;
}

/* ---------------------------------------------------------------- screen --- */
/* the glass: the sentence's end and the word typed (the cursor blinking, the letter picked there), the list below */
static void ue_draw_display(void)
{
    const int32_t x0 = 11, y0 = SP_DISP_Y + 12;
    char l1[64 + 24], c1[SV_COLS], c2[SV_COLS];
    uint8_t v1[SV_COLS], v2[SV_COLS];
    uint32_t i, k, len = 0, glow_from = 0xFFFFu, typed_from;
    sv_begin(SP_DISP_Y, SP_INFO_Y - SP_DISP_Y);
    sv_rbox(4, SP_DISP_Y + 4, 232, 68, SV_GLASS, SV_RIM);
    /* line 1: the last words, the word typed, the cursor (the letter picked) */
    for (i = ue.n > 4u ? ue.n - 4u : 0u; i < ue.n; i++) {
        const char *nm = SP_WNAME[ue.w[i] % SP_NWORD];
        if (i + 1u == ue.n && ue.t_glow && fm1_ms - ue.t_glow < 700u && !ue.nt)
            glow_from = len;
        while (*nm && len < 60u)
            l1[len++] = *nm++;
        l1[len++] = ' ';
    }
    typed_from = len;
    for (i = 0; i < ue.nt; i++)
        l1[len++] = ue.typed[i];
    l1[len++] = ue.cand && fm1_ms - ue.t_cand < 4000u ? ue.cand : (fm1_ms / 400u & 1u) ? '_' : ' ';
    for (k = 0; k < SV_COLS; k++) {
        int32_t at = (int32_t)len - SV_COLS + (int32_t)k;
        c1[k] = at >= 0 ? l1[at] : ' ';
        v1[k] = at < 0 ? 1u : (uint32_t)at + 1u == len ? 3u : (uint32_t)at >= typed_from ? 2u :
                (uint32_t)at >= glow_from ? 2u : 1u;
    }
    /* line 2: the list from the word selected (it underlined), or what NEXT will do */
    for (k = 0; k < SV_COLS; k++)
        c2[k] = ' ', v2[k] = 0;
    if (ue.nm) {
        uint32_t n, j = 0, w;
        const uint16_t *d = ue_dict(&n);
        for (w = ue.sel; w < ue.nm && j < SV_COLS; w++) {
            const char *nm = SP_WNAME[d[(ue.m0 + w) % n]];
            if (j && j + str_len(nm) > SV_COLS)
                break;
            while (*nm && j < SV_COLS) {
                c2[j] = *nm++;
                v2[j++] = w == ue.sel ? 2u : 1u;
            }
            j++;
        }
    } else if (ue.nt) {
        static const char S[SV_COLS + 1] = "NEW WORD: SPELLED   ";
        for (k = 0; k < SV_COLS; k++)
            c2[k] = S[k], v2[k] = 0;
    }
    for (k = 0; k < SV_COLS; k++) {
        int32_t cx = x0 + (int32_t)k * SV_CW;
        uint32_t l = v1[k];
        sv_char(cx, y0, c1[k], 0, l == 3u ? SV_HOT : l == 2u ? SV_ON : SV_MID, l >= 2u);
        if (v2[k] == 2u) {
            sv_rect(cx - 1, y0 + SV_LH + 25, SV_CW + 1, 4, SV_GLOW);
            sv_rect(cx, y0 + SV_LH + 26, SV_CW - 1, 2, SV_ON);
        }
        sv_char(cx, y0 + SV_LH, c2[k], 0, v2[k] == 2u ? SV_ON : v2[k] ? SV_MID : SV_DIM, v2[k] == 2u);
    }
    sp_popup_draw();
    cv_blit(0, SP_DISP_Y);
}

static void ue_draw_info(void)
{
    const int32_t y = SP_INFO_Y;
    char t[40];
    int32_t x;
    sv_begin(y, SP_KNOB_Y - y);
    sp_d(sp_s(t, "EDIT USER "), (int32_t)ue.slot + 1, 0);
    x = sv_text(4, y + 2, &AF_M, t, SV_LAB) + 10;
    sv_text(x, y + 4, &AF_S, ue.lang ? "DE" : "EN", SV_TEXT);
    sp_s(sp_d(sp_s(sp_d(sp_s(t, "WORDS "), ue.n, 0), " / "), (int32_t)SP_UMAX, 0), "");
    sv_text(4, y + 22, &AF_S, t, SV_TEXT);
    if (ue.nm)
        sp_s(sp_d(sp_s(sp_d(t, (int32_t)ue.sel + 1, 0), " / "), ue.nm, 0), ue.nt ? " MATCH" : " IN LIST");
    else
        sp_s(t, ue.nt ? "NOT IN LIST" : "");
    sv_text(120, y + 22, &AF_S, t, SV_MID);
    if (ue.njp && fm1_ms - ue.t_jp < UE_JUMP_MS)        /* the keys' prefix, while more letters may join it */
        sp_s(sp_s(sp_s(sp_s(t, "TO "), ue.jp), "...   ADD "), (char[2]){ue.cand, 0});
    else if (ue.cand)
        sp_s(sp_s(sp_s(t, "KEY "), (char[2]){ue.cand, 0}), "   ADD IT");
    else
        sp_s(t, "KEYS: A LETTER");
    sv_text(4, y + 40, &AF_S, t, ue.cand ? SV_HOT : SV_MID);
    cv_blit(0, (uint32_t)y);
}

/* the legend: the actions in three columns, each in its colour (the SMK-37: the pads; the FM-1: its buttons) */
static void ue_draw_legend(void)
{
    uint32_t a;
    sv_begin(SP_KNOB_Y, 240 - SP_KNOB_Y);
    sv_rect(0, SP_KNOB_Y, 240, 1, SV_GRID);
    for (a = 0; a < UE_N; a++) {
        const uint8_t *c = UE_RGB[a];
        int32_t x = (int32_t)(a % 3u) * 80, y = SP_KNOB_Y + 4 + (int32_t)(a / 3u) * 19;
        char t[24];
        uint16_t col = RGB(c[0], c[1], c[2]);
        uint32_t lit = ue.act == a && ue.t_act && fm1_ms - ue.t_act < 300u;
        if (ue_tag(a)[0]) {                                /* the FM-1: the button's name, then the action */
            int32_t tx = sv_text(x + 3, y + 1, &AF_S, ue_tag(a), lit ? SV_TEXT : SV_TAG);
            str_cpy(t, a == UE_PREVIEW ? "HEAR" : UE_NAME[a], sizeof t);   /* (PLAY PREVIEW: too long here) */
            sp_fit(t, x + 78 - tx);
            sv_text(tx, y + 1, &AF_S, t, lit ? SV_TEXT : col);
            continue;
        }
        sv_rect(x + 4, y + 3, 10, 10, col);                /* the SMK-37: the pad's colour */
        if (lit)
            sv_frame(x + 2, y + 1, 14, 14, SV_TEXT);
        str_cpy(t, UE_NAME[a], sizeof t);
        sp_fit(t, 60);
        sv_text(x + 18, y + 1, &AF_S, t, lit ? SV_TEXT : col);
    }
    sv_text(4, SP_KNOB_Y + 62, &AF_S, UE_SCROLL_HINT, SV_TAG);
    cv_blit(0, SP_KNOB_Y);
}

static void ue_draw(void)
{
    if (su.n++ % 4u == 0u || su.redraw)
        sp_draw_header();
    ue_draw_display();
    ue_draw_info();
    if (su.redraw || su.n % 4u == 0u) {
        su.redraw = 0;
        ue_draw_legend();
    }
}
