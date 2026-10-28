# -*- coding: utf-8 -*-
"""Build the oxjson extension for the interpreter that runs this script.

    python make_ext.py            build in place (oxjson/)
    python make_ext.py <dir>      build a copy of the package in <dir>/oxjson

Windows and Linux on x86-64: needs gcc and nasm; builds the assembly kernel and
the portable C code. Every other target (ARM64 Linux, macOS arm64/x86-64): needs a C compiler
only (cc, or $CC); builds the portable C code.

$OXJSON_CFLAGS replaces the default optimisation flags (-O2), e.g. "-O3 -mcpu=apple-m1".
"""
import os
import platform
import shutil
import subprocess
import sys
import sysconfig
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def run(cmd):
    print(" ".join(str(c) for c in cmd), flush=True)
    r = subprocess.call([str(c) for c in cmd], cwd=ROOT)
    if r:
        raise SystemExit(r)


def main():
    tag = "py%d%d" % sys.version_info[:2]
    out = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT
    obj = ROOT / "build" / "obj" / tag
    pkg = out / "oxjson"
    obj.mkdir(parents=True, exist_ok=True)
    pkg.mkdir(parents=True, exist_ok=True)
    inc = sysconfig.get_config_var("INCLUDEPY")
    win = os.name == "nt"
    mac = sys.platform == "darwin"
    cc = os.environ.get("CC") or ("gcc" if win else "cc")
    cflags = [*(os.environ.get("OXJSON_CFLAGS") or "-O2").split(), "-Wall", "-Iinclude", "-I" + inc]
    ext = sysconfig.get_config_var("EXT_SUFFIX") or (".pyd" if win else ".so")
    if win:
        cflags.append("-DMS_WIN64")
        dll = Path(sys.base_prefix) / ("python%d%d.dll" % sys.version_info[:2])
        link = ["-shared"]
        tail = [dll, "-lkernel32", "-static-libgcc"]
    else:
        cflags += ["-fPIC", "-fvisibility=hidden"]
        # the extension resolves Python symbols from the interpreter that loads it
        link = ["-bundle", "-undefined", "dynamic_lookup"] if mac else ["-shared"]
        tail = ["-lm"]
        if mac:
            # cibuildwheel cross-compiles Intel wheels on Apple Silicon: ARCHFLAGS="-arch x86_64"
            arch = (os.environ.get("ARCHFLAGS") or "").split()
            cflags += arch
            link += arch
    srcs = ["src/json_module.c", "src/jl_portable.c", "src/json_dumps_portable.c", "src/json_crt.c"]
    x86 = platform.machine().lower() in ("amd64", "x86_64")
    if x86 and not mac:
        # the AVX2 build next to the portable one: NASM kernel, vector sink, vector dumps
        fmt, kobj = ("win64", obj / "jl_kernel.obj") if win else ("elf64", obj / "jl_kernel.o")
        run(["nasm", "-f", fmt, "-I", "include", "-o", kobj, "src/jl_kernel.asm"])
        avx = ["-mavx2", "-mbmi"]
        if win:
            # MinGW does not keep the stack 32-byte aligned; never emit an aligned 256-bit move
            avx.append("-Wa,-muse-unaligned-vector-move")
        walk = obj / "json_dumps_walk.o"
        run([cc, *cflags, "-c", *avx, "src/json_dumps_walk.c", "-o", walk])
        loads = obj / "jl_loads.o"
        run([cc, *cflags, "-c", *avx[2:], "src/jl_loads.c", "-o", loads])
        cflags.append("-DJSON_AVX2_BUILD")
        srcs += [loads, walk, kobj]
    run([cc, *cflags, *link, *srcs, "-o", pkg / ("oxjson" + ext), *tail])
    if pkg != ROOT / "oxjson":
        for f in ("__init__.py", "__init__.pyi", "oxjson.pyi", "py.typed"):
            shutil.copy2(ROOT / "oxjson" / f, pkg / f)
    print("built", pkg)


if __name__ == "__main__":
    main()
