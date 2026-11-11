#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Build the host simulator (the app's C code with a stub HAL) and run a script.

  tools/sim.py [SCRIPT]          default script: host/<app>.sim
  tools/sim.py --app demo        another firmware/apps/ app (default: audiobase)
  tools/sim.py --no-sanitize     faster build, no ASan/UBSan
  tools/sim.py --test            host tests (flash storage) instead of a script

Script lines: frames N | key K down|up | btn NAME down|up | knob NAME STEPS |
master 0..1023 | midi STATUS D1 D2 | usbtone HZ [DBFS] (the computer plays a sine; 0 stops) |
usbrec 0|1|2 (the computer records: no stream, selected, taken) | screen PATH.ppm | wav PATH.wav | leds | quit
PPM screens are also saved as 2x PNG when Pillow is installed.
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import build  # noqa: E402

HOST = SRC / "build" / "host"


def host_flags(sanitize):
    flags = ["-O2", "-g", "-Wall", "-Wno-unused-function", "-Wno-unused-parameter", "-DFW_HOST", *build.rate_flags(),
             *build.name_flags(), *build.app_flags()]
    return flags + (["-fsanitize=address,undefined", "-fno-sanitize=shift-base", "-fno-sanitize-recover=undefined"] if sanitize else [])


def compile_cxx(sanitize):
    cxx = os.environ.get("CXX", "c++")
    objdir = HOST / ("cxx-asan" if sanitize else "cxx") / build.build_tag()
    objdir.mkdir(parents=True, exist_ok=True)
    flags = [*host_flags(sanitize), "-std=c++11", "-DTEST", "-Wno-unused-local-typedef", "-Wno-unused-variable",
             f"-I{build.app_dir().relative_to(SRC)}", "-Ifirmware/sys"]
    objs, jobs = [], []
    for src in build.cxx_sources(False):
        name = str(src.relative_to(SRC)).replace("/", "_").removesuffix(".cc")
        obj, dep = objdir / f"{name}.o", objdir / f"{name}.d"
        objs.append(obj)
        if build.stale(obj, dep):
            jobs.append([cxx, *flags, "-MMD", "-MF", str(dep), "-c", str(src.relative_to(SRC)), "-o", str(obj)])
    procs = [subprocess.Popen(j, cwd=SRC) for j in jobs]
    if any(p.wait() for p in procs):
        raise SystemExit("sim: c++ build failed")
    return objs


def compile_app_c(sanitize, inc):
    """the app's C built with the FPU on the target (its *.c other than app.c, the `c` lines of its build.txt)"""
    objdir = HOST / ("c-asan" if sanitize else "c") / build.build_tag()
    objdir.mkdir(parents=True, exist_ok=True)
    srcs = [f for f in sorted(build.app_dir().glob("*.c")) if f.name != "app.c"]
    srcs += [(SRC / p).resolve() for p in build.app_build_txt()["c"]]
    objs, jobs = [], []
    for src in srcs:
        obj = objdir / f"{build.obj_name(src, '.c')}.o"
        objs.append(obj)
        jobs.append([os.environ.get("CC", "cc"), "-std=c11", *host_flags(sanitize), *inc, "-c", str(src), "-o", str(obj)])
    procs = [subprocess.Popen(j, cwd=SRC) for j in jobs]
    if any(p.wait() for p in procs):
        raise SystemExit("sim: c build failed")
    return objs


def compile_host(name, sanitize):
    HOST.mkdir(parents=True, exist_ok=True)
    build.generate()
    cc = os.environ.get("CC", "cc")
    inc = [f"-I{SRC / d}" for d in ("host", "firmware/sys", build.app_dir().relative_to(SRC), "build/gen")]
    out = HOST / name
    if name != "sim":
        subprocess.run([cc, "-std=c11", *host_flags(sanitize), *inc, str(SRC / "host" / f"{name}.c"), "-o", str(out)],
                       check=True, cwd=SRC)
        return out
    objs = compile_cxx(sanitize) + compile_app_c(sanitize, inc)
    main = HOST / "sim.o"
    subprocess.run([cc, "-std=c11", *host_flags(sanitize), *inc, "-c", str(SRC / "host" / "sim.c"), "-o", str(main)],
                   check=True, cwd=SRC)
    subprocess.run([os.environ.get("CXX", "c++"), *host_flags(sanitize), str(main), *map(str, objs), "-lm", "-o",
                    str(out)], check=True)
    return out


def to_png(text):
    try:
        from PIL import Image
    except ImportError:
        return
    for line in text.splitlines():
        if line.startswith("screen "):
            ppm = Path(line.split(" ", 1)[1])
            png = ppm.with_suffix(".png")
            Image.open(ppm).resize((480, 480), Image.NEAREST).save(png)
            print(f"png {png}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script", nargs="?")
    ap.add_argument("--app", default=build.APP)
    ap.add_argument("--no-sanitize", action="store_true")
    ap.add_argument("--test", action="store_true")
    ap.add_argument("--fs", choices=("44", "48"), default="48")
    a = ap.parse_args()
    build.set_app(a.app)
    build.set_fs(a.fs)
    a.script = a.script or str(SRC / "host" / f"{a.app}.sim")
    if a.test:
        return subprocess.run([str(compile_host("storage_test", not a.no_sanitize))]).returncode
    sim = compile_host("sim", not a.no_sanitize)
    (SRC / "build" / "host" / "out").mkdir(parents=True, exist_ok=True)
    r = subprocess.run([str(sim)], stdin=open(a.script), capture_output=True, text=True, cwd=SRC)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    to_png(r.stdout)
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
