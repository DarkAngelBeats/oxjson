# oxjson

**English** | [Русский](README.ru.md)

A JSON library for CPython with the `orjson` interface: `loads` returns `dict` / `list`, `dumps` returns `bytes`.
The parser kernel is x86-64 assembly (NASM, AVX2). A C version of the same code, with NEON on ARM64
and SSE2 on older x86-64 CPUs, runs everywhere else.

```python
import oxjson

obj = oxjson.loads(b'{"id": 1, "tags": ["a", "b"], "score": 2.5}')
data = oxjson.dumps(obj)                                   # b'{"id":1,"tags":["a","b"],"score":2.5}'
text = oxjson.dumps(obj, option=oxjson.OPT_INDENT_2 | oxjson.OPT_SORT_KEYS)
```

## Install

```bash
pip install oxjson
```

Needs CPython 3.10–3.14. The wheel build configuration targets Python 3.10–3.14 on:

| platform | wheel |
|---|---|
| Linux x86-64 | manylinux, musllinux |
| Linux aarch64 | manylinux, musllinux |
| macOS | arm64, x86-64 |
| Windows x86-64 | win_amd64 |

Version 0.4.1 expands the configured wheel matrix. This table describes build targets; it is not a verification of every published artifact. Other targets require a source build and separate validation, see [Build](#build).

## Performance

Current local **oxjson 0.4.1** versus **orjson 3.13.0**, measured 29 October 2026 on Windows AMD64, Ryzen 9 5900X, CPython 3.11.4. Median of seven fresh-process speed samples per library/document, GC enabled. oxjson has lower medians in 11 of 14 tested scenarios; the summary includes all three losses.

| Scenario | oxjson | orjson |
|---|---:|---:|
| loads, catalog 1.8 MB | **2.176 ms** | 3.334 ms |
| loads, records 4.9 MB | **38.911 ms** | 52.784 ms |
| dumps, coords 2.2 MB | **1.922 ms** | 3.028 ms |
| dumps, config 34 KB | 21.69 us | **19.31 us** |
| dumps, catalog 1.8 MB | 476.92 us | **401.64 us** |
| dumps, records 4.9 MB | 6.657 ms | **6.001 ms** |

[Full current comparison and raw evidence](docs/current-comparison.md) include ranges, all seven documents, and 653 memory-test processes. Memory results are process private-commit measurements, not allocation totals; neither a universal memory advantage nor leak freedom is established. See [memory methodology](docs/memory-testing.md).

[Historical multi-platform tables](docs/benchmarks.md) retain their recorded versions and environments. These Windows results do not establish Linux or macOS performance or a before/after release regression.

## Status

Young project. The repository includes public conformance-suite checks and regression comparisons with `orjson`, listed under
[Tests](#tests). The locally verified scope for the current build is recorded in [current results](docs/current-comparison.md). GitHub Actions builds wheels and runs the package checks
(`.github/workflows/wheels.yml`). The differential fuzzer `tests/fuzz.py` ran 1.42 million cases (`loads` and `dumps`,
assembly and portable path, 20 minutes, Windows AMD64, Python 3.13.5, orjson 3.13.0) with no findings; it found one
`dumps` depth-limit bug before that, now fixed. It is not coverage-guided and has not run for long on Linux or macOS.
The project remains experimental for untrusted production inputs.

## Interface

| | |
|---|---|
| `loads(data)` | `data` is `bytes`, `bytearray`, `memoryview` or `str`. Raises `JSONDecodeError` (a subclass of `json.JSONDecodeError`) with `.pos`. |
| `dumps(obj, default=None, option=None)` | Returns `bytes`. `option` is a combination of the `OPT_*` constants, with the same names and values as in `orjson`. |
| `load(fp)`, `dump(obj, fp)` | Read from and write to a binary file object. |

`dumps` writes `str`, `int` (−2^63 … 2^64−1), `float`, `bool`, `None`, `list`, `tuple`, `dict`, dataclasses,
`datetime` / `date` / `time`, `enum.Enum`, `uuid.UUID`, and numpy arrays and scalars with `OPT_SERIALIZE_NUMPY`.
Other unsupported values are passed to `default` when provided.

Supported options: `OPT_APPEND_NEWLINE`, `OPT_INDENT_2`, `OPT_NAIVE_UTC`, `OPT_NON_STR_KEYS`,
`OPT_OMIT_MICROSECONDS`, `OPT_PASSTHROUGH_DATACLASS`, `OPT_PASSTHROUGH_DATETIME`, `OPT_PASSTHROUGH_SUBCLASS`,
`OPT_SERIALIZE_NUMPY`, `OPT_SORT_KEYS`, `OPT_STRICT_INTEGER`, `OPT_UTC_Z`.

### Differences from orjson

- `loads` has no nesting limit; orjson stops at 1024 levels. `dumps` without layout options has no limit either;
  with `OPT_INDENT_2`, `OPT_SORT_KEYS`, `OPT_NON_STR_KEYS` or `OPT_STRICT_INTEGER` a container 254 levels deep
  (the 255th nested one) is refused, as in orjson. Tuples count as containers here; orjson counts them inconsistently.
- No `orjson.Fragment`.
- `dumps(..., allow_nan=False)` raises `ValueError` for NaN and infinities instead of writing `null`.
- Errors from `dumps` are `TypeError`, not `orjson.JSONEncodeError` (which is a subclass of `TypeError`).

## Platforms

| target | code path |
|---|---|
| x86-64 with AVX2, Windows and Linux | assembly kernel |
| x86-64 without AVX2 | C with 16-byte SSE2 vectors, chosen when the module is imported |
| ARM64 (Apple Silicon, Linux) | C with 16-byte NEON vectors. macOS arm64 and Linux aarch64 wheels are built natively and the full test suite runs on both in CI. |
| anything else | plain C |

Python 3.10 – 3.14. The extension uses a few private CPython functions, so a new Python release may need a fix.
The free-threaded build and sub-interpreters are not supported: the key cache is shared by the process.

Set `OXJSON_NO_AVX2=1` before the import to force the portable path.

## Build

Needs a GCC-compatible C compiler (GCC or Clang) and, on Windows/Linux x86-64, `nasm` 2.16 or newer.

```bash
python make_ext.py
```

That builds `oxjson/oxjson.pyd` (or `.so`) for the interpreter that runs the script.
`python make_ext.py <dir>` puts a copy of the package into `<dir>/oxjson` instead.

For a wheel / sdist (PyPI):

```bash
python -m pip install build
python -m build
```

GitHub Actions builds wheels on push (`.github/workflows/wheels.yml`). Publishing to
PyPI is `.github/workflows/publish.yml` on a `v*` tag after Trusted Publishing is
configured for this repo (see [PyPI docs](https://docs.pypi.org/trusted-publishers/)).

## Checking a new machine

One script builds the extension, runs the standard regression checks and benchmark, and writes a single report file:

```bash
python -m pip install -r requirements-dev.txt
```

```bash
python tests/run_all.py
```

The report is `report-<system>-<machine>-py<XY>.txt` in the repository root; the benchmark numbers go to
`bench/results/` and into `docs/benchmarks.md`.

## Tests

```bash
python tests/check_versions.py
```

Compares `loads` with orjson on the files in `testdata/` and on 30 000 random documents, and checks that
repeated calls show no growth above the test thresholds. This is a leak regression check, not proof of leak freedom.

```bash
python tests/check_dumps_orjson.py
```

Compares `dumps` with orjson byte for byte on the tested types and option combinations, float text included
(every exponent, float keys, numpy float64 / float32 / float16).

```bash
python tests/check_portable.py
```

Checks that the assembly path and the portable path produce identical output.

`tests/vec16_selftest.c` is a stand-alone C program that checks the SSE2 / NEON helpers against plain C;
the build command is in its header.

```bash
python tests/check_conformance.py <dir>
```

Runs [JSONTestSuite](https://github.com/nst/JSONTestSuite),
[nativejson-benchmark](https://github.com/miloyip/nativejson-benchmark) and
[json_test_data](https://github.com/nlohmann/json_test_data) cloned into `<dir>`; the file header lists the commands.

`tests/check_loads.py` and `tests/check_dumps.py` compare with the standard `json` module on random data.

```bash
python tests/fuzz.py
```

Differential fuzzer for `loads` and `dumps` against orjson, on the assembly and the portable path; one minute by
default, `--seconds N` for longer. Every batch runs
in its own process; a crash, a hang or a result that differs from orjson is saved in `fuzz-out/` and makes the exit
code 1. Part of the `loads` inputs end right before an inaccessible memory page, so a read past the end of the
input crashes. `--replay` and `--replay-case` reproduce a saved finding. CI runs it for one minute per machine on every push
(`.github/workflows/fuzz.yml`), with a new seed each time.

Memory regression checks and raw evidence:

```bash
python tests/check_memory_measurements.py
python bench/memory_audit.py --output bench/results/memory-audit.json
```

See [memory methodology](docs/memory-testing.md). The audit records successful calls, rejected inputs, callback failures, an observer control, held/released results and repeated rounds. Passing finite checks does not prove leak freedom or a universal memory advantage. Conformance suites and the differential fuzzer are separate commands.

## Benchmarks

The numbers that matter are in [docs/benchmarks.md](docs/benchmarks.md): speed, historical process peak-memory deltas and
installed size against orjson, msgspec, ssrjson, yyjson, pysimdjson, ujson, python-rapidjson and the
standard library, on real documents (tens of KB to several MB) and Python 3.10 – 3.14. Each section
names its machine; numbers from different machines are not comparable.

A one-liner microbench on a tiny object only measures call overhead. Use it as an illustration, not
as the main comparison.

```bash
python bench/bench.py
```

```bash
python bench/report.py
```

## Layout

```
src/jl_kernel.asm          loads kernel, NASM, AVX2
src/jl_loads.c             builds Python objects for the kernel; the same kernel in C
src/jl_portable.c          jl_loads.c compiled as the portable build
src/json_dumps_walk.c      dumps
src/json_dumps_ext.inc     dataclass, datetime, enum, UUID, numpy and the layout options
src/json_dumps_portable.c  json_dumps_walk.c compiled as the portable build
include/json_vec16.h       16-byte vector helpers of the portable build: NEON and SSE2
src/json_module.c          the Python module
tools/                     generators of the tables in include/
```

## License

MPL-2.0, see `LICENSE`. Copyright (c) 2026 Lev Berezkin.

Third-party code and test documents (Zmij float digits, JSON fixtures) are under
their own MIT licenses; see [`THIRD_PARTY.md`](THIRD_PARTY.md) and `licenses/`.
