# usage: compare.py OUTDIR  -> dumps (and loads control) base vs cur vs orjson
import json, statistics as st, sys, os
O = sys.argv[1]
def load(n):
    p = os.path.join(O, n)
    return json.load(open(p)) if os.path.exists(p) else None
MiB = 1048576
print("SPEED, median us of 7 processes per series (series 1 | series 2)")
sp = {(t, s): load("speed-%s-%d.json" % (t, s)) for t in ("base", "cur") for s in (1, 2)}
docs = [x["document"] for x in sp[("base", 1)]["summaries"] if x["operation"] == "dumps"]
for op in ("dumps", "loads"):
    print("\n%s: doc | base ox | cur ox | cur/base | orjson (all 4 series) | cur ox/orjson" % op)
    for d in docs:
        g = lambda t, s, lib: next(x[lib]["median_us"] for x in sp[(t, s)]["summaries"] if x["document"] == d and x["operation"] == op)
        b = [g("base", s, "oxjson") for s in (1, 2)]; c = [g("cur", s, "oxjson") for s in (1, 2)]
        o = [g(t, s, "orjson") for t in ("base", "cur") for s in (1, 2)]
        oc = [g("cur", s, "orjson") for s in (1, 2)]
        print("%-14s %9.2f|%9.2f  %9.2f|%9.2f  %5.3f  %9.2f..%9.2f  %5.3f" % (
            os.path.basename(d)[:14], b[0], b[1], c[0], c[1], st.mean(c) / st.mean(b), min(o), max(o), st.mean(c) / st.mean(oc)))
    errs = sum(len(sp[k].get("errors", [])) for k in sp)
print("\nspeed errors:", errs)
print("\nMEMORY dumps, MiB, median [min-max] of 5 processes: one = held-baseline, six = held_many-released")
mem = {t: load("memory-%s.json" % t) for t in ("base", "cur")}
for t in mem: print(t, "status", mem[t].get("status"), "errors", len(mem[t]["errors"]), "elapsed", round(mem[t].get("elapsed_s", 0)))
def agg(m, lib, d, a, b):
    v = [(x[a]["current_bytes"] - x[b]["current_bytes"]) / MiB for x in m["samples"] if x["library"] == lib and x["document"] == d and x["operation"] == "dumps"]
    return "%.3f [%.3f-%.3f]" % (st.median(v), min(v), max(v)) if v else "-"
print("doc | ox base one | ox cur one | or one(cur run) | ox base six | ox cur six | or six(cur run)")
for d in docs:
    print("%-14s %s | %s | %s | %s | %s | %s" % (os.path.basename(d)[:14],
        agg(mem["base"], "oxjson", d, "held", "baseline"), agg(mem["cur"], "oxjson", d, "held", "baseline"), agg(mem["cur"], "orjson", d, "held", "baseline"),
        agg(mem["base"], "oxjson", d, "held_many", "released"), agg(mem["cur"], "oxjson", d, "held_many", "released"), agg(mem["cur"], "orjson", d, "held_many", "released")))
for t in mem:
    g = [x.get("maximum_post_warm_growth_bytes", 0) / MiB for x in mem[t]["samples"]]
    print(t, "max post-warm growth MiB %.3f, growth flags %d" % (max(g), sum(1 for x in mem[t]["samples"] if x.get("growth_flag"))))
