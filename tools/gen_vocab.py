#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 M. Geisler (genkigenki/makkuro)
"""Speak37's word data: every word of the sentence banks (assets/speak/banks.txt), the letters A..Z and the
numbers, spoken by espeak-ng and LPC-encoded here into TMS5220-style frames (energy, pitch, K1..K10).
No TI vocabulary is used: every word is rendered and encoded from scratch.

  tools/gen_vocab.py OUT.h [--wav DIR] [--refresh]

The encoder (our own):
  - espeak-ng (GPL-3.0-or-later, through its shared library: the pip package espeakng-loader, or a system
    libespeak-ng) speaks each word alone, US English, 22.05 kHz;
  - resampled to 8 kHz; pre-emphasis 0.9373 (the TI chips' speech is bright: the lattice models the emphasised
    spectrum, the playback can take it back, sp_dsp.c DEEMPH);
  - 10 ms frames (the chip has 25 ms; measured with a speech recogniser, 25 ms frames lost half the words that
    10 ms frames keep), a 30 ms Hamming window: autocorrelation, Levinson-Durbin to 10 reflection coefficients, each
    quantised to the nearest value of the TMS5220 tables (the chip's own coding, MAME tms5110r.hxx, BSD-3-Clause);
  - pitch: normalised autocorrelation of the 900 Hz low-passed signal, 50..500 Hz, voiced above 0.45 (a 3-frame
    median); unvoiced frames keep K1..K4 only (as the chip);
  - energy: the prediction residual's RMS; voiced frames scaled for an excitation of a fixed energy a period at a
    reference period (the playback keeps the level when the key moves the pitch, sp_dsp.c); per word normalised,
    the loudest frame at index 14;
  - silence trimmed at both ends; a repeat frame where the indices do not change; a stop frame at the end.
The bit stream (MSB first): E 4 [0 silence, 15 stop], R 1, P 6, then unless R: K1 5, K2 5, K3 4, K4 4, and when
P != 0: K5 4, K6 4, K7 4, K8 3, K9 3, K10 3.

The encoded words are cached in assets/speak/vocab_lpc.json (keyed by the text spoken and the encoder version), so a
build without espeak-ng uses the cache; --refresh encodes everything again. --wav DIR writes each word as the
reference decoder below plays it (8 kHz) for listening.
"""
import argparse
import hashlib
import json
import math
import os
import re
import sys
import wave
from pathlib import Path

import numpy as np

SRC = Path(__file__).resolve().parents[1]
BANKS = SRC / "assets" / "speak" / "banks.txt"
CACHE = SRC / "assets" / "speak" / "vocab_lpc.json"
ENC_VERSION = 4
FS = 8000
FRAME = 80                          # 10 ms (the chip's 25 ms frames lose too many words, see above)
WIN = 240                           # 30 ms
PRE = 0.9373
P_REF = 64.0                        # the reference period (125 Hz at 8 kHz): voiced energy is scaled for it

# ---- the TMS5220's coding tables (MAME tms5110r.hxx: TI_028X_LATER_ENERGY, TI_5220_PITCH, TI_5110_5220_LPC)
ENERGY = [0, 1, 2, 3, 4, 6, 8, 11, 16, 23, 33, 47, 63, 85, 114, 0]
PERIOD = [0, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
          40, 41, 42, 44, 46, 48, 50, 52, 53, 56, 58, 60, 62, 65, 68, 70, 72, 76, 78, 80, 84, 86, 91, 94, 98, 101,
          105, 109, 114, 118, 122, 127, 132, 137, 142, 148, 153, 159]
K = [[-501, -498, -497, -495, -493, -491, -488, -482, -478, -474, -469, -464, -459, -452, -445, -437, -412, -380,
      -339, -288, -227, -158, -81, -1, 80, 157, 226, 287, 337, 379, 411, 436],
     [-328, -303, -274, -244, -211, -175, -138, -99, -59, -18, 24, 64, 105, 143, 180, 215, 248, 278, 306, 331, 354,
      374, 392, 408, 422, 435, 445, 455, 463, 470, 476, 506],
     [-441, -387, -333, -279, -225, -171, -117, -63, -9, 45, 98, 152, 206, 260, 314, 368],
     [-328, -273, -217, -161, -106, -50, 5, 61, 116, 172, 228, 283, 339, 394, 450, 506],
     [-328, -282, -235, -189, -142, -96, -50, -3, 43, 90, 136, 182, 229, 275, 322, 368],
     [-256, -212, -168, -123, -79, -35, 10, 54, 98, 143, 187, 232, 276, 320, 365, 409],
     [-308, -260, -212, -164, -117, -69, -21, 27, 75, 122, 170, 218, 266, 314, 361, 409],
     [-256, -161, -66, 29, 124, 219, 314, 409],
     [-256, -176, -96, -15, 65, 146, 226, 307],
     [-205, -132, -59, 14, 87, 160, 234, 307]]
KBITS = [5, 5, 4, 4, 4, 4, 4, 3, 3, 3]
# the chirp (the excitation of a voiced period) of the first Speak & Spell chips: MAME TI_0280_PATENT_CHIRP, signed
CHIRP = [0x00, 0x2a, 0xd4, 0x32, 0xb2, 0x12, 0x25, 0x14, 0x02, 0xe1, 0xc5, 0x02, 0x5f, 0x5a, 0x05, 0x0f, 0x26, 0xfc,
         0xa5, 0xa5, 0xd6, 0xdd, 0xdc, 0xfc, 0x25, 0x2b, 0x22, 0x21, 0x0f, 0xff, 0xf8, 0xee, 0xed, 0xef, 0xf7, 0xf6,
         0xfa, 0x00, 0x03, 0x02, 0x01]
CHIRP = [c - 256 if c > 127 else c for c in CHIRP]
CHIRP_E = sum(c * c for c in CHIRP)


# ------------------------------------------------------------------ banks ---
LETTERS = [chr(ord("A") + i) for i in range(26)]
NUMBERS = ["ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE"]


VOICES = {"en": b"en-us", "de": b"de", "fr": b"fr", "it": b"it", "es": b"es", "nl": b"nl", "pt": b"pt-br",
          "sv": b"sv", "cmn": b"cmn", "ja": b"ja"}
LANGS = "|".join(VOICES)
VOICE_IDS = ["CLASSIC", "VINTAGE", "DEEP", "CHILD", "WHISPER", "ROBOT", "LABAI", "GIANT", "CHOIR"]
TOKEN = re.compile(r"([\w'\-]*)(?:\{([^}\s]+)\})?([.,!?:]*)$")


def fold(word, lang):
    """a word as the 16-segment display shows it: capitals A..Z (German ae oe ue ss, else the accents dropped)"""
    import unicodedata
    w = word.upper()
    if lang == "de":
        w = w.replace("Ä", "AE").replace("Ö", "OE").replace("Ü", "UE")
    w = w.replace("ß", "SS").replace("ẞ", "SS").replace("Œ", "OE").replace("Æ", "AE").replace("Ø", "OE")
    w = "".join(c for c in unicodedata.normalize("NFKD", w) if not unicodedata.combining(c))
    return w


def parse_banks(path=BANKS):
    """-> banks [(name, kind, voice, [(lang, shown, [token, ...]), ...])] (kind "", "sing" or "perc"; voice: the
    bank's own voice or -1), say {KEY: text}, kit [(KEY, note)]"""
    banks, say, kit, cur = [], {}, [], None
    for no, ln in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        m = re.fullmatch(r"\[([A-Z0-9 ]+?)((?:\s+(?:sing|perc|voice=[A-Z]+))*)\]", ln)
        if m:
            opts = m[2].split()
            if m[1] == "KIT":
                cur = "KIT"
                continue
            kind = "sing" if "sing" in opts else "perc" if "perc" in opts else ""
            vo = [o[6:] for o in opts if o.startswith("voice=")]
            if vo and vo[0] not in VOICE_IDS:
                raise SystemExit(f"{path}:{no}: voice {vo[0]}: one of {', '.join(VOICE_IDS)}")
            cur = (m[1].strip(), kind, VOICE_IDS.index(vo[0]) if vo else -1, [])
            banks.append(cur)
            continue
        m = re.fullmatch(r"([~!]?[A-Z0-9'\-]+)\s*=\s*(.+)", ln)
        if m:
            say[m[1]] = m[2].strip()
            continue
        if cur is None:
            raise SystemExit(f"{path}:{no}: a sentence before the first [BANK]")
        if cur == "KIT":
            for t in ln.split():
                k, _, n = t.partition("/")
                kit.append(("!" + k, int(n) if n else 60))
            continue
        lang = "en"
        m = re.match(r"(" + LANGS + r"):\s*", ln)
        if m:
            lang = m[1]
            ln = ln[m.end():]
        toks = ln.split()
        kind = cur[1]
        if toks and toks[0] == "~":                # a sung line in a spoken bank
            kind = "sing"
            toks = toks[1:]
        shown = []
        for t in toks:
            mt = TOKEN.fullmatch(t)
            if not mt or not re.search(r"\w", mt[1]):
                raise SystemExit(f"{path}:{no}: {t!r}: a word (letters, digits, ' and -), a {{pronunciation}} may follow")
            shown.append(fold(mt[1], lang) + mt[3])
        cur[3].append((lang, kind, " ".join(shown), toks))
    for name, kind, vo, s in banks:
        if not re.fullmatch(r"[A-Z0-9 ]+", name):
            raise SystemExit(f"{path}: bank name {name!r}: capitals and digits (the display)")
        if len(s) != 16:
            raise SystemExit(f"{path}: bank {name}: {len(s)} sentences, 16 needed")
        if len(name) > 12:
            raise SystemExit(f"{path}: bank name {name!r} longer than 12")
    if len(kit) != 16:
        raise SystemExit(f"{path}: [KIT perc]: {len(kit)} hits, 16 needed")
    return banks, say, kit


def word_key(tok):
    return re.sub(r"[^\w'\-]", "", tok.upper()).strip("-")


class Entry:
    """a word of the vocabulary: its key (unique), its name on the screen, what espeak-ng says, the voice, a hit's
    length (ms, 0: not a hit), its flags, lite (a word of the word lists only: encoded more compactly)"""
    def __init__(self, key, name, text, lang="en", ms=0, flags=0, lite=False):
        name = fold(name, lang)
        self.key, self.name, self.text, self.lang, self.ms, self.flags = key, name, text, lang, ms, flags
        self.lite = ms == 0                         # (every word but a hit: espeak-ng at 160 words a minute, K1..K10
                                                    # within a step repeat: as clear to a recogniser, ~40 % fewer bytes)

    def cache_key(self):
        k = f"{ENC_VERSION}:" + ("" if self.lang == "en" else self.lang + ":") + self.text
        return k + (f"@{self.ms}" if self.ms else "") + ("~lite160" if self.lite else "")


def sentence_entries(lang, toks, kind, say):
    """a sentence's words as vocabulary entries; a single letter next to another single letter (C A T) is that
    letter's name, the article A otherwise; in a sung bank every word is a syllable, in a percussion bank a hit;
    WORD-WORD is one unit said together; WORD{text} is said as text (espeak-ng's input: [[phonemes]] too)"""
    parts = []
    for t in toks:
        mt = TOKEN.fullmatch(t)
        parts.append((word_key(mt[1]), mt[2]))
    out = []
    pre = "" if lang == "en" else lang + ":"
    for i, (k, pron) in enumerate(parts):
        name = k
        lite = lang != "en"                         # (another language's words: the compact encoding)
        if kind == "perc":
            m = re.fullmatch(r"(.+?)\s+(\d+)", say.get("!" + k, ""))
            txt = m[1] if m else say.get("!" + k, k.lower())
            out.append(Entry("!" + k, name, txt, "en", int(m[2]) if m else 120, 1))
            continue
        if kind == "sing":
            txt = pron or (say.get("~" + k) if lang == "en" else None) or k.lower()
            out.append(Entry("~" + pre + k + ("{" + pron + "}" if pron else ""), name, txt, lang, 0, 2))
            continue
        if pron:
            out.append(Entry(pre + k + "{" + pron + "}", name, pron, lang, lite=lite))
            continue
        single = len(k) == 1 and k.isalpha()
        if single and lang != "en" and any(0 <= j < len(parts) and len(parts[j][0]) == 1 and parts[j][0].isalpha()
                                           for j in (i - 1, i + 1)):
            out.append(Entry(pre + k + "#", k, k.lower(), lang, lite=True))   # a letter, by its name in that language
            continue
        single = single and lang == "en"
        run = single and any(0 <= j < len(parts) and len(parts[j][0]) == 1 and parts[j][0].isalpha()
                             for j in (i - 1, i + 1))
        if single and (run or k not in "AI"):
            out.append(letter_entry(k, say))
        elif k == "A" and lang == "en":
            out.append(Entry("A", "A", "[[@]]"))                   # the article: a schwa
        else:
            txt = say.get(k) if lang == "en" and k in say else k.lower().replace("-", " ")
            out.append(Entry(pre + k, name, txt, lang, lite=lite))
    return out


def letter_entry(L, say):
    return Entry(L + "#", L, say.get(L, L.lower()))


def spoken(key, say):
    """(the reference scripts' helper) what espeak-ng is given for a plain English key"""
    if key.endswith("#"):
        return say.get(key[:-1], key[:-1].lower())
    if key == "A":
        return "[[@]]"
    return say.get(key, key.lower())


DE_ASCII = str.maketrans({"ä": "ae", "ö": "oe", "ü": "ue", "ß": "ss"})
# not in the word lists: words a family-friendly instrument does not need (a frequency list of film subtitles has them)
BLOCK = set("""sex sexy fuck fucking fucked shit damn hell ass bitch bastard dick cock pussy whore slut kill killed
killing kills dead die died murder gun guns drug drugs blood bloody god jesus christ nazi naked porn rape hate stupid
idiot crap piss bullshit sexual
scheiße scheisse fick ficken arsch hure verdammt mist töten tot tod mord blut nazi nackt dumm idiot""".split())


def word_list(path, lang, n):
    """the first n words of a word list (assets/speak/words_*.txt: a word a line, the most common first)"""
    out = []
    for ln in path.read_text(encoding="utf-8").splitlines():
        w = ln.strip()
        if not w or w.startswith("#"):
            continue
        out.append(w)
        if len(out) >= n:
            break
    return out


def perc_shape(frames, ms):
    """a hit: cut to ms, its energy falling ~30 dB over it (tight, no tail)"""
    n = max(1, ms // (FRAME * 1000 // FS))
    out = []
    for i, f in enumerate(frames[:n]):
        f = dict(f)
        if f["e"]:
            e = f["e"] - round(10 * i / n)
            if e < 1:
                f = dict(e=0, p=0, k=[])
            else:
                f["e"] = e
        out.append(f)
    while out and out[-1]["e"] == 0:
        out.pop()
    return out or frames[:1]


def nucleus(frames):
    """the frame a sung syllable holds: the loudest of the first 60 % of its longest voiced run"""
    best, run, start = (0, 0), 0, 0
    for i, f in enumerate(frames + [dict(e=0, p=0)]):
        if f["e"] and f["p"]:
            if not run:
                start = i
            run += 1
        else:
            if run > best[1] - best[0]:
                best = (start, start + run)
            run = 0
    a, b = best
    if b <= a:
        return 0
    span = range(a, a + max(1, (b - a) * 6 // 10))
    return max(span, key=lambda i: frames[i]["e"])


# ------------------------------------------------------------- espeak-ng ---
class Espeak:
    def __init__(self):
        import ctypes
        self.ct = ctypes
        lib, data = None, None
        try:
            import espeakng_loader as L
            lib, data = L.get_library_path(), L.get_data_path()
        except ImportError:
            for n in ("libespeak-ng.so.1", "libespeak-ng.so", "libespeak-ng.dylib"):
                try:
                    ctypes.CDLL(n)
                    lib = n
                    break
                except OSError:
                    pass
        if not lib:
            raise RuntimeError("no espeak-ng (pip3 install espeakng-loader, or the libespeak-ng package)")
        self.lib = ctypes.CDLL(lib)
        self.buf = []
        CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.POINTER(ctypes.c_short), ctypes.c_int, ctypes.c_void_p)

        def cb(w, n, ev):
            if n > 0:
                self.buf.append(np.ctypeslib.as_array(w, shape=(n,)).copy())
            return 0
        self.cb = CB(cb)
        self.sr = self.lib.espeak_Initialize(2, 0, data.encode() if data else None, 0)
        if self.sr <= 0:
            raise RuntimeError("espeak-ng did not start")
        self.lib.espeak_SetSynthCallback(self.cb)
        self.lib.espeak_SetVoiceByName(b"en-us")
        self.lib.espeak_SetParameter(1, 135, 0)    # rate, words a minute: clear, a little slow
        self.lib.espeak_SetParameter(3, 46, 0)     # pitch
        self.lib.espeak_SetParameter(4, 30, 0)     # pitch range: a calm contour
        self.lib.espeak_Info.restype = ctypes.c_char_p
        self.version = self.lib.espeak_Info(None).decode()

    def say(self, text, lang="en", rate=135):
        if getattr(self, "lang", "en") != lang or getattr(self, "rate", 135) != rate:
            self.lib.espeak_SetVoiceByName(VOICES[lang])
            self.lib.espeak_SetParameter(1, rate, 0)
            self.rate = rate
            self.lib.espeak_SetParameter(3, 46, 0)
            self.lib.espeak_SetParameter(4, 30, 0)
            self.lang = lang
        self.buf.clear()
        b = text.encode()
        self.lib.espeak_Synth(b, len(b) + 1, 0, 1, 0, 0x100, None, None)   # espeakPHONEMES: [[ ]] allowed
        self.lib.espeak_Synchronize()
        x = np.concatenate(self.buf).astype(np.float64) / 32768.0 if self.buf else np.zeros(0)
        return x, self.sr


# ---------------------------------------------------------------- encoder ---
def resample(x, sr):
    from scipy.signal import resample_poly
    g = math.gcd(FS, sr)
    return resample_poly(x, FS // g, sr // g)


def levinson(r, p=10):
    a = np.zeros(p + 1)
    a[0] = 1.0
    e = r[0]
    k = np.zeros(p)
    for i in range(1, p + 1):
        acc = r[i] + sum(a[j] * r[i - j] for j in range(1, i))
        ki = -acc / e if e > 0 else 0.0
        ki = max(-0.995, min(0.995, ki))
        k[i - 1] = ki
        an = a.copy()
        for j in range(1, i):
            an[j] = a[j] + ki * a[i - j]
        an[i] = ki
        a = an
        e *= 1.0 - ki * ki
    return k, e


def nearest(table, v):
    return min(range(len(table)), key=lambda i: abs(table[i] - v))


def pitch_track(x):
    """per frame: (period in samples or 0, voicing strength)"""
    from scipy.signal import butter, lfilter
    b, a = butter(4, 900.0 / (FS / 2))
    y = lfilter(b, a, x)
    n = len(x) // FRAME
    out = []
    for f in range(n):
        c = f * FRAME + FRAME // 2
        s = y[max(0, c - 160):c + 160]
        if len(s) < 200:
            out.append((0, 0.0))
            continue
        s = s - s.mean()
        e0 = float(np.dot(s, s))
        best, bl = 0.0, 0
        for lag in range(16, 161):
            if lag >= len(s) - 40:
                break
            u, v = s[:-lag], s[lag:]
            d = math.sqrt(float(np.dot(u, u)) * float(np.dot(v, v)))
            r = float(np.dot(u, v)) / d if d > 0 else 0.0
            if r > best + 0.02 or (bl and lag > 1.8 * bl and r > best * 0.97 and False):
                best, bl = r, lag
        out.append((bl, best) if e0 > 0 else (0, 0.0))
    return out


def encode_word(x, sr):
    """samples (any rate) -> a list of frames: dict(e, p, k) indices; silence trimmed"""
    x8 = resample(x, sr)
    peak = np.max(np.abs(x8)) if len(x8) else 0.0
    if peak <= 0:
        return []
    x8 = x8 / peak * 0.8
    pre = np.append(x8[0], x8[1:] - PRE * x8[:-1])
    pad = np.concatenate([np.zeros(WIN), pre, np.zeros(WIN)])
    padx = np.concatenate([np.zeros(WIN), x8, np.zeros(WIN)])
    win = np.hamming(WIN)
    nfr = len(x8) // FRAME + 1
    pt = pitch_track(padx)
    frames = []
    for f in range(nfr):
        c = WIN + f * FRAME + FRAME // 2
        seg = pad[c - WIN // 2:c + WIN // 2] * win
        raw = padx[c - FRAME // 2:c + FRAME // 2]
        rms_raw = math.sqrt(float(np.mean(raw * raw))) if len(raw) else 0.0
        r = np.array([np.dot(seg[:WIN - i], seg[i:]) for i in range(11)])
        if r[0] <= 1e-9:
            frames.append(dict(rms=0.0, v=0, k=np.zeros(10), p=0, raw=rms_raw))
            continue
        r[1:] *= np.exp(-0.5 * (2 * math.pi * 60.0 / FS * np.arange(1, 11)) ** 2)   # 60 Hz lag window
        r[0] *= 1.0001                                                                  # white-noise floor
        k, e = levinson(r)
        res = math.sqrt(max(e, 0.0) / float(np.dot(win, win)))
        fi = f + WIN // FRAME                    # (the pitch track has the same padding in frames)
        lag, strength = pt[fi] if fi < len(pt) else (0, 0.0)
        frames.append(dict(rms=res, v=1 if strength > 0.45 and lag else 0, k=k, p=lag, raw=rms_raw))
    # voicing: a 3-frame median (no single-frame flips); the period: a 3-frame median of the voiced ones
    vv = [fr["v"] for fr in frames]
    for i in range(1, len(frames) - 1):
        frames[i]["v"] = sorted(vv[i - 1:i + 2])[1]
    pp = [fr["p"] if fr["v"] else 0 for fr in frames]
    for i in range(len(frames)):
        w = [p for p in pp[max(0, i - 1):i + 2] if p]
        if frames[i]["v"]:
            frames[i]["p"] = sorted(w)[len(w) // 2] if w else 64
    # the excitation's amplitude: unvoiced noise RMS = A; voiced chirp at P_REF gives RMS A * sqrt(CHIRP_E / P_REF) / 128
    amp = []
    for fr in frames:
        a = fr["rms"]
        if fr["v"]:
            a = a * 128.0 / math.sqrt(CHIRP_E / P_REF)
        amp.append(a)
    top = max(amp) if amp else 0.0
    if top <= 0:
        return []
    out = []
    raw_top = max(fr["raw"] for fr in frames)
    for fr, a in zip(frames, amp):
        e = a / top * ENERGY[14]
        if fr["raw"] < raw_top * 0.005 or e < 0.5:           # (-46 dB under the word's peak: silence)
            ei = 0
        else:
            ei = min(range(1, 15), key=lambda i: abs(math.log(ENERGY[i]) - math.log(e)))
        if ei == 0:
            out.append(dict(e=0, p=0, k=[]))
            continue
        nk = 10 if fr["v"] else 4
        kq = [nearest(K[i], fr["k"][i] * 512.0) for i in range(nk)]
        out.append(dict(e=ei, p=nearest(PERIOD[1:], fr["p"]) + 1 if fr["v"] else 0, k=kq))
    while out and out[0]["e"] == 0:
        out.pop(0)
    while out and out[-1]["e"] == 0:
        out.pop()
    return out


def pack(frames, tol=0):
    """frames -> (bytes, bit count): the TMS5220-style stream, a stop frame at the end; tol: a frame whose K1..K10
    are each within tol steps of the last coded frame's repeats it (the word lists' words: about a third fewer bits)"""
    bits = []

    def put(v, n):
        for i in range(n - 1, -1, -1):
            bits.append((v >> i) & 1)
    prev = None
    for fr in frames:
        put(fr["e"], 4)
        if fr["e"] == 0:
            prev = None
            continue
        rep = prev is not None and (fr["p"] != 0) == (prev["p"] != 0) and len(fr["k"]) == len(prev["k"]) and \
            all(abs(x - y) <= tol for x, y in zip(fr["k"], prev["k"]))
        put(1 if rep else 0, 1)
        put(fr["p"], 6)
        if not rep:
            for i in range(10 if fr["p"] else 4):
                put(fr["k"][i], KBITS[i])
            prev = fr
    put(15, 4)
    nb = len(bits)
    bits += [0] * (-len(bits) % 8)
    return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)), nb


def unpack(data, nbits):
    """the reference reader: bytes -> frames (as pack wrote them; repeats filled in)"""
    pos = [0]

    def get(n):
        v = 0
        for _ in range(n):
            v = v << 1 | (data[pos[0] >> 3] >> (7 - (pos[0] & 7)) & 1)
            pos[0] += 1
        return v
    frames, prev = [], None
    while pos[0] < nbits:
        e = get(4)
        if e == 15:
            break
        if e == 0:
            frames.append(dict(e=0, p=0, k=[]))
            prev = None
            continue
        rep, p = get(1), get(6)
        if rep:
            k = list(prev["k"])
        else:
            k = [get(KBITS[i]) for i in range(10 if p else 4)]
        fr = dict(e=e, p=p, k=k)
        frames.append(fr)
        prev = fr
    return frames


def decode(frames, fs=FS, chirp=False):
    """the reference decoder (float), as sp_dsp.c plays a word at its own pitch: 8 kHz, an impulse a period (chirp:
    the patent chirp instead), the lattice; K1..K10 interpolated over each frame unless the voicing changes, the
    energy a step a frame (an interpolated energy smears the onsets: fewer words recognised)"""
    out = []
    x = [0.0] * 10
    cur = None
    ph, rnd = 0, 1
    for fr in frames:
        tgt = dict(e=ENERGY[fr["e"]], p=PERIOD[fr["p"]] if fr["p"] else 0,
                   k=[K[i][fr["k"][i]] / 512.0 if i < len(fr["k"]) else 0.0 for i in range(10)])
        if cur is None or (cur["p"] == 0) != (tgt["p"] == 0) or cur["e"] == 0 or tgt["e"] == 0:
            cur = dict(e=tgt["e"], p=tgt["p"], k=list(tgt["k"]))
            start = dict(cur)
        else:
            start = cur
        for n in range(FRAME):
            t = (n + 1) / FRAME
            e = tgt["e"]
            p = tgt["p"]
            k = [start["k"][i] + (tgt["k"][i] - start["k"][i]) * t for i in range(10)]
            if p:
                ph += 1
                if ph >= p:
                    ph = 0
                if chirp:
                    exc = (CHIRP[ph] if ph < len(CHIRP) else 0) / 128.0 * e * math.sqrt(p / P_REF)
                else:                                   # (the same energy a period as the chirp)
                    exc = (e * math.sqrt(CHIRP_E) / 128.0 if ph == 0 else 0.0) * math.sqrt(p / P_REF)
            else:
                rnd = (rnd >> 1) ^ (0xB800 if rnd & 1 else 0)
                exc = e if rnd & 1 else -e
            u = exc
            for i in range(9, -1, -1):
                u = u - k[i] * x[i]
                if i < 9:
                    x[i + 1] = x[i] + k[i] * u
            x[0] = u
            out.append(u)
        cur = dict(e=tgt["e"], p=tgt["p"], k=tgt["k"])
    y = np.array(out)
    return y


# ------------------------------------------------------------------- main ---
def glottal(n=256, open_q=0.42, close_q=0.16):
    """one period of a smooth glottal flow derivative (Rosenberg's pulse, differentiated), RMS 0.25 (Q15: 8192):
    the LAB AI voice's excitation (sp_dsp.c SP_EX_GLOT), the same energy a period as the impulse"""
    tp, tn = open_q * n, close_q * n
    g = np.zeros(n)
    for i in range(n):
        if i < tp:
            g[i] = 0.5 * (1 - math.cos(math.pi * i / tp))
        elif i < tp + tn:
            g[i] = math.cos(0.5 * math.pi * (i - tp) / tn)
    d = np.diff(np.append(g, g[0]))
    d -= d.mean()
    d *= 0.25 / math.sqrt(float(np.mean(d * d)))
    if np.max(np.abs(d)) >= 1.0:
        raise SystemExit("gen_vocab: the glottal pulse overflows Q15")
    return [int(round(v * 32768)) for v in d]


def tables():
    """the chip's coding tables and the playback's helpers, as sp_dsp.c reads them"""
    L = ["/* the TMS5220's coding (MAME tms5110r.hxx, BSD-3-Clause: TI_028X_LATER_ENERGY, TI_5220_PITCH,",
         " * TI_5110_5220_LPC; the K tables x 64 = Q15) and the first Speak & Spell chips' chirp (TI_0280_PATENT_CHIRP) */",
         "static const uint8_t SP_ENERGY[16] = {" + ", ".join(map(str, ENERGY)) + "};",
         "static const uint8_t SP_PERIOD[64] = {" + ", ".join(map(str, PERIOD)) + "};",
         "static const int16_t SP_K1[32] = {" + ", ".join(str(v * 64) for v in K[0]) + "};",
         "static const int16_t SP_K2[32] = {" + ", ".join(str(v * 64) for v in K[1]) + "};",
         "static const int16_t SP_K3_7[5][16] = {"]
    for i in range(2, 7):
        L.append("    {" + ", ".join(str(v * 64) for v in K[i]) + "},")
    L.append("};")
    L.append("static const int16_t SP_K8_10[3][8] = {")
    for i in range(7, 10):
        L.append("    {" + ", ".join(str(v * 64) for v in K[i]) + "},")
    L.append("};")
    L.append(f"static const int8_t SP_CHIRP[{len(CHIRP)}] = {{" + ", ".join(map(str, CHIRP)) + "};")
    L.append("/* 768 log2(SP_PERIOD[i]) (1/64 semitones): a frame's pitch against its word's median (the contour) */")
    L.append("static const int16_t SP_PLOG[64] = {" +
             ", ".join(str(int(round(768 * math.log2(p)))) if p else "0" for p in PERIOD) + "};")
    g = glottal()
    L.append("/* one period of a glottal flow derivative, RMS 8192 (sp_dsp.c SP_EX_GLOT) */")
    L.append("static const int16_t SP_GLOT[256] = {")
    for i in range(0, 256, 16):
        L.append("    " + ", ".join(map(str, g[i:i + 16])) + ",")
    L.append("};")
    return L


def short_name(name):
    """a bank's name in at most 6 characters: DAD JOKES 1 -> DAD 1, PERCUSSION -> PERC, SPELLING -> SPELL"""
    m = re.fullmatch(r"DAD JOKES (\d+)", name)
    if m:
        return "DAD " + m[1]
    return {"PERCUSSION": "PERC", "SPELLING": "SPELL", "MANTRAS": "MANTRA", "NUMBERS": "NUMBER"}.get(name, name[:6])


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


DICT_EN = SRC / "assets" / "speak" / "words_en.txt"
DICT_DE = SRC / "assets" / "speak" / "words_de.txt"
# the word lists' words in the build (the most common first): as many as the app area holds (BUILDING.md);
# SP_NDICT="EN DE" overrides
N_DICT = dict(zip(("en", "de"), map(int, os.environ.get("SP_NDICT", "650 600").split())))
N_KEEP = {"en": 770, "de": 720}                 # (the cache keeps the largest board's list words: build.py SMK-37)


SYSTEM = ("SAVED", "CANCELLED", "DELETED")      # the editor says them (sp_user.c SP_WID_*)
MOPE = SRC / "assets" / "speak" / "mope.txt"     # MOPE's phrase grammar (sp_mope.c)
MO_SLOTS = ("NOTE", "CHORD", "IVL", "N", "ADJ", "ASIDE", "THING")   # its slots: MO_SLOT + their index
MO_PUNCT = {"": 0, ".": 1, "!": 1, ":": 1, ",": 2, "?": 3}


def parse_mope(say, path=MOPE):
    """MOPE's grammar -> [(class, [fragment: [Entry | slot index, punct] ...])]: [NAME] a class of fragments, a line
    each; [=NAME] a slot's values (NOTE: its first word a letter's name); $NOTE $CHORD $IVL $N a slot"""
    classes, cur = [], None
    for no, ln in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        m = re.fullmatch(r"\[(=?[A-Z][A-Z_0-9]*)\]", ln)
        if m:
            cur = (m[1], [])
            classes.append(cur)
            continue
        if cur is None:
            raise SystemExit(f"{path}:{no}: a line before the first [CLASS]")
        frag, toks = [], ln.split()
        for i, t in enumerate(toks):
            ms = re.fullmatch(r"\$([A-Z]+)([.,!?:]*)", t)
            if ms:
                if ms[1] not in MO_SLOTS:
                    raise SystemExit(f"{path}:{no}: slot ${ms[1]}: one of " + " ".join("$" + x for x in MO_SLOTS))
                frag.append((MO_SLOTS.index(ms[1]), MO_PUNCT[ms[2][-1:]]))
                continue
            mt = TOKEN.fullmatch(t)
            if not mt or not re.search(r"\w", mt[1]):
                raise SystemExit(f"{path}:{no}: {t!r}: a word (letters, ' and -), a {{pronunciation}} may follow")
            if len(fold(mt[1], "en")) > 11:
                raise SystemExit(f"{path}:{no}: {t!r}: longer than 11 letters (the screen)")
            k = word_key(mt[1])
            if cur[0] == "=NOTE" and i == 0:
                e = letter_entry(k, say)                   # (a note's letter: its name, A as in A FLAT)
            else:
                e = sentence_entries("en", [t], "", say)[0]
            frag.append((e, MO_PUNCT[mt[3][-1:]]))
        cur[1].append(frag)
    names = [c for c, _ in classes]
    for need, n in (("=NOTE", 12), ("=QUAL", 10), ("=IVL", 13), ("=NUM", 18), ("=THING", 10), ("=CTL", 36)):
        if need not in names or len(classes[names.index(need)][1]) != n:
            raise SystemExit(f"{path}: [{need}] needs {n} lines")
    return classes


def fnv(b):
    h = 2166136261
    for c in b:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=Path)
    ap.add_argument("--wav", type=Path, help="write each word as decoded (8 kHz) into DIR")
    ap.add_argument("--refresh", action="store_true", help="encode every word again (needs espeak-ng)")
    a = ap.parse_args()
    banks, say, kit = parse_banks()
    ents, order = {}, []

    def add(e):
        if e.key not in ents:
            ents[e.key] = e
            order.append(e.key)
        elif not e.lite:
            ents[e.key].lite = False                # (a bank's word: the full encoding)
        return e.key
    for L in LETTERS:                               # the vocabulary: letters, numbers, the kit, the banks, the lists
        add(letter_entry(L, say))
    for k in NUMBERS:
        add(Entry(k, k, k.lower()))
    for k in SYSTEM:                                # the editor's own words
        add(Entry(k, k, k.lower()))
    kit_keys = []
    for k, _ in kit:
        m = re.fullmatch(r"(.+?)\s+(\d+)", say.get(k, ""))
        kit_keys.append(add(Entry(k, k[1:], m[1] if m else k[1:].lower(), "en", int(m[2]) if m else 120, 1)))
    sents = []
    for name, bkind, vo, ss in banks:
        for lang, kind, shown, toks in ss:
            es_ = sentence_entries(lang, toks, kind, say)
            sents.append((shown, [add(e) for e in es_]))
    mope = [(c, [[(x if isinstance(x, int) else add(x), p) for x, p in f] for f in fr]) for c, fr in parse_mope(say)]
    n_fixed = len(order)                            # (the words before the lists': theirs come after)
    dict_keys = {}
    for lang, path in (("en", DICT_EN), ("de", DICT_DE)):
        dk = []
        for w in word_list(path, lang, N_DICT[lang]):
            name = w.translate(DE_ASCII).upper() if lang == "de" else w.upper()
            if not re.fullmatch(r"[A-Z]+", name):
                continue
            key = name if lang == "en" else "de:" + name
            if lang == "en" and len(name) == 1:
                key = {"A": "A", "I": "I"}.get(name, name + "#")
            if key in ents:
                ents[key].lite = ents[key].lite and True
                dk.append(key)
                continue
            dk.append(add(Entry(key, name, w if lang == "de" else w.lower(), lang, lite=True)))
        dict_keys[lang] = dk
    keys = order
    idx = {k: i for i, k in enumerate(keys)}
    cache = json.loads(CACHE.read_text()) if CACHE.exists() and not a.refresh else {}
    es = None
    words = {}
    for k in keys:
        e = ents[k]
        ck = e.cache_key()
        if ck not in cache:
            if es is None:
                try:
                    es = Espeak()
                except Exception as x:
                    raise SystemExit(f"gen_vocab: {k} is not in {CACHE.name} and espeak-ng is missing ({x})")
                print(f"gen_vocab: espeak-ng {es.version}")
            x, sr = es.say(e.text, e.lang, 160 if e.lite else 135)
            fr = encode_word(x, sr)
            if e.ms:
                fr = perc_shape(fr, e.ms)
            if not fr:
                raise SystemExit(f"gen_vocab: {k!r} ({e.text!r}) came out silent")
            data, nb = pack(fr, 1 if e.lite else 0)
            cache[ck] = dict(bits=nb, hex=data.hex(), espeak=es.version)
        words[k] = cache[ck]
    keep = {ents[k].cache_key() for k in keys}
    for lang, path in (("en", DICT_EN), ("de", DICT_DE)):   # (the other board's list words stay in the cache)
        for w in word_list(path, lang, max(N_KEEP[lang], N_DICT[lang])):
            name = w.translate(DE_ASCII).upper() if lang == "de" else w.upper()
            keep.add(Entry(name, name, w if lang == "de" else w.lower(), lang, lite=True).cache_key())
    cache = {ck: v for ck, v in sorted(cache.items()) if ck in keep}
    CACHE.write_text(json.dumps(cache, indent=0, sort_keys=True) + "\n")

    blob, offs, meta = bytearray(), [], []
    for k in keys:
        w = words[k]
        data = bytes.fromhex(w["hex"])
        frames = unpack(data, w["bits"])
        voiced = sorted(f["p"] for f in frames if f["p"])
        p0 = voiced[len(voiced) // 2] if voiced else nearest(PERIOD, 64)     # (a period index: the word's median)
        offs.append(len(blob))
        flags = ents[k].flags
        gain = 256
        if flags & 1:                              # a hit: as loud as the loud words (a burst is quiet as it is)
            pk = float(np.max(np.abs(decode(frames)))) if frames else 0.0
            gain = int(round(256 * min(12.0, max(1.0, 340.0 / pk)))) if pk > 0 else 256
        meta.append((len(frames), p0, flags, nucleus(frames), gain))
        blob += data
        if a.wav:
            a.wav.mkdir(parents=True, exist_ok=True)
            y = decode(frames)
            y = y / max(1e-9, np.max(np.abs(y))) * 0.9
            fn = re.sub(r"[^A-Za-z0-9_]", "_", k)
            with wave.open(str(a.wav / (fn + ".wav")), "wb") as f:
                f.setnchannels(1)
                f.setsampwidth(2)
                f.setframerate(FS)
                f.writeframes((y * 32767).astype("<i2").tobytes())
    names = [ents[k].name for k in keys]
    vocab_hash = fnv("\n".join(f"{ents[k].lang}:{ents[k].name}" for k in keys).encode())
    L = ["/* generated by tools/gen_vocab.py from assets/speak/banks.txt and the word lists: do not edit */",
         "/* the words: rendered with espeak-ng, LPC-encoded by gen_vocab.py (TMS5220-style frames) */",
         f"#define SP_NWORD {len(keys)}u",
         f"#define SP_NBANK {len(banks)}u                 /* the banks of this file (the USER bank comes after them) */",
         f"#define SP_NLETTER 26u               /* words 0..25: the letters' names A..Z; 26..35: ZERO..NINE */",
         f"#define SP_NKIT {len(kit)}u                  /* the kit's hits (KIT mode), SP_KIT */",
         "#define SP_W_PERC 1u                  /* a hit: tight, one shot, full velocity */",
         "#define SP_W_SING 2u                  /* a sung syllable: its vowel held while the key is (frame nuc) */",
         "#define SP_W_DE 4u                    /* a German word (the word list: DE) */",
         *[f"#define SP_WID_{k} {order.index(k)}u" for k in SYSTEM],
         f"#define SP_VOCAB_HASH 0x{vocab_hash:08X}u /* the vocabulary (names, languages): stored sentences check it */",
         f"#define SP_LPC_BYTES {len(blob)}u",
         "static const uint8_t SP_LPC[] = {"]
    for i in range(0, len(blob), 24):
        L.append("    " + ", ".join(f"0x{b:02x}" for b in blob[i:i + 24]) + ",")
    L.append("};")
    L += tables()
    L.append("/* a word: its first byte in SP_LPC, its frames, its median voiced period (an SP_PERIOD index), SP_W_*,")
    L.append(" * the frame a sung syllable holds, its level (Q8: a hit is brought up to the loud words) */")
    L.append("typedef struct { uint32_t off; uint16_t nfr; uint8_t p0, flags; uint16_t nuc, gain; } sp_word_t;")
    L.append("static const sp_word_t SP_WORD[SP_NWORD] = {")
    for k, o, (nf, p0, fl, nuc, gain) in zip(keys, offs, meta):
        fl |= 4 if ents[k].lang == "de" else 0
        L.append(f"    {{{o}u, {nf}u, {p0}u, {fl}u, {nuc}u, {gain}u}},   /* {k} */")
    L.append("};")
    L.append("static const char *const SP_WNAME[SP_NWORD] = {")
    for i in range(0, len(names), 8):
        L.append("    " + ", ".join(c_str(n) for n in names[i:i + 8]) + ",")
    L.append("};")
    L.append("/* the kit (KIT mode: key k or pad k plays hit k % 16), the note each sounds at */")
    L.append("static const uint16_t SP_KIT[SP_NKIT] = {" + ", ".join(str(idx[k]) for k in kit_keys) + "};")
    L.append("static const uint8_t SP_KIT_NOTE[SP_NKIT] = {" + ", ".join(str(n) for _, n in kit) + "};")
    L.append("static const char *const SP_BANK_NAME[SP_NBANK] = {" + ", ".join(c_str(b[0]) for b in banks) + "};")
    L.append("/* the names for the knob block's cell (at most 6 characters) */")
    L.append("static const char *const SP_BANK_SHORT[SP_NBANK] = {" + ", ".join(c_str(short_name(b[0])) for b in banks) +
             "};")
    L.append("/* a bank's own voice (sp_dsp.c SP_*; -1: the voice in use stays) */")
    L.append("static const int8_t SP_BANK_VOICE[SP_NBANK] = {" + ", ".join(str(b[2]) for b in banks) + "};")
    L.append("/* the sentences, 16 a bank: the text as shown (a word = between spaces), its words in SP_WORD */")
    L.append("static const char *const SP_SENT[SP_NBANK * 16u] = {")
    for shown, _ in sents:
        L.append(f"    {c_str(shown)},")
    L.append("};")
    flat, start = [], []
    for shown, sk in sents:
        start.append(len(flat))
        flat += [idx[k] for k in sk]
    start.append(len(flat))
    L.append(f"static const uint16_t SP_SENT_W[{len(flat)}] = {{")
    for i in range(0, len(flat), 20):
        L.append("    " + ", ".join(str(v) for v in flat[i:i + 20]) + ",")
    L.append("};")
    L.append(f"static const uint16_t SP_SENT_AT[SP_NBANK * 16u + 1u] = {{" + ", ".join(map(str, start)) + "};")
    if len(keys) >= 0xF00:
        raise SystemExit(f"gen_vocab: {len(keys)} words: MO_W keeps a word in 12 bits below its slots (0xF00)")
    mw, mat, mcls = [], [], [0]
    for c, fr in mope:
        for f in fr:
            mat.append(len(mw))
            mw += [(x | 0xF00 if isinstance(x, int) else idx[x]) | p << 12 for x, p in f]
        mcls.append(len(mat))
    mat.append(len(mw))
    L.append("/* MOPE's phrase grammar (assets/speak/mope.txt, sp_mope.c): its classes of fragments, each fragment's")
    L.append(" * words (a word's id, its punctuation << 12: 1 a full stop, 2 a comma, 3 a question; MO_SLOT + k: slot k) */")
    L.append(f"#define MO_NCLS {len(mope)}u")
    for i, (c, _) in enumerate(mope):
        L.append(f"#define MO_C_{c.lstrip('=')} {i}u")
    L.append("#define MO_SLOT 0xF00u                /* " + ", ".join(f"+{i} ${n}" for i, n in enumerate(MO_SLOTS)) + " */")
    L.append(f"static const uint16_t MO_W[{len(mw)}] = {{")
    for i in range(0, len(mw), 16):
        L.append("    " + ", ".join(f"0x{v:04x}" for v in mw[i:i + 16]) + ",")
    L.append("};")
    L.append(f"static const uint16_t MO_AT[{len(mat)}] = {{")
    for i in range(0, len(mat), 20):
        L.append("    " + ", ".join(map(str, mat[i:i + 20])) + ",")
    L.append("};")
    L.append("static const uint16_t MO_CLS[MO_NCLS + 1u] = {" + ", ".join(map(str, mcls)) + "};")
    for lang in ("en", "de"):
        ids = sorted({idx[k] for k in dict_keys[lang]}, key=lambda i: names[i])
        L.append(f"/* the word list {lang.upper()} for the editor ({len(ids)} words), sorted by name (a prefix finds them) */")
        L.append(f"#define SP_NDICT_{lang.upper()} {len(ids)}u")
        L.append(f"static const uint16_t SP_DICT_{lang.upper()}[{len(ids)}] = {{")
        for i in range(0, len(ids), 20):
            L.append("    " + ", ".join(str(v) for v in ids[i:i + 20]) + ",")
        L.append("};")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text("\n".join(L) + "\n")
    secs = sum(m[0] for m in meta) * FRAME / FS
    lite = sum(len(bytes.fromhex(words[k]["hex"])) for k in keys[n_fixed:])
    print(f"gen_vocab: {len(keys)} words ({len(dict_keys['en'])} EN + {len(dict_keys['de'])} DE in the word lists), "
          f"{len(sents)} sentences in {len(banks)} banks, MOPE {len(mat) - 1} fragments in {len(mope)} classes, "
          f"{len(blob)} bytes of LPC ({lite} of them the "
          f"{len(keys) - n_fixed} words only the lists have), {secs:.1f} s of speech")
    return 0


if __name__ == "__main__":
    sys.exit(main())
