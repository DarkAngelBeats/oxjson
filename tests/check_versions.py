# -*- coding: utf-8 -*-
"""Check one build of oxjson against orjson under the running interpreter.

    python tests/check_versions.py [dir that holds the oxjson package]

Prints one summary line per part: files, random documents, memory, speed.
"""
import gc
import json
import os
import random
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PKG = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT
sys.path.insert(0, str(PKG))
import oxjson  # noqa: E402
import orjson  # noqa: E402

assert Path(oxjson.__file__).resolve().parent.parent == PKG, oxjson.__file__
DIVERSE = ROOT / "testdata" / "diverse"


def canon(v):
    out = []
    st = [v]
    while st:
        x = st.pop()
        if x is None or x is True or x is False:
            out.append(repr(x))
        elif isinstance(x, int):
            out.append("i%d" % x)
        elif isinstance(x, float):
            out.append("f" + struct.pack("<d", x).hex())
        elif isinstance(x, str):
            out.append("s" + x.encode("utf-8", "surrogatepass").hex())
        elif isinstance(x, list):
            out.append("[%d" % len(x))
            st.extend(reversed(x))
        elif isinstance(x, dict):
            out.append("{%d" % len(x))
            for k in reversed(list(x)):
                st.append(x[k])
                st.append(k)
    return "|".join(out)


def both(data):
    r = []
    for m in (oxjson, orjson):
        try:
            r.append(("ok", canon(m.loads(data))))
        except Exception:
            r.append(("err",))
    return r


def files():
    return sorted(ROOT.glob("testdata/**/*.json"))


def part_files():
    n = bad = known = 0
    for f in files():
        d = f.read_bytes()
        a, b = both(d)
        n += 1
        if a != b and f.name.startswith("deep_") and a[0] == "ok" and b[0] == "err":
            known += 1  # orjson stops at 1024 levels, oxjson has no limit
            continue
        if a != b:
            bad += 1
            if bad <= 5:
                print("   loads differs:", f.name)
            continue
        if a[0] == "ok":
            v = orjson.loads(d)
            try:
                if canon(oxjson.loads(oxjson.dumps(v))) != a[1]:
                    bad += 1
                    print("   dumps round trip differs:", f.name)
            except Exception as e:
                # 500+ nested arrays: orjson refuses too, so only report a crash of ours
                try:
                    orjson.dumps(v)
                    bad += 1
                    print("   dumps raised:", f.name, e)
                except Exception:
                    pass
    print("files: %d checked, %d differ from orjson (%d deeper than orjson's 1024-level limit)" % (n, bad, known))
    return bad


def rnd_doc(r, depth=0):
    k = r.random()
    if depth > 4 or k < 0.45:
        c = r.randint(0, 7)
        if c == 0:
            return str(r.randint(-10 ** r.randint(1, 21), 10 ** r.randint(1, 21)))
        if c == 1:
            return repr(r.uniform(-1e6, 1e6))
        if c == 2:
            return "%d.%se%+d" % (r.randint(0, 99), "".join(r.choice("0123456789") for _ in range(r.randint(1, 22))), r.randint(-320, 305))
        if c == 3:
            return r.choice(["true", "false", "null", "NaN", "Infinity", "-0", "01", "1.", ""])
        s = "".join(r.choice(["a", "b", "é", "中", "😀", '"', "\\", "\n", "\x01", "/", " ", "z" * 33, "\ud800"]) for _ in range(r.randint(0, 40)))
        return json.dumps(s, ensure_ascii=r.random() < 0.4)
    w = lambda: r.choice(["", " ", "\n", "\t "])
    if k < 0.75:
        return "[" + w() + ("," + w()).join(rnd_doc(r, depth + 1) for _ in range(r.randint(0, 6))) + w() + "]"
    return "{" + w() + ("," + w()).join(json.dumps("k%d" % r.randint(0, 50)) + w() + ":" + w() + rnd_doc(r, depth + 1) for _ in range(r.randint(0, 6))) + w() + "}"


def part_random(n=30000):
    r = random.Random(11)
    bad = 0
    for i in range(n):
        doc = rnd_doc(r)
        if r.random() < 0.3 and len(doc) > 1:
            k = r.randrange(len(doc))
            doc = doc[:k] + r.choice(["", ",", "]", "}", '"', "\\", "x", "{", ":", "1"]) + doc[k + 1:]
        data = doc.encode("utf-8", "surrogatepass")
        a, b = both(data)
        if a != b:
            bad += 1
            if bad <= 5:
                print("   differs:", ascii(doc[:120]), a[0], b[0])
    print("random documents: %d checked, %d differ from orjson" % (n, bad))
    return bad


class Odd:
    pass


def private_bytes():
    """Private commit on Windows, RSS on macOS, data segment on Linux; or None."""
    try:
        if os.name == "nt":
            import ctypes
            from ctypes import wintypes

            class PMC(ctypes.Structure):
                _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
                    (n, ctypes.c_size_t) for n in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                                                   "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                                                   "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]
            pmc = PMC()
            pmc.cb = ctypes.sizeof(PMC)
            k32 = ctypes.WinDLL("kernel32")
            k32.GetCurrentProcess.restype = wintypes.HANDLE
            psapi = ctypes.WinDLL("psapi")
            psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(PMC), wintypes.DWORD]
            if not psapi.GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb):
                return None
            return pmc.PagefileUsage
        if sys.platform == "darwin":
            import subprocess
            return int(subprocess.check_output(["ps", "-o", "rss=", "-p", str(os.getpid())]).strip()) * 1024
        with open("/proc/self/statm") as f:
            return int(f.read().split()[5]) * os.sysconf("SC_PAGE_SIZE")
    except Exception:
        return None


def part_memory():
    """Allocated blocks must not grow when the same calls repeat."""
    big = {"k%d" % i: [i, i * 1.5, "v%d" % i, None, True, {"n": [1, 2, 3], "s": "é" * 40}] for i in range(300)}
    text = json.dumps(big).encode()
    deep = b"[" * 600 + b"]" * 600
    bad_docs = [text[:k] for k in range(1, len(text), 97)] + [b'["\xff"]', b'{"a":NaN}', b"[1,]", b"\xef\xbb\xbf{}", b'["\\ud800"]', b'{"a":1e999}']
    cyc = []
    cyc.append(cyc)

    def loads_ok():
        oxjson.loads(text)
        oxjson.loads(text.decode())
        oxjson.loads(bytearray(text))
        oxjson.loads(memoryview(text))
        oxjson.loads(deep)

    def loads_err():
        for d in bad_docs:
            try:
                oxjson.loads(d)
            except ValueError:
                pass

    def dumps_ok():
        oxjson.dumps(big)
        oxjson.dumps([2 ** 64 - 1, -2 ** 63, 1e300, "x" * 100000])
        oxjson.dumps({"a": Odd()}, default=lambda o: {"odd": 1})

    def dumps_err():
        for o in ({"a": Odd()}, {1: 2}, [2 ** 64], cyc, {"a": [1, 2, {"b": Odd()}]}, float("nan")):
            try:
                oxjson.dumps(o, allow_nan=False) if isinstance(o, float) else oxjson.dumps(o)
            except (TypeError, ValueError, RecursionError):
                pass
        try:
            oxjson.dumps({"a": Odd()}, default=lambda o: 1 / 0)
        except ZeroDivisionError:
            pass

    bad = 0
    for name, fn, reps in (("loads", loads_ok, 300), ("loads errors", loads_err, 60), ("dumps", dumps_ok, 300), ("dumps errors", dumps_err, 3000)):
        for _ in range(20):
            fn()
        gc.collect()
        # five rounds: caches and freelists still warm up in the first ones on slow runners
        # (e.g. [183, 42, 21, ...]); a real leak grows by the same amount every round
        grow = []
        for _ in range(5):
            b0 = sys.getallocatedblocks()
            for _ in range(reps):
                fn()
            gc.collect()
            grow.append(sys.getallocatedblocks() - b0)
        leak = min(grow) > 20
        bad += leak
        print("   %-13s blocks after %d repeats, five rounds: %s%s" % (name, reps, grow, "  GROWTH FLAG" if leak else ""))
    # memory outside Python objects (malloc buffers): the whole run again must not add megabytes
    for fn, reps in ((loads_ok, 300), (loads_err, 60), (dumps_ok, 300), (dumps_err, 3000)):
        for _ in range(reps):
            fn()
    p1 = private_bytes()
    for _ in range(3):
        for fn, reps in ((loads_ok, 300), (loads_err, 60), (dumps_ok, 300), (dumps_err, 3000)):
            for _ in range(reps):
                fn()
    p2 = private_bytes()
    if p1 is not None and p2 is not None:
        grown = (p2 - p1) / 1048576.0
        if grown > 8:
            bad += 1
        print("   process memory: %+.1f MiB over three more full rounds%s" % (grown, "  GROWTH FLAG" if grown > 8 else ""))
    else:
        print("   process memory: UNAVAILABLE (native-memory check not performed)")
        bad += 1
    rc = sys.getrefcount(text)
    for _ in range(1000):
        oxjson.loads(text)
    if sys.getrefcount(text) != rc:
        bad += 1
        print("   input refcount changed")
    print("memory: %d failed checks (4 block groups, process growth, input refcount)" % bad)
    return bad


def part_speed():
    docs = [("small config", ROOT / "testdata/engine/atlas.json"), ("float array", DIVERSE / "numbers.json"),
            ("text with Unicode", DIVERSE / "twitter.json"), ("coordinates", DIVERSE / "canada.json"),
            ("uniform records", ROOT / "testdata/bench/large_50k.json")]

    def mean10(fn, a):
        tot = 0.0
        for _ in range(10):
            t = time.perf_counter()
            x = fn(a)
            tot += time.perf_counter() - t
            del x
        return tot / 10 * 1e6

    print("   %-18s loads us ours / orjson    dumps us ours / orjson" % "document")
    for name, p in docs:
        if not p.exists():
            continue
        d = p.read_bytes()
        o = orjson.loads(d)
        for m in (oxjson, orjson):
            m.loads(d)
            m.dumps(o)
        res = [sum(mean10(fn, a) for _ in range(3)) / 3 for fn, a in
               ((oxjson.loads, d), (orjson.loads, d), (oxjson.dumps, o), (orjson.dumps, o))]
        print("   %-18s %10.1f / %-10.1f %10.1f / %-10.1f" % (name, *res))


if __name__ == "__main__":
    print("Python %s, oxjson %s, orjson %s" % (sys.version.split()[0], oxjson.__version__, orjson.__version__))
    bad = part_files() + part_random() + part_memory()
    part_speed()
    print("RESULT", "FAIL" if bad else "OK")
    sys.exit(1 if bad else 0)
