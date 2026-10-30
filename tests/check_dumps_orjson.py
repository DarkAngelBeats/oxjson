# -*- coding: utf-8 -*-
"""dumps of oxjson against orjson.dumps: same bytes, or both refuse.

    python tests/check_dumps_orjson.py [dir that holds the oxjson package]

Covers the types and OPT_* options orjson has beyond plain JSON values.
Float text is compared byte for byte too, every exponent included (check_float_text).
"""
import dataclasses
import datetime as dt
import enum
import itertools
import random
import struct
import sys
import uuid
from pathlib import Path
from typing import ClassVar

sys.path.insert(0, str(Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]))
import oxjson as jp  # noqa: E402
import orjson  # noqa: E402

try:
    import numpy as np
except ImportError:
    np = None

try:
    from zoneinfo import ZoneInfo
except ImportError:
    ZoneInfo = None


class Color(enum.Enum):
    RED = 1
    NAME = "name"
    NONE = None
    PAIR = (1, 2)


class IntE(enum.IntEnum):
    A = 7


class StrE(str, enum.Enum):
    A = "a"


class Flag(enum.Flag):
    X = 1
    Y = 2


@dataclasses.dataclass
class Point:
    x: int
    y: float
    tag: str = "p"
    _hidden: int = 5
    kind: ClassVar[str] = "cls"


@dataclasses.dataclass
class Nested:
    p: Point
    items: list
    when: dt.datetime
    color: Color
    opt: object = None


@dataclasses.dataclass(frozen=True)
class Frozen:
    a: int


if sys.version_info >= (3, 10):
    @dataclasses.dataclass(slots=True)
    class Slotted:
        a: int
        b: str
else:
    Slotted = Frozen


class MyStr(str):
    pass


class MyInt(int):
    pass


class MyList(list):
    pass


class MyDict(dict):
    pass


class MyFloat(float):
    pass


class Odd:
    pass


def default(o):
    if isinstance(o, Odd):
        return {"odd": True}
    if isinstance(o, (set, frozenset)):
        return sorted(o)
    if isinstance(o, complex):
        return [o.real, o.imag]
    raise TypeError


UTC = dt.timezone.utc
TZ = [None, UTC, dt.timezone(dt.timedelta(hours=5, minutes=30)), dt.timezone(dt.timedelta(hours=-8)),
      dt.timezone(dt.timedelta(0))]
# offsets with seconds (old local mean times): rounded to the minute
TZ += [dt.timezone(dt.timedelta(seconds=s)) for s in (1, 29, 30, 1049, 1050, 18059, 21570, 86399, -1, -29, -30, -1050, -21570, -86399)]
if ZoneInfo:
    try:
        TZ += [ZoneInfo("Europe/Amsterdam"), ZoneInfo("UTC")]
    except Exception:
        pass


def cases():
    out = []
    for tz in TZ:
        out.append(dt.datetime(2024, 2, 29, 13, 5, 9, 0, tzinfo=tz))
        out.append(dt.datetime(1, 1, 1, 0, 0, 0, 123456, tzinfo=tz))
        out.append(dt.datetime(9999, 12, 31, 23, 59, 59, 999999, tzinfo=tz))
        out.append(dt.datetime(2000, 1, 2, 3, 4, 5, 6, tzinfo=tz))
    out += [dt.date(2024, 2, 29), dt.date(1, 1, 1), dt.date(9999, 12, 31), dt.time(0, 0, 0), dt.time(23, 59, 59, 999999),
            dt.time(1, 2, 3, 40), dt.time(12, 0, tzinfo=UTC), dt.timedelta(seconds=5)]
    out += [uuid.UUID(int=0), uuid.UUID("12345678-1234-5678-1234-567812345678"), uuid.uuid5(uuid.NAMESPACE_DNS, "x")]
    out += [Color.RED, Color.NAME, Color.NONE, Color.PAIR, IntE.A, StrE.A, Flag.X, Flag.X | Flag.Y]
    p = Point(1, 2.5)
    out += [p, Nested(p, [p, Color.RED, uuid.UUID(int=5)], dt.datetime(2020, 5, 17, tzinfo=UTC), Color.NAME, {"k": p}),
            Frozen(3), Slotted(1, "s") if Slotted is not Frozen else Frozen(4), Point, [p] * 3, {"p": p}]
    out += [MyStr("sub"), MyInt(42), MyList([1, 2]), MyDict(a=1), MyFloat(1.5), (1, 2), (), [(1, (2, 3))],
            {"t": (1, "a", None)}]
    out += [Odd(), [Odd(), {1, 2, 3}], {"a": Odd()}, complex(1, 2), {1, 2}, frozenset([3]), b"bytes", bytearray(b"x"),
            object(), range(3), memoryview(b"m")]
    out += [None, True, False, 0, -1, 2 ** 53, 2 ** 53 - 1, -(2 ** 53), 2 ** 63, 2 ** 64 - 1, 2 ** 64, -(2 ** 63), -(2 ** 63) - 1,
            1.5, -0.0, 0.1, 123456.789, float("nan"), float("inf"), float("-inf"), "", "a", "é中😀", 'q"\\\n\x01', "\ud800"]
    out += [[], {}, [[]], {"a": {}}, [1, [2, [3, [4, []]]]], {"b": 1, "a": 2, "aa": 3, "A": 4, "é": 5, "": 6},
            {"z": {"y": [1, {"x": None}], "b": "t"}, "a": [True, 1.25]}, [{"k": i, "v": [i, str(i)]} for i in range(5)]]
    out += [{1: "int"}, {2.5: "float"}, {True: 1, False: 0}, {None: 0}, {1: "a", "1": "b"}, {(1, 2): 3},
            {dt.date(2020, 1, 1): 1}, {dt.datetime(2020, 1, 1, 12, tzinfo=UTC): 1}, {dt.time(1, 2): 1},
            {uuid.UUID(int=1): 1}, {Color.RED: 1, Color.NAME: 2}, {IntE.A: 1}, {StrE.A: 1}, {float("nan"): 1},
            {2 ** 64: 1}, {2 ** 63: 1}, {-(2 ** 63): 1}, {b"b": 1}, {Odd(): 1}, {3: 1, 1: 2, 2: 3, "10": 4, 10: 5},
            {"b": {2: [dt.date(2020, 1, 1)], 1: None}, "a": {True: Color.RED}}, {MyStr("ms"): 1}, {MyInt(3): 1}]
    cyc = []
    cyc.append(cyc)
    deep = cur = []
    for _ in range(300):
        nxt = []
        cur.append(nxt)
        cur = nxt
    out += [cyc, deep]
    if np is not None:
        out += [np.array([1, 2, 3]), np.array([[1, 2], [3, 4]], dtype=np.int8), np.array([], dtype=np.float64),
                np.array([1.5, 2.25, -0.5]), np.array([0.1, 0.25, 3.5], dtype=np.float32), np.array([True, False]),
                np.array([1, 2], dtype=np.uint64), np.array([2 ** 63], dtype=np.uint64), np.zeros((2, 0, 3)),
                np.arange(24, dtype=np.int16).reshape(2, 3, 4), np.arange(6).reshape(2, 3).T, np.array(["a", "b"]),
                np.array([1 + 2j]), np.array(5), np.int64(7), np.int8(-3), np.uint16(9), np.float64(2.5), np.float32(0.1),
                np.bool_(True), np.array([1.5], dtype=np.float16), {"a": np.array([1, 2]), "n": np.int32(4)},
                np.array(["2021-01-01T00:00:00", "2021-06-01T12:30:00.5"], dtype="datetime64[ms]"),
                np.datetime64("2021-01-01T00:00:00.000001"), np.array([None, 1], dtype=object)]
    return out


def run(m, obj, **kw):
    try:
        return ("ok", m.dumps(obj, **kw))
    except RecursionError:
        return ("err", "RecursionError")
    except Exception as e:
        return ("err", type(e).__name__)


def check_float_text():
    """Float text must match orjson byte for byte: every decimal exponent of
    binary64, random bit patterns, float keys, and numpy float64 / float32 /
    float16 (orjson writes the narrow types with float32 digits and layout)."""
    r = random.Random(17)
    xs = [0.0, -0.0, 5e-324, 2.2250738585072014e-308, 1.7976931348623157e308, 9007199254740993.0]
    for e in range(-325, 309):
        for m in (1.0, 1.5, 9.99, 1.2345678901234567, 2.5, 7.0, 9.999999999999999):
            try:
                xs.append(m * 10.0 ** e)
            except OverflowError:
                pass
    for _ in range(20000):
        xs.append(struct.unpack("<d", struct.pack("<Q", r.getrandbits(64)))[0])
        xs.append(r.random() * 10 ** r.randint(-12, 20) * r.choice([1, -1]))
    xs = [x for x in xs if x == x and abs(x) != float("inf")]
    objs = [[x] for x in xs] + [xs[i:i + 50] for i in range(0, len(xs), 50)]
    objs += [{x: 1} for x in xs[:3000]]
    if np is not None:
        with np.errstate(over="ignore"):
            f32 = np.array(xs, dtype=np.float32)
            f16 = np.array(xs, dtype=np.float16)
        f32 = f32[np.isfinite(f32)]
        f16 = f16[np.isfinite(f16)]
        objs += [f32[i:i + 50] for i in range(0, len(f32), 50)]
        objs += [f16[i:i + 50] for i in range(0, len(f16), 50)]
        objs += [f32[i] for i in range(0, len(f32), 7)] + [f16[i] for i in range(0, len(f16), 7)]
        objs += [np.array(xs[:500], dtype=np.float64), [np.float32(1e-5), 1e-5, {"a": np.float16(6e-6)}]]
    bad = 0
    opt = orjson.OPT_NON_STR_KEYS | orjson.OPT_SERIALIZE_NUMPY
    for o in objs:
        a, b = run(jp, o, option=opt), run(orjson, o, option=opt)
        if a != b:
            bad += 1
            if bad <= 5:
                print("   FLOAT %s: ours %s, orjson %s" % (ascii(o)[:60], ascii(a[1])[:80], ascii(b[1])[:80]))
    print("float text: %d objects, %d differ from orjson" % (len(objs), bad))
    return bad


def check_layout_depth():
    """With a layout option both libraries refuse a container 254 levels deep
    (the 255th nested one) and accept a scalar there. Tuples are left out:
    orjson counts them inconsistently, oxjson counts them like lists."""
    bad = 0
    leaves = [("int", lambda: 1), ("str", lambda: "s"), ("[]", list), ("{}", dict)]
    wraps = [("list", lambda o: [o]), ("dict", lambda o: {"d": o}), ("dataclass", lambda o: Point(o, 1.0))]
    layout = [orjson.OPT_INDENT_2, orjson.OPT_SORT_KEYS, orjson.OPT_NON_STR_KEYS, orjson.OPT_STRICT_INTEGER,
              orjson.OPT_INDENT_2 | orjson.OPT_SORT_KEYS]
    for wname, wrap in wraps:
        for lname, leaf in leaves:
            for n in (252, 253, 254, 255):
                o = leaf()
                for _ in range(n):
                    o = wrap(o)
                for opt in layout:
                    a, b = run(jp, o, option=opt), run(orjson, o, option=opt)
                    if a[0] != b[0]:
                        bad += 1
                        if bad <= 5:
                            print("   DEPTH %s x%d around %s, opt=%d: ours %s, orjson %s" % (
                                wname, n, lname, opt, a[0], b[0]))
    print("layout depth limit: %d differ from orjson" % bad)
    return bad


def main():
    bits = [orjson.OPT_INDENT_2, orjson.OPT_NAIVE_UTC, orjson.OPT_NON_STR_KEYS, orjson.OPT_OMIT_MICROSECONDS,
            orjson.OPT_SERIALIZE_NUMPY, orjson.OPT_SORT_KEYS, orjson.OPT_STRICT_INTEGER, orjson.OPT_UTC_Z,
            orjson.OPT_PASSTHROUGH_SUBCLASS, orjson.OPT_PASSTHROUGH_DATETIME, orjson.OPT_APPEND_NEWLINE,
            orjson.OPT_PASSTHROUGH_DATACLASS]
    for name in dir(orjson):
        if name.startswith("OPT_"):
            assert getattr(jp, name) == getattr(orjson, name), name
    opts = {0}
    for b in bits:
        opts.add(b)
    for a, b in itertools.combinations(bits, 2):
        opts.add(a | b)
    r = random.Random(3)
    for _ in range(40):
        opts.add(sum(b for b in bits if r.random() < 0.4))
    cs = cases()
    deep_at = next(i for i, o in enumerate(cs) if isinstance(o, list) and len(o) == 1 and o is not o[0]
                   and isinstance(o[0], list) and len(o[0]) == 1 and isinstance(o[0][0], list) and len(o[0][0]) == 1
                   and isinstance(o[0][0][0], list) and len(o[0][0][0]) == 1)
    n = bad = 0
    shown = {}
    for obj in cs:
        for opt in sorted(opts):
            for d in (None, default):
                kw = {}
                if opt:
                    kw["option"] = opt
                if d:
                    kw["default"] = d
                a, b = run(jp, obj, **kw), run(orjson, obj, **kw)
                n += 1
                same = a == b if a[0] == "ok" and b[0] == "ok" else a[0] == b[0]
                if obj is cs[deep_at] and a[0] == "ok" and b[0] == "err":
                    same = True  # 300 levels: orjson stops at 254, the compact writer here has no limit
                if not same:
                    bad += 1
                    key = (type(obj).__name__, a[0], b[0])
                    if shown.get(key, 0) < 2 and len(shown) < 40:
                        shown[key] = shown.get(key, 0) + 1
                        print("   DIFF opt=%d default=%s %s\n      ours:   %s\n      orjson: %s" % (
                            opt, bool(d), ascii(obj)[:90], ascii(a[1])[:150], ascii(b[1])[:150]))
    bad += check_layout_depth()
    bad += check_float_text()
    # positional form and bad options
    assert jp.dumps([1], None, orjson.OPT_INDENT_2) == orjson.dumps([1], None, orjson.OPT_INDENT_2)
    for badopt in (-1, 4096, 1.5, "1", True):
        a, b = run(jp, 1, option=badopt), run(orjson, 1, option=badopt)
        if a[0] != b[0]:
            bad += 1
            print("   DIFF option=%r ours %s orjson %s" % (badopt, a, b))
    print("dumps vs orjson %s: %d calls, %d differ (numpy %s)" % (orjson.__version__, n, bad, np.__version__ if np else "absent"))
    # the same calls again and again must not leave objects behind
    import gc

    def sweep():
        for obj in cs:
            for opt in (0, orjson.OPT_INDENT_2 | orjson.OPT_SORT_KEYS | orjson.OPT_NON_STR_KEYS, orjson.OPT_SERIALIZE_NUMPY,
                        orjson.OPT_PASSTHROUGH_SUBCLASS | orjson.OPT_STRICT_INTEGER | orjson.OPT_APPEND_NEWLINE):
                run(jp, obj, option=opt, default=default)
                run(jp, obj, option=opt)

    for _ in range(3):
        sweep()
    # a leak grows with the number of calls; allocator noise does not
    grow = []
    for reps in (5, 20, 80):
        gc.collect()
        b0 = sys.getallocatedblocks()
        for _ in range(reps):
            sweep()
        gc.collect()
        grow.append(sys.getallocatedblocks() - b0)
    leak = grow[2] > 80 and grow[2] > 3 * max(grow[0], 1) and grow[1] > 2 * max(grow[0], 1)
    bad += leak
    print("memory: blocks left after 5 / 20 / 80 sweeps: %s%s" % (grow, "  LEAK" if leak else ""))
    print("RESULT", "FAIL" if bad else "OK")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
