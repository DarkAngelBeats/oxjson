# -*- coding: utf-8 -*-
"""Build, test and benchmark on this machine; everything goes to one report file.

    python -m pip install -r requirements-dev.txt
    python tests/run_all.py

Writes report-<system>-<machine>-py<XY>.txt in the repository root and
bench/results/py<XY>-<system>-<machine>.json. Exit code 0 when every step passed.
"""
import os
import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PY = sys.executable


def step(log, title, cmd, env=None):
    print("==", title, flush=True)
    log.write("\n== %s\n$ %s\n" % (title, " ".join(str(c) for c in cmd)))
    try:
        p = subprocess.run([str(c) for c in cmd], cwd=ROOT, capture_output=True, text=True, env=env,
                           encoding="utf-8", errors="replace")
        out, rc = (p.stdout or "") + (p.stderr or ""), p.returncode
    except FileNotFoundError as e:
        out, rc = str(e), 127
    log.write(out)
    log.write("[exit %d]\n" % rc)
    log.flush()
    tail = [l for l in out.strip().splitlines() if l.strip()][-1:] or [""]
    print("   %s  %s" % ("ok  " if rc == 0 else "FAIL", tail[0][:150]), flush=True)
    return rc == 0


def main():
    tag = "%s-%s-py%d%d" % (platform.system().lower(), platform.machine().lower(), *sys.version_info[:2])
    report = ROOT / ("report-%s.txt" % tag)
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    cc = os.environ.get("CC") or ("gcc" if os.name == "nt" else "cc")
    exe = ROOT / "build" / ("vec16_selftest.exe" if os.name == "nt" else "vec16_selftest")
    (ROOT / "build").mkdir(exist_ok=True)
    ok = True
    with open(report, "w", encoding="utf-8") as log:
        log.write("machine: %s %s, %s\npython: %s\n" % (platform.system(), platform.machine(),
                                                         platform.processor() or "?", sys.version))
        ok &= step(log, "vector helpers: compile", [cc, "-O2", "-Iinclude", "tests/vec16_selftest.c", "-o", exe])
        ok &= step(log, "vector helpers: self-test", [exe])
        if not step(log, "build the extension", [PY, "make_ext.py"]):
            print("build failed, see", report)
            return 1
        ok &= step(log, "loads against orjson, leaks", [PY, "tests/check_versions.py"], env)
        ok &= step(log, "dumps against orjson", [PY, "tests/check_dumps_orjson.py"], env)
        ok &= step(log, "loads against json, random data", [PY, "tests/check_loads.py"], env)
        ok &= step(log, "dumps against json, random data", [PY, "tests/check_dumps.py"], env)
        if platform.machine().lower() in ("amd64", "x86_64"):
            ok &= step(log, "assembly path against portable path", [PY, "tests/check_portable.py"], env)
            no_avx = dict(env, OXJSON_NO_AVX2="1")
            ok &= step(log, "portable path: loads against orjson", [PY, "tests/check_versions.py"], no_avx)
            ok &= step(log, "portable path: dumps against orjson", [PY, "tests/check_dumps_orjson.py"], no_avx)
        ok &= step(log, "memory measurement self-tests", [PY, "tests/check_memory_measurements.py"], env)
        ok &= step(log, "memory audit", [PY, "bench/memory_audit.py"], env)
        if platform.machine().lower() in ("amd64", "x86_64"):
            ok &= step(log, "portable memory audit", [PY, "bench/memory_audit.py", "--portable", "--output", "bench/results/memory-audit-portable.json"], env)
        benchmark_ok = step(log, "benchmark", [PY, "bench/bench.py"], env)
        ok &= benchmark_ok
        if benchmark_ok:
            ok &= step(log, "benchmark tables", [PY, "bench/report.py"], env)
    print("\n%s. Report: %s" % ("ALL STEPS PASSED" if ok else "SOME STEPS FAILED", report))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
