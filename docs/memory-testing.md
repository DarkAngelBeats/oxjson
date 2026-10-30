# Memory measurement and publication policy

Process memory, Python allocation counts and native allocation totals are different measurements. Do not describe one as another or infer leak freedom from a passing finite test.

## Historical data

`bench/bench.py` measures the difference between process high-water marks before and after a first call in a fresh process, retaining the result until the second reading. The historical summary takes the minimum of three processes. Input loading, imports and, for dumps, construction of the input object have already happened. Their previous peaks can hide allocations during the operation. A zero delta is inconclusive. Positive deltas can also undercount memory used by the call. Minimum selection does not reliably remove loader noise.

Windows uses peak private commit (`PeakPagefileUsage`); Linux uses peak RSS (`VmHWM`), macOS uses `ru_maxrss`. These have different meanings. Values are MiB (2**20 bytes), even in historical tables labelled MB. Do not compare these metrics across operating systems. Raw historical files are preserved; zero deltas are displayed as unavailable, and historical memory deltas are not ranked or highlighted as winners. Legacy files lack repeat distributions. The former README memory comparison and article ratios have been withdrawn pending stronger evidence.

## Evidence collection

Run both libraries in the same interpreter and environment:

```sh
python tests/check_memory_measurements.py
python bench/memory_audit.py --output bench/results/memory-audit.json
python bench/memory_audit.py --portable --output bench/results/memory-audit-portable.json
```

The audit supports Windows, Linux and macOS (current RSS sampled with `ps` on macOS). Missing metrics, invalid output, crashes or timeouts are failures, not zeros. Each library/document/operation/repeat runs in a fresh process. Defaults: three documents, both libraries, seven workloads (loads, dumps, malformed loads, document truncated at its final non-whitespace byte, cyclic dumps, failing default callback and a no-parser observer control), three process repeats, five rounds of twenty calls after warmup. Errors are caught only when expected; acceptance of invalid input fails the worker.

Each sample records input and extension SHA-256, library version, baseline memory, memory with the first result held, memory after release and GC, memory while retaining three results simultaneously (`--retain`), memory after releasing that batch, and each post-warmup round. Current memory is private commit on Windows and RSS on Linux/macOS. The small malformed-input and cyclic-output cases keep the document as baseline context; the truncated-document case parses nearly the entire document before rejection. Callback tests verify that the intended callback exception is propagated or chained. The historical peak metric is recorded separately. All raw samples are retained; first-call peak-delta summaries include minimum, median, maximum and an explicit inconclusive flag if any sample is zero. Measurements include runtime and allocator caches; retained-minus-baseline is not a recursive object-size measurement. GC is enabled. Defaults provide regression coverage, not a long-duration soak or comprehensive coverage of error paths.

A current-memory increase exceeding max(8 MiB, 3 × input size) at any sampled post-warmup round flags the run for investigation and returns a failure; each sample records `input_bytes` and `growth_threshold_bytes`. This is a regression threshold, not a demonstrated leak boundary: allocator growth may trigger it, while smaller or slower leaks can escape. The input-size term was added on 30 October 2026 after macOS CI (macos-14, Python 3.12) flagged oxjson on `large_50k.json` (4.9 MB): resident memory rose in one or two steps of one buffer (about 4.7 MiB each, 9.5 MiB at most) and then stayed flat for the remaining rounds, while Windows and Linux showed at most 0.008 MiB. This pattern fits an allocator keeping freed large buffers resident; it was not attributed with an allocation profiler. A buffer kept on every call would cross the threshold within one round. Runs before that date used a fixed 8 MiB; none of the published ones crossed it. The separate Python-block/refcount checks in `tests/check_versions.py` cover additional failure modes. Increase calls and rounds for sustained testing. Both final and maximum sampled growth are retained; a final drop does not hide an earlier threshold crossing. Samples only observe round boundaries and cannot detect every transient allocation. Native allocation tools or a dedicated profiler are still needed to attribute allocations precisely.

## Before publishing

Retain commands, platform/interpreter, library versions, extension and input hashes, raw process repeats, metric names and units. Report distributions and retained/released checkpoints. Investigate failed or inconclusive samples; do not discard them by choosing the smallest successful run. Recheck both AVX2 and portable paths. Claims apply only to measured builds, inputs and workloads. Memory measured during a combined endurance run cannot be attributed to either library individually. No precise memory-saving ratio should be inferred solely from historical high-water-mark deltas.

## Initial local validation, 29 October 2026 (older competitor and harness)

The initial harness used Windows / Python 3.11.4, oxjson 0.4.1 and installed orjson 3.10.12. The [expanded current comparison](current-comparison.md) supersedes it for comparisons with orjson 3.13.0. The old sampler rebuilt ctypes classes on Windows during snapshots; the current sampler initializes its reader once and includes an observer control. These are different versions and conditions from the older farmgpu comparison with orjson 3.13.0. Six measurement self-tests passed, including a touched 32 MiB buffer, worker crashes, missing output and failed audit propagation. This run loaded an earlier oxjson extension (SHA-256 `5ca5fcf7…`, recorded in each sample); its sources were not recorded, and it is not the build described by `current-build-20261029.json` (extension `509f71bb…`).

- [Default-path raw evidence](../bench/results/memory-audit-windows-py311.json): 72 fresh-process samples across both libraries; maximum post-warmup current-memory increase 1.887 MiB; no threshold flags or worker errors. Five of 24 first-call peak-delta groups contain zero values and are inconclusive.
- [Portable-path raw evidence](../bench/results/memory-audit-portable-windows-py311.json): 36 fresh-process oxjson samples; maximum increase 0.898 MiB; no threshold flags or worker errors. Five of 12 first-call peak-delta groups are inconclusive.

Every sample includes five rounds of twenty calls and a separate batch retaining three results. The existing file/random/reference/block checks also passed: 229 files, 30,000 random documents, no failed memory checks, approximately 0.6 MiB growth over three additional workload rounds. The 8 MiB growth threshold was not crossed. No precise memory-saving ratio or guarantee of leak freedom follows from these results. Linux and macOS execution of the new audit remains unverified locally.

## Expanded comparison of current builds

See [current comparison](current-comparison.md) for the 653-process matrix with oxjson 0.4.1 and orjson 3.13.0, per-document distributions, targeted 30-round series and retained/released evidence. This comparison includes a six-result batch and retains min/median/max across independent processes. Native process metrics cannot be turned into a precise allocation total or a universal saving ratio.

The source/toolchain identity is in [current-build-20261029.json](../bench/results/current-build-20261029.json). The raw reports include exact interpreter/library versions, input and loaded-extension SHA-256, script hashes, parameters and UTC timestamps. The observed reader-control growth was zero in this run; it is not subtracted from parser measurements.
