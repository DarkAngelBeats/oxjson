"""Compare current local oxjson and installed orjson; preserve all speed repeats."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from bench import DOCS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "bench/results/speed-current.json")
    parser.add_argument("--repeats", type=int, default=5)
    args = parser.parse_args()
    if args.repeats < 3:
        parser.error("at least three process repeats are required")
    import oxjson
    import orjson
    modules = {"oxjson": oxjson, "orjson": orjson}
    report = {"started_utc": datetime.now(timezone.utc).isoformat(), "python": sys.version,
              "platform": platform.platform(), "processor": platform.processor(),
              "executable": sys.executable, "command": sys.argv,
              "metric": "microseconds_per_call", "gc_enabled": True,
              "method": "Each sample is a fresh process; worker warms once, averages 3 rounds of 10 timed calls. Library order alternates between repeats. Summaries are medians of process samples.",
              "versions": {k: m.__version__ for k, m in modules.items()},
              "binaries": {}, "documents": {}, "samples": [], "summaries": [], "errors": []}
    for lib in modules:
        binary = Path(importlib.import_module(lib + "." + lib).__file__)
        report["binaries"][lib] = {"path": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
    for label, doc in DOCS:
        data = (ROOT / doc).read_bytes()
        report["documents"][doc] = {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
        obj = json.loads(data)
        for lib, mod in modules.items():
            if mod.loads(data) != obj or json.loads(mod.dumps(obj)) != obj:
                raise RuntimeError("correctness mismatch: " + lib + " " + doc)
        for repeat in range(args.repeats):
            for lib in (("oxjson", "orjson") if repeat % 2 == 0 else ("orjson", "oxjson")):
                try:
                    p = subprocess.run([sys.executable, str(ROOT / "bench/bench.py"), "--worker", lib, doc, "speed", str(ROOT)],
                                       capture_output=True, text=True, timeout=300, env=dict(os.environ, PYTHONIOENCODING="utf-8"))
                    if p.returncode:
                        raise RuntimeError("worker exit %d: %s" % (p.returncode, p.stderr[-1000:]))
                    row = json.loads(p.stdout)
                    if row["version"] != report["versions"][lib] or not all(math.isfinite(row[k]) and row[k] > 0 for k in ("loads_us", "dumps_us")):
                        raise ValueError("invalid speed response")
                    report["samples"].append(row | {"library": lib, "document": doc, "repeat": repeat})
                except Exception as exc:
                    report["errors"].append({"library": lib, "document": doc, "repeat": repeat, "error": str(exc)})
        for operation in ("loads", "dumps"):
            summary = {"document": doc, "operation": operation}
            for lib in modules:
                values = [s[operation + "_us"] for s in report["samples"] if s["library"] == lib and s["document"] == doc]
                if len(values) == args.repeats:
                    summary[lib] = {"min_us": min(values), "median_us": statistics.median(values), "max_us": max(values)}
            report["summaries"].append(summary)
        print(doc, "done", flush=True)
    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    report["status"] = "FAIL" if report["errors"] else "OK"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(report["status"], args.output)
    return 1 if report["errors"] else 0


if __name__ == "__main__":
    sys.exit(main())
