# Contributing to Speak37

Thanks for helping! Please use this repository's Issues for bugs you can reproduce (one issue per bug) and its
Discussions for ideas, questions, demos and anything else.

**FM-1: Need feedback how to improve the usage on the FM-1.** Its controls differ from the SMK-37's (no pads, faders
or wheels); ideas for a better mapping are very welcome.

## Reporting a bug

Please include the Speak37 version (the boot screen and the console's `status`), the keyboard (SMK-37 Pro, Elite,
MKE-P37 or FM-1), your computer's OS if the installer is involved, the steps to reproduce it, what you expected and
what happened. A short video helps. For MOPE, the console's `mope` (USB serial) lists what it is waiting for.

## Pull requests

- keep it small and focused on one thing;
- make sure you understand the change and can explain it;
- test it on a real keyboard;
- run `tests/run_tests.sh` (see [BUILDING.md](BUILDING.md));
- new files carry the GPL-3.0-only SPDX header.

By contributing you agree that your contribution is licensed under GPL-3.0-only.

Speak37 is built on [Felucca](https://github.com/hugelton/Felucca) (Leo Kuroshita / Hügelton Instruments); changes
to the platform itself may belong upstream there.
