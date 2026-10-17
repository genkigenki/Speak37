#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 M. Geisler (genkigenki/makkuro)
"""The editor's word lists (assets/speak/words_en.txt, words_de.txt) from a frequency list: the most common words,
letters only, family-friendly (gen_vocab.py BLOCK), each once.

  tools/pick_words.py en_50k.txt en assets/speak/words_en.txt [N]

The source: FrequencyWords by Hermit Dave (https://github.com/hermitdave/FrequencyWords, content/2018/<lang>/
<lang>_50k.txt, from OpenSubtitles 2018), content licensed CC BY-SA 4.0 (LICENSES/CC-BY-SA-4.0-FrequencyWords.txt):
the lists made here are an adaptation under the same licence."""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_vocab import BLOCK  # noqa: E402

KEEP_OUT = {"cake"}                                 # (kept out on purpose: no references to other works)


def main():
    src, lang, out = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    n = int(sys.argv[4]) if len(sys.argv) > 4 else 1500
    ok = re.compile(r"[a-z]+" if lang == "en" else r"[a-zäöüß]+")
    words, seen = [], set()
    for ln in open(src, encoding="utf-8"):
        w = ln.split()[0].lower() if ln.split() else ""
        if not ok.fullmatch(w) or w in BLOCK or w in KEEP_OUT or w in seen:
            continue
        if len(w) == 1 and not (lang == "en" and w in "ai"):
            continue
        seen.add(w)
        words.append(w)
        if len(words) >= n:
            break
    out.write_text(f"# the {len(words)} most common {lang.upper()} words (most common first), letters only, family-friendly\n"
                   "# from FrequencyWords by Hermit Dave (OpenSubtitles 2018), CC BY-SA 4.0: an adaptation, CC BY-SA 4.0\n"
                   + "\n".join(words) + "\n", encoding="utf-8")
    print(f"{out}: {len(words)} words")


if __name__ == "__main__":
    main()
