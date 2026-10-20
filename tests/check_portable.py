# -*- coding: utf-8 -*-
"""Compare the AVX2 path and the portable C path of one build.

    python tests/check_portable.py [dir that holds the oxjson package]

Runs itself twice (OXJSON_NO_AVX2 unset and set to 1) and compares a digest
of every loads result and every dumps output over the test files and random data.
"""
import hashlib
import json
import os
import random
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def worker(pkg):
    sys.path.insert(0, pkg)
    import oxjson
    h = hashlib.sha256()
    n = 0
    fs = sorted(ROOT.glob("testdata/**/*.json"))
    for f in fs:
        d = f.read_bytes()
        try:
            v = oxjson.loads(d)
        except ValueError as e:
            h.update(b"E" + str(getattr(e, "pos", "")).encode())
            continue
        try:
            out = oxjson.dumps(v)
        except Exception as e:
            out = type(e).__name__.encode()
        h.update(out)
        n += 1
    r = random.Random(5)
    for i in range(400000):
        k = r.random()
        if k < 0.6:
            x = struct.unpack("<d", struct.pack("<Q", r.getrandbits(64)))[0]
            if x != x or x in (float("inf"), float("-inf")):
                continue
        elif k < 0.8:
            x = r.uniform(-1e6, 1e6)
        elif k < 0.9:
            x = round(r.uniform(-1e4, 1e4), r.randint(0, 6))
        else:
            x = float(r.randint(-10 ** 17, 10 ** 17))
        out = oxjson.dumps(x)
        if oxjson.loads(out) != x:
            h.update(b"ROUNDTRIP")
        h.update(out)
    for i in range(20000):
        s = "".join(r.choice(["a", "é", "中", "😀", '"', "\\", "\n", "\x01", "/", " ", "z" * 40, "\x7f"]) for _ in range(r.randint(0, 90)))
        o = {s[:7]: [s, i, -i, 2 ** 63 + i, None, True, {"k" * (i % 40): s}]}
        out = oxjson.dumps(o)
        h.update(out)
        h.update(oxjson.dumps(oxjson.loads(out)))
        h.update(oxjson.dumps(oxjson.loads(json.dumps(o))))
    print(n, h.hexdigest())


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--worker":
        worker(sys.argv[2])
    else:
        pkg = str(Path(sys.argv[1]).resolve()) if len(sys.argv) > 1 else str(ROOT)
        res = []
        for flag in (None, "1"):
            env = dict(os.environ)
            env.pop("OXJSON_NO_AVX2", None)
            if flag:
                env["OXJSON_NO_AVX2"] = flag
            p = subprocess.run([sys.executable, __file__, "--worker", pkg], capture_output=True, text=True, env=env)
            res.append(p.stdout.strip() or ("crash %s %s" % (p.returncode, p.stderr[-300:])))
        print("AVX2:    ", res[0])
        print("portable:", res[1])
        print("RESULT", "OK" if res[0] == res[1] and "crash" not in res[0] else "FAIL")
        sys.exit(0 if res[0] == res[1] else 1)
