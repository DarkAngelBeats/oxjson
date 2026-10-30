# -*- coding: utf-8 -*-
"""Conformance run of oxjson and orjson (stdlib json as reference) over
public JSON test suites. The suites are data only; nothing from them is executed.

    git clone --depth 1 https://github.com/nst/JSONTestSuite            <dir>/JSONTestSuite
    git clone --depth 1 https://github.com/miloyip/nativejson-benchmark <dir>/nativejson
    git clone --depth 1 https://github.com/nlohmann/json_test_data      <dir>/nlohmann

    python tests/check_conformance.py <dir> [report.json]

Each library runs in its own process, so a crash is recorded for the file that
caused it and the run continues with the next file.
"""
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIBS = ("json", "oxjson", "orjson")


def cases(base):
    """(suite, path, expect) with expect in 'y', 'n', 'i'."""
    out = []

    def add(suite, d, rule, pattern="*.json"):
        d = base / d
        if not d.is_dir():
            return
        for p in sorted(d.rglob(pattern)):
            if p.is_file():
                out.append((suite, str(p), rule(p.name)))

    def prefix(name):
        return name[0] if name[:2] in ("y_", "n_", "i_") else "i"

    def checker(name):
        if name.startswith("pass"):
            return "y"
        # fail01: a scalar root is valid since RFC 7159. fail18: depth 20 is a limit, not grammar.
        # nativejson-benchmark marks both with _EXCLUDE.
        if name.startswith("fail"):
            return "i" if name in ("fail1.json", "fail01.json", "fail18.json") or "EXCLUDE" in name else "n"
        return "i"

    def yes(name):
        return "y"

    def info(name):
        return "i"

    add("nst/test_parsing", "JSONTestSuite/test_parsing", prefix)
    add("nst/test_transform", "JSONTestSuite/test_transform", info)
    add("nst2/test_parsing", "nlohmann/nst_json_testsuite2/test_parsing", prefix)
    add("nativejson/jsonchecker", "nativejson/data/jsonchecker", checker)
    add("nativejson/roundtrip", "nativejson/data/roundtrip", yes)
    add("nlohmann/json_tests", "nlohmann/json_tests", checker)
    add("nlohmann/json_roundtrip", "nlohmann/json_roundtrip", yes)
    add("nlohmann/json.org", "nlohmann/json.org", yes)
    add("nlohmann/json_testsuite", "nlohmann/json_testsuite", yes)
    add("nlohmann/json_nlohmann_tests", "nlohmann/json_nlohmann_tests", info)
    add("nlohmann/regression", "nlohmann/regression", info)
    add("nlohmann/naughty_strings", "nlohmann/big-list-of-naughty-strings", yes)
    return out


def canon(v):
    """Type-exact text of a value: int and float differ, floats by bits."""
    out = []
    stack = [v]
    while stack:
        x = stack.pop()
        if isinstance(x, str) and x.startswith("\x00tok"):
            out.append(x[4:])
        elif x is None or x is True or x is False:
            out.append(repr(x))
        elif isinstance(x, int):
            out.append("i%d" % x)
        elif isinstance(x, float):
            out.append("f" + (x.hex() if x == x else "nan"))
        elif isinstance(x, str):
            out.append("s%d:" % len(x))
            out.append(x.encode("utf-8", "surrogatepass").hex())
        elif isinstance(x, (list, tuple)):
            out.append("[%d" % len(x))
            stack.extend(reversed(x))
        elif isinstance(x, dict):
            out.append("{%d" % len(x))
            for k in sorted(x, key=lambda s: s.encode("utf-8", "surrogatepass"), reverse=True):
                stack.append(x[k])
                stack.append(k)
        else:
            out.append("?" + type(x).__name__)
    return hashlib.sha1("|".join(out).encode()).hexdigest()


def worker(lib, listfile, outfile, start):
    sys.setrecursionlimit(1000000)
    sys.path.insert(0, str(ROOT))
    mod = __import__(lib)
    if lib == "json":
        dumps = lambda o: json.dumps(o, separators=(",", ":"), ensure_ascii=False).encode("utf-8", "surrogatepass")
    else:
        dumps = mod.dumps
    paths = json.load(open(listfile, encoding="utf-8"))
    with open(outfile, "a", encoding="utf-8") as f:
        for i in range(start, len(paths)):
            f.write("START %d\n" % i)
            f.flush()
            data = open(paths[i], "rb").read()
            r = {"i": i}
            try:
                v = mod.loads(data)
                r["ok"] = True
            except RecursionError:
                r["ok"] = False
                r["err"] = "RecursionError"
            except Exception as e:
                r["ok"] = False
                r["err"] = "%s: %s" % (type(e).__name__, str(e)[:120])
            if r["ok"]:
                try:
                    r["val"] = canon(v)
                    r["repr"] = repr(v)[:160]
                except Exception as e:
                    r["val"] = "canon-fail " + type(e).__name__
                try:
                    out = dumps(v)
                    r["exact"] = out == data.strip()
                    r["dump"] = out[:160].decode("utf-8", "replace")
                    r["back"] = canon(mod.loads(out)) == r["val"]
                except RecursionError:
                    r["dumperr"] = "RecursionError"
                except Exception as e:
                    r["dumperr"] = "%s: %s" % (type(e).__name__, str(e)[:120])
            f.write("R " + json.dumps(r) + "\n")
            f.flush()
        f.write("DONE\n")


def run_lib(lib, listfile, n, tmp):
    outfile = os.path.join(tmp, "out_%s.txt" % lib)
    if os.path.exists(outfile):
        os.remove(outfile)
    res = {}
    start = 0
    while start < n:
        p = subprocess.run([sys.executable, "-I", __file__, "--worker", lib, listfile, outfile, str(start)],
                           capture_output=True, text=True, timeout=3600)
        last = -1
        done = False
        for line in open(outfile, encoding="utf-8"):
            if line.startswith("START "):
                last = int(line[6:])
            elif line.startswith("R "):
                r = json.loads(line[2:])
                res[r["i"]] = r
            elif line.startswith("DONE"):
                done = True
        if done:
            break
        if last < 0:
            raise SystemExit("worker %s did not start: %s" % (lib, p.stderr[-400:]))
        if last not in res:
            res[last] = {"i": last, "ok": None, "err": "CRASH exit=%s" % p.returncode}
        start = last + 1
    return res


def main():
    base = Path(sys.argv[1]).resolve()
    report = sys.argv[2] if len(sys.argv) > 2 else None
    cs = cases(base)
    tmp = str(base / "_run")
    os.makedirs(tmp, exist_ok=True)
    listfile = os.path.join(tmp, "list.json")
    json.dump([c[1] for c in cs], open(listfile, "w", encoding="utf-8"))
    res = {lib: run_lib(lib, listfile, len(cs), tmp) for lib in LIBS}

    sys.path.insert(0, str(ROOT))
    import oxjson
    import orjson
    print("oxjson %s, orjson %s, Python %s, %d files" % (
        oxjson.__version__, orjson.__version__, sys.version.split()[0], len(cs)))

    suites = []
    for c in cs:
        if c[0] not in suites:
            suites.append(c[0])
    rel = lambda p: os.path.relpath(p, base).replace(os.sep, "/")
    rows = []
    fails = {lib: [] for lib in LIBS}
    print("\n%-30s %5s | %-22s | %-22s | %-22s" % ("suite (must accept / must reject)", "files", *LIBS))
    for s in suites:
        idx = [i for i, c in enumerate(cs) if c[0] == s]
        ny = sum(1 for i in idx if cs[i][2] == "y")
        nn = sum(1 for i in idx if cs[i][2] == "n")
        cells = []
        for lib in LIBS:
            y = n = crash = 0
            for i in idx:
                r = res[lib][i]
                e = cs[i][2]
                if r["ok"] is None:
                    crash += 1
                    fails[lib].append((rel(cs[i][1]), "CRASH"))
                elif e == "y":
                    if r["ok"]:
                        y += 1
                    else:
                        fails[lib].append((rel(cs[i][1]), "rejected valid: " + r["err"]))
                elif e == "n":
                    if not r["ok"]:
                        n += 1
                    else:
                        fails[lib].append((rel(cs[i][1]), "accepted invalid -> " + r.get("repr", "")[:60]))
            cells.append("y %d/%d n %d/%d%s" % (y, ny, n, nn, " CRASH %d" % crash if crash else ""))
            rows.append((s, lib, y, ny, n, nn, crash))
        print("%-30s %5d | %-22s | %-22s | %-22s" % (s, len(idx), *cells))

    for lib in LIBS:
        print("\n== %s: wrong on %d files with a fixed expectation" % (lib, len(fails[lib])))
        for name, why in fails[lib]:
            print("  %-70s %s" % (name, why))

    # value agreement where two libraries both accept
    def differ(a, b):
        out = []
        for i, c in enumerate(cs):
            ra, rb = res[a][i], res[b][i]
            if ra["ok"] and rb["ok"] and ra.get("val") != rb.get("val"):
                out.append((rel(c[1]), ra.get("repr", "")[:70], rb.get("repr", "")[:70]))
        return out

    for a, b in (("oxjson", "orjson"), ("oxjson", "json"), ("orjson", "json")):
        d = differ(a, b)
        print("\n== value differs, %s vs %s: %d files" % (a, b, len(d)))
        for name, x, y in d[:40]:
            print("  %s\n      %s: %s\n      %s: %s" % (name, a, x, b, y))

    # accept/reject disagreement on implementation-defined files
    print("\n== implementation-defined files (i_, transform, regression): accept / reject / crash")
    for lib in LIBS:
        idx = [i for i, c in enumerate(cs) if c[2] == "i"]
        a = sum(1 for i in idx if res[lib][i]["ok"])
        cr = sum(1 for i in idx if res[lib][i]["ok"] is None)
        print("  %-12s %d / %d / %d" % (lib, a, len(idx) - a - cr, cr))
    print("\n== implementation-defined files where oxjson and orjson decide differently")
    for i, c in enumerate(cs):
        if c[2] != "i":
            continue
        a, b = res["oxjson"][i], res["orjson"][i]
        if bool(a["ok"]) != bool(b["ok"]) or a["ok"] is None or b["ok"] is None:
            show = lambda r: ("accept " + r.get("repr", "")[:50]) if r["ok"] else ("reject " + str(r.get("err"))[:50])
            print("  %s\n      oxjson: %s\n      orjson:      %s" % (rel(c[1]), show(a), show(b)))

    print("\n== dumps(loads(x)): byte-exact on roundtrip suites, and loads(dumps(v)) == v on every accepted file")
    for lib in LIBS:
        rt = [i for i, c in enumerate(cs) if "roundtrip" in c[0] and res[lib][i]["ok"]]
        ex = sum(1 for i in rt if res[lib][i].get("exact"))
        acc = [i for i in range(len(cs)) if res[lib][i]["ok"]]
        back = sum(1 for i in acc if res[lib][i].get("back"))
        derr = [i for i in acc if "dumperr" in res[lib][i]]
        print("  %-12s exact %d/%d   value kept %d/%d   dumps raised %d" % (lib, ex, len(rt), back, len(acc), len(derr)))
        for i in rt:
            if not res[lib][i].get("exact"):
                print("      not exact %-52s -> %s" % (rel(cs[i][1]), res[lib][i].get("dump", res[lib][i].get("dumperr"))[:60]))
        for i in acc:
            r = res[lib][i]
            if "dumperr" in r:
                print("      dumps raised %-49s %s" % (rel(cs[i][1]), r["dumperr"][:70]))
            elif not r.get("back"):
                print("      value changed %-48s" % rel(cs[i][1]))

    if report:
        json.dump({"cases": [(c[0], rel(c[1]), c[2]) for c in cs],
                   "results": {lib: [res[lib][i] for i in range(len(cs))] for lib in LIBS}},
                  open(report, "w", encoding="utf-8"), indent=0)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--worker":
        worker(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]))
    else:
        main()
