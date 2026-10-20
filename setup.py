# -*- coding: utf-8 -*-
"""PEP 517 shim: compile oxjson via make_ext.py (gcc + nasm on x86-64)."""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

from setuptools import Extension, setup
from setuptools.command.build_ext import build_ext

ROOT = Path(__file__).resolve().parent


class BuildOxjson(build_ext):
    def build_extensions(self) -> None:
        out = Path(self.build_lib).resolve()
        out.mkdir(parents=True, exist_ok=True)
        cmd = [sys.executable, str(ROOT / "make_ext.py"), str(out)]
        print(" ".join(cmd), flush=True)
        subprocess.check_call(cmd, cwd=ROOT)

        # setuptools expects the extension at get_ext_fullpath; make_ext.py already
        # wrote oxjson/oxjson.<ext> under build_lib. Copy if the names differ.
        for ext in self.extensions:
            built_dir = out / "oxjson"
            candidates = sorted(
                list(built_dir.glob("oxjson*.pyd")) + list(built_dir.glob("oxjson*.so"))
            )
            if not candidates:
                raise RuntimeError("make_ext.py produced no oxjson extension in %s" % built_dir)
            target = Path(self.get_ext_fullpath(ext.name))
            target.parent.mkdir(parents=True, exist_ok=True)
            src = candidates[0]
            if src.resolve() != target.resolve():
                shutil.copy2(src, target)
                src.unlink()


setup(
    ext_modules=[
        Extension(
            "oxjson.oxjson",
            sources=[
                "src/json_module.c",
                "src/jl_portable.c",
                "src/json_dumps_portable.c",
                "src/json_crt.c",
                "src/jl_loads.c",
                "src/json_dumps_walk.c",
                "src/jl_kernel.asm",
            ],
            include_dirs=["include"],
        )
    ],
    cmdclass={"build_ext": BuildOxjson},
)
