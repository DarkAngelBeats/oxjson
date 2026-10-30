"""Fresh-process memory evidence, not an allocator profiler.

python bench/memory_audit.py --output bench/results/memory-audit.json
Use --portable for the OXJSON_NO_AVX2 path. Every repeat is a fresh process.
"""
import argparse
from datetime import datetime, timezone
import time
import gc
import hashlib
import json
import os
import platform
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from bench import peak_memory, windows_memory_reader


def current_memory():
    if os.name == "nt":
        return windows_memory_reader()().PagefileUsage
    if sys.platform == "darwin":
        return int(subprocess.check_output(["ps", "-o", "rss=", "-p", str(os.getpid())]).strip()) * 1024
    if sys.platform.startswith("linux"):
        for line in Path("/proc/self/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
    raise RuntimeError("current process memory unavailable on this platform")


def snapshot():
    return {"current_bytes": current_memory(), "peak_bytes": peak_memory(),
            "python_blocks": sys.getallocatedblocks()}


OPERATIONS = ("loads", "dumps", "loads_error", "loads_truncated", "dumps_error", "default_error", "noop")


def growth_metrics(warm, samples):
    final = samples[-1]["current_bytes"] - warm["current_bytes"]
    maximum = max(s["current_bytes"] - warm["current_bytes"] for s in samples)
    return final, maximum, maximum > 8 * 1048576


def worker(lib, document, operation, rounds, calls, retain):
    import importlib
    module = importlib.import_module(lib)
    data = (ROOT / document).read_bytes()
    obj = json.loads(data) if operation == "dumps" else None
    if operation not in OPERATIONS:
        raise ValueError("unknown memory operation: " + operation)
    malformed = data.rstrip()[:-1] if operation == "loads_truncated" else b'{"a": [1, 2,'
    marker = object()
    class CallbackError(Exception):
        pass
    def rejected_default(value):
        raise CallbackError("intentional callback failure")
    cycle = []
    cycle.append(cycle)
    def invoke():
        if operation == "loads":
            return module.loads(data)
        if operation == "dumps":
            return module.dumps(obj)
        if operation == "noop":
            return None
        if operation == "default_error":
            try:
                module.dumps([marker], default=rejected_default)
            except Exception as exc:
                # orjson wraps callback exceptions in TypeError; oxjson propagates.
                if isinstance(exc, CallbackError) or isinstance(exc.__cause__, CallbackError):
                    return None
                raise
            raise AssertionError("callback failure was swallowed")
        # Both libraries must reject these inputs; successful returns are failures.
        if operation in ("loads_error", "loads_truncated"):
            try:
                module.loads(malformed)
            except ValueError:
                return None
        else:
            try:
                module.dumps(cycle)
            except (TypeError, ValueError, RecursionError):
                return None
        raise AssertionError("invalid input was accepted")
    # Initialize the measurement API before the baseline (ctypes imports allocate).
    snapshot()
    gc.collect()
    baseline = snapshot()
    result = invoke()
    held = snapshot()
    del result
    gc.collect()
    released = snapshot()
    retained = [invoke() for _ in range(retain)]
    held_many = snapshot()
    del retained
    gc.collect()
    released_many = snapshot()
    for _ in range(calls):
        invoke()
    gc.collect()
    warm = snapshot()
    samples = []
    for _ in range(rounds):
        for _ in range(calls):
            invoke()
        gc.collect()
        samples.append(snapshot())
    delta, max_delta, flagged = growth_metrics(warm, samples)
    module_file = Path(importlib.import_module(lib + "." + lib).__file__)
    binaries = [module_file]
    return {"library": lib, "version": module.__version__, "document": document,
            "operation": operation,
            "workload": {"loads_error": "fixed malformed JSON", "loads_truncated": "document truncated at last non-whitespace byte",
                         "dumps_error": "self-referential list", "default_error": "failing default callback", "noop": "observer control"}.get(operation, "document"),
            "malformed_input_sha256": hashlib.sha256(malformed).hexdigest() if operation in ("loads_error", "loads_truncated") else None,
            "input_sha256": hashlib.sha256(data).hexdigest(),
            "binaries": [{"path": str(p), "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in binaries],
            "baseline": baseline, "held": held, "released": released, "warm": warm,
            "held_many": held_many, "released_many": released_many, "retain_count": retain,
            "rounds": samples, "calls_per_round": calls,
            "first_call_peak_delta_bytes": held["peak_bytes"] - baseline["peak_bytes"],
            "post_warm_growth_bytes": delta,
            "maximum_post_warm_growth_bytes": max_delta, "growth_flag": flagged}


def collect(command, env):
    p = subprocess.run(command, env=env, capture_output=True, text=True, timeout=300)
    if p.returncode:
        raise RuntimeError("worker exit %d: %s" % (p.returncode, p.stderr[-1000:]))
    value = json.loads(p.stdout)
    validate_sample(value)
    return value


def validate_sample(value):
    if not isinstance(value, dict):
        raise ValueError("invalid memory worker output")
    try:
        checkpoints = [value[k] for k in ("baseline", "held", "released", "held_many", "released_many", "warm")] + value["rounds"]
        if not value["rounds"] or not value["binaries"]:
            raise ValueError("missing rounds or extension identity")
        for point in checkpoints:
            for field in ("current_bytes", "peak_bytes", "python_blocks"):
                if type(point[field]) is not int or point[field] < 0:
                    raise ValueError("invalid memory metric")
        final, maximum, flag = growth_metrics(value["warm"], value["rounds"])
        if (value["post_warm_growth_bytes"], value["maximum_post_warm_growth_bytes"], value["growth_flag"]) != (final, maximum, flag):
            raise ValueError("inconsistent growth metrics")
        if value["first_call_peak_delta_bytes"] != value["held"]["peak_bytes"] - value["baseline"]["peak_bytes"]:
            raise ValueError("inconsistent peak delta")
    except (KeyError, TypeError) as exc:
        raise ValueError("incomplete memory worker output") from exc


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "bench/results/memory-audit.json")
    parser.add_argument("--libs", nargs="+", choices=["oxjson", "orjson"], default=["oxjson", "orjson"])
    parser.add_argument("--docs", nargs="+", default=["testdata/engine/atlas.json", "testdata/diverse/canada.json", "testdata/bench/large_50k.json"])
    parser.add_argument("--operations", nargs="+", choices=OPERATIONS, default=list(OPERATIONS))
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--calls", type=int, default=20)
    parser.add_argument("--retain", type=int, default=3)
    parser.add_argument("--portable", action="store_true")
    parser.add_argument("--worker", nargs=3, metavar=("LIB", "DOCUMENT", "OPERATION"))
    args = parser.parse_args()
    if min(args.repeats, args.rounds, args.calls, args.retain) < 1:
        parser.error("repeats, rounds, calls and retain must be positive")
    if args.worker:
        print(json.dumps(worker(*args.worker, args.rounds, args.calls, args.retain)))
        return 0
    env = dict(os.environ, PYTHONIOENCODING="utf-8")
    if args.portable:
        env["OXJSON_NO_AVX2"] = "1"
    started = time.monotonic()
    report = {"schema_version": 2, "started_utc": datetime.now(timezone.utc).isoformat(),
              "audit_script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "metrics_script_sha256": hashlib.sha256((ROOT / "bench/bench.py").read_bytes()).hexdigest(),
              "python": sys.version, "executable": sys.executable, "command": sys.argv,
              "processor": platform.processor(), "platform": platform.platform(),
              "metric": "private_commit_bytes" if os.name == "nt" else "resident_bytes",
              "portable_requested": env.get("OXJSON_NO_AVX2") == "1",
              "gc_enabled_in_workers": True, "settings": vars(args) | {"output": str(args.output)}, "samples": [], "summaries": [], "errors": [],
              "interpretation": "Process-level measurements include runtime and results. Zero HWM delta is inconclusive. Growth flags require investigation; absence of a flag does not prove no leaks. Threshold: 8 MiB at any sampled round after warmup."}
    for lib in args.libs:
        for doc in args.docs:
            for op in args.operations:
                group = []
                for repeat in range(args.repeats):
                    try:
                        value = collect([sys.executable, __file__, "--worker", lib, doc, op,
                                         "--rounds", str(args.rounds), "--calls", str(args.calls), "--retain", str(args.retain)], env)
                        value["repeat"] = repeat
                        group.append(value)
                        report["samples"].append(value)
                    except Exception as exc:
                        report["errors"].append({"library": lib, "document": doc, "operation": op, "repeat": repeat, "error": str(exc)})
                if len(group) == args.repeats:
                    deltas = [s["first_call_peak_delta_bytes"] for s in group]
                    report["summaries"].append({"library": lib, "document": doc, "operation": op,
                        "peak_delta_min": min(deltas), "peak_delta_median": statistics.median(deltas),
                        "peak_delta_max": max(deltas), "peak_delta_inconclusive": any(d == 0 for d in deltas)})
                print(lib, doc, op, "%d/%d samples" % (len(group), args.repeats), flush=True)
    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    report["elapsed_s"] = time.monotonic() - started
    report["status"] = "FAIL" if report["errors"] or any(s["growth_flag"] for s in report["samples"]) else "NO_GROWTH_FLAG"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(report["status"], args.output)
    return 1 if report["status"] == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
