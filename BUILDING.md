# Building Felucca

The build makes three files in `build/`:

| File | What |
| --- | --- |
| `felucca.bin` | the firmware app |
| `loader/ota.bin` | the update loader |
| `felucca.fwsc` | the installable package (app + loader) |

## Prerequisites (macOS)

- Python 3 with Pillow and fontTools: `pip3 install Pillow fonttools` (the UI font and icons are
  rasterised at build time)
- Docker Desktop. The JieLi toolchain is Linux x86-64 only; the build runs each tool in a
  `linux/amd64` `debian:bookworm-slim` container (Rosetta on Apple silicon). Keep the source
  tree in a folder Docker can share, e.g. under `/Users`.
- The JieLi Linux toolchain (clang 4.0.1 for pi32v2, from JieLi's package server):

  ```
  tools/get_toolchain.sh            # installs to ~/.jieli/toolchain
  ```

- The JieLi AC79 SDK (Apache-2.0). The package uses three of its files
  (`cpu/wl82/tools/uboot.boot`, `cfg_tool.bin`, `cfg/eq_cfg_hw.bin`); they are not part of this tree.

  ```
  git clone --depth 1 --branch AC79NN_SDK_V1.2.1_2023-12-13 \
      https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK.git ~/fw-AC79_AIoT_SDK
  ```

- Node.js (optional, for the web tests).
- Emscripten (optional, `emcc`: the browser emulator, `web/emu/`).

On Linux x86-64 the toolchain runs natively and Docker is not needed.

## Build

```
./build.sh
```

`JIELI_TOOLCHAIN` and `AC79_SDK` override the default locations
(`~/.jieli/toolchain`, `~/fw-AC79_AIoT_SDK`).

`./build.sh --release 1.0` makes a release build: the package identity becomes `FM-1_910`
and the version string `v1.0`; the package is `build/felucca-1.0.fwsc`, and
`build/release-1.0/` holds what a release ships: the package, the app
(`felucca-1.0-app.bin`), `SHA256SUMS`, the sample attribution, `LICENSE`, `LICENSING.md` and
`LICENSES/` (the package contains Apache-2.0 SDK files, so the licence texts travel with it).

Build options (environment, `0` or `1`; defaults in `firmware/src/felucca.c`, `core.h` and `icons.c`):

| Flag | Default | |
| --- | --- | --- |
| `FELUCCA_FLASH` | 1 | settings, presets and projects in flash |
| `FELUCCA_OTA` | 1 | update entry (needs `FELUCCA_FLASH`) |
| `FELUCCA_CDC` | 1 | USB serial console |
| `FELUCCA_CDC_DEFAULT` | 1 | `0`: the console is built in but left out of USB from boot (as MENU > USB SERIAL OFF) |
| `FELUCCA_UAC` | 1 | USB audio input (the master output, stereo, 44.1 or 48 kHz) |
| `FELUCCA_UAC_48K` | 1 | `0`: the USB audio input at 44.1 kHz only, as up to 1.0.5.2 (`firmware/src/usb.c`) |
| `FELUCCA_UAC_AS_FIRST` | 1 | the audio control header lists the audio streaming interface before the MIDI one, so the USB audio driver of older macOS (up to 15) finds it; `0`: 1.0 to 1.5's order, byte for byte (`firmware/src/usb.c`) |
| `FELUCCA_UART` | 1 (SMK-37 Pro: 0) | TRS MIDI IN; must stay 0 on the SMK-37 Pro (PH8 is a key row there) |
| `FELUCCA_USBHOST` | 0 (SMK-37 Pro: 1) | USB host mode, SMK-37 Pro only (`firmware/src/usbhost.c`): enumerates a USB device and shows it |
| `FELUCCA_SLICE` | 1 | the SLICE engine |
| `FELUCCA_ICONS` | 1 | parameter icons on the knob cards |
| `FELUCCA_FM4` | 0 | the retired DIGITAL engine (4-operator FM) instead of its FM6 conversion |

`FELUCCA_USB_LAYOUT` (`0` to `3`, default `0`) picks other USB descriptor layouts for testing; see
`firmware/src/usb.c`.

**Speak37.** `BOARD=smk37 ./build.sh` (or `./build.sh --board smk37`) builds Speak37 for the M-Vave SMK-37 Pro: the
compilation unit `firmware/src/speak.c` (Felucca's platform files and `sp_*.c`, not Felucca's synthesizer; store
owner tag SPEAK37), with `-DBOARD_SMK37=1` for the app and the update loader, identity `SMK-37 Pro_500` (release
`SMK-37 Pro_5XY`), the SMK flash layout (PRCT 0x9D000, the stock region table) and the package `build/speak37.fwsc`
(MODEL=elite / mke: `speak37-elite.fwsc`, `speak37-mke.fwsc`, under that model's name). `BOARD=fm1speak ./build.sh`
builds it for the FM-1: `build/speak37-fm1.fwsc`, identity `FM-1_500`, the FM-1's layout. The Speak37 update
loader is a USB-MIDI device "Speak37 Update" answering as `ota-<model>_500`.

Every build makes `build/gen/sp_vocab.h` (tools/gen_vocab.py): the sentences of `assets/speak/banks.txt` and every
word LPC-encoded. A word not in the cache `assets/speak/vocab_lpc.json` is spoken by espeak-ng (`pip3 install
espeakng-loader numpy scipy`, or a system libespeak-ng) and encoded; `tools/gen_vocab.py OUT.h --refresh` encodes
all of them again, `--wav DIR` writes each as the reference decoder plays it.

The editor's word lists (`assets/speak/words_en.txt`, `words_de.txt`, made by `tools/pick_words.py`) go in as far
as the app area holds them with a margin: build.py sets `SP_NDICT="EN DE"` to `684 640` for the SMK-37 (its app slot
619,520 bytes: the image 613,968) and `572 528` for the FM-1 (581,564 bytes: 576,992); a list word costs about 170
bytes. MOPE's words (`assets/speak/mope.txt`, its grammar: gen_vocab.py writes its fragments as MO_W, MO_AT, MO_CLS)
come before the lists'. Set `SP_NDICT` yourself to try other sizes; the cache keeps the SMK-37's words for both boards.

The intermediate files (`build/felucca.bin`, `build/loader/ota.bin`, `build/felucca.dis`) are shared: build the
FM-1's Felucca again before `tests/run_tests.sh`. The default stays Felucca on the FM-1, byte for byte.
`tests/sp_budget.py build/felucca.dis` lists the loops of a Speak37 build's audio interrupt (the target's CPU cost).

USB host mode (SMK-37 Pro, `FELUCCA_USBHOST`, first step: enumeration only): the USB port leaves device mode and
becomes a full-speed host, enumerates what is plugged in (a USB microphone) and shows on the screen VID:PID, the
manufacturer and product strings, the speed, the interfaces, endpoints and audio formats (channels, bits, sample
rates). It never starts by itself: console `host`, or hold < and > together for 3 s. < / > scroll the list; any
other button, 60 s without a device or 120 s in all return to device mode (the update page and the console work
again). Every step and descriptor field goes to a RAM log: console `logdump`, `hoststate` (the summary and the raw
descriptors). The firmware switches no 5 V supply and the SMK-37 Pro gives none on its port: put a self-powered
hub between keyboard and device (or use a Y cable). A hub is driven minimally: its ports powered and polled, the
first full-speed device behind it reset and enumerated (a low-speed one is not supported). The screen keeps the
outcome (OK / FAILED and why) until host mode ends and lists the newest log lines under the device's.

## Samples

The CC0 instrument samples that the SAMPLE engine uses are in `assets/samples-cc0/`
(Versilian Studios, see `ATTRIBUTION.txt` there). `tools/fetch_cc0.py` downloads them
again from the source repositories. Without that folder the build still works, with no
instrument sets in the SAMPLE engine and no PIANO in SLICE (the PIANO set's middle C): they play a plain sine
at the note's pitch instead, and the screen says NO SAMPLE.

## Browser emulator

```
./build.sh
web/emu/build.sh
python3 -m http.server -d build/emu 8790     # open http://localhost:8790/
```

`web/emu/felucca_web.c` builds the firmware's sources as the host tests do, against a simulated FM-1, and
Emscripten compiles it to WebAssembly (`build/emu/felucca.wasm`, with `index.html`, `worklet.js` and the font in `fonts/`). The page
keeps the flash's storage sectors (projects, user presets, settings) in the browser. Not emulated: USB (audio,
MIDI out, the editor, updates), TRS MIDI and the user sample slots. A build of it is on the site:
<https://hugelton.github.io/Felucca/webapp/try/>.

## Tests

```
tests/run_tests.sh
```

Runs the host tests and, with Node.js, the web page tests (and, with Emscripten too, the browser emulator's). Run it after `./build.sh`
(it uses `build/` and needs `AC79_SDK` set as for the build). The suites cover flash storage,
user presets, projects of every format, backup, the keys and knobs, MIDI (USB, TRS, clock,
control), USB audio, the update entry and loader, the command-line installer, the UI (the real
drawing code against stubs: every screen in every palette is rendered and checked for clipped or
overlapping text; PNGs land in `build/ui_new/`), every engine (DRUM, NOISE, PHYS, FM6, SLICE, the
DIGITAL conversion), the chord keys, the modulation matrix, the FX layer, the reverbs, the SLICER
and swing. With `DAISYSP` pointing at a DaisySP checkout, the PHYS models are also compared with
their floating-point originals; without it that test is skipped.

The regression suite (`tests/regress.c`) renders every engine and preset and compares a
hash of each render with `tests/golden.txt`; it also checks levels, voices and the CPU
cost (`tests/cpu_baseline.txt`, `tests/target_budget.txt`). After an intended change of
the sound, `GOLDEN_UPDATE=1 sh tests/run_tests.sh` rewrites the hashes; `BUDGET_UPDATE=1`
does the same for the cost files.

## Install

Use the web installer in Chrome or Edge:
<https://hugelton.github.io/Felucca/webapp/installer/>. It installs the released package.

From the command line (needs `pip3 install mido python-rtmidi`):

```
python3 tools/fm1_install.py build/felucca.fwsc
python3 tools/fm1_install.py --info          # identity of the connected FM-1
```

Or, to install your own build from the web installer, make a local copy of the site and open it from `localhost`
(Web MIDI needs a secure context):

```
python3 web/make_site.py build/felucca.fwsc dev /tmp/felucca-site
cd /tmp/felucca-site && python3 -m http.server 8000
# open http://localhost:8000/webapp/installer/
```

Installing firmware is at your own risk. If an install fails and the FM-1 no longer
starts, recovery needs [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter).
