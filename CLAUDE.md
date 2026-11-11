# FM1_AudioBase

The base FM-1 firmware (M-VAVE FM-1, JieLi AC79, pi32v2): USB audio from the computer through the FM-1 to its outputs
(the codec and, back to the computer, a USB audio input), MASTER the volume, GLO a visualizer. A fork of MOOD_FM1,
itself on GATOR's cut-down Felucca platform (GPL-3.0-only). Forks build on it (next: FM1_Clouds /
Locus). Public repo: everything in English.

- GPL-3.0-only, like MOOD_FM1 and GATOR: never copy its code into a clean-room project (an agent that has read this
  code does not edit one).
- Unity build: `firmware/sys/fw.c` includes every `.c`; include order matters. Apps live in `firmware/apps/NAME/`
  (`app.c`, optional `*.c` / `*.cc` built separately with the FPU, `build.txt`, `third_party.txt`, `name.txt`,
  `logo.png`); `--app NAME` selects one (default `audiobase`; `demo` is the small synth example).
- C in the unity build is built without FPU (as ORBIT); the app's other `*.c` and `*.cc` with `-mcpu=r3 -mfprev1`.
  Cross with ints/pointers only (`apps/audiobase/ab_fw.h`). No static constructors (build fails on `.init_array`).
- `build.txt`: `define FW_USB_PLAY=1` (the computer plays to the FM-1), `define FELUCCA_UAC=1` (it records the FM-1),
  `define FW_USB_PRODUCT="FM-1"` (must contain "FM-1": the installers look for it), `c PATH`, `include DIR`, `small FILE`.
- USB audio (`sys/usb.c`): playback 48 kHz stereo, adaptive, resampled to the codec clock; capture at the codec's rate,
  async, the codec's samples bit for bit (`uac_tap` in `sys/audio.c`, after the app's volume). Audio streaming
  interfaces come before MIDI in the AC header (macOS 15 and older need that). Both are serviced from TIMER5 at 2 kHz,
  nested into the render (`sys/main.c`).
- The sample rate lives in `firmware/sys/fw_rate.h` only; 48 kHz (48,076.9 Hz) is the default and the only released
  package. `FW_FS` is the rounded integer, `FW_FS_HZ` the exact float.
- `firmware/hal/`, `firmware/loader/`, `sys/ota.c`, `sys/recovery.c`, `sys/storage.c`, `sys/midi_uart.c` come from
  ORBIT and are hardware-verified there: change them only with a reason. The loader must stay byte-identical unless
  deliberately changed (it is what makes reflashing safe). The packer already stores the SDK SPL BANKCB/ENC-encoded.
- Register access only in `hal/`; `tools/build.py` fails on MMIO literals, volatile casts or inline asm in `sys/`, `apps/`, `loader/`.
- `app_audio` runs in the ALNK0 ISR (256 frames, 5.3 ms); TIMER5 (input, USB) outranks it. No float in the unity
  build, no malloc. The app must drain `fw_midi_read` every frame (a full ring holds back the installer's SysEx).
- Target build: `PYTHON=.venv/bin/python ./build.sh [--app NAME]` (Docker on macOS; SDK files vendored in `tools/sdk/`).
  Host check: `.venv/bin/python tools/sim.py` (ASan/UBSan, fatal; `usbtone` / `usbrec` fake the USB audio). Host
  results say nothing about timing. Emulator end to end: FM-1Ception `fm1-live` (BUILDING.md). Update paths:
  `tests/update_tests.sh build/audiobase.fwsc build/app.bin` (STOCK_V15 / STOCK_V16: M-VAVE's official V15 / V16 `.fwsc`).
- `sys/audio.c` acknowledges the ALNK half before rendering (ChoralRoot 0.18, heard on hardware); OTA sets
  `fw_audio_hold` so the app does not render; `fm1_cstart` raises the supplies to stock (SLOOP 2.7). Keep these.
- Web installer: `web/` (fm1ota.js / fm1pkg.js are SLOOP 2.7's; fm1ota.js's FM1_PORT also takes the loader's macOS names). `web/make_site.py [--release
  build/release-X.Y]` -> `build/site`; `node web/test_web.mjs PKG` is the protocol test.
- Releases: tag `vX.Y` (one digit each, identity FM-1_9XY) -> `.github/workflows/release.yml` (`tools/release.py`) ->
  `pages.yml`. Release notes in `docs/releases/X.Y.md`. `LICENSE`, `LICENSING.md`, `LICENSES/` travel with every package.
- New files carry `SPDX-License-Identifier: GPL-3.0-only`; keep the existing copyright headers. A file changed for
  FM1_AudioBase gets a line "Modified for FM1_AudioBase, 2026: what". A new third-party part goes into LICENSING.md,
  README's licence table, LICENSES/ and the installer's Licences section.
- The splash stays `FW_SPLASH_MS` (main.c) from drawing it; `app_init` runs meanwhile and must not draw.
- Never claim hardware testing that did not happen.
