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

Поддерживается CPython 3.10–3.14. Для версии 0.4.0 есть готовые wheels:

| Платформа | Python |
|---|---|
| Linux x86-64 (manylinux) | 3.10–3.14 |
| macOS arm64 | 3.10–3.14 |
| Windows x86-64 | пока только **3.12** |

Wheels для musllinux пока нет: на Alpine и похожих дистрибутивах пакет собирается из исходников. На Windows
с другой версией Python соберите пакет сами (см. [Сборка](#сборка)) или дождитесь расширенного набора wheels.

## Производительность

Сравнение пути x86-64 AVX2 с `orjson` 3.13 на Python 3.12 и реальных документах. Полные таблицы с другими
библиотеками, расходом памяти и другими версиями Python — в [docs/benchmarks.md](docs/benchmarks.md).

| | oxjson | orjson |
|---|---:|---:|
| Размер после установки | **~250 КБ** | ~315 КБ |
| loads, каталог 1,8 МБ | **2,1 мс** | 4,0 мс |
| loads, записи 4,9 МБ | **30,5 мс** | 44,9 мс |
| dumps, координаты 2,2 МБ | **2,3 мс** | 3,5 мс |
| Пиковая память loads, записи 4,9 МБ | **32 МБ** | 96 МБ |

*Замеры сделаны на Windows AMD64; ядро NASM AVX2 там то же, что на Linux x86-64. С цифрами для macOS arm64
их сравнивать нельзя.*

## Состояние проекта

Проект молодой. Он проходит публичные наборы тестов на соответствие стандарту и выдаёт те же результаты,
что `orjson`, на проверках из раздела [Тесты](#тесты). GitHub Actions собирает wheels и прогоняет проверки
пакета (`.github/workflows/wheels.yml`). **Фаззинг-тестирования пока не было**, поэтому не стоит разбирать
им недоверенные данные в production, пока оно не проведено.

## Интерфейс

| | |
|---|---|
| `loads(data)` | `data` — `bytes`, `bytearray`, `memoryview` или `str`. При ошибке бросает `JSONDecodeError` (подкласс `json.JSONDecodeError`) с атрибутом `.pos`. |
| `dumps(obj, default=None, option=None)` | Возвращает `bytes`. `option` — комбинация констант `OPT_*`; имена и значения у них те же, что в `orjson`. |
| `load(fp)`, `dump(obj, fp)` | Чтение из бинарного файлового объекта и запись в него. |

`dumps` умеет сериализовать `str`, `int` (от −2^63 до 2^64−1), `float`, `bool`, `None`, `list`, `tuple`, `dict`,
dataclass, `datetime` / `date` / `time`, `enum.Enum`, `uuid.UUID`, а с `OPT_SERIALIZE_NUMPY` — ещё массивы
и скаляры numpy. Объекты остальных типов передаются в `default`.

Поддерживаемые опции: `OPT_APPEND_NEWLINE`, `OPT_INDENT_2`, `OPT_NAIVE_UTC`, `OPT_NON_STR_KEYS`,
`OPT_OMIT_MICROSECONDS`, `OPT_PASSTHROUGH_DATACLASS`, `OPT_PASSTHROUGH_DATETIME`, `OPT_PASSTHROUGH_SUBCLASS`,
`OPT_SERIALIZE_NUMPY`, `OPT_SORT_KEYS`, `OPT_STRICT_INTEGER`, `OPT_UTC_Z`.

### Отличия от orjson

- Числа с плавающей точкой записываются так же, как их печатает Python: `1e-05`, `1e+16`. orjson пишет `1e-5`,
  `1e16`. Значения при этом одинаковые.
- У `loads` нет ограничения на глубину вложенности, а orjson останавливается на 1024 уровнях. У `dumps` без опций
  форматирования ограничения тоже нет. С `OPT_INDENT_2`, `OPT_SORT_KEYS`, `OPT_NON_STR_KEYS` или
  `OPT_STRICT_INTEGER` предел — 254 уровня, как у orjson.
- Нет `orjson.Fragment`.
- `dumps(..., allow_nan=False)` бросает `ValueError` на NaN и бесконечностях, а не записывает `null`.
- `dumps` при ошибке бросает `TypeError`, а не `orjson.JSONEncodeError` (который и сам — подкласс `TypeError`).

## Платформы

| Платформа | Реализация |
|---|---|
| x86-64 с AVX2, Windows и Linux | ядро на ассемблере |
| x86-64 без AVX2 | C с 16-байтными векторами SSE2; выбирается при импорте модуля |
| ARM64 (Apple Silicon, Linux) | C с 16-байтными векторами NEON. Wheels для macOS arm64 собираются и тестируются в CI. Для Linux aarch64 пакет собирается кросс-компиляцией, но тестируется меньше: если собираетесь использовать его там в production, сначала запустите на этой машине `tests/vec16_selftest.c` и тесты. |
| Остальные | обычный C |

Поддерживается Python 3.10–3.14. Расширение использует несколько приватных функций CPython, поэтому
с выходом новой версии Python может понадобиться исправление. Free-threaded сборка и субинтерпретаторы
не поддерживаются: кэш ключей общий для всего процесса.

Чтобы принудительно включить портативную реализацию, задайте `OXJSON_NO_AVX2=1` до импорта модуля.

## Сборка

Нужен `gcc`, а на x86-64 ещё и `nasm` версии 2.16 или новее.

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

Один скрипт собирает расширение, запускает все тесты и бенчмарк и сохраняет результаты в один файл:

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
что при повторных вызовах нет утечек памяти.

```bash
python tests/check_dumps_orjson.py
```

Побайтно сравнивает вывод `dumps` с orjson для всех поддерживаемых типов и сочетаний опций.

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

## Бенчмарки

Основные результаты — в [docs/benchmarks.md](docs/benchmarks.md): скорость, пиковое потребление памяти
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
