<h1 align="center">FM1_AudioBase</h1>

<p align="center">
  The M-VAVE FM-1 as a USB audio interface: the base FM-1 firmware to fork from<br>
  <a href="https://stevexfer.github.io/FM1_AudioBase/"><b>Web installer</b></a> ·
  <a href="https://github.com/stevexfer/FM1_AudioBase/releases">Releases</a> ·
  <a href="BUILDING.md">Building</a> ·
  <a href="docs/DEVELOPING.md">Developing</a>
</p>

FM1_AudioBase does one thing: what the computer plays reaches the FM-1's outputs, MASTER sets the volume, and GLO
shows a visualizer of the sound. It is a clean, small firmware to start new FM-1 firmware from: an app's DSP goes
between the USB audio and the volume.

- **USB audio output "FM-1"** (the computer plays to it): 48 kHz, 16-bit stereo, resampled to the codec's clock.
- **USB audio input "FM-1"** (the computer records it): what the FM-1 plays, after the volume, 48 kHz 16-bit stereo,
  the codec's own samples.
- **MASTER** is the volume of both: off at the bottom, 0 dB at the top, ramped per block (no zipper noise).
- **The screen:** the two streams' state, L/R peak meters, the volume in dB, CPU, USB and MIDI activity. The PLAY
  LED is lit while the computer plays to the FM-1, REC while it records from it.
- **GLO** toggles the visualizer (the waveform, fed back on itself; MOOD_FM1's), GLO lit while it shows.
- USB-MIDI and TRS MIDI in are kept running (drained), so the installers always reach the FM-1.

Tested on an FM-1: both USB audio streams, MASTER and the visualizer. 

PLEASE NOTE: THIS IS VERY EARLY DEVELOPMENT, 0.1 means 0.1 :).  
I welcome pull requests that improve while keeping the premise simple!

> [!WARNING]
> **Installing custom firmware is at your own risk.** FM1_AudioBase is unofficial firmware, provided as is, without
> any warranty (GPL-3.0 sections 15 and 16). It is not made, endorsed or supported by M-VAVE. A failed or interrupted
> install can leave the FM-1 unable to start until it is recovered with a USB tool. You can return to the official
> firmware at any time.

## Install

1. Charge the FM-1 and plug it straight into the computer with a data cable. Close apps that use MIDI.
2. Open the **[web installer](https://stevexfer.github.io/FM1_AudioBase/)** in Chrome or Edge (Safari and Firefox have
   no Web MIDI with SysEx).
3. Press **Install**. Leave the cable alone until it says Done.

From the command line, with a package from [Releases](https://github.com/stevexfer/FM1_AudioBase/releases) or your own
build:

```bash
pip install mido python-rtmidi
python3 tools/fm1_install.py build/audiobase.fwsc
```

**Back to the official firmware:** download FM-1 V15 from [m-vave.com/download](https://www.m-vave.com/download) and
write it with the web installer's "Back to the official firmware". FM1_AudioBase uses the Felucca platform's update
loader, so other Felucca-platform firmware (SLOOP, ORBIT, ChoralRoot, GATOR, MOOD_FM1, AMB-1) installs over it the
usual way.

**If something goes wrong:** an interrupted write resumes when you press Install again. After two failed boots in a
row the FM-1 opens USB RESCUE by itself (or hold OCT− at power-on), and the installer works there too. An FM-1 that
does not start at all can be recovered with [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker).

## Build

See **[BUILDING.md](BUILDING.md)**: the toolchain, `./build.sh`, the host simulator and tests, the emulator, and how
a release is made. [docs/DEVELOPING.md](docs/DEVELOPING.md) describes the platform and its app API.

## Credits

FM1_AudioBase is a fork of MOOD_FM1, the FM-1 port of MOOD, which was built on
[GATOR](https://github.com/npi3pak/fm1-gator) by npi3pak. It stands on the work of:

- **Leo Kuroshita, Hügelton Instruments**: [Felucca](https://github.com/hugelton/Felucca), the FM-1 platform: HAL,
  update loader, USB-MIDI, USB audio, OTA, storage and installers (GPL-3.0).
- **npi3pak**: [GATOR](https://github.com/npi3pak/fm1-gator), the platform cut down and its release tooling.
- **dspaudio**: [ORBIT](https://github.com/dspaudio/orbit), the platform GATOR was cut down from.
- **isod89**: [SLOOP](https://github.com/isod89/sloop-fm1), the web install protocol and the stock supply levels.
- **Quixotic7**: [ChoralRoot](https://github.com/Quixotic7/ChoralRootFM1), the audio interrupt fix and the release
  layout; [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker).
- **Charles Vestal**: [AMB-1](https://github.com/charlesvestal/fm1-amb), which showed the FPU in use on the FM-1.
- **Dimitar Toshkov Zhekov**: the Terminus font (SIL OFL 1.1).
- **JieLi Technology**: three AC79 SDK files in every package (Apache-2.0).

## License

FM1_AudioBase is free software under the [GNU General Public License v3.0 only](LICENSE), provided without any
warranty. The parts it is built from keep their licences and copyright notices:

| Part | Author | Licence | Text |
|---|---|---|---|
| FM1_AudioBase, MOOD_FM1's visualizer, GATOR (the app, web installer, release tooling) | stevexfer; npi3pak | GPL-3.0-only | [LICENSE](LICENSE) |
| Felucca platform: HAL, update loader, USB-MIDI, USB audio, OTA, storage, tools (through ORBIT and SLOOP, modified) | Leo Kuroshita, Hügelton Instruments | GPL-3.0-only | [LICENSE](LICENSE) |
| Web install protocol `fm1ota.js`, `fm1pkg.js` (from SLOOP 2.7, unchanged) | Leo Kuroshita, Hügelton Instruments; isod89 | GPL-3.0-only | [LICENSE](LICENSE) |
| Terminus font (rasterised into the firmware) | Dimitar Toshkov Zhekov | SIL OFL 1.1 | [LICENSES/OFL-Terminus.txt](LICENSES/OFL-Terminus.txt) |
| Three AC79 SDK files in every package (`tools/sdk`, unmodified) | JieLi Technology | Apache-2.0 | [LICENSES/Apache-2.0-JieLi-SDK.txt](LICENSES/Apache-2.0-JieLi-SDK.txt) |

Every release and the web installer carry these licence texts next to the packages, and each release's complete
source is its tag in this repository. [LICENSING.md](LICENSING.md) has the details.

"M-VAVE" and "FM-1" are trademarks of their respective owners. FM1_AudioBase is not affiliated with, endorsed by or
supported by them.
