# Developing on the platform

FM1_AudioBase is one app on a small firmware platform for the FM-1 (JieLi AC79, pi32v2 core), cut down from
[ORBIT](https://github.com/dspaudio/orbit) (ORBIT ← SLOOP ← Felucca) for GATOR, then carried by MOOD_FM1. The
hardware-proven parts are kept: the HAL, the update loader, USB-MIDI, USB audio, OTA, flash storage and the `.fwsc`
packer. Everything else is a thin API for apps.

| | |
|---|---|
| Screen | ST7789 240×240 RGB565, drawn in strips through a canvas of up to 240×124 (no frame buffer) |
| Audio | I2S stereo at 48 kHz (or 44.1 kHz with `--fs 44`), `app_audio()` called from the interrupt for 256 frames (5.8 ms) |
| Input | 27 note keys, 14 buttons, 7 encoders, the MASTER pot, the battery |
| LEDs | under every key and button |
| MIDI | USB-MIDI in / out, TRS MIDI IN (3.5 mm) |
| USB audio | an output the computer plays to (`FW_USB_PLAY`) and an input it records (`FELUCCA_UAC`), both optional, per app |
| Storage | 2 slots of 3840 B in flash, A/B writes with CRC (a power loss keeps the last copy) |
| Update | OTA over USB-MIDI with `tools/fm1_install.py` or the web installer, no programmer |
| Safety | watchdog, boot-loop guard → USB RESCUE, crash screen with PC / vector, stack guards |

## Layout

```
firmware/
  apps/NAME/         one folder per app (build: --app NAME)
    app.c            app_init / app_frame / app_audio
    *.cc             C++ (optional): built separately, with the FPU
    *.c              C other than app.c (optional): built separately, with the FPU
    build.txt        `define NAME=VALUE`, `include DIR`, `c PATH` (more FPU C), `small FILE` (built -Os)
    third_party.txt  the third_party/ folders to build with the app
    name.txt         the name on the splash (default FM-1 PLAYGROUND)
    logo.png         the splash logo, any size: tools/gen_logo.py fits it into 240×240, 256 colours
  cxx/               minimal <algorithm> / <cmath> / <new> and the C++ runtime (the toolchain has none)
  sys/               the system layer
    fw.h             the app API (types, constants, buttons, encoders)
    fw.c             the translation unit (unity build): the include order matters
    main.c           start, main loop, TIMER5, crash screen, UBOOT
    audio.c          the I2S interrupt → app_audio, CPU load
    input.c          buttons / encoders / keys → fw_input_t, LEDs, MIDI API
    flash.c          flash driver, storage slots, OTA hooks
    gfx.c lcd.c      canvas, fonts, ST7789
    usb.c ota.c recovery.c storage.c midi_uart.c libc.c bootguard.h   from ORBIT
  hal/               hardware registers (only here), from ORBIT
  loader/            the update loader (byte-identical to ORBIT's)
  app.ld crt0.S      memory map and entry
host/                the simulator: the same app code on macOS / Linux → WAV and PNG
tools/               build.py, release.py, generators, fm1_install.py, sim.py, sdk/ (JieLi files, Apache-2.0)
web/                 the web installer and its site builder
```

## App API

An app is a folder `firmware/apps/NAME/` with an `app.c` (it may `#include` its own `.c` files) and three functions:

```c
static void app_init(void);                           // once, before the audio starts; the splash is up: do not draw
static void app_frame(const fw_input_t *in);          // main loop, ~66 Hz
static void app_audio(int32_t *lr, uint32_t frames);  // INTERRUPT: frames = 256, LRLR…, ±32767
```

`fw_input_t`, per frame:

| Field | What |
|---|---|
| `ms` | milliseconds since start |
| `held / pressed / released` | button masks, `FW_BIT(FW_BTN_PLAY)` etc. |
| `keys / keys_pressed / keys_released` | masks of the 27 keys (bit 0 = F3 = MIDI 53, `FW_KEY_BASE_NOTE`) |
| `knob[FW_KNOB_*]` | encoder detents since the last frame (+ = clockwise) |
| `master` | the pot, 0..1023 |
| `battery` | raw battery ADC |

Buttons: `FX SCL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT_DOWN OCT_UP`.
Encoders: `SELECT ALGO PRESET 1 2 3 4`.

```c
fw_led_button(FW_BTN_PLAY, on);   fw_led_note_key(k, on);   fw_leds_keys(mask);
fw_midi_read(&m) -> 0/1           fw_midi_send(status, d1, d2)        // main loop only
fw_store_load(slot, dst, max)     fw_store_save(slot, src, len)       // 2 slots × 3840 B
fm1_irq_off() / fm1_irq_on()      // critical section against app_audio (main loop only)
fw_stats.cpu_q8                   // audio interrupt load, 256 = 100%
FW_NOTE_INC[note]                 // uint32 phase increment of a MIDI note at FW_FS
FW_SINE[FW_SINE_N]                // int16 sine, 1024 points
```

Drawing (`gfx.c`): `lcd_fill(x, y, w, h, color)`, `draw_text_box(x, y, w, &FONT_S, "txt", color, align)`, and for
graphics the canvas: `cv_begin(w, h, bg)` → `cv_text / cv_rect / cv_line / cv_pset` → `cv_blit(x, y)`. The canvas is
at most 240×124, so a screen is drawn in strips. Fonts: `FONT_S` (16 px), `FONT_L` (32 px). Colours: `RGB(r, g, b)`,
`C_BLACK C_WHITE C_HI C_GRAY C_DIM C_ACCENT`.

## Rules

- **`app_audio` is an interrupt.** No drawing, flash, waiting or `fw_midi_send` there. The budget is 5.8 ms for 256
  frames; watch `fw_stats.cpu_q8`.
- **The second core** (`sys/core1.c`, `hal/fm1_cpu1.h`): from `app_audio`, `fw_core1_run(fn, arg)` hands it one job
  (1 = taken; 0 = do the work here), `fw_core1_wait()` before using what it wrote (-1: it gave up and is held, do its
  work here, one core from then on). The job must touch only what this core leaves alone until the wait; it runs from
  the flash on a 4 KB stack, with the FPU.
- **float only outside `app.c`.** The unity build (`fw.c` and `app.c`) is built without the FPU, as ORBIT is, where
  `float` would be slow software emulation (the build fails if any is linked). The app's other `*.c` and its `*.cc`
  are built with the single-precision FPU (`-mcpu=r3 -mfprev1`, as the AC79 SDK); they reach `app.c` with integers
  and pointers only (`apps/audiobase/ab_fw.h`: functions `app.c` defines for them, not `static`). The FPU has no `double`. Static constructors do not run: create such objects with
  placement new in `app_init` (the build fails on `.init_array`).
- **Do not block `app_frame` for more than a few seconds:** the watchdog (~8 s) restarts the device.
- No `malloc` and no libc: static buffers only. Mark large buffers `FW_POOL` (336 KB, ~58 KB of it the canvas; the
  build keeps 8 KB free).
- Hardware registers only in `hal/`. `build.py` fails on MMIO addresses, `volatile` casts or inline asm in `sys/`,
  `apps/`, `loader/`.
- `fw_store_save` erases a sector with interrupts off: the sound stops meanwhile. Do not save while playing.
- During an update `app_audio` is not called (`fw_audio_hold`): the update gets the whole CPU.
- The ALNK interrupt is acknowledged **before** the render (`sys/audio.c`, as ChoralRoot 0.18): otherwise a render
  that ends after the buffer boundary eats the next interrupt and the DMA repeats the old half.
- At start the VDC14 / SYSVDD supplies are raised to M-VAVE's stock levels (`fm1_supplies_stock`, from SLOOP 2.7 /
  X0X 1.0.9): at the loader's levels some units misread flash and SRAM.

The package format, the loader and the OTA protocol are those of SLOOP / ORBIT, so FM1_AudioBase installs and is
replaced the same way they are. The USB descriptors stay Felucca's (VID:PID 1209:0001); an app names the device with
`FW_USB_PRODUCT` (build.txt), which must contain "FM-1" so that the installers find it.

## USB audio

`sys/usb.c`, set per app in `build.txt` (`define FW_USB_PLAY=1`, `define FELUCCA_UAC=1`): one USB audio function
with both streams, listed before MIDI (older macOS needs the audio streaming interfaces first).

- **Output** (`FW_USB_PLAY`, isochronous adaptive OUT): the computer plays 48 kHz 16-bit stereo; the platform
  resamples it to the codec's clock (cubic) and keeps the latency steady. In `app_audio`: `usb_play_begin()`, then
  `usb_play_read(buf, frames)` (`buf`: `2 * frames` int16, L R …); 0 means silence (no stream, or not heard yet).
  `usb_play_enable(1)` once in `app_init`; `usb_play_state()`: 0 no stream, 1 selected, 2 heard.
- **Input** (`FELUCCA_UAC`, isochronous asynchronous IN): what `app_audio` returns, after the app's volume, the
  codec's samples bit for bit, at the codec's rate (packets of 48 frames, one more or less to follow the clock).
  It needs nothing from the app; `usb_rec_state()`: 0 no stream, 1 selected, 2 the computer takes it.

Both are serviced at 2 kHz from TIMER5, which nests into the render, so a render that takes its whole block does
not starve them.

| If | Then |
|---|---|
| the firmware crashes at start | after 2 failed boots in a row **USB RESCUE** starts (no sound): install a working package |
| you need RESCUE by hand | hold **OCT−** at power-on |
| UBOOT (JieLi's ROM loader) | hold **OCT− + OCT+** for 5 seconds, or SysEx `F0 22 24 35 7D F7` |
| nothing starts at all | [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker), [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter) |
