# Half-hour run on Linux: oxjson 0.4.0 vs orjson 3.13.0

**Result:** oxjson is faster than orjson in all 12 combinations of document and operation. It was also faster
within 177 of 178 cycles; the one exception is writing `canada.json`, where oxjson was 1% slower in one cycle. This
is the result of one build on one machine, not a claim about all inputs, platforms or the current release.

In version 0.4.0 the library was called `json_parser`, so that name appears in the raw data and in the script.
[Russian version](report.ru.md).

## How it was measured

- Machine: Linux x86-64 (kernel 6.8, glibc 2.39), Intel Core i3-13100, 8 threads, about 64 GB of memory.
  Python 3.12.3, garbage collector on.
- 14 October 2026, 19:46–20:16 UTC, 30 minutes. The server was running other jobs at the time: the one-minute
  load average rose from 3.73 to 4.16.
- Time was split into 5-second segments. In each segment one library calls one operation on one document many
  times in a row, and the time of every call is recorded. A cycle covers all 24 combinations of document,
  operation and library. The order of the two libraries in each pair alternates between cycles.
- Before measuring, the script checks that both libraries read and write every document identically.
- 354 segments in total, 17,763,060 calls, no exceptions.

The script is [benchmark.py](benchmark.py), parameters and file hashes are in [metadata.json](metadata.json), every
segment is in [samples.jsonl](samples.jsonl).

## Speed

Time is the mean per call over all segments, weighted by the number of calls. "oxjson / orjson per cycle" is the
ratio of the two libraries' times within each cycle, from the best cycle to the worst. Below 1 means oxjson is
faster.

### Parsing (`loads`)

| Document | oxjson | orjson | Faster by | oxjson / orjson per cycle | Cycles where oxjson was slower |
|---|---:|---:|---:|---:|---:|
| config, 34 KB (`atlas.json`) | 49.8 µs | 53.9 µs | 7.7% | 0.86–0.95 | 0 of 15 |
| floats, 150 KB (`numbers.json`) | 0.09 ms | 0.14 ms | 31.9% | 0.65–0.74 | 0 of 15 |
| API response with text, 647 KB (`twitter.json`) | 0.78 ms | 0.93 ms | 16.2% | 0.74–0.95 | 0 of 15 |
| coordinates, 2.2 MB (`canada.json`) | 4.19 ms | 4.72 ms | 11.2% | 0.85–0.94 | 0 of 15 |
| 3D mesh, 3.0 MB (`marine_ik.json`) | 6.22 ms | 9.60 ms | 35.2% | 0.58–0.70 | 0 of 15 |
| uniform records, 4.9 MB (`large_50k.json`) | 18.80 ms | 28.07 ms | 33.0% | 0.55–0.81 | 0 of 14 |

### Writing (`dumps`)

| Document | oxjson | orjson | Faster by | oxjson / orjson per cycle | Cycles where oxjson was slower |
|---|---:|---:|---:|---:|---:|
| config, 34 KB (`atlas.json`) | 11.1 µs | 14.9 µs | 25.4% | 0.69–0.76 | 0 of 15 |
| floats, 150 KB (`numbers.json`) | 0.15 ms | 0.16 ms | 8.0% | 0.85–0.94 | 0 of 15 |
| API response with text, 647 KB (`twitter.json`) | 0.12 ms | 0.18 ms | 35.0% | 0.62–0.75 | 0 of 15 |
| coordinates, 2.2 MB (`canada.json`) | 1.76 ms | 1.90 ms | 7.4% | 0.86–1.01 | 1 of 15 |
| 3D mesh, 3.0 MB (`marine_ik.json`) | 2.46 ms | 2.80 ms | 12.4% | 0.86–0.92 | 0 of 14 |
| uniform records, 4.9 MB (`large_50k.json`) | 3.69 ms | 4.14 ms | 10.8% | 0.87–0.92 | 0 of 14 |

## First run

An hour earlier, from 18:09 to 18:39 UTC, the [first run](first-run/summary.md) used an earlier build of the same
version on the same machine and files. Then oxjson lost in three combinations: parsing `numbers.json` by 6.2%,
writing `canada.json` by 8.1% and parsing `canada.json` by 1.1%. Fixes went into the build between the runs; the
version number stayed the same, and the binaries' hashes differ:

- first run: SHA-256 `5c48542484658cb80838771fcb999e943c096503309ba702f4f5bccf97261192`;
- this run: SHA-256 `abd234a65d405e2d070f5704f1e46958b258c8c2972221cabbe91a25069e6153`.

The sources of the second build were not saved separately, so the exact set of fixes cannot be reconstructed
from these files.

## Limitations

- One run, on a server with background load.
- Memory is not compared here: the script records only the memory of the whole process, which runs both libraries
  at once.
- To repeat the run with the current oxjson, replace `import json_parser` with `import oxjson as json_parser` in
  `benchmark.py`.
