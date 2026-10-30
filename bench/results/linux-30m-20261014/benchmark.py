#!/usr/bin/env python3
"""Sustained, GC-on JSON loads/dumps benchmark with process-memory tracking."""
from __future__ import annotations

import argparse
import gc
import hashlib
import importlib.metadata
import json
import os
import platform
import socket
import statistics
import sys
import time
from pathlib import Path

DOCS = (
    "testdata/engine/atlas.json",
    "testdata/diverse/numbers.json",
    "testdata/diverse/twitter.json",
    "testdata/diverse/canada.json",
    "testdata/diverse/marine_ik.json",
    "testdata/bench/large_50k.json",
)


def proc_mem() -> tuple[int, int]:
    values = {}
    for line in Path("/proc/self/status").read_text().splitlines():
        if line.startswith(("VmRSS:", "VmHWM:")):
            key, value, _unit = line.split()
            values[key[:-1]] = int(value) * 1024
    return values.get("VmRSS", 0), values.get("VmHWM", 0)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * (len(ordered) - 1)))]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--duration", type=int, default=1800)
    parser.add_argument("--segment", type=float, default=5.0)
    args = parser.parse_args()
    root = args.root.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(root))
    import json_parser
    import orjson

    data = {rel: (root / rel).read_bytes() for rel in DOCS}
    objects = {rel: json.loads(raw) for rel, raw in data.items()}
    for rel, raw in data.items():
        expected = orjson.loads(raw)
        if json_parser.loads(raw) != expected:
            raise RuntimeError(f"loads mismatch: {rel}")
        if orjson.loads(json_parser.dumps(objects[rel])) != expected:
            raise RuntimeError(f"dumps mismatch: {rel}")

    extension = Path(json_parser.__file__).parent
    binaries = sorted(extension.glob("*.so"))
    metadata = {
        "host": socket.gethostname(),
        "platform": platform.platform(),
        "python": sys.version,
        "parser_version": getattr(json_parser, "__version__", "unknown"),
        "orjson_version": importlib.metadata.version("orjson"),
        "root": str(root),
        "duration_requested_s": args.duration,
        "segment_target_s": args.segment,
        "extension": [{"path": str(p), "bytes": p.stat().st_size, "sha256": sha256(p)} for p in binaries],
        "documents": {rel: {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()} for rel, raw in data.items()},
        "cpu_count": os.cpu_count(),
        "loadavg_start": os.getloadavg(),
        "memtotal_kib": next((int(s.split()[1]) for s in Path("/proc/meminfo").read_text().splitlines() if s.startswith("MemTotal:")), 0),
        "gc_enabled": gc.isenabled(),
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    }
    (args.out / "metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")

    operations = []
    for rel in DOCS:
        for op in ("loads", "dumps"):
            for name in ("json_parser", "orjson"):
                operations.append((rel, op, name))

    start = time.monotonic()
    deadline = start + args.duration
    records = 0
    failures = 0
    with (args.out / "samples.jsonl").open("w", encoding="utf-8", buffering=1) as log:
        while time.monotonic() < deadline:
            cycle = records // len(operations)
            slot = records % len(operations)
            rel, op, name = operations[slot]
            # Rotate parser order between cycles to reduce time-order bias.
            if cycle % 2:
                name = "orjson" if name == "json_parser" else "json_parser"
            module = json_parser if name == "json_parser" else orjson
            fn = module.loads if op == "loads" else module.dumps
            arg = data[rel] if op == "loads" else objects[rel]
            gc.collect()
            rss_before, peak_before = proc_mem()
            cpu_before = time.process_time()
            begin = time.monotonic()
            stop = min(begin + args.segment, deadline)
            per_call_ms = []
            calls = 0
            error = None
            try:
                while time.monotonic() < stop:
                    for _ in range(10):
                        t0 = time.perf_counter_ns()
                        result = fn(arg)
                        per_call_ms.append((time.perf_counter_ns() - t0) / 1e6)
                        del result
                        calls += 1
            except Exception as exc:
                error = f"{type(exc).__name__}: {exc}"
                failures += 1
            elapsed = time.monotonic() - begin
            cpu_s = time.process_time() - cpu_before
            rss_end, peak_end = proc_mem()
            gc.collect()
            rss_after_gc, _ = proc_mem()
            row = {
                "elapsed_total_s": time.monotonic() - start,
                "cycle": cycle,
                "document": rel,
                "operation": op,
                "parser": name,
                "calls": calls,
                "wall_s": elapsed,
                "cpu_s": cpu_s,
                "cpu_pct_one_core": 100 * cpu_s / elapsed if elapsed else 0,
                "mean_ms": statistics.fmean(per_call_ms) if per_call_ms else None,
                "p50_ms": percentile(per_call_ms, 0.5) if per_call_ms else None,
                "p95_ms": percentile(per_call_ms, 0.95) if per_call_ms else None,
                "ops_per_s_wall": calls / elapsed if elapsed else 0,
                "rss_before_bytes": rss_before,
                "rss_end_bytes": rss_end,
                "rss_after_gc_bytes": rss_after_gc,
                "peak_before_bytes": peak_before,
                "peak_end_bytes": peak_end,
                "gc_counts": gc.get_count(),
                "error": error,
            }
            log.write(json.dumps(row, ensure_ascii=False) + "\n")
            records += 1
            if records % 12 == 0 or error:
                print(json.dumps({"records": records, "elapsed_s": round(row["elapsed_total_s"], 1), "last": [rel, op, name], "failures": failures, "rss_mib": round(rss_after_gc / 1048576, 1)}), flush=True)

    metadata["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    metadata["elapsed_s"] = time.monotonic() - start
    metadata["records"] = records
    metadata["failures"] = failures
    metadata["loadavg_end"] = os.getloadavg()
    metadata["rss_final_bytes"], metadata["peak_final_bytes"] = proc_mem()
    (args.out / "metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    print(json.dumps({"done": True, "elapsed_s": metadata["elapsed_s"], "records": records, "failures": failures, "out": str(args.out)}), flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
