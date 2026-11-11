# Building FM1_AudioBase

## What you need

- Python 3 with Pillow (`pip install -r requirements.txt`)
- JieLi's Linux toolchain (clang for pi32v2): `sh tools/get_toolchain.sh` puts it in `~/.jieli/toolchain`
- Docker on macOS or ARM Linux: the toolchain is linux/amd64 only, and each tool then runs in a container.
  On x86-64 Linux it runs natively.

The three JieLi AC79 SDK files every package holds (Apache-2.0) are in `tools/sdk/`; `AC79_SDK` can point at a full
SDK checkout instead. `tools/build.py` checks their SHA-256.

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
sh tools/get_toolchain.sh
PYTHON=.venv/bin/python ./build.sh                 # build/audiobase.fwsc   (48 kHz)
PYTHON=.venv/bin/python ./build.sh --app demo      # build/demo.fwsc        (the small demo synth)
```

Options: `--app NAME` (`firmware/apps/NAME`, default `audiobase`), `--fs 48|44` (the codec clock,
`firmware/sys/fw_rate.h`: 48 kHz by default; 44 is for comparison, and its USB audio runs at 44.1 kHz), `--name NAME` (the package's file name), `--release X.Y` (identity `FM-1_9XY`, version
`X.Y` on the splash). Environment: `FW_UART=0` (no TRS MIDI), `FW_UBOOT_COMBO=0` (no OCT−+OCT+ 5 s), `FELUCCA_OTA_DRYRUN=1`
(an update runs to the end but writes nothing), `JOBS=N` (parallel C++ compiles). C++ is rebuilt incrementally
(`build/cxx/APP/`).

Every build checks the image: the app slot and RAM sizes, 8 KB of pool headroom, no ROM calls, no soft-float
routines, no `.init_array`, and no register access outside `firmware/hal/`.

## Install a local build

```bash
pip install mido python-rtmidi
python3 tools/fm1_install.py --info               # identity of the connected FM-1
python3 tools/fm1_install.py build/audiobase.fwsc
```

Or from the browser: `python3 web/make_site.py` makes the installer site from the local builds in `build/site`;
`python3 -m http.server -d build/site 8000`, then http://localhost:8000 in Chrome or Edge (Web MIDI needs localhost
or HTTPS).

## Host simulator and tests

The app's code runs on the host against a stub HAL, built with ASan and UBSan (any finding fails the run): sound to
WAV, the screen to PNG. It says nothing about timing or CPU load: that is measured on the device only.

```bash
.venv/bin/python tools/sim.py                      # host/audiobase.sim -> build/host/out/*.png, audiobase.wav
.venv/bin/python tools/sim.py --app demo
.venv/bin/python tools/sim.py --test               # storage test (A/B slots, an interrupted write)
node web/test_web.mjs build/audiobase.fwsc         # the installer's protocol against an emulated FM-1
sh tests/update_tests.sh build/audiobase.fwsc build/app.bin   # OTA and loader (STOCK_V15 / STOCK_V16: M-VAVE's files)
```

A script is a list of lines: `frames N`, `key K down|up`, `btn PLAY down|up`, `knob K1 3`, `master 800`,
`midi 0x90 60 100`, `usbtone 1000 -6` (the computer plays a sine at -6 dBFS to the FM-1; `usbtone 0` stops),
`usbrec 2` (the computer records it: 0 no stream, 1 selected, 2 taken), `screen out.ppm`, `wav out.wav`, `logo`,
`leds`, `bench MS`.

## The whole firmware in the emulator

[FM-1Ception](https://github.com/stevexfer/FM-1Ception)'s `fm1-live` runs the package itself (both cores, the codec,
the USB host): the computer plays a file to the FM-1's USB audio output and records its USB audio input.

```bash
# tone.raw: s16le stereo at 48 kHz, no header
FM1_AUDIO_FRAME_NS=20800 FM1_USB_PLAY=tone.raw FM1_USB_WAV=rec.raw FM1_USB_RATE=48000 \
  FM1_BENCH_WAV=out.raw FM1_BENCH_PANEL=panel.ppm FM1_BENCH_KEYS='0:pot:1023,2500:pot:512,3000:GLO,3100:GLO^' \
  ../FM-1Ception/target/release/fm1-live --fwsc build/audiobase.fwsc --usb --bench 5000
```

`out.raw` is the codec's output (the emulator reads its words >> 8: 6 dB under the USB stream), `rec.raw` what the
computer recorded, `panel.ppm` the panel at the end. `FM1_BENCH_KEYS`: `MS:pot:N` the MASTER pot, `MS:NAME` /
`MS:NAME^` a button down / up.

## Releasing

A release is a tag `vX.Y` (one digit each: the package identity is `FM-1_9XY`).

1. Add `docs/releases/X.Y.md` (the release notes) and commit.
2. `git tag v0.1 && git push origin v0.1`.
3. `.github/workflows/release.yml` builds the package (`tools/release.py`), runs the simulator and the installer
   test, and creates the GitHub release with `audiobase-X.Y.fwsc`, `SHA256SUMS` and the licences.
4. It then starts `.github/workflows/pages.yml`, which takes the package from the release, checks it against
   `SHA256SUMS`, makes the site (`web/make_site.py --release`) and deploys it to GitHub Pages.
5. From then on every push to `main` that changes `web/` publishes the site again, with the latest release's
   package (`pages.yml`; run by hand it takes a version, blank for the latest).

The same locally: `tools/release.py X.Y` makes `build/release-X.Y/`, and
`python3 web/make_site.py --release build/release-X.Y` the site.

GitHub Pages must be set to deploy from GitHub Actions once: Settings → Pages → Source: GitHub Actions.
`.github/workflows/ci.yml` builds and tests every push and pull request.
