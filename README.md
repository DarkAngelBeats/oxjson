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

Needs CPython 3.10–3.14. Prebuilt wheels, each for Python 3.10 – 3.14:

| platform | wheel |
|---|---|
| Linux x86-64 | manylinux, musllinux |
| Linux aarch64 | manylinux, musllinux |
| macOS | arm64, x86-64 |
| Windows x86-64 | win_amd64 |

The 0.4.0 release on PyPI has only Linux x86-64 (manylinux), macOS arm64 and Windows with Python 3.12;
the full set above comes with the next release. Anything else (Windows ARM64, 32-bit) needs a
source build, see [Build](#build).

## Performance

x86-64 AVX2 path vs `orjson` 3.13 — Python 3.12, real documents.
Full tables (more libs, memory, other Pythons): [docs/benchmarks.md](docs/benchmarks.md).

| | oxjson | orjson |
|---|---:|---:|
| installed size | **~250 KB** | ~315 KB |
| loads, catalog 1.8 MB | **2.1 ms** | 4.0 ms |
| loads, records 4.9 MB | **30.5 ms** | 44.9 ms |
| dumps, coords 2.2 MB | **2.3 ms** | 3.5 ms |
| peak mem loads, records 4.9 MB | **32 MB** | 96 MB |

*Machine: Windows AMD64 (same NASM AVX2 kernel as Linux x86-64). Not comparable to macOS arm64 numbers.*

## Status

Young project. It passes the public conformance suites and agrees with `orjson` on the checks listed under
[Tests](#tests). GitHub Actions builds wheels and runs the package checks
(`.github/workflows/wheels.yml`). It has **not been fuzzed**. Do not feed it untrusted input in
production before that is done.

## Interface

| | |
|---|---|
| `loads(data)` | `data` is `bytes`, `bytearray`, `memoryview` or `str`. Raises `JSONDecodeError` (a subclass of `json.JSONDecodeError`) with `.pos`. |
| `dumps(obj, default=None, option=None)` | Returns `bytes`. `option` is a combination of the `OPT_*` constants, with the same names and values as in `orjson`. |
| `load(fp)`, `dump(obj, fp)` | Read from and write to a binary file object. |

`dumps` writes `str`, `int` (−2^63 … 2^64−1), `float`, `bool`, `None`, `list`, `tuple`, `dict`, dataclasses,
`datetime` / `date` / `time`, `enum.Enum`, `uuid.UUID`, and numpy arrays and scalars with `OPT_SERIALIZE_NUMPY`.
Anything else goes to `default`.

Supported options: `OPT_APPEND_NEWLINE`, `OPT_INDENT_2`, `OPT_NAIVE_UTC`, `OPT_NON_STR_KEYS`,
`OPT_OMIT_MICROSECONDS`, `OPT_PASSTHROUGH_DATACLASS`, `OPT_PASSTHROUGH_DATETIME`, `OPT_PASSTHROUGH_SUBCLASS`,
`OPT_SERIALIZE_NUMPY`, `OPT_SORT_KEYS`, `OPT_STRICT_INTEGER`, `OPT_UTC_Z`.

### Differences from orjson

- Floats are written the way Python prints them: `1e-05`, `1e+16`. orjson writes `1e-5`, `1e16`. The values are the same.
- `loads` has no nesting limit; orjson stops at 1024 levels. `dumps` without layout options has no limit either;
  with `OPT_INDENT_2`, `OPT_SORT_KEYS`, `OPT_NON_STR_KEYS` or `OPT_STRICT_INTEGER` it stops at 254 levels, as orjson does.
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

Needs `gcc` and, on x86-64, `nasm` 2.16 or newer.

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

One script builds the extension, runs every test and the benchmark, and writes a single report file:

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
repeated calls do not leak.

```bash
python tests/check_dumps_orjson.py
```

Compares `dumps` with orjson byte for byte over the supported types and option combinations.

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

## Benchmarks

The numbers that matter are in [docs/benchmarks.md](docs/benchmarks.md): speed, peak memory and
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
