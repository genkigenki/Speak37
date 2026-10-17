# Speak37 licensing

Speak37 is Felucca's platform with a speaking instrument on it. Its own parts are by M. Geisler
(genkigenki/makkuro), GPL-3.0-only like the rest:

| What | Licence | Where |
| --- | --- | --- |
| Speak37: the speech engine (the LPC decoder, the voices, the effects), the instrument, the screen, the panels | GPL-3.0-only | `firmware/src/sp_*.c`, `firmware/src/speak.c` |
| The LPC encoder and the word data it makes (rendered with espeak-ng, encoded by it) | GPL-3.0-only | `tools/gen_vocab.py`, `assets/speak/vocab_lpc.json` |
| The sentences (original text; the songs traditional or in the public domain, their authors dead more than 70 years) | GPL-3.0-only | `assets/speak/banks.txt` |
| MOPE: its listener, its comments and their grammar, its jokes (original text), its sulk's pieces, its robot, its activities | GPL-3.0-only | `firmware/src/sp_mope*.c`, `assets/speak/mope.txt` |
| RHYTHM: the 16 grooves and the transport | GPL-3.0-only | `firmware/src/sp_rhythm.c`, `firmware/src/sp_dsp.c` |
| The 808's constants generator and its host stubs | GPL-3.0-only | `tools/gen_808.c`, `tests/ref808/` |
| The sentence editor (USER) | GPL-3.0-only | `firmware/src/sp_user.c` |
| The word lists' filter | GPL-3.0-only | `tools/pick_words.py` |
| The tests | GPL-3.0-only | `tests/sp_*.c`, `tests/sp_budget.py` |

Third-party material in Speak37:

| What | Licence | Where |
| --- | --- | --- |
| The TMS5220's coding tables (energy, pitch, K1..K10) and the first Speak & Spell chips' chirp, as tabulated by MAME (`src/devices/sound/tms5110r.hxx`, Frank Palazzolo, Couriersud, Jonathan Gevaryahu): numeric constants of the chip | BSD-3-Clause | `tools/gen_vocab.py` (the generated `build/gen/sp_vocab.h`), `LICENSES/BSD-3-Clause-MAME.txt` |
| Talkie (Peter Knight; Armin Joachimsmeyer's <https://github.com/ArminJo/Talkie>): the reference for the decoder's structure (the lattice, the frame format); no code copied | GPL-3.0 | credit only |
| espeak-ng (<https://github.com/espeak-ng/espeak-ng>): the offline source of the word recordings: it speaks every word at build time (through its shared library), and only its output, analysed and LPC-encoded by `tools/gen_vocab.py`, is in this tree (`assets/speak/vocab_lpc.json`) and in the firmware; no espeak-ng code or data | GPL-3.0-or-later | build tool only |
| The editor's word lists: the most frequent English and German words of FrequencyWords by Hermit Dave (<https://github.com/hermitdave/FrequencyWords>, from OpenSubtitles 2018), filtered (letters only, family-friendly) and cut to the first 1500; the builds take the first 572 / 528 (FM-1) or 684 / 640 (SMK-37) | CC BY-SA 4.0 (the lists; FrequencyWords' code is MIT, none of it used) | `assets/speak/words_en.txt`, `assets/speak/words_de.txt`, `LICENSES/CC-BY-SA-4.0-FrequencyWords.txt` |
| RHYTHM's 808: the drum voices of FM1 Open by M-VAVE (<https://github.com/m-vave/fm1_open>, `third_party/synth/drum808.c`, commit `5d3eac2`: their DaisySP / Plaits adaptation), ported to fixed point; its constants written by `tools/gen_808.c` from the original, which `tests/sp_808_test.c` also runs as the reference. Copyright (c) 2020 Electrosmith, Corp, Emilie Gillet; 2026 M-VAVE. No other part of FM1 Open (its SDK, libraries, engine) is used | MIT | `firmware/src/sp_808.c`, `firmware/src/sp_808_tab.h`, `third_party/fm1_open/` (the originals, unchanged, not built into the firmware), `LICENSES/MIT-fm1_open-drum808.txt` |
| Felucca's drum voices (`firmware/src/drum_voice.c`, Leo Kuroshita / Hügelton Instruments): RHYTHM's earlier kit (`SP_808=0`), used unchanged (with Felucca's `dsp.c` helpers and generated tables; the 808 uses `dsp.c`'s sine) | GPL-3.0-only | `firmware/src/drum_voice.c`, `firmware/src/dsp.c` |

No TI ROM vocabulary (the speech data of TI's chips) is used anywhere: every word is rendered by espeak-ng and
encoded by `tools/gen_vocab.py`.

# Felucca licensing

Felucca is free software, licensed under the GNU General Public License, version 3 only
(`GPL-3.0-only`, full text in `LICENSE`). That covers the code and its own assets. A few
bundled or ported parts keep their own licences; they are listed below, and their licence
texts are in `LICENSES/`.

Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments

## What is GPL-3.0-only

Every file in this tree that carries an `SPDX-License-Identifier: GPL-3.0-only` header:

- the firmware: `firmware/` (app, HAL, update loader)
- the build script and tools: `build.sh`, `tools/`
- the web pages (installer, editor, the browser emulator `web/emu/`) and their tests: `web/` (not the Fukiai and DotGothic16 fonts, below)
- the host tests: `tests/`

Three source files are ports and keep the licence of their originals:
`firmware/src/phys_dsp.c` (DaisySP, MIT), `firmware/src/phys_symp.c` (Rings, MIT) and
`firmware/src/fm6_core.c` (msfa, Apache-2.0). The firmware built with them is GPL-3.0-only as
a whole.

You may use, study, change and share Felucca under the GPL. If you distribute Felucca, or
firmware derived from it, you must also give your recipients its complete corresponding
source under the same licence. That includes devices that ship with modified Felucca inside.

## Hügelton Instruments' own work

All by Hügelton Instruments (Leo Kuroshita), in this tree:

| What | Licence | Where |
| --- | --- | --- |
| Felucca: firmware, tools, web pages, tests | GPL-3.0-only | `LICENSE` |
| The PHASE engine's waveforms: a C port of the oscillator of CrispyZebra (<https://github.com/hugelton/CrispyZebra>) | GPL-3.0 | `firmware/src/eng_phase.c` |
| The DRUM voices and kit | GPL-3.0-only | `firmware/src/drum_voice.c`, `firmware/src/eng_drum.c` |
| The Hügelton Sample Pack: Felucca's drum sounds, made by `tools/gen_waves.py`, from which the SLICE engine's BREAK is built (not CC0) | GPL-3.0-only | `tools/gen_waves.py` |
| The controls picture | GPL-3.0-only | `docs/controls.jpg` |
| The Fukiai icon font (<https://github.com/hugelton/Fukiai>): the firmware's icons (rasterised at build time by `tools/gen_aa_icons.py`) and the web editor's | MIT | `web/fukiai.ttf`, `LICENSES/MIT-Fukiai.txt`, `web/FUKIAI-LICENSE.txt` |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Inter Tight font by The Inter Project Authors: the UI text, rasterised into the firmware at build time (`tools/gen_aa_font.py`; the generated tables are not offered as a font, and the font declares no Reserved Font Name) | SIL OFL 1.1 | `assets/fonts/InterTight[wght].ttf`, `LICENSES/OFL-InterTight.txt` (also `assets/fonts/OFL.txt`) |
| Instrument samples (Versilian Studios VSCO-2 Community Edition, VCSL): the SAMPLE sets, also the SLICE engine's PIANO (the PIANO set's middle C) | CC0 1.0 | `assets/samples-cc0/`, provenance in `ATTRIBUTION.txt` there |
| DaisySP by Electrosmith, Corp and Emilie Gillet (<https://github.com/electro-smith/DaisySP>): the PHYS engine's modal and string models and the resonator, ported to fixed point | MIT | `firmware/src/phys_dsp.c`, `LICENSES/MIT-DaisySP.txt` |
| Rings by Emilie Gillet (<https://github.com/pichenettes/eurorack>): the PHYS engine's sympathetic strings, ported to fixed point | MIT | `firmware/src/phys_symp.c`, `LICENSES/MIT-Rings.txt` |
| msfa by Google Inc. and Pascal Gauthier, from Dexed (<https://github.com/asb2m10/dexed>): the FM6 engine's synthesis, ported to integer C (Dexed itself is GPL-3.0; only msfa is used; the FM6 factory patches are Felucca's own) | Apache-2.0 | `firmware/src/fm6_core.c`, `LICENSES/Apache-2.0-msfa.txt` |
| klattsch by Tony Gies (<https://github.com/tgies/klattsch>): design reference for the VOICE engine; no code copied. Formant data from Klatt (1980) / Hillenbrand et al. (1995) | MIT (klattsch) | credit only |
| X0X by charlesvestal (<https://github.com/charlesvestal/fm1-x0x>), a Felucca fork: the design of the browser emulator (the worklet, the device clock driven by its audio, the exports); its files credit it in their headers | GPL-3.0 | `web/emu/` |
| DotGothic16 font by The DotGothic16 Project Authors (<https://github.com/fontworks-fonts/DotGothic16>): the browser emulator's knob labels, served with its page (cut to printable ASCII; the font declares no Reserved Font Name) | SIL OFL 1.1 | `web/emu/fonts/DotGothic16-subset.woff`, `LICENSES/OFL-DotGothic16.txt` (also `web/emu/fonts/OFL.txt`) |
| JieLi AC79 SDK by JieLi Technology: three of its files go into every `.fwsc` package (below); none are in this tree | Apache-2.0 | `LICENSES/Apache-2.0.txt` |

On the device, HOME held > ABOUT opens the information screen; turning PRESETS scrolls on into
CREDITS, a short list of these authors, licences and source URLs.

## JieLi SDK files in the packages (Apache-2.0)

A `.fwsc` package made by `tools/build.py` (with `tools/fm1pkg_make.py`; this is the package the
web installer installs) holds three unmodified files from the JieLi AC79 SDK
(<https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK>, `cpu/wl82/tools/`). They are read from your SDK
checkout at build time (see BUILDING.md); no SDK files are in this tree.

| File in the package | What it is |
| --- | --- |
| `uboot.boot` | the first-stage boot loader (SPL) |
| `cfg_tool.bin` | the chip configuration block |
| `eq_cfg_hw.bin` | the default hardware EQ table |

They are licensed under the Apache License, Version 2.0 (`LICENSES/Apache-2.0.txt`), not under
the GPL, and their copyright stays with JieLi Technology. The SDK has no NOTICE file. Apache-2.0
files may be distributed together with GPL-3.0 code; Felucca itself stays GPL-3.0-only.

Every distribution of a package carries the licence texts with it: the release folder
(`tools/build.py --release`) and the site (`web/make_site.py`, next to the package in
`firmware/` and linked from the installer page).

## No vendor material

No M-VAVE material is part of Felucca. (Speak37 adds one exception, published by M-VAVE under the MIT licence:
FM1 Open's `drum808.c`, RHYTHM's 808, listed above.) The firmware links no vendor code and contains no vendor
data, and the packages hold only Felucca and the three SDK files above. The installer's
"Return to official V15" holds only the official file's size and SHA-256: you select the
official firmware file you downloaded yourself, and it is checked and installed in your browser,
never uploaded or redistributed.

## Speak & Spell

"Speak & Spell" is a trademark of its owner; Speak37 names it only to describe the style it imitates. Speak37 is not
affiliated with or endorsed by Texas Instruments.

## Contributions

Contributions are welcome under GPL-3.0-only.

## Trademarks

"Felucca" and "Hügelton Instruments" are names of Hügelton Instruments.

"M-VAVE" and "FM-1" are trademarks of their respective owners. Felucca is independent
firmware that runs on FM-1 hardware. It is not affiliated with, endorsed by or supported
by those owners.

## Radio

Felucca never enables the Bluetooth / Wi-Fi radio of the hardware.
