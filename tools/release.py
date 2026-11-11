#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 npi3pak
"""Build the FM1_AudioBase release folder: the package, its checksum and every licence it needs.

  tools/release.py X.Y            -> build/release-X.Y/
      audiobase-X.Y.fwsc          the package, 48 kHz (identity FM-1_9XY, version X.Y on the splash)
      SHA256SUMS                  its SHA-256
      LICENSE LICENSING.md LICENSES/   GPL-3.0 and the third-party licences (the package holds Apache-2.0 SDK files)

JIELI_TOOLCHAIN defaults to ~/.jieli/toolchain (tools/get_toolchain.sh), AC79_SDK to tools/sdk.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
VARIANTS = [("48", "")]


def main():
    if len(sys.argv) != 2 or not re.fullmatch(r"\d\.\d(-[A-Za-z0-9]+)?", sys.argv[1]):
        raise SystemExit("usage: tools/release.py X.Y   (one digit each: the identity is FM-1_9XY)")
    version = sys.argv[1]
    out = SRC / "build" / f"release-{version}"
    env = {**os.environ, "JIELI_TOOLCHAIN": os.environ.get("JIELI_TOOLCHAIN", str(Path.home() / ".jieli" / "toolchain"))}
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    names = []
    for fs, suffix in VARIANTS:
        name = f"audiobase-{version}{suffix}"
        subprocess.run([sys.executable, str(SRC / "tools" / "build.py"), "--app", "audiobase", "--fs", fs,
                        "--release", version, "--name", name], check=True, cwd=SRC, env=env)
        shutil.copy(SRC / "build" / f"{name}.fwsc", out / f"{name}.fwsc")
        names.append(f"{name}.fwsc")
    sums = "".join(f"{hashlib.sha256((out / n).read_bytes()).hexdigest()}  {n}\n" for n in names)
    (out / "SHA256SUMS").write_text(sums)
    shutil.copy(SRC / "LICENSE", out / "LICENSE")
    shutil.copy(SRC / "LICENSING.md", out / "LICENSING.md")
    shutil.copytree(SRC / "LICENSES", out / "LICENSES")
    print(sums, end="")
    print(f"release: {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
