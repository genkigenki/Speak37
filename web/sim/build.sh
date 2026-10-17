#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 M. Geisler (genkigenki/makkuro)
# The Speak37 simulator: the firmware's sources to WebAssembly (Emscripten), plus the page.
#   web/sim/build.sh  ->  build/sim/{index.html, worklet.js, sim.wasm}
#   then: python3 -m http.server -d build/sim 8737, open http://localhost:8737
# The generated headers: the fonts, keycaps, palettes and tables of a firmware build (build/gen) when there is one,
# else made here (python3 with Pillow); the words (sp_vocab.h) always made fresh from assets/speak (the LPC cache,
# no espeak-ng needed). Needs emcc (Emscripten); wasm-opt (binaryen, it comes with Emscripten) if found.
set -e
cd "$(dirname "$0")/../.."
[ -d "$HOME/emsdk/upstream/emscripten" ] && PATH="$HOME/emsdk/upstream/emscripten:$HOME/emsdk/upstream/bin:$PATH"
command -v emcc >/dev/null 2>&1 || { echo "sim: no emcc (Emscripten: ~/emsdk)"; exit 1; }
PY="${PYTHON:-python3}"
G=build/sim/gen
mkdir -p "$G"
gen() {   # gen HEADER TOOL ARGS..: from build/gen if a firmware build made it, else the tool
    h=$1; shift
    if [ -f "build/gen/$h" ]; then cp "build/gen/$h" "$G/$h"
    elif [ ! -f "$G/$h" ]; then "$PY" "$@" >/dev/null; fi
}
gen ui_fonts.h tools/gen_aa_font.py "$G/ui_fonts.h" --preset inter-tight
gen ui_keycaps.h tools/gen_aa_keycaps.py "$G/ui_keycaps.h"
gen ui_palettes.h tools/gen_ui_palettes.py "$G/ui_palettes.h"
gen felucca_tables.h tools/gen_tables.py "$G/felucca_tables.h"
gen vo_font.h tools/gen_aa_font.py "$G/vo_font.h" --face "T=assets/fonts/InterTight[wght].ttf@700:20:32-90" --huff ""
SP_NDICT="${SP_NDICT:-684 640}" "$PY" tools/gen_vocab.py "$G/sp_vocab.h" >/dev/null   # (the SMK-37's word lists)
emcc -Os -std=gnu11 -w -I"$G" --no-entry -sSTANDALONE_WASM -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=16777216 \
    -sFILESYSTEM=0 -o build/sim/sim.wasm web/sim/sim.c
if command -v wasm-opt >/dev/null 2>&1; then
    wasm-opt -Os --enable-bulk-memory --enable-nontrapping-float-to-int --enable-sign-ext --enable-mutable-globals \
        build/sim/sim.wasm -o build/sim/sim.wasm
fi
cp web/sim/index.html web/sim/worklet.js build/sim/
echo "sim: build/sim ($(wc -c < build/sim/sim.wasm | tr -d ' ') B wasm)"
