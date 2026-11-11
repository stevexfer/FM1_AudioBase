#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 npi3pak
"""Make the installer site, a static folder for GitHub Pages (or any HTTPS / localhost server):

  index.html installer.js fm1ota.js fm1pkg.js            the page (web/)
  firmware/audiobase-VERSION.fwsc                        the package
  firmware/manifest.json                                 its title, identity, size and SHA-256 (the page checks it)
  emu/                                                   the browser emulator (web/emu/, tools/vendor_emu.sh), this firmware in its catalog
  firmware/SHA256SUMS LICENSE LICENSING.md LICENSES/     what goes with the package (GPL-3.0, the SDK's Apache-2.0)

  python3 web/make_site.py --release build/release-X.Y   the release folder of tools/release.py
  python3 web/make_site.py                               the local build: build/audiobase.fwsc
  python3 -m http.server -d build/site 8000              then http://localhost:8000 in Chrome or Edge
"""
import argparse
import hashlib
import json
import re
import shutil
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent
sys.path.insert(0, str(SRC / "tools"))
import fm1_install  # noqa: E402

REPO = "https://github.com/stevexfer/FM1_AudioBase"
PAGE = ["index.html", "installer.js", "fm1ota.js", "fm1pkg.js"]
AUTHOR = "stevexfer"
# the emulator's header links its SMK-37 page, which is not copied
EMU_DEVICES = '<nav class="devices" aria-label="Device"><span aria-current="page">FM-1</span><a href="smk37/">SMK-37</a></nav>'
LOADER_MARK = b"FELUCCA-LOADER-1"
PACKAGES = [
    {"id": "audiobase", "suffix": "", "title": "FM1_AudioBase", "tag": "",
     "desc": "USB audio through the M-VAVE FM-1, with its volume and a visualizer, 48 kHz."},
]


def fw_version():
    m = re.search(r'#define FW_VERSION "([^"]+)"', (SRC / "firmware/sys/fw.c").read_text())
    return m[1] if m else "?"


def describe(meta, file, raw, version):
    product = fm1_install.product_of(raw)
    if not re.fullmatch(r"FM-1_9\d\d", product):
        raise SystemExit(f"{file}: identity {product!r} is not a Felucca-platform package (FM-1_9xx)")
    if LOADER_MARK not in raw:
        raise SystemExit(f"{file}: no Felucca update loader in it")
    return {"id": meta["id"], "file": file, "title": meta["title"], "tag": meta["tag"], "desc": meta["desc"],
            "version": version, "product": product, "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}


def emulator(out, listed, version):
    """The emulator at emu/, its catalog the packages: their codec at 48,076.9 Hz (an I2S frame
    20.8 us; the context at 48 kHz), a computer on the USB port playing into them."""
    emu = HERE / "emu"
    if not (emu / "fm1_web.wasm").exists():
        raise SystemExit(f"{emu}: no emulator (tools/vendor_emu.sh ../FM-1Ception)")
    shutil.copytree(emu, out / "emu")
    page = out / "emu" / "index.html"
    html = page.read_text(encoding="utf-8")
    if EMU_DEVICES not in html:
        raise SystemExit(f"{page}: its device links have changed (make_site.py's EMU_DEVICES)")
    page.write_text(html.replace(EMU_DEVICES, ""), encoding="utf-8")
    firmwares = [{"id": p["id"], "name": p["title"], "version": version, "author": AUTHOR, "license": "GPL-3.0",
                  "about": p["desc"], "repo": REPO, "package": f"../firmware/{p['file']}",
                  "usb": True, "rate": 48000, "frameNs": 20800} for p in listed]
    (out / "emu" / "firmwares.json").write_text(json.dumps({"default": firmwares[0]["id"], "firmwares": firmwares},
                                                           indent=1) + "\n", encoding="utf-8")


def sources(a):
    if a.release:
        version = a.version or a.release.name.removeprefix("release-")
        return version, a.release, [(m, f"audiobase-{version}{m['suffix']}.fwsc") for m in PACKAGES]
    return a.version or fw_version(), a.build, [(m, f"audiobase{m['suffix']}.fwsc") for m in PACKAGES]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=SRC / "build" / "site")
    ap.add_argument("--release", type=Path, help="a release folder (tools/release.py): every package must be there")
    ap.add_argument("--build", type=Path, default=SRC / "build", help="local builds (without --release)")
    ap.add_argument("--version", help="the version shown (default: the release folder's, or FW_VERSION)")
    a = ap.parse_args()
    version, folder, wanted = sources(a)
    present = [(m, f) for m, f in wanted if (folder / f).exists()]
    if a.release and len(present) != len(wanted):
        raise SystemExit(f"{folder}: missing {[f for m, f in wanted if (m, f) not in present]}")
    if not present:
        raise SystemExit(f"no package in {folder}: ./build.sh, or tools/release.py X.Y")
    if a.out.exists():
        shutil.rmtree(a.out)
    fw = a.out / "firmware"
    fw.mkdir(parents=True)
    for f in PAGE:
        shutil.copy(HERE / f, a.out / f)
    listed = [describe(m, f, (folder / f).read_bytes(), version) for m, f in present]
    for p in listed:
        shutil.copy(folder / p["file"], fw / p["file"])
    source = f"{REPO}/tree/v{version}" if a.release else REPO
    (fw / "manifest.json").write_text(json.dumps({"source": source, "packages": listed}, indent=1) + "\n",
                                      encoding="utf-8")
    (fw / "SHA256SUMS").write_text("".join(f"{p['sha256']}  {p['file']}\n" for p in listed))
    shutil.copy(SRC / "LICENSE", fw / "LICENSE")
    shutil.copy(SRC / "LICENSING.md", fw / "LICENSING.md")
    shutil.copytree(SRC / "LICENSES", fw / "LICENSES")
    emulator(a.out, listed, version)
    (a.out / ".nojekyll").write_text("")
    for p in listed:
        print(f"site: firmware/{p['file']}  {p['size']} B  {p['product']}  {p['sha256'][:12]}")
    print(f"site: {a.out}  (python3 -m http.server -d {a.out} 8000)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
