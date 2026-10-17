#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 M. Geisler (genkigenki/makkuro)
"""Speak37's CPU budget on the target, from the pi32v2 listing of a Speak37 build (build.py BOARD=smk37 or fm1speak
writes build/felucca.dis): the loops of the audio ISR (the sound is inlined into fm1_alnk0_irq), each a range closed
by a backward branch, with their instruction counts. The per-sample loops are told apart by what they do:
  voice    the per-output-sample loop of a voice (the resampler, its gains): its body without the lattice step
  lattice  the ten-stage lattice (its loop body, or unrolled: the code between the resampler's test and its end)
  bus      the bus's per-sample loop (tone, delay, reverb, DC blocker; the reverb's comb / all-pass loops inside)
  drums    RHYTHM's 808 (s8_run, sp_808.c, out of line): its per-sample loop, a ringing lane each
  tones    MOPE's tone voices (mo_tone_run, sp_mope_snd.c, out of line): its per-sample loop, a sounding tone each
The estimate: cycles a sample = bus + voices x (voice + lattice x R / fs), at 1.5 cycles an instruction (pi32v2: a
mostly single-cycle pipeline; loads, 64-bit multiplies and taken branches cost more), against the clock (240 MHz,
the lowest the units run at; measured 320-360). A static count: it changes only when the compiled code changes.
  tests/sp_budget.py [DIS]"""
import re
import sys

dis = sys.argv[1] if len(sys.argv) > 1 else "build/felucca.dis"
FUNCS = ["fm1_alnk0_irq", "s8_run", "mo_tone_run"]  # the audio ISR (the sound inlined); the 808's lane block; a tone
LABEL = re.compile(r"^([A-Za-z_][A-Za-z_0-9.]*):$")
INSN = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$")
TARGET = re.compile(r"goto -?\d+ <[^>]*: ([0-9a-f]+) >")

code, cur = {f: [] for f in FUNCS}, None
for line in open(dis):
    m = LABEL.match(line.strip())
    if m and not m.group(1).startswith("."):
        cur = m.group(1)
        continue
    if cur not in code:
        continue
    m = INSN.match(line)
    if m:
        code[cur].append((int(m.group(1), 16), m.group(3)))
if not code[FUNCS[0]]:
    raise SystemExit(f"{dis}: no {FUNCS[0]}: not a Speak37 build?")
for func in FUNCS:
    insns = code[func]
    if not insns:
        print(f"{func}: not in this build")
        continue
    addrs = [a for a, _ in insns]
    loops = []
    for a, t in insns:
        m = TARGET.search(t)
        if m:
            tgt = int(m.group(1), 16)
            if tgt <= a and tgt >= addrs[0]:
                n = sum(1 for x in addrs if tgt <= x <= a)
                mul = sum(1 for x in addrs[addrs.index(tgt):addrs.index(a) + 1]
                          if "*" in dict(insns)[x] or "mul" in dict(insns)[x])
                loops.append((tgt, a, n, mul))
    loops = sorted(set(loops))
    print(f"{func}: {len(insns)} instructions, {len(loops)} loops")
    for lo, hi, n, mul in loops:
        inner = [l for l in loops if l != (lo, hi, n, mul) and l[0] >= lo and l[1] <= hi]
        own = n - sum(l[2] for l in inner if not any(o != l and o[0] <= l[0] and o[1] >= l[1] and o[0] >= lo and o[1] <= hi
                                                      and o != (lo, hi, n, mul) for o in inner))
        print(f"  loop 0x{lo:x}..0x{hi:x}: {n:4d} insns ({own:4d} outside its inner loops, {mul:3d} multiplies), "
              f"{len(inner)} inner")
