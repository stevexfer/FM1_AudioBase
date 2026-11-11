#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Modified for GATOR by npi3pak, 2026, from ORBIT 0.6.0 (dspaudio/orbit)
"""Build the FM-1 firmware: the app, the update loader and an installable .fwsc package.

  tools/build.py [--release X.Y[-suffix]] [--name NAME]

Outputs in build/: app.bin, loader/ota.bin (update loader), NAME.fwsc (package).
See README.md for the toolchain and the SDK.

The JieLi toolchain is Linux x86-64 only. JIELI_TOOLCHAIN points at it; on
macOS (or with JIELI_DOCKER=1) each tool runs in a linux/amd64 container.
"""
import argparse
import hashlib
import os
import platform
import re
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
FW = SRC / "firmware"
OUT = SRC / "build"
GEN = OUT / "gen"
LDR = OUT / "loader"
sys.path.insert(0, str(SRC / "tools"))
import fm1pkg_make  # noqa: E402
import lz4blk  # noqa: E402

APP_XIP = 0x02000120                # app.bin offset 0 in the XIP map; the SPL jumps here
APP_SLOT = fm1pkg_make.APP_SLOT
LOADER_LOAD = 0x01C0A800
LOADER_NAME = b"usb_hid_ota.bin"    # the file name the SPL looks for
DOCKER_IMAGE = os.environ.get("JIELI_DOCKER_IMAGE", "debian:bookworm-slim")
CFLAGS = ["-Os", "-ffunction-sections", "-fno-builtin", "-Wall", "-Wno-unused-function"]
# C++ (firmware/apps/APP/*.cc and the third_party/ dirs listed in its third_party.txt): the AC79 core with its single-precision FPU (-mcpu=r3 -mfprev1, as the
# AC79 SDK builds). Floats live in the general registers, so the C side (generic) calls it through int / pointer
# arguments only. No exceptions, RTTI or static constructors (nothing runs .init_array).
CXXFLAGS = ["-mcpu=r3", "-mfprev1", "-O2", "-std=gnu++11", "-DTEST", "-fno-math-errno", "-fno-exceptions",
            "-fno-rtti", "-fno-threadsafe-statics", "-fno-use-cxa-atexit", "-nostdinc++", "-ffunction-sections",
            "-fdata-sections", "-Wall", "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-local-typedef",
            "-Wframe-larger-than=1024",
            "-Ifirmware/cxx", "-Ifirmware/sys"]
# C built with the FPU (firmware/apps/APP/*.c other than app.c, and the `c` lines of its build.txt): the same
# core and float ABI as the C++; app.c (generic) reaches them through int / pointer arguments only.
CFPUFLAGS = ["-mcpu=r3", "-mfprev1", "-O2", "-std=gnu99", "-fno-math-errno", "-fno-builtin-printf",
             "-ffunction-sections", "-fdata-sections", "-Wall", "-Wno-unused-function", "-Wframe-larger-than=1024",
             "-Ifirmware/sys"]
# C built with the FPU (firmware/apps/APP/*.c other than app.c, and the `c` lines of its build.txt): the same
# core and float ABI as the C++; app.c (generic) reaches them through int / pointer arguments only.
CFPUFLAGS = ["-mcpu=r3", "-mfprev1", "-O2", "-std=gnu99", "-fno-math-errno", "-fno-builtin-printf",
             "-ffunction-sections", "-fdata-sections", "-Wall", "-Wno-unused-function", "-Wframe-larger-than=1024",
             "-Ifirmware/sys"]
APP = os.environ.get("FW_APP", "audiobase")   # firmware/apps/APP: --app
FS48 = os.environ.get("FW_FS48", "1") == "1"   # the 48 kHz codec clock, the default (firmware/sys/fw_rate.h); --fs 44
CXX_TARGET_ONLY = ["firmware/cxx/runtime.cc"]
LIBS = ["libm.a", "libcompiler_rt.a"]
ELF_SECTIONS = {".text", ".ram_text", ".c1_text", ".data", ".bss", ".pool", ".noinit"}
LINE = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")

# SDK files of AC79NN_SDK_V1.2.1_2023-12-13 (the tested version)
SDK_SHA256 = {
    "uboot.boot": "4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4",
    "cfg_tool.bin": "276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b",
    "cfg/eq_cfg_hw.bin": "41167491bffed4651750719c973d2758adeb9021a5670d02d6a53c85ed80ea7d",
}

PRODUCT = "FM-1_900"                # package identity; release builds are FM-1_9XY
VERSION = None                      # FW_VERSION for release builds (default: firmware/sys/fw.c)


def toolchain():
    tc = os.environ.get("JIELI_TOOLCHAIN")
    if not tc or not (Path(tc) / "pi32v2" / "bin" / "clang").exists():
        raise SystemExit("JIELI_TOOLCHAIN must point at the JieLi Linux toolchain "
                         "(the directory with pi32v2/ and common/; see tools/get_toolchain.sh)")
    return Path(tc).resolve()


def use_docker():
    native = platform.system() == "Linux" and platform.machine() in ("x86_64", "AMD64")
    return os.environ.get("JIELI_DOCKER", "0" if native else "1") == "1"


def tc_path(rel):
    """a path inside the toolchain, as the tool sees it (in the container: /opt/jieli)"""
    return f"/opt/jieli/{rel}" if use_docker() else str(toolchain() / rel)


MOUNT = SRC.parent                  # the container sees SRC's parent, so apps can build sources from beside SRC


def tc(tool, *args):
    """run a toolchain binary (pi32v2/bin/..., common/bin/...) with cwd SRC; paths relative to SRC"""
    rel = [os.path.relpath(Path(a).resolve(), SRC) if isinstance(a, Path) else a for a in args]
    if tool == "cc":                # the toolchain's cc wrapper needs python3; call clang directly
        tool, rel = "pi32v2/bin/clang", ["-target", "pi32v2", *rel]
    if tool == "ccfpu":
        tool, rel = "pi32v2/bin/clang", ["-target", "pi32v2", "-isystem", tc_path("pi32v2/include"), *rel]
    if tool == "cxx":
        tool, rel = "pi32v2/bin/clang++", ["-target", "pi32v2", "-isystem", tc_path("pi32v2/include"), *rel]
    if use_docker():
        cmd = ["docker", "run", "--rm", "--platform", "linux/amd64", "-v", f"{MOUNT}:/work",
               "-v", f"{toolchain()}:/opt/jieli:ro", "-w", f"/work/{SRC.name}", DOCKER_IMAGE, f"/opt/jieli/{tool}", *rel]
    else:
        cmd = [str(toolchain() / tool), *rel]
    r = subprocess.run(cmd, cwd=SRC, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"build: {tool} failed")
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    return r.stdout


def tc_all(*cmds):
    if not cmds:
        return []
    with ThreadPoolExecutor(min(len(cmds), int(os.environ.get("JOBS", "8")))) as ex:
        return list(ex.map(lambda c: tc(*c), cmds))


def app_dir():
    d = FW / "apps" / APP
    if not (d / "app.c").exists():
        raise SystemExit(f"no app {APP!r}: firmware/apps/{APP}/app.c is missing")
    return d


def cxx_dirs():
    listed = app_dir() / "third_party.txt"
    extra = [ln.strip() for ln in listed.read_text().splitlines() if ln.strip()] if listed.exists() else []
    return [app_dir(), *[SRC / d for d in extra]]


def cxx_sources(target):
    found = [f for d in cxx_dirs() for f in sorted(d.rglob("*.cc"))]
    if not found:
        return []
    return found + [SRC / f for f in CXX_TARGET_ONLY] if target else found


def stale(obj, dep):
    """the object is missing, or older than its source or a header it included (clang -MMD)"""
    if not obj.exists() or not dep.exists():
        return True
    t = obj.stat().st_mtime
    deps = dep.read_text().replace("\\\n", " ").split(":", 1)[-1].split()
    return any(not (SRC / d).exists() or (SRC / d).stat().st_mtime > t for d in deps)


def app_build_txt():
    """firmware/apps/APP/build.txt: `c PATH` (C built with the FPU), `include DIR`, `define NAME[=VALUE]`,
    `small FILE` (one of the app's own *.c, built for size: -Os in place of -O2); paths relative to SRC (they may
    reach beside it: ../Source/...)"""
    f = app_dir() / "build.txt"
    out = {"c": [], "include": [], "define": [], "small": []}
    if not f.exists():
        return out
    for no, ln in enumerate(f.read_text().splitlines(), 1):
        ln = ln.split("#", 1)[0].strip()
        if not ln:
            continue
        kind, _, arg = ln.partition(" ")
        if kind not in out or not arg.strip():
            raise SystemExit(f"{f}:{no}: expected `c PATH`, `include DIR`, `define NAME[=VALUE]` or `small FILE`")
        out[kind].append(arg.strip())
    return out


def app_flags():
    bt = app_build_txt()
    return [*[f"-I{d}" for d in bt["include"]], *[f"-D{d}" for d in bt["define"]]]


def obj_name(src, suffix):
    return os.path.relpath(src, SRC).replace("../", "up_").replace("/", "_").removesuffix(suffix)


def build_cxx():
    objdir = OUT / "cxx" / build_tag()
    objs, cmds = [], []
    for src in cxx_sources(True):
        name = obj_name(src, ".cc")
        obj, dep = objdir / f"{name}.o", objdir / f"{name}.d"
        objs.append(obj)
        if stale(obj, dep):
            cmds.append(("cxx", *CXXFLAGS, *rate_flags(), f"-I{app_dir().relative_to(SRC)}",
                         f"-I{app_gen().relative_to(SRC)}", *app_flags(), "-MMD", "-MF", dep, "-c", src, "-o", obj))
    srcs = [f for f in sorted(app_dir().glob("*.c")) if f.name != "app.c"]
    srcs += [(SRC / p).resolve() for p in app_build_txt()["c"]]
    small = set(app_build_txt()["small"])
    if small - {f.name for f in srcs}:
        raise SystemExit(f"build.txt: small: no {', '.join(sorted(small - {f.name for f in srcs}))} in the app")
    for src in srcs:
        name = obj_name(src, ".c")
        obj, dep = objdir / f"{name}.o", objdir / f"{name}.d"
        objs.append(obj)
        if stale(obj, dep):
            flags = ["-Os" if f == "-O2" and src.name in small else f for f in CFPUFLAGS]
            cmds.append(("ccfpu", *flags, *rate_flags(), f"-I{app_dir().relative_to(SRC)}", *app_flags(),
                         "-MMD", "-MF", dep, "-c", src, "-o", obj))
    objdir.mkdir(parents=True, exist_ok=True)
    if cmds:
        print(f"c++ / fpu c: {len(cmds)} of {len(objs)} files")
    tc_all(*cmds)
    return objs


def app_gen():
    """the app's own generated headers (on the C++ include path)"""
    return GEN / APP


def generate():
    """generated headers (font, tables, the logo)"""
    GEN.mkdir(parents=True, exist_ok=True)
    tools = SRC / "tools"
    cmds = [[tools / "gen_font.py", GEN / "felucca_font.h"],
            [tools / "gen_tables.py", GEN / "fw_tables.h", "48" if FS48 else "44"],
            [tools / "gen_logo.py", GEN / "fw_logo.h", app_dir() / "logo.png", "--preview", GEN / "logo.png"]]
    procs = [subprocess.Popen([sys.executable, *map(str, c)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True) for c in cmds]
    failed = []
    for c, p in zip(cmds, procs):
        sys.stdout.write(p.communicate()[0])
        if p.returncode:
            failed.append(c[0].name)
    if failed:
        raise SystemExit(f"build: {', '.join(failed)} failed")


# ---- update loader

def crc16(d, c=0):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


def ldr_head(dcrc, a, b, attr, name):
    body = struct.pack("<HIIBBH16s", dcrc, a, b, attr, 0, 0, name)
    return struct.pack("<H", crc16(body)) + body


def ldr_wrap(image):
    """ota.bin = outer JLFS head (32 B) + inner head (32 B, load address) +
    blocks [clen u32][dlen u32][LZ4 block], dlen 4096 except the last"""
    blocks = bytearray()
    for i in range(0, len(image), 4096):
        raw = image[i:i + 4096]
        c = lz4blk.compress(raw)
        if len(c) > 4096 or lz4blk.decompress(c) != raw:
            raise SystemExit(f"loader: block {i // 4096} does not compress below 4096 B")
        blocks += struct.pack("<II", len(c), len(raw)) + c
    inner = ldr_head(crc16(image), len(image), LOADER_LOAD, 0, LOADER_NAME)
    body = inner + bytes(blocks)
    return ldr_head(crc16(body), 0x20, len(body), 0x41, LOADER_NAME) + body


def build_loader():
    LDR.mkdir(parents=True, exist_ok=True)
    src = FW / "loader"
    flags = [*CFLAGS, "-Ifirmware/hal", "-Ifirmware/sys"]
    tc_all(("cc", "-c", src / "crt0_ldr.S", "-o", LDR / "crt0_ldr.o"),
           ("cc", *flags, "-c", src / "loader.c", "-o", LDR / "loader.o"))
    elf = LDR / "loader.elf"
    tc("pi32v2/bin/ld", "--gc-sections", "-e", "_start", "-T", src / "loader.ld",
       LDR / "crt0_ldr.o", LDR / "loader.o", "-o", elf)
    _, dis, hdr, syms = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, LDR / "loader.bin"),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-h", elf),
                               ("common/bin/objdump", "-t", elf))
    (LDR / "loader.dis").write_text(dis)
    for ln in hdr.splitlines():     # the loader rewrites the flash: nothing may run from XIP
        p = ln.split()
        if len(p) > 4 and p[1].startswith(".") and int(p[3], 16) >= 0x02000000 and int(p[2], 16):
            raise SystemExit(f"loader: section {p[1]} at {p[3]} is outside RAM")
    image = (LDR / "loader.bin").read_bytes()
    if not image or len(image) > 0x14000:
        raise SystemExit("loader: image empty or too big")
    ota = ldr_wrap(image)
    (LDR / "ota.bin").write_bytes(ota)
    bss = [int(ln.split()[0], 16) for ln in syms.splitlines() if ln.rstrip().endswith("_bss_end")]
    print(f"loader: image {len(image)} B at {LOADER_LOAD:#x}" + (f", bss end {bss[0]:#x}" if bss else ""))
    print(f"loader: ota.bin {len(ota)} B")
    return ota


# ---- app

def build_app():
    flags = [*CFLAGS, *rate_flags(), "-Ifirmware/hal", "-Ifirmware/sys", f"-I{app_dir().relative_to(SRC)}", "-Ibuild/gen"]
    for flag in ("FW_UART", "FW_UBOOT_COMBO", "FELUCCA_OTA_DRYRUN"):
        v = os.environ.get(flag)    # unset: the default in firmware/sys/fw.c
        if v in ("0", "1"):
            flags.append(f"-D{flag}={v}")
    flags += name_flags()
    flags += [f"-D{d}" for d in app_build_txt()["define"]]
    flags.append(f'-DFELUCCA_ID="{PRODUCT}"')
    flags.append("-DFM1_CPU1_USED=1")   # the second core (hal/fm1_cpu1.h): the app only, never the loader
    if VERSION:
        flags.append(f'-DFW_VERSION="{VERSION}"')
    cxx_objs = build_cxx()
    tc_all(("cc", "-c", FW / "crt0.S", "-o", OUT / "crt0.o"),
           ("cc", "-c", FW / "hal" / "fm1_vec.S", "-o", OUT / "fm1_vec.o"),
           ("cc", "-c", FW / "hal" / "fm1_isr.S", "-o", OUT / "fm1_isr.o"),
           ("cc", "-c", FW / "hal" / "fm1_cpu1.S", "-o", OUT / "fm1_cpu1.o"),
           ("cc", *flags, "-c", FW / "sys" / "fw.c", "-o", OUT / "app.o"))
    elf = OUT / "app.elf"
    tc("pi32v2/bin/ld", "-T", FW / "app.ld", OUT / "crt0.o", OUT / "fm1_vec.o", OUT / "fm1_isr.o",
       OUT / "fm1_cpu1.o", OUT / "app.o", *cxx_objs, "--start-group", *[tc_path(f"pi32v2/lib/r3/{lib}") for lib in LIBS], "--end-group",
       "-o", elf)
    headers = tc("common/bin/objdump", "-h", elf)
    for sect in ("text.bin", "data.bin", "ramtext.bin", "c1text.bin"):
        (OUT / sect).unlink(missing_ok=True)
    *_, syms, dis, rt = tc_all(("common/bin/objcopy", "-O", "binary", "-j", ".text", elf, OUT / "text.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".data", elf, OUT / "data.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".ram_text", elf, OUT / "ramtext.bin"),
                               ("common/bin/objcopy", "-O", "binary", "-j", ".c1_text", elf, OUT / "c1text.bin"),
                               ("common/bin/objdump", "-t", elf),
                               ("common/bin/objdump", "-d", elf),
                               ("common/bin/objdump", "-d", "-j", ".ram_text", elf))
    (OUT / "app.dis").write_text(dis)
    (OUT / "app.sym").write_text(syms)
    (OUT / "app.sections").write_text(headers)

    def symv(name):
        return int(re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M).group(1), 16)
    img = bytearray((OUT / "text.bin").read_bytes())
    # .ram_text, .c1_text and .data follow .text at their load addresses; crt0 copies them by words
    for sect, lname in (("ramtext.bin", "_rt_load"), ("c1text.bin", "_c1_load"), ("data.bin", "_data_load")):
        load = symv(lname)
        if load % 4:
            raise SystemExit(f"{lname} {load:#x} is not word aligned")
        blob = (OUT / sect).read_bytes() if (OUT / sect).exists() else b""
        if blob:
            if load - APP_XIP < len(img):
                raise SystemExit(f"{lname} overlaps the image")
            img += b"\xff" * (load - APP_XIP - len(img))
            img += blob
    img += b"\xff" * (-len(img) % 4)
    (OUT / "app.bin").write_bytes(img)
    return bytes(img), syms, dis, rt, headers


def check(img, syms, dis, rt, headers):
    errors, notes = [], []
    for ln in headers.splitlines():       # nothing the startup code does not know about (.init_array, .eh_frame ...)
        p = ln.split()
        if len(p) > 4 and p[0].isdigit() and p[1].startswith(".") and int(p[2], 16) and p[1] not in ELF_SECTIONS:
            errors.append(f"unexpected section {p[1]} ({int(p[2], 16)} B)")
    m = re.search(r"^([0-9a-f]+) .*\s_start$", syms, re.M)
    if not m or int(m.group(1), 16) != APP_XIP:
        errors.append(f"_start is not at {APP_XIP:#x}")
    if img[:4] != bytes.fromhex("04818000"):
        errors.append(f"image starts with {img[:4].hex()}, not the entry stub")
    rt_calls = [ln for ln in rt.splitlines() if re.search(r"\bcall\b", ln)]
    if rt_calls:                    # RAM code runs with the flash off: no calls into XIP
        errors.append(f".ram_text contains calls: {rt_calls[:3]}")
    else:
        notes.append(f".ram_text: {len([ln for ln in rt.splitlines() if LINE.match(ln)])} insns, no calls")
    for ln in dis.splitlines():     # nothing may call or load an address in the chip ROM
        mm = LINE.match(ln)
        if not mm:
            continue
        for v in re.findall(r"= (-?\d+) <|call -?\d+ <[^:>]*: ([0-9a-f]+) >", mm.group(3)):
            val = (int(v[0]) & 0xFFFFFFFF) if v[0] else int(v[1], 16) & 0xFFFFFFFF
            if 0xFFC00000 <= val < 0xFFD00000:
                errors.append(f"reference to ROM address {val:#010x}")
    if len(img) > APP_SLOT:
        errors.append(f"image {len(img)} B exceeds the app slot")
    # C++ runs on the single-precision FPU: a soft-float or soft-double routine means a double crept in, or a
    # unit was built without -mfprev1 (after fm1-amb's check)
    soft = sorted(set(re.findall(r"\s(__(?:add|sub|mul|div|neg|cmp|eq|ne|lt|le|gt|ge|un|extend|trunc|fix|fixuns|float"
                                 r"|floatun|pow)\w*[sd]f\w*)$", syms, re.M)))
    if soft:
        errors.append(f"soft floating-point routines linked: {soft[:6]}")
    else:
        notes.append("no soft floating-point routines (FPU single precision only)")

    def sym(name):
        mm = re.search(r"^([0-9a-f]+) .*\s" + name + r"$", syms, re.M)
        return int(mm.group(1), 16) if mm else 0
    bss = sym("_bss_end") - 0x01C08000
    pool = sym("_pool_end") - sym("_pool_start")
    notes.append(f"image {len(img)} B; RAM .data+.bss {bss} B of 98304; pool {pool} B of {0x54000}")
    if bss > 96 * 1024:
        errors.append("RAM region overflow")
    if 0x54000 - pool < 8192:                     # keep >= 8 KiB of the pool spare
        errors.append(f"pool headroom {0x54000 - pool} B < 8192 B")
    return errors, notes


# Every hardware register access lives in hal/. In sys/, app/ and loader/ (comments stripped):
#  - no volatile pointer cast, except of a C object's address (`*(volatile T *)&x`: a read-once
#    of a RAM flag shared with an ISR, e.g. usb.c ota_wire_send);
#  - no literal in a register or reserved window (core SFRs 0x10000-0x13FFF, SFC 0x40000-0x43FFF,
#    GPIO/IOMAP 0x50000-0x51FFF, CPU 0x1EE0000-0x1EEFFFF, RAM top 0x01C7F000- (boot info,
#    mailbox, vectors), XIP 0x02000000-0x020FFFFF);
#  - no inline asm, except the empty compiler barrier RING_PUBLISH() (emits no instruction);
#  - no HAL register macro (a hal/ #define that is, or expands to, a volatile access).
# Linker symbols (_bss_start[], _rt_load[], ...) are plain C objects and pass.
MMIO_LIT = re.compile(r"\b0x0*(1[0-3][0-9a-f]{3}|4[0-3][0-9a-f]{3}|5[01][0-9a-f]{3}|1ee[0-9a-f]{4}|"
                      r"1c7f[0-9a-f]{3}|20[0-9a-f]{5})u?l?\b", re.I)
MMIO_CAST = re.compile(r"\(\s*(?:const\s+)?volatile\b[^()]*\*\s*\)(?!\s*&)")
MMIO_ASM = re.compile(r"\b(?:__asm__|asm)\b(?!\s+volatile\s*\(\s*\"\"\s*:::\s*\"memory\"\s*\))")


def mmio_check():
    def strip(s):
        s = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
        return re.sub(r"//[^\n]*", "", s)
    defs = {}
    for h in sorted((FW / "hal").glob("*.h")):
        for m in re.finditer(r"^#define\s+(\w+)(?:\([^)]*\))?\s+(.*)$", strip(h.read_text()), re.M):
            defs[m.group(1)] = m.group(2)
    regs = {n for n, b in defs.items() if re.search(r"\bvolatile\b", b)}
    while True:                                   # macros built on register macros (FM1_WR_LIMIT_H -> FM1_X2)
        more = {n for n, b in defs.items() if n not in regs and set(re.findall(r"\w+", b)) & regs}
        if not more:
            break
        regs |= more
    errors = []
    for f in sorted([*(FW / "sys").glob("*.[ch]"), *(FW / "apps").rglob("*.[ch]"), *(FW / "apps").rglob("*.cc"),
                     *(FW / "cxx").glob("*.cc"), *(FW / "loader").glob("*.c")]):
        for no, ln in enumerate(strip(f.read_text()).splitlines(), 1):
            where = f"{f.relative_to(FW)}:{no}"
            for rx, what in ((MMIO_LIT, "register/window address"), (MMIO_CAST, "volatile pointer cast"),
                             (MMIO_ASM, "inline asm")):
                m = rx.search(ln)
                if m:
                    errors.append(f"{where}: {what} outside hal/ ({m.group(0).strip()}); add a hal/ helper")
            used = set(re.findall(r"\b[A-Z_][A-Z0-9_]*\b", ln)) & regs
            if used:
                errors.append(f"{where}: HAL register macro {sorted(used)[0]} outside hal/; use a hal/ helper")
    return errors


def set_app(name):
    global APP
    APP = name
    app_dir()


def set_fs(khz):
    global FS48
    FS48 = khz == "48"


def name_flags():
    """firmware/apps/APP/name.txt: the name on the splash (FW_NAME); none: the default in firmware/sys/fw.c"""
    f = app_dir() / "name.txt"
    name = f.read_text().strip() if f.exists() else ""
    if not re.fullmatch(r"[ -~]{0,24}", name) or '"' in name or "\\" in name:
        raise SystemExit(f"{f}: one line of printable ASCII, at most 24 characters")
    return [f'-DFW_NAME="{name}"'] if name else []


def rate_flags():
    return [f"-DFW_FS48={int(FS48)}"]


def build_tag():
    return APP + ("" if FS48 else "-44k")


def main():
    global PRODUCT, VERSION
    ap = argparse.ArgumentParser()
    ap.add_argument("--release", metavar="X.Y", help="release build: identity FM-1_9XY, version string X.Y")
    ap.add_argument("--sdk", type=Path, help="JieLi AC79 SDK checkout (default: $AC79_SDK, else tools/sdk)")
    ap.add_argument("--app", default=APP, help=f"firmware/apps/APP to build (default: {APP})")
    ap.add_argument("--fs", choices=("44", "48"), default="48" if FS48 else "44",
                    help="codec rate: 48 = 48 kHz (default), 44 = the 44.1 kHz family")
    ap.add_argument("--name", help="package file name without .fwsc (default: the app name, -44k with --fs 44)")
    a = ap.parse_args()
    set_app(a.app)
    set_fs(a.fs)
    name = f"{a.name or build_tag()}.fwsc"
    if a.release:                   # one digit each: the identity has room for two
        m = re.fullmatch(r"(\d)\.(\d)(-[A-Za-z0-9]+)?", a.release)
        if not m:
            raise SystemExit(f"--release {a.release}: use X.Y or X.Y-suffix, one digit each")
        PRODUCT = "FM-1_9" + m[1] + m[2]
        VERSION = a.release.upper()
    fm1pkg_make.SDK = a.sdk
    for rel, sha in SDK_SHA256.items():          # fail early without the SDK
        if hashlib.sha256(fm1pkg_make.sdk_file(rel)).hexdigest() != sha:
            print(f"warning: SDK {rel} differs from AC79NN_SDK_V1.2.1; the package will not match the reference")
    OUT.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(2) as ex:
        gen, ldr = ex.submit(generate), ex.submit(build_loader)
        gen.result()
        ota = ldr.result()
    img, syms, dis, rt, headers = build_app()
    errors, notes = check(img, syms, dis, rt, headers)
    hal_err = mmio_check()
    errors += hal_err
    if not hal_err:
        notes.append("register access: hal/ only (sys/, apps/, loader/ clean)")
    for n in notes:
        print("  ok   ", n)
    for e in errors:
        print("  FAIL ", e)
    if errors:
        raise SystemExit("build: checks failed")
    pkg = fm1pkg_make.ufw(fm1pkg_make.flash_image(img, fm1pkg_make.KEY), ota, PRODUCT)
    (OUT / name).write_bytes(pkg)
    print(f"app      {OUT / 'app.bin'}  {len(img)} B")
    print(f"loader   {LDR / 'ota.bin'}  {len(ota)} B")
    print(f"package  {OUT / name}  {len(pkg)} B, identity {PRODUCT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

