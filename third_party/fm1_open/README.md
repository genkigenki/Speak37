# fm1_open: the 808 drum voices (reference)

Unchanged copies of four MIT files from M-VAVE's FM1 Open (<https://github.com/m-vave/fm1_open>, commit
`5d3eac2573dbd054254fabda6fde099adc594348`, `third_party/synth/`): the 808 drum voices, a DaisySP / Plaits
adaptation by M-VAVE.

    Copyright (c) 2020 Electrosmith, Corp, Emilie Gillet; 2026 M-VAVE.   MIT (LICENSES/MIT-fm1_open-drum808.txt)

They are float code and are not built into the firmware. Speak37's RHYTHM plays a fixed-point port of them,
`firmware/src/sp_808.c` (MIT as well); `tools/gen_808.c` runs these originals on the host to write the port's
constants (`firmware/src/sp_808_tab.h`), and `tests/sp_test.c` compares the port with them, sound by sound.
Nothing else of FM1 Open is used (not its SDK, its libraries or its engine).
