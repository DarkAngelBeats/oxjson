# -*- coding: utf-8 -*-
"""Render bench/results/py*.json as docs/benchmarks.md.

    python bench/report.py
"""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ORDER = ["oxjson", "orjson", "msgspec", "ssrjson", "yyjson", "pysimdjson", "ujson", "rapidjson", "json"]


def fmt_us(v):
    if v is None:
        return "-"
    if v >= 1000:
        return "%.2f ms" % (v / 1000)
    return "%.1f us" % v


def fmt_mb(v):
    if v is None:
        return "-"
    return "%.1f" % (v / 1048576.0)


def table(res, key, fmt, libs):
    out = ["| document | " + " | ".join(libs) + " |", "|---|" + "---:|" * len(libs)]
    for doc in res["docs"]:
        vals = [res["libs"][l]["docs"].get(doc, {}).get(key) for l in libs]
        good = [v for v in vals if v is not None]
        best = min(good) if good else None
        cells = []
        for v in vals:
            s = fmt(v)
            cells.append("**%s**" % s if v is not None and v == best else s)
        out.append("| %s | %s |" % (doc, " | ".join(cells)))
    return "\n".join(out)


def main():
    files = sorted((ROOT / "bench" / "results").glob("py*.json"))
    runs = [json.loads(f.read_text(encoding="utf-8")) for f in files]
    if not runs:
        raise SystemExit("no result files in bench/results")
    md = ["# Benchmarks", "",
          "Produced by `bench/bench.py` and `bench/report.py`. Every number comes from a separate process.",
          "",
          "- **Speed**: one call at a time, garbage collector on, mean of 10 calls, mean of three such rounds.",
          "  `loads` takes `bytes` and returns `dict` / `list`. `dumps` writes the object that `json.loads` built.",
          "- **Memory**: growth of the process peak memory across the first `loads` call in a fresh process,",
          "  best of three processes. It includes the returned Python objects.",
          "- **Disk**: size of the installed package files.",
          "- The best value in a row is bold. `-` means the library has no such function or no wheel for that Python.",
          "",
          "Each section names its machine; numbers from different machines are not comparable.",
          ""]
    name = lambda r: "Python %s, %s %s" % (r["python"], r["system"], r["machine"])
    md += ["## Library versions", "", "| run | " + " | ".join(ORDER) + " |", "|---|" + "---|" * len(ORDER)]
    for r in runs:
        md.append("| %s | %s |" % (name(r), " | ".join(r["libs"].get(l, {}).get("version", "-") for l in ORDER)))
    md += ["", "## Installed size, KB", "", "| run | " + " | ".join(l for l in ORDER if l != "json") + " |",
           "|---|" + "---:|" * (len(ORDER) - 1)]
    for r in runs:
        cells = []
        for l in ORDER:
            if l == "json":
                continue
            v = r["libs"].get(l, {}).get("disk_bytes")
            cells.append("%.0f" % (v / 1024) if v else "-")
        md.append("| %s | %s |" % (name(r), " | ".join(cells)))
    for r in runs:
        libs = [l for l in ORDER if l in r["libs"]]
        md += ["", "## %s" % name(r), "", "CPU: %s" % (r.get("processor") or "not reported"), "", "### loads", "", table(r, "loads_us", fmt_us, libs), "",
               "### dumps", "", table(r, "dumps_us", fmt_us, libs), "",
               "### Peak memory growth in loads, MB", "", table(r, "loads_peak_bytes", fmt_mb, libs), "",
               "### Peak memory growth in dumps, MB", "", table(r, "dumps_peak_bytes", fmt_mb, libs)]
    (ROOT / "docs" / "benchmarks.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    print("wrote docs/benchmarks.md from", [f.name for f in files])


if __name__ == "__main__":
    main()
