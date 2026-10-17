// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 M. Geisler (genkigenki/makkuro)
// The simulator's wasm in Node, as the worklet runs it: the boot screen, then the sentences' screen, silent until a
// key; a key speaks (audio, the pad lit); a pad, a knob, a button do their job; PATCH: MOPE (its screen, it talks
// back to a chord, its poses, the light game on the pads, its sulk and its piece); the cost against real time and
// the longest render call. Screens as PPM into OUTDIR when given.
//   node web/sim/sim_test.mjs build/sim/sim.wasm [OUTDIR]
import fs from "fs";
const bytes = fs.readFileSync(process.argv[2] || "build/sim/sim.wasm");
const out = process.argv[3];
const { instance } = await WebAssembly.instantiate(bytes, {});
const ex = instance.exports;
if (ex._initialize) ex._initialize();
ex.sim_init(1234);
ex.sim_load(0, 0);
const FS = ex.sim_rate();
let fails = 0, peak = 0, worst = 0, busy = 0, total = 0;
const check = (what, ok) => { console.log(`sim: ${what.padEnd(72)} ${ok ? "ok" : "FAIL"}`); if (!ok) fails++; };
function run(sec) {
  const n = Math.round(sec * FS / 128);
  for (let i = 0; i < n; i++) {
    const t0 = performance.now();
    ex.sim_render(128);
    const dt = performance.now() - t0;
    worst = Math.max(worst, dt); busy += dt; total += 128 / FS * 1000;
    const l = new Float32Array(ex.memory.buffer, ex.sim_out_l(), 128), r = new Float32Array(ex.memory.buffer, ex.sim_out_r(), 128);
    for (let j = 0; j < 128; j++) peak = Math.max(peak, Math.abs(l[j]), Math.abs(r[j]));
  }
}
const fb = () => new Uint16Array(ex.memory.buffer, ex.sim_fb(), 240 * 240);
const ink = () => { const f = fb(); let n = 0; for (let i = 0; i < f.length; i++) if (f[i] !== f[0]) n++; return n; };
const pads = () => new Uint8Array(ex.memory.buffer, ex.sim_pads(), 48);
function save(name) {
  if (!out) return;
  const f = fb(), b = Buffer.alloc(240 * 240 * 3);
  for (let i = 0; i < 240 * 240; i++) {
    const c = f[i];
    b[3 * i] = ((c >> 11) & 31) * 255 / 31; b[3 * i + 1] = ((c >> 5) & 63) * 255 / 63; b[3 * i + 2] = (c & 31) * 255 / 31;
  }
  fs.mkdirSync(out, { recursive: true });
  fs.writeFileSync(`${out}/${name}.ppm`, Buffer.concat([Buffer.from("P6\n240 240\n255\n"), b]));
}
const tap = (b) => { ex.sim_button(b, 1); run(0.06); ex.sim_button(b, 0); run(0.06); };
const pad = (p) => { ex.sim_pad(p, 100); run(0.06); ex.sim_pad(p, 0); run(0.06); };

run(0.9);
check(`the boot screen (${ink()} px of ink)`, !ex.sim_booted() && ink() > 2000);
save("boot");
run(1.0);
check(`booted: the sentences' screen (${ink()} px of ink)`, ex.sim_booted() && ink() > 5000);
check(`silent until a key (peak ${peak.toFixed(4)})`, peak < 0.001);
save("start");
ex.sim_key(12, 1, 100);
run(0.6);
check(`a key speaks (peak ${peak.toFixed(3)})`, peak > 0.02 && peak <= 1.0 && ex.sim_speaking());
check("... its pad lit (the sentence in use, at full)", Math.max(...pads().slice(0, 3)) > 100);
save("speaking");
ex.sim_key(12, 0);
run(1.5);
peak = 0;
pad(5);
ex.sim_key(14, 1, 90); run(0.4); ex.sim_key(14, 0);
check(`pad 6: its sentence, a key speaks it (peak ${peak.toFixed(3)})`, peak > 0.05);
const before = Array.from(fb());
ex.sim_enc(2, 1); run(0.3);
check("K3: the next voice (the screen shows it)", fb().some((c, i) => c !== before[i]));
tap(8);
check("SCALE: LETTERS (its LED lit)", (ex.sim_leds() >> 8) & 1);
tap(8); tap(8);
run(1.0);

// MOPE
tap(12);
run(0.3);
check("PATCH: MOPE (mode 3, the LED lit)", ex.sim_mode() === 3 && ((ex.sim_leds() >> 12) & 1));
run(2.5);
save("mope");
check(`MOPE's screen (${ink()} px of ink)`, ink() > 3000);
const lines0 = ex.sim_mope(5), pose0 = ex.sim_mope(2);
peak = 0;
for (const k of [12, 16, 19]) ex.sim_key(k, 1, 100);
run(0.8);
for (const k of [12, 16, 19]) ex.sim_key(k, 0);
let t = 0;
while (t < 12 && ex.sim_mope(5) === lines0) { run(0.1); t += 0.1; }
check(`a C major chord: MOPE comments (line ${ex.sim_mope(5)}, after ${t.toFixed(1)} s)`, ex.sim_mope(5) !== lines0);
t = 0;
let talked = false;
while (t < 6) { run(0.1); t += 0.1; if (ex.sim_mope(3)) { talked = true; save("mope_talking"); break; } }
check(`... it talks (peak ${peak.toFixed(3)})`, talked && peak > 0.05);
check(`... a new pose (${pose0} -> ${ex.sim_mope(2)})`, ex.sim_mope(2) !== pose0);
while (t < 20 && ex.sim_mope(3)) { run(0.1); t += 0.1; }
run(1.0);
ex.sim_mope_poke(0);
t = 0;
while (t < 10 && !ex.sim_mope(7)) { run(0.05); t += 0.05; }
check(`the light game: running (after ${t.toFixed(1)} s)`, ex.sim_mope(7));
run(0.4);
const g = ex.sim_mope(6), lit = pads().slice(3 * g, 3 * g + 3);
check(`... the moving light on pad ${g + 1} (${Array.from(lit)})`, Math.max(...lit) > 60);
save("mope_game");
pad(g);
run(3);
for (let i = 0; i < 8 && ex.sim_mope(1) === 4; i++) { pad(ex.sim_mope(6)); run(2.5); }
t = 0;
while (t < 25 && ex.sim_mope(1) === 4) { run(0.2); t += 0.2; }
check("... over", ex.sim_mope(1) !== 4);
while (ex.sim_mope(3)) run(0.2);
run(1);
ex.sim_mope_poke(1);
t = 0;
while (t < 40 && !ex.sim_mope(8)) { run(0.2); t += 0.2; }
check(`bored: it sulks and plays its piece (after ${t.toFixed(1)} s)`, ex.sim_mope(8));
peak = 0;
run(3);
save("mope_sulk");
check(`... audible (peak ${peak.toFixed(3)})`, peak > 0.02);
tap(12);
run(0.5);
check("PATCH again: back to the sentences", ex.sim_mode() !== 3);
console.log(`sim: cost ${(busy / total * 100).toFixed(1)} % of real time, the longest call ${worst.toFixed(2)} ms ` +
  `(a call is ${(128 / FS * 1000).toFixed(2)} ms of sound)`);
check("cost under 40 % of real time", busy / total < 0.4);
console.log(fails ? `sim: ${fails} FAILED` : "sim: all ok");
process.exit(fails ? 1 : 0);
