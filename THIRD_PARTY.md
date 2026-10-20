# Third-party material

oxjson itself is MPL-2.0 (see `LICENSE`). This file lists third-party code and
data shipped in the repository and the licenses that apply to them.

## Zmij float-to-decimal digits

- Files: `include/json_f64_fmt.h` (digit conversion), table generated into
  `include/json_f64_pow10.h` by `tools/gen_pow10_table.py`
- Origin: Zmij algorithm by Victor Zverovich
  ([vitaut/zmij](https://github.com/vitaut/zmij)), via the Rust port by
  David Tolnay ([dtolnay/zmij](https://github.com/dtolnay/zmij) / crates.io `zmij`)
- License: MIT — full text in [`licenses/MIT-zmij.txt`](licenses/MIT-zmij.txt)

## Benchmark / test JSON documents in `testdata/diverse`

### From nativejson-benchmark (MIT)

These fixtures are the well-known files also shipped with
[miloyip/nativejson-benchmark](https://github.com/miloyip/nativejson-benchmark)
(`data/`), MIT License, Copyright (c) 2014 Milo Yip.

| file | notes |
|---|---|
| `canada.json` | Canada border GeoJSON |
| `citm_catalog.json` | large indented catalog |
| `twitter.json` | tweets with CJK |

Full text: [`licenses/MIT-nativejson-benchmark.txt`](licenses/MIT-nativejson-benchmark.txt).

### Redistributed via simdjson / simdjson-data (MIT)

These files are redistributed from the simdjson project's example corpora
([simdjson/simdjson](https://github.com/simdjson/simdjson) `jsonexamples/`,
and/or [simdjson/simdjson-data](https://github.com/simdjson/simdjson-data)).
simdjson is dual-licensed Apache-2.0 OR MIT; this repository redistributes the
listed documents under the MIT option.

| file |
|---|
| `apache_builds.json` |
| `gsoc-2018.json` |
| `marine_ik.json` |
| `numbers.json` |
| `twitterescaped.json` |

Full text: [`licenses/MIT-simdjson.txt`](licenses/MIT-simdjson.txt).

### Generated for this project

No third-party license. Created for oxjson tests and benches:

`scientific_exp.json`, `deep_mixed.json`, `int_only_index.json`, `unicode_mix.json`.

## Not shipped, only used as dependencies when testing

`requirements-dev.txt` pulls in orjson, msgspec, and other libraries for
comparison tests and benchmarks. Those packages are not copied into this
repository; their own licenses apply when you install them.
