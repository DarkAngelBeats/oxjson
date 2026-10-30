# 30-minute parser endurance test on farmgpu

> Historical first build. The [repeat run after fixes](../report.md) is faster than orjson in all 12 measured file/operation combinations. The results below describe the original build only.

Start: 2026-10-14T18:09:38Z  
Finish: 2026-10-14T18:39:38Z  
Duration: 1800.1 s; records: 354; errors: 0  
Host: farmgpu; Python: 3.12.3; parser: 0.4.0; orjson: 3.13.0

GC remained enabled. Each segment measures single calls; reported time is the call-weighted mean. Parser and orjson ran alternately for each file and operation.

| Document | Operation | Parser ms/op | orjson ms/op | Parser / orjson | Calls each |
|---|---|---:|---:|---:|---:|
| large_50k.json | loads | 19.2659 | 28.0785 | 0.686× | 3150 / 2260 |
| large_50k.json | dumps | 3.7163 | 4.1899 | 0.887× | 18870 / 16770 |
| canada.json | loads | 4.7485 | 4.6965 | 1.011× | 13070 / 13250 |
| canada.json | dumps | 2.0675 | 1.9124 | 1.081× | 36330 / 39270 |
| marine_ik.json | loads | 7.0965 | 9.4654 | 0.750× | 8800 / 6840 |
| marine_ik.json | dumps | 2.4991 | 2.7991 | 0.893× | 28060 / 25070 |
| numbers.json | loads | 0.1478 | 0.1392 | 1.062× | 416670 / 438630 |
| numbers.json | dumps | 0.1427 | 0.1553 | 0.919× | 524800 / 482530 |
| twitter.json | loads | 0.7760 | 0.9278 | 0.836× | 85750 / 73140 |
| twitter.json | dumps | 0.1181 | 0.1835 | 0.644× | 633210 / 408090 |
| atlas.json | loads | 0.0461 | 0.0495 | 0.932× | 1507940 / 1409300 |
| atlas.json | dumps | 0.0114 | 0.0150 | 0.761× | 6478800 / 4942090 |

## Process memory

Post-GC RSS first → last: 141.3 → 147.8 MiB.  
Maximum observed RSS: 175.4 MiB; process high-water mark: 193.8 MiB.

These values include the benchmark harness, input bytes, and reference objects. Separate fresh-process measurements are needed to attribute allocation to a single parser call.

## Errors

No parser exceptions during measured segments.
