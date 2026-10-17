// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 M. Geisler (genkigenki/makkuro)
// Speak37 in an AudioWorklet: sim.wasm is the firmware's sound, MOPE, UI and screen against a simulated SMK-37.
// Each render quantum runs the device's clock (44118 Hz; resampled, linear, when the browser runs at another rate);
// ~30 times a second the screen, the pads and the LEDs go out, and the store when the firmware saved it.
const GAIN = 2;   // (the device's line out has its ceiling at -6 dBFS: here at full scale)
class Speak37 extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ex = null;
    this.sent = -1;
    this.saves = 0;
    this.usaves = 0;
    this.last = 0;
    this.pos = 0;          // (the resampler: between source frames a and b)
    this.a = [0, 0];
    this.b = [0, 0];
    this.buf = null;
    this.bi = 0;
    this.port.onmessage = (e) => this.onMessage(e.data);
  }
  async onMessage(m) {
    if (m.type === "load") {
      try {
        const mod = await WebAssembly.compile(m.wasm);
        const imports = {};
        for (const i of WebAssembly.Module.imports(mod)) {       // (none expected; stubs if the toolchain adds any)
          imports[i.module] = imports[i.module] || {};
          if (i.kind === "function") imports[i.module][i.name] = () => 0;
        }
        const inst = await WebAssembly.instantiate(mod, imports);
        const ex = inst.exports;
        if (ex._initialize) ex._initialize();
        ex.sim_init((m.seed >>> 0) || 1);
        const put = (ptr, size, data) => {
          if (!data || data.byteLength !== size) return 0;
          new Uint8Array(ex.memory.buffer, ptr, size).set(new Uint8Array(data));
          return 1;
        };
        ex.sim_load(put(ex.sim_store(), ex.sim_store_size(), m.store), put(ex.sim_ustore(), ex.sim_ustore_size(), m.ustore));
        this.ex = ex;
        this.saves = ex.sim_saves();
        this.usaves = ex.sim_usaves();
        this.step = ex.sim_rate() / sampleRate;
        this.port.postMessage({ type: "ready", rate: ex.sim_rate() });
      } catch (err) {
        this.port.postMessage({ type: "error", message: String((err && err.message) || err) });
      }
      return;
    }
    const ex = this.ex;
    if (!ex) return;
    if (m.type === "pad") ex.sim_pad(m.pad, m.vel);
    else if (m.type === "key") ex.sim_key(m.key, m.down ? 1 : 0, m.vel || 100);
    else if (m.type === "button") ex.sim_button(m.b, m.down ? 1 : 0);
    else if (m.type === "enc") ex.sim_enc(m.e, m.steps | 0);
    else if (m.type === "fader") ex.sim_fader(m.i, m.v);
    else if (m.type === "wheel") ex.sim_wheel(m.i, m.raw);
    else if (m.type === "midi") ex.sim_midi(m.d[0] | 0, m.d[1] | 0, m.d[2] | 0);
  }
  publish() {
    const ex = this.ex, mem = ex.memory.buffer, msg = { type: "frame" }, tr = [];
    const d = ex.sim_draws();
    if (d !== this.sent) {
      this.sent = d;
      msg.fb = new Uint16Array(mem, ex.sim_fb(), 240 * 240).slice();
      tr.push(msg.fb.buffer);
    }
    msg.pads = Array.from(new Uint8Array(mem, ex.sim_pads(), 48));
    msg.leds = ex.sim_leds();
    msg.mode = ex.sim_mode();
    if (ex.sim_saves() !== this.saves) {                         // (the firmware saved: the page keeps it)
      this.saves = ex.sim_saves();
      msg.store = new Uint8Array(mem, ex.sim_vst(), ex.sim_store_size()).slice().buffer;
    }
    if (ex.sim_usaves() !== this.usaves) {
      this.usaves = ex.sim_usaves();
      msg.ustore = new Uint8Array(mem, ex.sim_ustore(), ex.sim_ustore_size()).slice().buffer;
    }
    this.port.postMessage(msg, tr);
  }
  next() {                                                     // (one source frame, for the resampler)
    const ex = this.ex;
    if (!this.buf || this.bi >= 64) {
      ex.sim_render(64);
      this.buf = [new Float32Array(ex.memory.buffer, ex.sim_out_l(), 64).slice(),
                  new Float32Array(ex.memory.buffer, ex.sim_out_r(), 64).slice()];
      this.bi = 0;
    }
    this.a = this.b;
    this.b = [this.buf[0][this.bi], this.buf[1][this.bi]];
    this.bi++;
  }
  process(inputs, outputs) {
    const ex = this.ex;
    if (!ex) return true;
    const out = outputs[0], n = out[0].length, L = out[0], R = out[1] || out[0];
    if (Math.abs(this.step - 1) < 1e-6) {
      ex.sim_render(n);
      const l = new Float32Array(ex.memory.buffer, ex.sim_out_l(), n), r = new Float32Array(ex.memory.buffer, ex.sim_out_r(), n);
      for (let i = 0; i < n; i++) {
        L[i] = Math.max(-1, Math.min(1, l[i] * GAIN));
        if (R !== L) R[i] = Math.max(-1, Math.min(1, r[i] * GAIN));
      }
    } else {
      for (let i = 0; i < n; i++) {
        this.pos += this.step;
        while (this.pos >= 1) { this.pos -= 1; this.next(); }
        const f = this.pos, a = this.a, b = this.b;
        L[i] = Math.max(-1, Math.min(1, (a[0] + (b[0] - a[0]) * f) * GAIN));
        if (R !== L) R[i] = Math.max(-1, Math.min(1, (a[1] + (b[1] - a[1]) * f) * GAIN));
      }
    }
    if (currentTime - this.last >= 1 / 30) {
      this.last = currentTime;
      this.publish();
    }
    return true;
  }
}
registerProcessor("speak37", Speak37);
