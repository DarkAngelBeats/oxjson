# Benchmarks

Produced by `bench/bench.py` and `bench/report.py`. Every number comes from a separate process.

- **Speed**: one call at a time, garbage collector on, mean of 10 calls, mean of three such rounds.
  `loads` takes `bytes` and returns `dict` / `list`. `dumps` writes the object that `json.loads` built.
- **Memory**: growth of the process peak memory across the first `loads` call in a fresh process,
  best of three processes. It includes the returned Python objects.
- **Disk**: size of the installed package files.
- The best value in a row is bold. `-` means the library has no such function or no wheel for that Python.

Each section names its machine; numbers from different machines are not comparable.

## Library versions

| run | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---|---|---|---|---|---|---|---|---|
| Python 3.10.18, Windows AMD64 | 0.4.0 | 3.13.0 | 0.22.0 | 0.0.24 | 4.0.6 | 7.0.2 | 6.0.0 | 1.25 | stdlib |
| Python 3.11.4, Windows AMD64 | 0.4.0 | 3.13.0 | 0.22.0 | 0.0.24 | 4.0.6 | 7.0.2 | 6.0.0 | 1.25 | stdlib |
| Python 3.12.7, Darwin arm64 | 0.4.0 | 3.13.0 | 0.22.0 | 0.0.24 | 4.0.6 | 7.0.2 | 6.0.0 | 1.25 | stdlib |
| Python 3.12.10, Windows AMD64 | 0.4.0 | 3.13.0 | 0.22.0 | 0.0.24 | 4.0.6 | 7.0.2 | 6.0.0 | 1.25 | stdlib |
| Python 3.13.5, Windows AMD64 | 0.4.0 | 3.13.0 | 0.22.0 | 0.0.24 | 4.0.6 | 7.0.2 | 6.0.0 | 1.25 | stdlib |
| Python 3.14.0rc1, Windows AMD64 | 0.4.0 | 3.13.0 | 0.22.0 | 0.0.24 | - | - | 6.0.0 | 1.25 | stdlib |

## Installed size, KB

| run | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Python 3.10.18, Windows AMD64 | 243 | 315 | 427 | 2017 | 931 | 8057 | 645 | 945 |
| Python 3.11.4, Windows AMD64 | 244 | 315 | 428 | 2013 | 931 | 8057 | 645 | 945 |
| Python 3.12.7, Darwin arm64 | 130 | 344 | 567 | 655 | 973 | 9145 | 158 | 907 |
| Python 3.12.10, Windows AMD64 | 249 | 315 | 433 | 2029 | 931 | 8063 | 645 | 947 |
| Python 3.13.5, Windows AMD64 | 254 | 315 | 433 | 2027 | 931 | 8062 | 645 | 947 |
| Python 3.14.0rc1, Windows AMD64 | 253 | 315 | 436 | 2026 | - | - | 645 | 950 |

## Python 3.10.18, Windows AMD64

CPU: AMD64 Family 25 Model 33 Stepping 0, AuthenticAMD

### loads

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | 47.0 us | 78.6 us | 63.8 us | **42.0 us** | 90.6 us | 129.6 us | 116.8 us | 193.8 us | 163.4 us |
| float array, 150 KB | **167.8 us** | 262.8 us | 229.7 us | 174.5 us | 172.6 us | 408.7 us | 503.7 us | 2.05 ms | 1.09 ms |
| API reply with text, 647 KB | **652.0 us** | 1.03 ms | 1.43 ms | 900.3 us | 1.52 ms | 2.18 ms | 2.28 ms | 3.90 ms | 3.43 ms |
| catalog, 1.8 MB | **2.42 ms** | 3.49 ms | 4.10 ms | 3.25 ms | 4.41 ms | 5.22 ms | 5.40 ms | 9.02 ms | 8.64 ms |
| coordinates, 2.2 MB | **7.49 ms** | 8.68 ms | 9.63 ms | 7.79 ms | 8.79 ms | 11.11 ms | 13.65 ms | 47.37 ms | 32.12 ms |
| 3D mesh, 3.0 MB | **8.69 ms** | 10.18 ms | 11.04 ms | 9.22 ms | 13.99 ms | 16.44 ms | 16.03 ms | 31.88 ms | 24.52 ms |
| uniform records, 4.9 MB | **47.26 ms** | 57.78 ms | 57.47 ms | 58.11 ms | 76.67 ms | 75.00 ms | 68.68 ms | 75.50 ms | 65.39 ms |

### dumps

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | 23.1 us | 19.9 us | 24.6 us | **15.7 us** | 41.8 us | - | 94.4 us | 119.6 us | 209.6 us |
| float array, 150 KB | 187.7 us | 249.7 us | 352.6 us | **181.3 us** | 420.3 us | - | 762.0 us | 3.05 ms | 2.94 ms |
| API reply with text, 647 KB | **168.2 us** | 294.9 us | 425.9 us | 304.8 us | 1.22 ms | - | 1.74 ms | 2.00 ms | 3.21 ms |
| catalog, 1.8 MB | 533.3 us | **491.0 us** | 701.1 us | 539.0 us | 1.63 ms | - | 2.55 ms | 2.79 ms | 6.22 ms |
| coordinates, 2.2 MB | **2.26 ms** | 2.88 ms | 4.54 ms | 2.90 ms | 5.70 ms | - | 11.49 ms | 43.26 ms | 45.34 ms |
| 3D mesh, 3.0 MB | 3.68 ms | 5.07 ms | 6.90 ms | **3.42 ms** | 7.71 ms | - | 12.13 ms | 34.05 ms | 40.23 ms |
| uniform records, 4.9 MB | 6.61 ms | 6.63 ms | 9.15 ms | **6.60 ms** | 20.91 ms | - | 26.47 ms | 31.23 ms | 63.04 ms |

### Peak memory growth in loads, MB

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** |
| float array, 150 KB | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** |
| API reply with text, 647 KB | **0.0** | 4.8 | **0.0** | 0.7 | **0.0** | 8.5 | 0.9 | 1.3 | 2.5 |
| catalog, 1.8 MB | **1.5** | 21.8 | 1.9 | 10.3 | 6.3 | 28.0 | 10.1 | 8.3 | 4.1 |
| coordinates, 2.2 MB | **6.4** | 31.9 | 8.2 | 17.2 | 14.2 | 37.9 | 15.7 | 12.1 | 9.6 |
| 3D mesh, 3.0 MB | 11.8 | 44.8 | **10.8** | 25.9 | 22.3 | 55.9 | 25.0 | 18.3 | 13.5 |
| uniform records, 4.9 MB | **37.0** | 102.1 | 46.3 | 69.8 | 90.5 | 120.2 | 71.9 | 46.5 | 41.0 |

### Peak memory growth in dumps, MB

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | - | **0.0** | **0.0** | **0.0** |
| float array, 150 KB | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | - | **0.0** | **0.0** | **0.0** |
| API reply with text, 647 KB | **0.0** | **0.0** | **0.0** | **0.0** | 2.4 | - | **0.0** | **0.0** | 1.4 |
| catalog, 1.8 MB | **0.0** | **0.0** | **0.0** | **0.0** | 1.3 | - | **0.0** | **0.0** | **0.0** |
| coordinates, 2.2 MB | **0.2** | 5.0 | 3.3 | 1.0 | 9.4 | - | 1.7 | 2.4 | 4.4 |
| 3D mesh, 3.0 MB | **0.0** | 15.6 | 0.7 | **0.0** | 12.8 | - | **0.0** | 0.2 | 2.4 |
| uniform records, 4.9 MB | 7.1 | **2.9** | 3.2 | 6.6 | 34.6 | - | 7.2 | 4.9 | 5.3 |

## Python 3.11.4, Windows AMD64

CPU: AMD64 Family 25 Model 33 Stepping 0, AuthenticAMD

### loads

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | **43.2 us** | 53.9 us | 63.9 us | 43.5 us | 88.6 us | 132.1 us | 112.2 us | 194.2 us | 153.7 us |
| float array, 150 KB | **147.8 us** | 234.8 us | 251.8 us | 187.2 us | 230.1 us | 394.9 us | 495.6 us | 1.90 ms | 964.9 us |
| API reply with text, 647 KB | **805.4 us** | 1.17 ms | 1.23 ms | 887.9 us | 1.35 ms | 2.07 ms | 2.10 ms | 3.66 ms | 3.32 ms |
| catalog, 1.8 MB | **2.27 ms** | 3.40 ms | 3.73 ms | 2.90 ms | 4.22 ms | 4.77 ms | 5.35 ms | 8.63 ms | 7.65 ms |
| coordinates, 2.2 MB | **8.06 ms** | 8.95 ms | 9.89 ms | 8.69 ms | 8.58 ms | 11.34 ms | 13.98 ms | 48.32 ms | 31.14 ms |
| 3D mesh, 3.0 MB | **8.48 ms** | 9.44 ms | 11.14 ms | 9.54 ms | 13.31 ms | 16.31 ms | 15.54 ms | 32.13 ms | 23.79 ms |
| uniform records, 4.9 MB | **40.60 ms** | 53.28 ms | 57.00 ms | 51.01 ms | 71.86 ms | 69.49 ms | 64.76 ms | 71.80 ms | 57.61 ms |

### dumps

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | 29.0 us | 19.8 us | 22.3 us | **18.0 us** | 39.5 us | - | 90.5 us | 120.1 us | 213.5 us |
| float array, 150 KB | **164.0 us** | 244.0 us | 345.7 us | 179.0 us | 393.6 us | - | 711.4 us | 3.26 ms | 2.89 ms |
| API reply with text, 647 KB | 229.9 us | 222.5 us | 303.5 us | **211.4 us** | 1.36 ms | - | 1.69 ms | 1.99 ms | 3.29 ms |
| catalog, 1.8 MB | 459.3 us | 393.7 us | 532.7 us | **324.2 us** | 1.55 ms | - | 2.52 ms | 3.08 ms | 6.66 ms |
| coordinates, 2.2 MB | **2.21 ms** | 3.04 ms | 4.34 ms | 2.71 ms | 5.39 ms | - | 10.72 ms | 44.43 ms | 46.05 ms |
| 3D mesh, 3.0 MB | 3.74 ms | 4.89 ms | 6.60 ms | **3.57 ms** | 7.86 ms | - | 12.49 ms | 33.38 ms | 40.21 ms |
| uniform records, 4.9 MB | 6.65 ms | 6.02 ms | 8.92 ms | **5.97 ms** | 19.50 ms | - | 27.66 ms | 27.96 ms | 60.53 ms |

### Peak memory growth in loads, MB

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | **0.0** | 0.4 | **0.0** | **0.0** | 0.2 | 0.4 | 0.1 | **0.0** | 0.0 |
| float array, 150 KB | 0.2 | 1.7 | **0.0** | 0.8 | 0.5 | 2.2 | 0.2 | 0.3 | 0.2 |
| API reply with text, 647 KB | **0.4** | 7.6 | 1.3 | 4.4 | 2.5 | 10.3 | 3.7 | 5.0 | 4.3 |
| catalog, 1.8 MB | **3.0** | 23.4 | **3.0** | 11.5 | 8.4 | 29.9 | 11.8 | 10.2 | 6.4 |
| coordinates, 2.2 MB | **7.4** | 32.9 | 9.3 | 19.1 | 16.2 | 39.9 | 17.1 | 13.5 | 10.7 |
| 3D mesh, 3.0 MB | 12.6 | 45.8 | **11.4** | 27.1 | 22.5 | 55.6 | 25.1 | 17.0 | 15.0 |
| uniform records, 4.9 MB | **33.1** | 97.8 | 42.5 | 65.5 | 87.0 | 116.4 | 66.3 | 41.9 | 37.2 |

### Peak memory growth in dumps, MB

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | 0.0 | **0.0** | **0.0** | **0.0** | 0.1 | - | 0.0 | **0.0** | 0.1 |
| float array, 150 KB | 0.4 | 2.0 | 0.2 | 0.3 | 0.5 | - | 0.3 | **0.2** | 1.3 |
| API reply with text, 647 KB | **0.0** | **0.0** | **0.0** | **0.0** | 3.0 | - | **0.0** | **0.0** | 1.5 |
| catalog, 1.8 MB | **0.0** | **0.0** | **0.0** | **0.0** | 1.5 | - | **0.0** | **0.0** | 1.2 |
| coordinates, 2.2 MB | 1.4 | 4.0 | 2.5 | **1.3** | 8.4 | - | 2.4 | 3.9 | 4.8 |
| 3D mesh, 3.0 MB | **0.0** | 17.3 | 0.5 | **0.0** | 13.3 | - | 1.3 | 1.4 | 4.2 |
| uniform records, 4.9 MB | 7.7 | 3.6 | **2.8** | 7.7 | 35.3 | - | 8.1 | 6.4 | 7.4 |

## Python 3.12.7, Darwin arm64

CPU: arm

### loads

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | 40.6 us | 41.2 us | 42.2 us | **40.5 us** | 54.9 us | 1.28 ms | 56.0 us | 112.8 us | 105.9 us |
| float array, 150 KB | **111.7 us** | 129.2 us | 140.4 us | 130.5 us | 140.0 us | 4.01 ms | 323.4 us | 445.8 us | 470.0 us |
| API reply with text, 647 KB | 605.1 us | 626.0 us | 689.9 us | **561.4 us** | 628.5 us | 22.93 ms | 985.9 us | 2.01 ms | 1.49 ms |
| catalog, 1.8 MB | **1.60 ms** | 1.84 ms | 1.77 ms | 1.70 ms | 1.61 ms | 46.39 ms | 2.08 ms | 3.98 ms | 3.12 ms |
| coordinates, 2.2 MB | 3.84 ms | 3.98 ms | 4.79 ms | 3.56 ms | **3.04 ms** | 73.27 ms | 6.90 ms | 15.55 ms | 15.22 ms |
| 3D mesh, 3.0 MB | 5.19 ms | 5.54 ms | 5.41 ms | 5.21 ms | **5.03 ms** | 107.48 ms | 7.62 ms | 11.47 ms | 11.63 ms |
| uniform records, 4.9 MB | 19.77 ms | 19.19 ms | 17.51 ms | 21.59 ms | **16.66 ms** | 320.42 ms | 19.41 ms | 29.72 ms | 29.00 ms |

### dumps

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | 10.9 us | 12.4 us | 13.4 us | **7.3 us** | 23.5 us | - | 50.4 us | 74.3 us | 100.0 us |
| float array, 150 KB | 109.3 us | 182.0 us | 210.0 us | **72.6 us** | 138.1 us | - | 556.8 us | 2.04 ms | 1.90 ms |
| API reply with text, 647 KB | 105.7 us | 155.7 us | 203.8 us | **91.4 us** | 472.0 us | - | 700.0 us | 908.3 us | 1.39 ms |
| catalog, 1.8 MB | 227.0 us | 274.5 us | 304.3 us | **214.5 us** | 560.4 us | - | 1.28 ms | 1.40 ms | 2.39 ms |
| coordinates, 2.2 MB | 1.66 ms | 2.08 ms | 2.11 ms | **1.22 ms** | 2.27 ms | - | 8.62 ms | 27.87 ms | 28.26 ms |
| 3D mesh, 3.0 MB | 1.96 ms | 3.02 ms | 4.13 ms | **1.56 ms** | 3.06 ms | - | 8.17 ms | 21.32 ms | 22.15 ms |
| uniform records, 4.9 MB | 3.01 ms | 3.49 ms | 5.01 ms | **2.83 ms** | 7.16 ms | - | 14.46 ms | 14.45 ms | 25.49 ms |

### Peak memory growth in loads, MB

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | 0.0 | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** |
| float array, 150 KB | 0.3 | 0.3 | 0.0 | 0.3 | 0.2 | 0.7 | **0.0** | 0.4 | 0.4 |
| API reply with text, 647 KB | 1.2 | 1.9 | **0.9** | 1.6 | 2.5 | 3.2 | 1.5 | 4.9 | 3.2 |
| catalog, 1.8 MB | 3.3 | 5.7 | **3.1** | 4.8 | 7.1 | 7.9 | 4.4 | 11.6 | 6.8 |
| coordinates, 2.2 MB | **8.1** | 12.3 | 8.4 | 9.7 | 12.3 | 13.8 | 8.5 | 12.8 | 10.9 |
| 3D mesh, 3.0 MB | 11.9 | 18.1 | **10.5** | 14.3 | 20.1 | 22.8 | 12.3 | 16.4 | 13.9 |
| uniform records, 4.9 MB | **32.2** | 56.5 | 38.1 | 43.6 | 85.1 | 68.0 | 43.0 | 41.0 | 36.5 |

### Peak memory growth in dumps, MB

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | - | **0.0** | **0.0** | **0.0** |
| float array, 150 KB | **0.0** | **0.0** | **0.0** | **0.0** | 0.0 | - | **0.0** | 0.1 | 0.1 |
| API reply with text, 647 KB | 0.5 | **0.3** | 0.3 | **0.3** | 2.5 | - | 0.8 | 0.9 | 1.6 |
| catalog, 1.8 MB | 0.4 | **0.2** | 0.2 | 0.2 | 2.2 | - | 0.7 | 0.7 | 1.4 |
| coordinates, 2.2 MB | 2.0 | **1.7** | 1.8 | 1.8 | 7.5 | - | 1.8 | 1.8 | 1.9 |
| 3D mesh, 3.0 MB | 1.7 | 1.6 | 1.5 | 1.5 | 10.7 | - | 3.2 | **1.5** | 1.7 |
| uniform records, 4.9 MB | 4.7 | 4.4 | 1.1 | **0.5** | 27.1 | - | 4.4 | 9.1 | 9.3 |

## Python 3.12.10, Windows AMD64

CPU: AMD64 Family 25 Model 33 Stepping 0, AuthenticAMD

### loads

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | 53.6 us | 97.2 us | 89.0 us | **44.5 us** | 87.3 us | 155.8 us | 117.0 us | 213.4 us | 181.0 us |
| float array, 150 KB | **128.0 us** | 288.8 us | 313.8 us | 214.0 us | 203.9 us | 434.9 us | 562.6 us | 2.00 ms | 1.15 ms |
| API reply with text, 647 KB | **994.2 us** | 1.33 ms | 1.43 ms | 1.22 ms | 1.49 ms | 2.38 ms | 2.90 ms | 4.46 ms | 3.87 ms |
| catalog, 1.8 MB | **2.10 ms** | 3.96 ms | 3.42 ms | 2.82 ms | 3.04 ms | 4.83 ms | 6.00 ms | 10.22 ms | 9.08 ms |
| coordinates, 2.2 MB | 6.34 ms | 9.02 ms | 8.97 ms | **5.90 ms** | 6.24 ms | 10.25 ms | 13.87 ms | 52.33 ms | 36.69 ms |
| 3D mesh, 3.0 MB | **7.59 ms** | 11.53 ms | 11.71 ms | 9.75 ms | 11.66 ms | 16.48 ms | 15.05 ms | 32.52 ms | 26.26 ms |
| uniform records, 4.9 MB | **30.51 ms** | 44.88 ms | 41.43 ms | 36.70 ms | 44.78 ms | 57.78 ms | 48.05 ms | 60.18 ms | 59.78 ms |

### dumps

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | 15.7 us | 18.5 us | 24.1 us | **15.7 us** | 43.0 us | - | 92.1 us | 128.5 us | 161.9 us |
| float array, 150 KB | **142.8 us** | 256.8 us | 379.5 us | 194.5 us | 482.9 us | - | 800.0 us | 3.88 ms | 3.55 ms |
| API reply with text, 647 KB | 259.9 us | 242.5 us | 522.0 us | **201.2 us** | 1.39 ms | - | 2.40 ms | 2.29 ms | 3.80 ms |
| catalog, 1.8 MB | 443.9 us | 454.6 us | 842.4 us | **341.8 us** | 1.59 ms | - | 3.02 ms | 2.99 ms | 5.25 ms |
| coordinates, 2.2 MB | **2.28 ms** | 3.54 ms | 4.75 ms | 3.28 ms | 6.41 ms | - | 12.72 ms | 55.35 ms | 51.77 ms |
| 3D mesh, 3.0 MB | **2.99 ms** | 5.54 ms | 8.20 ms | 3.77 ms | 9.34 ms | - | 13.00 ms | 40.12 ms | 40.38 ms |
| uniform records, 4.9 MB | **6.19 ms** | 7.01 ms | 9.41 ms | 6.83 ms | 24.39 ms | - | 29.85 ms | 32.55 ms | 58.42 ms |

### Peak memory growth in loads, MB

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | **0.0** | 0.3 | **0.0** | **0.0** | 1.1 | 0.5 | 0.1 | **0.0** | 0.0 |
| float array, 150 KB | 0.2 | 2.7 | **0.1** | 1.8 | 1.5 | 3.2 | 1.4 | 1.5 | 0.3 |
| API reply with text, 647 KB | 0.4 | 8.4 | **0.3** | 4.5 | 3.5 | 11.3 | 4.7 | 5.0 | 3.7 |
| catalog, 1.8 MB | **3.0** | 24.4 | **3.0** | 12.5 | 8.4 | 29.9 | 11.8 | 10.8 | 6.4 |
| coordinates, 2.2 MB | **7.4** | 33.8 | 8.4 | 19.1 | 16.1 | 39.8 | 18.2 | 13.6 | 10.5 |
| 3D mesh, 3.0 MB | 12.5 | 45.5 | **12.2** | 27.0 | 22.5 | 55.6 | 25.2 | 17.6 | 14.6 |
| uniform records, 4.9 MB | **32.1** | 95.5 | 38.6 | 63.5 | 79.0 | 112.4 | 62.5 | 42.1 | 36.3 |

### Peak memory growth in dumps, MB

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | **0.0** | **0.0** | **0.0** | **0.0** | 0.1 | - | 0.0 | **0.0** | **0.0** |
| float array, 150 KB | 0.4 | 2.0 | **0.2** | 0.4 | 0.6 | - | 0.3 | 0.3 | 0.3 |
| API reply with text, 647 KB | **0.0** | **0.0** | **0.0** | **0.0** | 2.2 | - | **0.0** | **0.0** | 0.8 |
| catalog, 1.8 MB | **0.0** | **0.0** | **0.0** | **0.0** | 2.3 | - | **0.0** | **0.0** | 0.3 |
| coordinates, 2.2 MB | **1.3** | 4.0 | 2.6 | 1.3 | 9.0 | - | 2.3 | 3.0 | 3.1 |
| 3D mesh, 3.0 MB | 1.3 | 17.2 | 2.2 | 0.1 | 13.6 | - | **0.1** | 1.1 | 1.3 |
| uniform records, 4.9 MB | 7.3 | **3.5** | 3.7 | 7.2 | 34.8 | - | 8.1 | 6.1 | 5.8 |

## Python 3.13.5, Windows AMD64

CPU: AMD64 Family 25 Model 33 Stepping 0, AuthenticAMD

### loads

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | **43.8 us** | 56.4 us | 74.7 us | 52.2 us | 92.1 us | 134.2 us | 116.0 us | 207.8 us | 194.5 us |
| float array, 150 KB | **145.4 us** | 276.7 us | 244.7 us | 177.4 us | 205.7 us | 395.9 us | 549.1 us | 1.99 ms | 1.15 ms |
| API reply with text, 647 KB | **853.0 us** | 1.04 ms | 1.42 ms | 931.4 us | 1.31 ms | 2.18 ms | 2.05 ms | 3.89 ms | 3.69 ms |
| catalog, 1.8 MB | **1.89 ms** | 3.09 ms | 3.04 ms | 2.40 ms | 3.18 ms | 4.14 ms | 4.50 ms | 7.87 ms | 7.88 ms |
| coordinates, 2.2 MB | **5.21 ms** | 6.68 ms | 8.17 ms | 5.70 ms | 5.70 ms | 9.29 ms | 11.74 ms | 47.56 ms | 32.61 ms |
| 3D mesh, 3.0 MB | **7.11 ms** | 9.00 ms | 9.84 ms | 7.61 ms | 9.95 ms | 14.01 ms | 12.78 ms | 30.04 ms | 24.41 ms |
| uniform records, 4.9 MB | **27.78 ms** | 34.75 ms | 34.96 ms | 31.34 ms | 39.65 ms | 47.50 ms | 40.97 ms | 54.74 ms | 48.35 ms |

### dumps

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | **14.7 us** | 16.6 us | 24.1 us | 15.6 us | 43.7 us | - | 90.7 us | 118.8 us | 149.4 us |
| float array, 150 KB | **150.3 us** | 255.6 us | 338.6 us | 180.5 us | 388.4 us | - | 722.7 us | 3.93 ms | 3.31 ms |
| API reply with text, 647 KB | 152.8 us | 275.6 us | 312.4 us | **139.3 us** | 1.19 ms | - | 1.80 ms | 1.77 ms | 3.18 ms |
| catalog, 1.8 MB | 439.8 us | 449.8 us | 785.5 us | **413.6 us** | 1.49 ms | - | 2.60 ms | 2.83 ms | 4.49 ms |
| coordinates, 2.2 MB | **2.39 ms** | 3.03 ms | 4.29 ms | 2.95 ms | 5.49 ms | - | 10.55 ms | 49.71 ms | 48.12 ms |
| 3D mesh, 3.0 MB | **3.00 ms** | 4.95 ms | 6.99 ms | 3.21 ms | 7.83 ms | - | 12.06 ms | 36.29 ms | 37.27 ms |
| uniform records, 4.9 MB | 5.99 ms | 6.11 ms | 9.36 ms | **5.68 ms** | 19.33 ms | - | 25.86 ms | 26.98 ms | 45.27 ms |

### Peak memory growth in loads, MB

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | **0.0** | **0.0** | **0.0** | **0.0** | 0.1 | 0.5 | 0.1 | **0.0** | 0.0 |
| float array, 150 KB | 0.2 | 1.8 | 0.1 | 0.8 | 0.5 | 2.2 | **0.0** | 0.4 | 0.3 |
| API reply with text, 647 KB | 1.4 | 8.4 | **1.3** | 4.4 | 2.4 | 10.4 | 3.7 | 4.9 | 3.8 |
| catalog, 1.8 MB | 3.0 | 23.4 | **3.0** | 11.5 | 8.4 | 29.9 | 11.9 | 10.2 | 6.4 |
| coordinates, 2.2 MB | **8.4** | 33.8 | 9.4 | 19.1 | 16.2 | 39.9 | 17.1 | 13.7 | 11.6 |
| 3D mesh, 3.0 MB | 12.6 | 45.6 | **11.3** | 27.0 | 22.5 | 55.6 | 25.6 | 17.1 | 14.4 |
| uniform records, 4.9 MB | **33.1** | 94.5 | 38.5 | 62.5 | 78.0 | 112.4 | 62.4 | 41.1 | 36.6 |

### Peak memory growth in dumps, MB

| document | oxjson | orjson | msgspec | ssrjson | yyjson | pysimdjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | 0.1 | **0.0** | **0.0** | **0.0** | 0.1 | - | 0.0 | **0.0** | 0.0 |
| float array, 150 KB | 0.4 | 2.0 | **0.3** | 0.4 | 0.7 | - | **0.3** | 0.3 | 0.3 |
| API reply with text, 647 KB | **0.0** | **0.0** | **0.0** | **0.0** | 3.0 | - | **0.0** | **0.0** | 0.7 |
| catalog, 1.8 MB | **0.0** | **0.0** | **0.0** | **0.0** | 1.2 | - | **0.0** | **0.0** | **0.0** |
| coordinates, 2.2 MB | 1.3 | 4.9 | 2.5 | **1.2** | 9.8 | - | 4.2 | 3.9 | 3.1 |
| 3D mesh, 3.0 MB | 0.3 | 16.2 | 0.8 | **0.0** | 13.9 | - | 0.2 | 0.4 | 0.8 |
| uniform records, 4.9 MB | 8.7 | **3.5** | 4.7 | 7.2 | 34.9 | - | 8.3 | 6.1 | 7.0 |

## Python 3.14.0rc1, Windows AMD64

CPU: AMD64 Family 25 Model 33 Stepping 0, AuthenticAMD

### loads

| document | oxjson | orjson | msgspec | ssrjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | 53.5 us | 58.5 us | 71.3 us | **44.6 us** | 112.9 us | 229.0 us | 171.9 us |
| float array, 150 KB | **146.7 us** | 279.1 us | 259.6 us | 220.3 us | 516.8 us | 1.85 ms | 1.02 ms |
| API reply with text, 647 KB | **774.1 us** | 1.22 ms | 1.20 ms | 788.8 us | 2.13 ms | 3.75 ms | 3.43 ms |
| catalog, 1.8 MB | **1.64 ms** | 2.71 ms | 2.71 ms | 2.18 ms | 4.30 ms | 8.33 ms | 7.55 ms |
| coordinates, 2.2 MB | 5.69 ms | 6.91 ms | 7.53 ms | **5.18 ms** | 11.70 ms | 47.04 ms | 30.02 ms |
| 3D mesh, 3.0 MB | **6.47 ms** | 8.85 ms | 8.82 ms | 7.65 ms | 13.12 ms | 29.51 ms | 24.12 ms |
| uniform records, 4.9 MB | **24.67 ms** | 33.63 ms | 30.70 ms | 29.06 ms | 41.35 ms | 51.34 ms | 45.60 ms |

### dumps

| document | oxjson | orjson | msgspec | ssrjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | 23.4 us | 17.6 us | 24.6 us | **12.4 us** | 99.2 us | 133.9 us | 138.0 us |
| float array, 150 KB | **150.9 us** | 245.6 us | 358.8 us | 183.6 us | 681.3 us | 3.51 ms | 3.19 ms |
| API reply with text, 647 KB | 251.1 us | 385.1 us | 323.0 us | **164.4 us** | 1.76 ms | 1.64 ms | 3.01 ms |
| catalog, 1.8 MB | 460.8 us | 466.3 us | 833.4 us | **331.2 us** | 2.76 ms | 2.82 ms | 4.59 ms |
| coordinates, 2.2 MB | **2.39 ms** | 3.26 ms | 4.34 ms | 2.76 ms | 11.23 ms | 50.56 ms | 48.60 ms |
| 3D mesh, 3.0 MB | **3.06 ms** | 4.93 ms | 6.93 ms | 3.33 ms | 12.21 ms | 36.53 ms | 35.93 ms |
| uniform records, 4.9 MB | **5.94 ms** | 6.25 ms | 8.86 ms | 5.96 ms | 27.71 ms | 28.54 ms | 46.14 ms |

### Peak memory growth in loads, MB

| document | oxjson | orjson | msgspec | ssrjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** |
| float array, 150 KB | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** |
| API reply with text, 647 KB | **0.0** | 4.5 | **0.0** | **0.0** | **0.0** | 1.3 | 1.4 |
| catalog, 1.8 MB | 1.7 | 20.7 | **0.0** | 7.9 | 7.7 | 7.8 | 4.8 |
| coordinates, 2.2 MB | 6.7 | 31.5 | **6.1** | 16.3 | 14.4 | 10.6 | 9.1 |
| 3D mesh, 3.0 MB | 10.2 | 43.1 | **9.2** | 24.8 | 22.9 | 15.1 | 12.6 |
| uniform records, 4.9 MB | **31.0** | 92.7 | 37.3 | 60.3 | 60.8 | 40.1 | 35.5 |

### Peak memory growth in dumps, MB

| document | oxjson | orjson | msgspec | ssrjson | ujson | rapidjson | json |
|---|---:|---:|---:|---:|---:|---:|---:|
| small config, 34 KB | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** |
| float array, 150 KB | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** |
| API reply with text, 647 KB | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** |
| catalog, 1.8 MB | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | **0.0** | 0.1 |
| coordinates, 2.2 MB | **0.2** | 3.3 | 3.5 | 1.7 | 1.7 | 2.7 | 2.6 |
| 3D mesh, 3.0 MB | **0.0** | 16.2 | 3.3 | **0.0** | 0.6 | 0.5 | 1.4 |
| uniform records, 4.9 MB | 7.4 | **3.9** | 4.7 | 7.0 | 10.0 | 6.4 | 5.6 |
