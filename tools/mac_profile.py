# -*- coding: utf-8 -*-
"""Why is loads slower on Apple Silicon? Build variants, time them, sample the hot spots.

    python tools/mac_profile.py [out dir]         (default build/mac-profile)
    python tools/mac_profile.py --resign ...      CI only: ad-hoc re-sign the interpreter
                                                  if `sample` cannot attach to it

1. builds the portable extension with several compilers / flags (make_ext.py, $CC, $OXJSON_CFLAGS)
2. times loads on the benchmark documents: every variant and orjson in fresh processes,
   interleaved over several rounds; the table keeps the best round, the spread shows the noise
3. samples a -O2 -g build and orjson with macOS `sample` on large_50k and canada
4. writes report.md (tables + top-of-stack profile) and results.json to the out dir
"""
import json
import os
import platform
import random
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOCS = [
    ("small config", "testdata/engine/atlas.json"),
    ("float array", "testdata/diverse/numbers.json"),
    ("text", "testdata/diverse/twitter.json"),
    ("catalog", "testdata/diverse/citm_catalog.json"),
    ("coordinates", "testdata/diverse/canada.json"),
    ("3D mesh", "testdata/diverse/marine_ik.json"),
    ("uniform records", "testdata/bench/large_50k.json"),
]
PROFILE_DOCS = ["uniform records", "coordinates"]
ROUNDS = 5
SAMPLE_SECONDS = 6


def variants():
    native = "-mcpu=apple-m1" if sys.platform == "darwin" else "-mcpu=native"
    v = [
        ("clang -O2 (as shipped)", "cc", "-O2"),
        ("clang -O3", "cc", "-O3"),
        ("clang -O2 %s" % native, "cc", "-O2 " + native),
        ("clang -O3 %s" % native, "cc", "-O3 " + native),
        ("clang -O3 %s -flto" % native, "cc", "-O3 -flto " + native),
    ]
    # Apple's gcc is clang; Homebrew's real GCC is gcc-NN
    for n in range(16, 11, -1):
        gcc = shutil.which("gcc-%d" % n)
        if gcc:
            v += [("gcc-%d -O2" % n, gcc, "-O2"), ("gcc-%d -O3 -mcpu=native" % n, gcc, "-O3 -mcpu=native")]
            break
    return v


def slug(name):
    return re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_")


def build(name, cc, flags, out):
    d = out / "variants" / slug(name)
    env = dict(os.environ, CC=cc, OXJSON_CFLAGS=flags)
    r = subprocess.run([sys.executable, str(ROOT / "make_ext.py"), str(d)], cwd=ROOT, env=env,
                       capture_output=True, text=True)
    if r.returncode:
        print("build failed:", name, "\n", r.stdout[-2000:], r.stderr[-2000:], flush=True)
        return None
    return d


# ------------------------------------------------------------------ timing


def bench_worker(pkg):
    if pkg == "orjson":
        import orjson as m
    else:
        sys.path.insert(0, pkg)
        import oxjson as m
        assert Path(m.__file__).resolve().is_relative_to(Path(pkg).resolve()), m.__file__
    loads = m.loads
    res = {}
    for label, f in DOCS:
        data = (ROOT / f).read_bytes()
        loads(data)
        n = max(1, int(150_000 / max(1, len(data) // 64)))  # roughly 0.1-0.3 s per repeat
        best = float("inf")
        for _ in range(5):
            t = time.perf_counter()
            for _ in range(n):
                loads(data)
            best = min(best, (time.perf_counter() - t) / n)
        res[label] = best * 1e6
    print(json.dumps(res))


def run_bench(entries):
    """entries: [(name, pkg)]; returns {name: {doc: [us per round]}}"""
    out = {name: {label: [] for label, _ in DOCS} for name, _ in entries}
    for rnd in range(ROUNDS):
        order = entries[:]
        random.shuffle(order)
        for name, pkg in order:
            r = subprocess.run([sys.executable, __file__, "--bench-worker", pkg],
                               capture_output=True, text=True, cwd=ROOT)
            if r.returncode:
                print("bench failed:", name, r.stderr[-2000:], flush=True)
                continue
            for label, us in json.loads(r.stdout.strip().splitlines()[-1]).items():
                out[name][label].append(us)
        print("round %d/%d done" % (rnd + 1, ROUNDS), flush=True)
    return out


# ------------------------------------------------------------------ sampling

LOOP = r"""
import sys, time
pkg, path = sys.argv[1], sys.argv[2]
if pkg == "orjson":
    import orjson as m
else:
    sys.path.insert(0, pkg)
    import oxjson as m
data = open(path, "rb").read()
loads = m.loads
for _ in range(20):
    loads(data)
print("ready", flush=True)
t = time.time()
while time.time() - t < %d:
    loads(data)
""" % (SAMPLE_SECONDS + 6)


def exe_of(pid):
    r = subprocess.run(["ps", "-o", "comm=", "-p", str(pid)], capture_output=True, text=True)
    return r.stdout.strip()


def sample_once(pkg, path, dest, sudo):
    p = subprocess.Popen([sys.executable, "-c", LOOP, pkg, str(path)], stdout=subprocess.PIPE, text=True)
    try:
        p.stdout.readline()
        cmd = (["sudo", "-n"] if sudo else []) + ["sample", str(p.pid), str(SAMPLE_SECONDS), "1", "-mayDie",
                                                  "-file", str(dest)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        ok = r.returncode == 0 and dest.exists() and dest.stat().st_size > 0
        return ok, exe_of(p.pid), (r.stdout + r.stderr)[-1500:]
    finally:
        p.kill()
        p.wait()


def sample(pkg, path, dest, resign):
    if not shutil.which("sample"):
        return "no `sample` tool (not macOS?)"
    log = []
    for sudo in (False, True):
        ok, exe, msg = sample_once(pkg, path, dest, sudo)
        if ok:
            return None
        log.append(("sudo " if sudo else "") + "sample: " + msg)
    if resign and exe:
        # the python.org build has the hardened runtime, which blocks task_for_pid; CI VMs are disposable
        r = subprocess.run(["sudo", "-n", "codesign", "--force", "--sign", "-", exe], capture_output=True, text=True)
        log.append("re-signed %s: %s" % (exe, (r.stdout + r.stderr).strip() or "ok"))
        ok, _, msg = sample_once(pkg, path, dest, True)
        if ok:
            return None
        log.append("after re-sign: " + msg)
    return "\n".join(log)


# "sym  (in image)  [+ off | load address ... [addr]]  count"; stripped images (orjson) print ??? and an address
TOP = re.compile(r"^\s+(.+?)\s+\(in (.+?)\)\s.*?(\d+)\s*$")


def top_of_stack(path):
    """[(symbol, image, samples)] from sample's 'Sort by top of stack' section"""
    rows, on = [], False
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("Sort by top of stack"):
            on = True
            continue
        if on:
            if not line.strip():
                if rows:
                    break
                continue
            m = TOP.match(line)
            if m:
                rows.append((m.group(1), m.group(2), int(m.group(3))))
    return rows


# ------------------------------------------------------------------ report


def env_info():
    def sh(*c):
        try:
            return subprocess.run(c, capture_output=True, text=True).stdout.strip()
        except OSError:
            return ""
    import orjson
    lines = [
        "machine: %s %s, %s" % (platform.system(), platform.machine(),
                                sh("sysctl", "-n", "machdep.cpu.brand_string") or platform.processor()),
        "os: %s" % (sh("sw_vers", "-productVersion") or platform.release()),
        "python: %s" % sys.version.split()[0],
        "orjson: %s" % orjson.__version__,
        "cc: %s" % (sh("cc", "--version").splitlines() or ["?"])[0],
    ]
    return lines


def fmt_table(names, data, base):
    labels = [l for l, _ in DOCS]
    out = ["| variant | " + " | ".join(labels) + " |", "|---|" + "---:|" * len(labels)]
    for n in names:
        cells = []
        for l in labels:
            xs = data[n][l]
            if not xs:
                cells.append("-")
                continue
            b = min(xs)
            ref = min(data[base][l]) if data[base][l] else None
            spread = (max(xs) - b) / b * 100
            rel = " (%.2fx)" % (b / ref) if ref and n != base else ""
            cells.append("%.0f%s ±%.0f%%" % (b, rel, spread))
        out.append("| %s | %s |" % (n, " | ".join(cells)))
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    resign = "--resign" in sys.argv
    out = Path(args[0]).resolve() if args else ROOT / "build" / "mac-profile"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    rep = ["# loads on %s: variants and profile" % platform.machine(), ""] + ["    " + l for l in env_info()] + [""]

    built = []
    for name, cc, flags in variants():
        d = build(name, cc, flags, out)
        built.append((name, str(d)) if d else None)
        if not d:
            rep.append("- build failed: `%s` (%s %s)" % (name, cc, flags))
    entries = [e for e in built if e]
    base = entries[0][0]
    # the same binary twice: how far apart identical code lands on this machine
    entries.insert(1, (base + " again", entries[0][1]))
    entries.append(("orjson", "orjson"))
    data = run_bench(entries)

    names = [n for n, _ in entries]
    rep += ["", "## loads, µs (best of %d rounds; ±spread across rounds; (x) vs %s)" % (ROUNDS, base), ""]
    rep += fmt_table(names, data, base)
    rep += ["", "## the same, relative to orjson", ""]
    rep += fmt_table(names, data, "orjson")

    prof = build("profile -O2 -g", "cc", "-O2 -g -fno-omit-frame-pointer", out)
    profiles = {}
    rep += ["", "## where the time goes (macOS `sample`, top of stack, %d s at 1 ms)" % SAMPLE_SECONDS]
    for label in PROFILE_DOCS:
        path = ROOT / dict(DOCS)[label]
        for who, pkg in (("oxjson -O2 -g", str(prof) if prof else None), ("orjson", "orjson")):
            if not pkg:
                continue
            dest = out / ("sample-%s-%s.txt" % (slug(label), slug(who)))
            err = sample(pkg, path, dest, resign)
            rep += ["", "### %s: %s" % (label, who), ""]
            if err:
                rep += ["sampling failed:", "", "```", err, "```"]
                continue
            rows = top_of_stack(dest)
            total = sum(c for _, _, c in rows) or 1
            by_img = {}
            for _, img, c in rows:
                by_img[img] = by_img.get(img, 0) + c
            rep.append("by image: " + ", ".join("%s %.0f%%" % (i, c * 100 / total)
                                                 for i, c in sorted(by_img.items(), key=lambda x: -x[1])))
            rep += ["", "| % | samples | symbol | image |", "|---:|---:|---|---|"]
            for sym, img, c in rows[:30]:
                rep.append("| %.1f | %d | `%s` | %s |" % (c * 100 / total, c, sym, img))
            profiles["%s / %s" % (label, who)] = rows
    (out / "results.json").write_text(json.dumps({"bench": data, "profiles": profiles}, indent=1))
    (out / "report.md").write_text("\n".join(rep) + "\n", encoding="utf-8")
    print("\n".join(rep))
    print("\nwrote", out / "report.md")


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--bench-worker":
        bench_worker(sys.argv[2])
    else:
        main()
