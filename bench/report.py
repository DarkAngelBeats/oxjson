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
    if 0 < v < 0.05 * 1048576:
        return "<0.1"
    return "%.1f" % (v / 1048576.0)


def table(res, key, fmt, libs):
    out = ["| document | " + " | ".join(libs) + " |", "|---|" + "---:|" * len(libs)]
    for doc in res["docs"]:
        vals = [res["libs"][l]["docs"].get(doc, {}).get(key)
                if not res["libs"][l]["docs"].get(doc, {}).get("error") else None for l in libs]
        if key.endswith("_peak_bytes"):
            vals = [v if v is not None and v > 0 else None for v in vals]
        good = [v for v in vals if v is not None]
        best = min(good) if good else None
        cells = []
        for v in vals:
            s = fmt(v)
            cells.append("**%s**" % s if v is not None and v == best and not key.endswith("_peak_bytes") else s)
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
          "  minimum of three processes. These are historical high-water-mark deltas, not allocation totals.",
          "  Windows measures peak private commit; Linux/macOS measure peak RSS. Includes returned objects.",
          "  Earlier import/input peaks can hide allocations. Zero deltas are inconclusive and shown as `-`.",
          "  Legacy result files have no raw repeat samples; precise ratios require new measurements.",
          "  See [memory methodology](memory-testing.md) and `bench/memory_audit.py` for retained/released memory and repeated calls.",
          "- **Disk**: size of the installed package files.",
          "- Best measured speed values are bold; historical memory deltas are not ranked. `-` means unavailable, failed, unsupported, or an inconclusive zero memory delta.",
          "",
          "Each section names its machine; numbers from different machines are not comparable.",
          ""]
    name = lambda r: "Python %s, %s %s" % (r["python"], r["system"], r["machine"])
    md += ["## Current local build", "", "See the [comparison with orjson 3.13.0, 29 October 2026](current-comparison.md) for the new speed repeats, expanded memory matrix, exact versions and raw evidence. The multi-platform tables below retain their recorded historical versions.", ""]
    md += ["## Sustained Linux run", "",
           "Two 30-minute runs on 14 October 2026 used json_parser 0.4.0 and orjson 3.13.0 on Python 3.12.3, Linux x86-64 (Intel Core i3-13100). The report, metadata and raw samples of both runs are in [`bench/results/linux-30m-20261014`](../bench/results/linux-30m-20261014/report.md). They are separate from both the current local Windows comparison and the historical short-process tables below; no current 0.4.1 Linux memory result is claimed here.", ""]
    md += ["## Library versions", "", "| run | " + " | ".join(ORDER) + " |", "|---|" + "---|" * len(ORDER)]
    for r in runs:
        md.append("| %s | %s |" % (name(r), " | ".join(r["libs"].get(l, {}).get("version", "-") for l in ORDER)))
    md += ["", "## Installed size, KiB", "", "| run | " + " | ".join(l for l in ORDER if l != "json") + " |",
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
               "### Historical process peak delta in loads, MiB", "", table(r, "loads_peak_bytes", fmt_mb, libs), "",
               "### Historical process peak delta in dumps, MiB", "", table(r, "dumps_peak_bytes", fmt_mb, libs)]
    (ROOT / "docs" / "benchmarks.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    print("wrote docs/benchmarks.md from", [f.name for f in files])


if __name__ == "__main__":
    main()
