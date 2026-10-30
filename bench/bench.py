# -*- coding: utf-8 -*-
"""Speed, peak memory and installed size of JSON libraries that return dict / list.

    python bench/bench.py [dir that holds the oxjson package]

Writes bench/results/py<XY>-<system>-<machine>.json for the running interpreter. Every measurement
runs in its own process. bench/report.py turns the result files into a table.

speed    one call at a time, GC on, mean of 10 calls, mean of three such rounds
memory   growth of the process peak memory across the first call on a fresh process
disk     bytes of the installed package files
"""
import gc
from functools import lru_cache
import importlib
import json
import os
import platform
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = [
    ("small config, 34 KB", "testdata/engine/atlas.json"),
    ("float array, 150 KB", "testdata/diverse/numbers.json"),
    ("API reply with text, 647 KB", "testdata/diverse/twitter.json"),
    ("catalog, 1.8 MB", "testdata/diverse/citm_catalog.json"),
    ("coordinates, 2.2 MB", "testdata/diverse/canada.json"),
    ("3D mesh, 3.0 MB", "testdata/diverse/marine_ik.json"),
    ("uniform records, 4.9 MB", "testdata/bench/large_50k.json"),
]
# name -> (import name, distribution name)
LIBS = {
    "oxjson": ("oxjson", None),
    "orjson": ("orjson", "orjson"),
    "msgspec": ("msgspec", "msgspec"),
    "ssrjson": ("ssrjson", "ssrjson"),
    "yyjson": ("yyjson", "yyjson"),
    "pysimdjson": ("simdjson", "pysimdjson"),
    "ujson": ("ujson", "ujson"),
    "rapidjson": ("rapidjson", "python-rapidjson"),
    "json": ("json", None),
}


def adapter(name):
    """(loads, dumps or None, version) of one library."""
    mod = importlib.import_module(LIBS[name][0])
    if name == "json":
        ver = "stdlib"
    elif LIBS[name][1]:
        from importlib import metadata
        ver = metadata.version(LIBS[name][1])
    else:
        ver = mod.__version__
    if name == "json":
        return mod.loads, lambda o: mod.dumps(o, separators=(",", ":"), ensure_ascii=False).encode("utf-8"), ver
    if name == "msgspec":
        return mod.json.decode, mod.json.encode, ver
    if name == "ssrjson":
        return mod.loads, getattr(mod, "dumps_to_bytes", mod.dumps), ver
    if name == "yyjson":
        return (lambda d: mod.Document(d).as_obj), (lambda o: mod.Document(o).dumps()), ver
    if name == "pysimdjson":
        return mod.loads, None, ver
    return mod.loads, mod.dumps, ver


@lru_cache(maxsize=1)
def windows_memory_reader():
    """Initialize ctypes once; repeated measurements must not rebuild FFI classes."""
    import ctypes
    from ctypes import wintypes
    class PMC(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
            (n, ctypes.c_size_t) for n in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage", "QuotaNonPagedPoolUsage",
                "PagefileUsage", "PeakPagefileUsage")]
    pmc = PMC()
    pmc.cb = ctypes.sizeof(pmc)
    kernel = ctypes.WinDLL("kernel32")
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    api = ctypes.WinDLL("psapi")
    api.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMC), wintypes.DWORD]
    def read():
        if not api.GetProcessMemoryInfo(kernel.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb):
            raise OSError("GetProcessMemoryInfo failed")
        return pmc
    return read


def peak_memory():
    """Peak private commit on Windows; peak RSS on Linux/macOS, in bytes."""
    if os.name == "nt":
        return windows_memory_reader()().PeakPagefileUsage
    if sys.platform == "darwin":
        import resource
        return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss  # bytes on macOS
    with open("/proc/self/status") as f:
        for line in f:
            if line.startswith("VmHWM:"):
                return int(line.split()[1]) * 1024
    raise RuntimeError("process peak memory unavailable")


def disk_size(name, pkg_dir):
    if name == "oxjson":
        d = Path(pkg_dir) / "oxjson"
        return sum(p.stat().st_size for p in d.iterdir() if p.is_file() and p.suffix in (".pyd", ".so", ".py", ".pyi", ".typed"))
    if name == "json":
        return None
    from importlib import metadata
    total = 0
    for f in metadata.files(LIBS[name][1]) or []:
        p = f.locate()
        if os.path.isfile(p):
            total += os.path.getsize(p)
    return total


def mean10(fn, arg):
    tot = 0.0
    for _ in range(10):
        t = time.perf_counter()
        r = fn(arg)
        tot += time.perf_counter() - t
        del r
    return tot / 10 * 1e6


def worker(name, doc, what, pkg_dir):
    sys.path.insert(0, pkg_dir)
    loads, dumps, ver = adapter(name)
    data = (ROOT / doc).read_bytes()
    out = {"version": ver}
    if what == "speed":
        obj = json.loads(data)
        loads(data)
        out["loads_us"] = sum(mean10(loads, data) for _ in range(3)) / 3
        if dumps:
            dumps(obj)
            out["dumps_us"] = sum(mean10(dumps, obj) for _ in range(3)) / 3
    elif what == "mem_loads":
        gc.collect()
        p0 = peak_memory()
        v = loads(data)
        out["loads_peak_bytes"] = peak_memory() - p0
        del v
    elif what == "mem_dumps":
        obj = json.loads(data)
        if dumps:
            gc.collect()
            p0 = peak_memory()
            v = dumps(obj)
            out["dumps_peak_bytes"] = peak_memory() - p0
            del v
    print(json.dumps(out))


def main():
    pkg_dir = str(Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else str(ROOT)
    res = {"python": platform.python_version(), "machine": platform.machine(), "system": platform.system(),
           "processor": platform.processor(),
           "memory_metric": "peak_private_commit_bytes" if os.name == "nt" else "peak_rss_bytes",
           "memory_summary": "minimum_of_three_hwm_deltas; zero_is_inconclusive", "libs": {}, "docs": [d[0] for d in DOCS]}
    for name in LIBS:
        p = subprocess.run([sys.executable, __file__, "--probe", name, pkg_dir], capture_output=True, text=True, timeout=120)
        if p.returncode != 0:
            raise RuntimeError("library probe failed: %s: %s" % (name, p.stderr))
        info = json.loads(p.stdout.strip().splitlines()[-1])
        if "unavailable" in info:
            print("%-12s not installed: %s" % (name, info["unavailable"]))
            continue
        lib = {"version": info["version"], "disk_bytes": info["disk"], "docs": {}}
        for label, doc in DOCS:
            row = {}
            for what in ("speed", "mem_loads", "mem_dumps"):
                # Historical summary: minimum of three HWM deltas; retain every sample.
                # A zero delta is inconclusive, not zero allocation.
                runs = 1 if what == "speed" else 3
                vals = []
                for _ in range(runs):
                    p = subprocess.run([sys.executable, __file__, "--worker", name, doc, what, pkg_dir],
                                       capture_output=True, text=True, timeout=900)
                    try:
                        if p.returncode != 0:
                            raise RuntimeError("worker exit %s: %s" % (p.returncode, p.stderr[-200:]))
                        vals.append(json.loads(p.stdout.strip().splitlines()[-1]))
                    except Exception:
                        vals.append({"error": (p.stderr or "worker failed or no output")[-200:]})
                row.setdefault("raw_samples", {})[what] = vals
                if any("error" in v for v in vals):
                    row["error"] = next(v["error"] for v in vals if "error" in v)
                    continue
                for k in ("loads_us", "dumps_us", "loads_peak_bytes", "dumps_peak_bytes", "error"):
                    xs = [v[k] for v in vals if k in v]
                    if xs:
                        row[k] = min(xs) if k != "error" else xs[0]
            lib["docs"][label] = row
        res["libs"][name] = lib
        print("%-12s %-10s done" % (name, lib["version"]), flush=True)
    out = ROOT / "bench" / "results"
    out.mkdir(parents=True, exist_ok=True)
    f = out / ("py%d%d-%s-%s.json" % (*sys.version_info[:2], platform.system().lower(), platform.machine().lower()))
    f.write_text(json.dumps(res, indent=1), encoding="utf-8")
    print("wrote", f)
    if any("error" in row for lib in res["libs"].values() for row in lib["docs"].values()):
        raise SystemExit(1)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--worker":
        worker(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
    elif len(sys.argv) > 1 and sys.argv[1] == "--probe":
        sys.path.insert(0, sys.argv[3])
        try:
            _, _, v = adapter(sys.argv[2])
        except ModuleNotFoundError as exc:
            if exc.name != LIBS[sys.argv[2]][0].split(".")[0]:
                raise
            print(json.dumps({"unavailable": str(exc)}))
        else:
            print(json.dumps({"version": v, "disk": disk_size(sys.argv[2], sys.argv[3])}))
    else:
        main()
