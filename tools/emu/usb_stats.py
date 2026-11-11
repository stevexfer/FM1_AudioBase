#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The USB audio output's counters (sys/usb.c pb) from a RAM dump of fm1-live (FM1_DUMPRAM).

  tools/emu/usb_stats.py RAM.bin [build/app.sym]
"""
import re
import struct
import sys
from pathlib import Path

ram = Path(sys.argv[1]).read_bytes()
syms = Path(sys.argv[2] if len(sys.argv) > 2 else Path(__file__).resolve().parents[2] / "build" / "app.sym").read_text()


def at(name):
    return int(re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M).group(1), 16) - 0x1C00000


pb = at("pb")
alt, want, on, flush, pend, play = ram[pb:pb + 6]
yl, yr = struct.unpack_from("<4h", ram, pb + 6), struct.unpack_from("<4h", ram, pb + 14)
frac, fill_q8, step, starts, pkts, drops, ovr, dry, skips = struct.unpack_from("<IiiIIIIII", ram, pb + 24)
w, r = struct.unpack_from("<I", ram, at("pb_w"))[0], struct.unpack_from("<I", ram, at("pb_r"))[0]
print(f"alt {alt} want {want} on {on} play {play}; ring fill {w - r} (smoothed {fill_q8 / 256:.1f}), step {step} "
      f"({(step / 65536 - 1) * 1e6:+.0f} ppm)")
print(f"streams {starts}, packets {pkts}, dropped (ring full) {drops}, overruns {ovr}, dry {dry}, held up {skips}")
