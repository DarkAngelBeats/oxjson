# oxjson

[English](README.md) | **Русский**

Библиотека для работы с JSON в CPython с тем же интерфейсом, что у `orjson`: `loads` возвращает `dict` или `list`,
`dumps` — `bytes`. Ядро парсера написано на ассемблере x86-64 (NASM, AVX2). На остальных платформах работает
та же логика, переписанная на C: на ARM64 она использует NEON, на старых x86-64 — SSE2.

```python
import oxjson

obj = oxjson.loads(b'{"id": 1, "tags": ["a", "b"], "score": 2.5}')
data = oxjson.dumps(obj)                                   # b'{"id":1,"tags":["a","b"],"score":2.5}'
text = oxjson.dumps(obj, option=oxjson.OPT_INDENT_2 | oxjson.OPT_SORT_KEYS)
```

## Установка

```bash
pip install oxjson
```

Поддерживается CPython 3.10–3.14. Готовые wheels для Python 3.10–3.14:

| Платформа | wheel |
|---|---|
| Linux x86-64 | manylinux, musllinux |
| Linux aarch64 | manylinux, musllinux |
| macOS | arm64, x86-64 |
| Windows x86-64 | win_amd64 |

Для других платформ (Windows ARM64, 32 бита) нужна сборка из исходников, см. [Сборка](#сборка).

## Производительность

**oxjson 0.4.2** против **orjson 3.13.0** на Windows AMD64 (Ryzen 9 5900X, CPython 3.11.4): медиана семи отдельных процессов на библиотеку и документ, GC включён. oxjson быстрее в 11 сценариях из 14; в таблице показаны все три проигрыша.

Цифры сняты 29 октября 2026, до того как в 0.4.2 изменилась запись float. 30 октября запись (`dumps`) выпущенного кода сравнили с той сборкой на этой же машине и на Linux: скорость и память не изменились ([отчёт](bench/results/dumps-remeasure-20261030/report.md)); чтение (`loads`) не менялось.

| Сценарий | oxjson | orjson |
|---|---:|---:|
| loads, каталог 1,8 МБ | **2,176 мс** | 3,334 мс |
| loads, записи 4,9 МБ | **38,911 мс** | 52,784 мс |
| dumps, координаты 2,2 МБ | **1,922 мс** | 3,028 мс |
| dumps, конфиг 34 КБ | 21,69 мкс | **19,31 мкс** |
| dumps, каталог 1,8 МБ | 476,92 мкс | **401,64 мкс** |
| dumps, записи 4,9 МБ | 6,657 мс | **6,001 мс** |

[Полное сравнение и исходные данные](docs/current-comparison.md) включают диапазоны, все семь документов и 653 запуска проверки памяти. Измеряется private commit процесса, а не объём выделений парсера; ни преимущество по памяти на любых входах, ни отсутствие всех утечек не установлены. См. [методику памяти](docs/memory-testing.md).

[Исторические таблицы разных платформ](docs/benchmarks.md) сохраняют исходные версии и условия. Эти Windows-результаты не определяют скорость на Linux/macOS или изменение скорости между релизами.

## Состояние проекта

Проект молодой. В репозитории есть проверки публичных наборов соответствия и сравнения с `orjson` из раздела [Тесты](#тесты). Локально проверенный объём для текущей сборки указан в [актуальных результатах](docs/current-comparison.md). GitHub Actions собирает wheels и прогоняет проверки
пакета (`.github/workflows/wheels.yml`). Дифференциальный фаззер `tests/fuzz.py` прогнал 1,42 млн случаев (`loads` и
`dumps`, ассемблерный и портативный путь, 20 минут, Windows AMD64, Python 3.13.5, orjson 3.13.0) без находок; до
этого он нашёл одну ошибку предела глубины в `dumps`, она исправлена. Фаззер не управляется покрытием кода и долго
на Linux и macOS не запускался.
Проект остаётся экспериментальным для недоверенных данных в production.

## Интерфейс

| | |
|---|---|
| `loads(data)` | `data` — `bytes`, `bytearray`, `memoryview` или `str`. При ошибке бросает `JSONDecodeError` (подкласс `json.JSONDecodeError`) с атрибутом `.pos`. |
| `dumps(obj, default=None, option=None)` | Возвращает `bytes`. `option` — комбинация констант `OPT_*`; имена и значения у них те же, что в `orjson`. |
| `load(fp)`, `dump(obj, fp)` | Чтение из бинарного файлового объекта и запись в него. |

`dumps` умеет сериализовать `str`, `int` (от −2^63 до 2^64−1), `float`, `bool`, `None`, `list`, `tuple`, `dict`,
dataclass, `datetime` / `date` / `time`, `enum.Enum`, `uuid.UUID`, а с `OPT_SERIALIZE_NUMPY` — ещё массивы
и скаляры numpy. Неподдерживаемые значения передаются в `default`, если callback задан.

Поддерживаемые опции: `OPT_APPEND_NEWLINE`, `OPT_INDENT_2`, `OPT_NAIVE_UTC`, `OPT_NON_STR_KEYS`,
`OPT_OMIT_MICROSECONDS`, `OPT_PASSTHROUGH_DATACLASS`, `OPT_PASSTHROUGH_DATETIME`, `OPT_PASSTHROUGH_SUBCLASS`,
`OPT_SERIALIZE_NUMPY`, `OPT_SORT_KEYS`, `OPT_STRICT_INTEGER`, `OPT_UTC_Z`.

### Отличия от orjson

- У `loads` нет ограничения на глубину вложенности, а orjson останавливается на 1024 уровнях. У `dumps` без опций
  форматирования ограничения тоже нет. С `OPT_INDENT_2`, `OPT_SORT_KEYS`, `OPT_NON_STR_KEYS` или
  `OPT_STRICT_INTEGER` контейнер на глубине 254 (255-й вложенный) отклоняется, как у orjson. Кортежи здесь считаются
  контейнерами; orjson считает их непоследовательно.
- Нет `orjson.Fragment`.
- `dumps(..., allow_nan=False)` бросает `ValueError` на NaN и бесконечностях, а не записывает `null`.
- `dumps` при ошибке бросает `TypeError`, а не `orjson.JSONEncodeError` (который и сам — подкласс `TypeError`).

## Платформы

| Платформа | Реализация |
|---|---|
| x86-64 с AVX2, Windows и Linux | ядро на ассемблере |
| x86-64 без AVX2 | C с 16-байтными векторами SSE2; выбирается при импорте модуля |
| ARM64 (Apple Silicon, Linux) | C с 16-байтными векторами NEON. Wheels для macOS arm64 и Linux aarch64 собираются нативно, и на обеих платформах в CI прогоняется полный набор тестов. |
| Остальные | обычный C |

Поддерживается Python 3.10–3.14. Расширение использует несколько приватных функций CPython, поэтому
с выходом новой версии Python может понадобиться исправление. Free-threaded сборка и субинтерпретаторы
не поддерживаются: кэш ключей общий для всего процесса.

Чтобы принудительно включить портативную реализацию, задайте `OXJSON_NO_AVX2=1` до импорта модуля.

## Сборка

Нужен C-компилятор с интерфейсом GCC (GCC или Clang), а на Windows/Linux x86-64 ещё и `nasm` версии 2.16 или новее.

```bash
python make_ext.py
```

Команда собирает `oxjson/oxjson.pyd` (или `.so`) для того интерпретатора, которым запущен скрипт.
`python make_ext.py <dir>` вместо этого кладёт копию пакета в `<dir>/oxjson`.

Сборка wheel и sdist для PyPI:

```bash
python -m pip install build
python -m build
```

GitHub Actions собирает wheels при каждом push (`.github/workflows/wheels.yml`). Публикацию на PyPI
выполняет `.github/workflows/publish.yml` при появлении тега `v*`; для этого в репозитории должен быть
настроен Trusted Publishing (см. [документацию PyPI](https://docs.pypi.org/trusted-publishers/)).

## Проверка на новой машине

Один скрипт собирает расширение, запускает стандартные регрессионные проверки и бенчмарк и сохраняет результаты в один файл:

```bash
python -m pip install -r requirements-dev.txt
```

```bash
python tests/run_all.py
```

Отчёт сохраняется в корне репозитория под именем `report-<system>-<machine>-py<XY>.txt`. Результаты
бенчмарка записываются в `bench/results/` и в `docs/benchmarks.md`.

## Тесты

```bash
python tests/check_versions.py
```

Сравнивает `loads` с orjson на файлах из `testdata/` и на 30 000 случайных документов, а также проверяет,
нет ли роста памяти выше порогов проверки. Успешный конечный тест не доказывает отсутствие всех утечек.

```bash
python tests/check_dumps_orjson.py
```

Побайтно сравнивает вывод `dumps` с orjson на проверяемых типах и сочетаниях опций, включая запись дробных
(все степени, дробные ключи, numpy float64 / float32 / float16).

```bash
python tests/check_portable.py
```

Проверяет, что ассемблерная и портативная реализации дают одинаковый результат.

`tests/vec16_selftest.c` — отдельная программа на C, которая сверяет векторные функции SSE2 / NEON с обычным C.
Команда для её сборки указана в начале файла.

```bash
python tests/check_conformance.py <dir>
```

Прогоняет [JSONTestSuite](https://github.com/nst/JSONTestSuite),
[nativejson-benchmark](https://github.com/miloyip/nativejson-benchmark) и
[json_test_data](https://github.com/nlohmann/json_test_data), склонированные в `<dir>`. Нужные команды
перечислены в начале файла.

`tests/check_loads.py` и `tests/check_dumps.py` сравнивают результаты со стандартным модулем `json`
на случайных данных.

```bash
python tests/fuzz.py
```

Дифференциальный фаззер `loads` и `dumps` против orjson, на ассемблерном и портативном пути; по умолчанию одна
минута, `--seconds N` для более долгого прогона. Каждая пачка
случаев идёт в отдельном процессе; падение, зависание или результат, отличный от orjson, сохраняются в
`fuzz-out/`, и код выхода становится 1. Часть входов `loads` заканчивается прямо перед недоступной страницей
памяти, поэтому чтение за концом входа приводит к падению. `--replay` и `--replay-case` воспроизводят
сохранённую находку. CI запускает его на минуту на каждой машине при каждом push (`.github/workflows/fuzz.yml`), каждый раз с новым
зерном.

Проверки памяти с сохранением исходных измерений:

```bash
python tests/check_memory_measurements.py
python bench/memory_audit.py --output bench/results/memory-audit.json
```

См. [методику](docs/memory-testing.md). Аудит включает успешные вызовы, ошибки разбора и callback, контроль без парсера, удержание и освобождение результатов и повторные серии. Успешный прогон не доказывает отсутствие всех утечек или преимущество по памяти на любых входах. Публичные наборы соответствия и фаззер запускаются отдельными командами.

## Бенчмарки

Основные результаты — в [docs/benchmarks.md](docs/benchmarks.md): скорость, исторические приросты пиковой памяти процесса
и размер пакета в сравнении с orjson, msgspec, ssrjson, yyjson, pysimdjson, ujson, python-rapidjson
и стандартной библиотекой. Замеры сделаны на реальных документах (от десятков килобайт до нескольких мегабайт)
и на Python 3.10–3.14. В каждом разделе указано, на какой машине он снят; результаты с разных машин
сравнивать нельзя.

Микробенчмарк на крошечном объекте измеряет в основном накладные расходы на вызов функции. Как иллюстрация
он годится, но судить по нему, какая библиотека быстрее, не стоит.

```bash
python bench/bench.py
```

```bash
python bench/report.py
```

## Структура репозитория

```
src/jl_kernel.asm          ядро loads на NASM с AVX2
src/jl_loads.c             создание объектов Python для ядра; то же ядро на C
src/jl_portable.c          jl_loads.c в портативной сборке
src/json_dumps_walk.c      dumps
src/json_dumps_ext.inc     dataclass, datetime, enum, UUID, numpy и опции форматирования
src/json_dumps_portable.c  json_dumps_walk.c в портативной сборке
include/json_vec16.h       16-байтные векторные функции портативной сборки: NEON и SSE2
src/json_module.c          модуль Python
tools/                     генераторы таблиц для include/
```

## Лицензия

MPL-2.0, см. `LICENSE`. Copyright (c) 2026 Lev Berezkin.

Сторонний код и тестовые документы (алгоритм вывода чисел Zmij, JSON-фикстуры) распространяются
под собственными лицензиями MIT; подробнее — в [`THIRD_PARTY.md`](THIRD_PARTY.md) и `licenses/`.
