# -*- coding: utf-8 -*-
"""Differential fuzzer for oxjson.loads and oxjson.dumps against orjson.

    python tests/fuzz.py                      # 60 seconds, both targets, both code paths
    python tests/fuzz.py --seconds 600 --jobs 8
    python tests/fuzz.py --target loads --seed 7
    python tests/fuzz.py --replay fuzz-out/crash-loads-....bin
    python tests/fuzz.py --replay-case dumps:7:12345

Every batch of cases runs in its own process, so a crash or a hang does not
stop the run. A batch that dies is run again with each input written to disk
first; the input that killed it is saved in fuzz-out/. Results that differ
from orjson are saved there too. Exit code 1 when anything was found.

loads: inputs are mutated documents from testdata/ and generated documents,
aimed at number edge cases, escapes, invalid UTF-8, lengths around the 32- and
64-byte vector widths, deep nesting and truncated input. Some inputs are placed
so that the document ends right before an inaccessible memory page: a read
past the end of the input crashes there instead of passing unnoticed.

dumps: random object trees of every supported type with random options,
compared with orjson byte for byte. Some cases use a default() that changes the container
being written; those only have to finish without crashing.

Generation is deterministic: case i of seed s is the same on every run, so
--replay-case reproduces any case by its number.
"""
import argparse
import ctypes
import dataclasses
import datetime as dt
import enum
import hashlib
import json
import mmap
import os
import platform
import random
import re
import struct
import subprocess
import sys
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "fuzz-out"
BATCH = 1500
BATCH_TIMEOUT = 180
MAX_LEN = 1 << 18

X86 = platform.machine().lower() in ("amd64", "x86_64")


# ---------------------------------------------------------------- comparison

def canon(v):
    """Exact, type-aware fingerprint of a loads() result (float bits, key order)."""
    out = []
    st = [v]
    while st:
        x = st.pop()
        if x is None or x is True or x is False:
            out.append(repr(x))
        elif isinstance(x, int):
            out.append("i%d" % x)
        elif isinstance(x, float):
            out.append("f" + struct.pack("<d", x).hex())
        elif isinstance(x, str):
            out.append("s" + x.encode("utf-8", "surrogatepass").hex())
        elif isinstance(x, list):
            out.append("[%d" % len(x))
            st.extend(reversed(x))
        elif isinstance(x, dict):
            out.append("{%d" % len(x))
            for k in reversed(list(x)):
                st.append(x[k])
                st.append(k)
        else:
            out.append("?" + type(x).__name__)
    return "|".join(out)




# ---------------------------------------------------------------- guard page

class GuardBuf:
    """Builds bytes objects whose data (and trailing NUL) end right before a
    PROT_NONE / PAGE_NOACCESS page. The objects live in memory Python does not
    own; they are immortal and are never freed."""

    def __init__(self, cap=MAX_LEN + 4096):
        self.page = mmap.PAGESIZE
        size = (cap + 2 * self.page + self.page - 1) // self.page * self.page
        self.size = size
        if os.name == "nt":
            k32 = ctypes.WinDLL("kernel32", use_last_error=True)
            k32.VirtualAlloc.restype = ctypes.c_void_p
            k32.VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_uint32]
            k32.VirtualProtect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32,
                                           ctypes.POINTER(ctypes.c_uint32)]
            base = k32.VirtualAlloc(None, size, 0x3000, 0x04)  # MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE
            if not base:
                raise OSError("VirtualAlloc failed")
            old = ctypes.c_uint32()
            if not k32.VirtualProtect(base + size - self.page, self.page, 0x01, ctypes.byref(old)):  # NOACCESS
                raise OSError("VirtualProtect failed")
        else:
            self._m = mmap.mmap(-1, size)
            base = ctypes.addressof(ctypes.c_char.from_buffer(self._m))
            libc = ctypes.CDLL(None, use_errno=True)
            libc.mprotect.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
            if libc.mprotect(base + size - self.page, self.page, 0) != 0:  # PROT_NONE
                raise OSError("mprotect failed")
        self.base = base
        self.guard = base + size - self.page
        self.hdr = sys.getsizeof(b"") - 1  # offset of ob_sval
        proto = b"" if sys.version_info >= (3, 12) else b"\x00"
        self.header = bytearray(ctypes.string_at(id(proto), self.hdr))
        if sys.version_info < (3, 12):
            struct.pack_into("<q", self.header, 0, 1 << 40)  # never reaches zero
        # ob_size at 16, ob_shash at 24 (64-bit builds)
        struct.pack_into("<q", self.header, 24, -1)
        self.align = 1 if X86 else 8

    def make(self, data):
        n = len(data)
        addr = self.guard - 1 - n - self.hdr
        addr -= addr % self.align
        if addr < self.base:
            raise ValueError("input of %d bytes does not fit the guard buffer" % n)
        struct.pack_into("<q", self.header, 16, n)
        ctypes.memmove(addr, bytes(self.header), self.hdr)
        ctypes.memmove(addr + self.hdr, data, n)
        ctypes.memset(addr + self.hdr + n, 0, 1)
        return ctypes.cast(addr, ctypes.py_object).value


def guard_selftest(g):
    # each object must be dropped before the next make(): the next one is written over it
    for d in (b"", b"[1]", b"x" * 4095, bytes(range(256)) * 3):
        ok = (lambda o: type(o) is bytes and len(o) == len(d) and o == d)(g.make(d))
        assert ok, "guard page bytes object is broken"


# ---------------------------------------------------------------- loads cases

NUMS = [b"0", b"-0", b"-0.0", b"0.0", b"1e999", b"-1e999", b"1e-400", b"2.2250738585072011e-308",
        b"2.2250738585072014e-308", b"4.9406564584124654e-324", b"5e-324", b"1.7976931348623157e308",
        b"1.7976931348623159e308", b"9223372036854775807", b"9223372036854775808", b"-9223372036854775808",
        b"-9223372036854775809", b"18446744073709551615", b"18446744073709551616", b"-18446744073709551616",
        b"0.1", b"0.087", b"1.65", b"9007199254740993", b"9007199254740993.0", b"1.0", b"1E5", b"1e05", b"0e0",
        b"0E-0", b"123456789012345678901234567890", b"1" * 400, b"0." + b"0" * 400 + b"1", b"1" + b"0" * 310,
        b"1." + b"9" * 40, b"7.2057594037927933e16", b"8.98846567431158e307", b"3.14159265358979323846264338",
        b"12345678", b"1234567890123456", b"12345678901234567", b"-1234567.12345678e-12", b"0.000001", b"1e-7"]
BAD_NUMS = [b"1.", b".1", b"01", b"-01", b"1e", b"1e+", b"-", b"+1", b"--1", b"1..2", b"1e1e1", b"0x10", b"1_0",
            b"NaN", b"Infinity", b"-Infinity", b"nan", b"inf", b"1.e5", b"-.5", b"00", b"1ee5"]
STR_BITS = ["a", "\\\"", "\\\\", "\\/", "\\b", "\\f", "\\n", "\\r", "\\t", "\\u0000", "\\u001f", "\\u0041", "\\u00e9",
            "\\u4e2d", "\\ud83d\\ude00", "\\ud800", "\\udc00", "\\udc00\\ud800", "\\ud800\\u0041", "\\uDBFF\\uDFFF",
            "\\u", "\\u12", "\\uzzzz", "\\x41", "\\", "é", "中", "😀", "\x7f", " ", "z" * 31, "z" * 32, "z" * 33]
RAW = [b"\xc0\x80", b"\xc1\xbf", b"\xed\xa0\x80", b"\xed\xbf\xbf", b"\xf4\x90\x80\x80", b"\xf5\x80\x80\x80",
       b"\xf0\x9f\x98", b"\xe4\xb8", b"\xc3", b"\xff", b"\xfe", b"\x80", b"\xbf", b"\x00", b"\x01", b"\x1f", b"\x7f",
       b"\xef\xbb\xbf", b"\xe0\x80\xaf", b"\xf0\x80\x80\x80", b"\xee\x80\x80"]
TOKENS = [b"true", b"false", b"null", b"tru", b"fals", b"nul", b"truex", b"[", b"]", b"{", b"}", b",", b":", b"[]",
          b"{}", b'""', b'"a":', b",,", b"[,]", b"{,}", b'{"a"}', b'{"a":}', b'{"a" 1}', b"[1 2]", b" ", b"\t",
          b"\r\n", b"\n", b"\x0b", b"\xc2\xa0", b"/*c*/", b"//", b'"', b"\\", b"\x00"]
WS = [b"", b" ", b"\n", b"\t ", b"\r\n  "]


def gen_str(r):
    """A JSON string literal, sized around the vector widths."""
    target = r.choice([0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 47, 63, 64, 65, 95, 127, 128, 129, 255, 256, 4097])
    parts = []
    n = 0
    plain = r.random() < 0.4
    while n < target:
        p = "z" if plain and r.random() < 0.9 else r.choice(STR_BITS)
        parts.append(p)
        n += len(p.encode("utf-8"))
    s = "".join(parts).encode("utf-8")
    if r.random() < 0.15:
        k = r.randrange(len(s) + 1)
        s = s[:k] + r.choice(RAW) + s[k:]
    return b'"' + s + b'"'


def gen_value(r, depth=0, budget=None):
    if budget is None:
        budget = [r.choice([50, 400, 3000])]  # nodes left for the whole document
    budget[0] -= 1
    k = r.random()
    if depth > 5 or k < 0.5 or budget[0] <= 0:
        c = r.randrange(6)
        if c == 0:
            return r.choice(NUMS)
        if c == 1:
            return repr(r.uniform(-1e6, 1e6)).encode()
        if c == 2:
            return ("%d.%se%+d" % (r.randint(0, 99), "".join(r.choice("0123456789") for _ in range(r.randint(1, 25))),
                                   r.randint(-340, 320))).encode()
        if c == 3:
            return str(r.randint(-(1 << r.randint(1, 70)), 1 << r.randint(1, 70))).encode()
        if c == 4:
            return r.choice([b"true", b"false", b"null"])
        return gen_str(r)
    w = lambda: r.choice(WS)
    n = r.choice([0, 1, 2, 3, 5, 8, 16, 33, 100])
    if k < 0.75:
        return b"[" + w() + (b"," + w()).join(gen_value(r, depth + 1, budget) for _ in range(n)) + w() + b"]"
    keys = []
    for _ in range(n):
        keys.append(gen_str(r) if r.random() < 0.3 else json.dumps("k%d" % r.randint(0, 40)).encode())
    return b"{" + w() + (b"," + w()).join(kk + w() + b":" + w() + gen_value(r, depth + 1, budget)
                                          for kk in keys) + w() + b"}"


NUM_RE = re.compile(rb"-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?")
STR_RE = re.compile(rb'"(?:[^"\\]|\\.)*"')


def mutate(r, d, corpus):
    m = r.randrange(14)
    n = len(d)
    if m == 0 and n:
        i = r.randrange(n)
        return d[:i] + bytes([d[i] ^ (1 << r.randrange(8))]) + d[i + 1:]
    if m == 1 and n:
        i = r.randrange(n)
        return d[:i] + r.choice(RAW + [bytes([r.randrange(256)])]) + d[i + 1:]
    if m == 2:
        i = r.randrange(n + 1)
        return d[:i] + r.choice(TOKENS + NUMS + BAD_NUMS + RAW) + d[i:]
    if m == 3 and n:
        i = r.randrange(n)
        j = min(n, i + r.choice([1, 2, 4, 8, 32, 200]))
        return d[:i] + d[j:]
    if m == 4 and n:
        i = r.randrange(n)
        j = min(n, i + r.randint(1, 300))
        return d[:j] + d[i:j] + d[j:]
    if m == 5 and n:
        return d[:r.randrange(n)]
    if m == 6:
        o = r.choice(corpus)
        return d[:r.randint(0, n)] + o[r.randint(0, len(o)):]
    if m == 7:
        ms = list(NUM_RE.finditer(d))
        if ms:
            x = r.choice(ms)
            return d[:x.start()] + r.choice(NUMS + BAD_NUMS) + d[x.end():]
    if m == 8:
        ms = list(STR_RE.finditer(d))
        if ms:
            x = r.choice(ms)
            return d[:x.start()] + gen_str(r) + d[x.end():]
    if m == 9:
        k = r.choice([1, 2, 31, 32, 33, 254, 255, 1023, 1024, 1025, 3000])
        o, c = r.choice([(b"[", b"]"), (b'{"a":', b"}")])
        if r.random() < 0.2:
            c = c[:0]  # unclosed
        return o * k + d + c * k
    if m == 10:
        return b" " * r.randrange(64) + d + b" " * r.randrange(4)
    if m == 11:
        tok = r.choice(NUMS + [gen_str(r), b"true", b"null", b"[]", b"{}", b'{"k":1}'])
        return b"[" + b",".join([tok] * r.choice([1, 8, 9, 16, 17, 64, 65, 1000])) + b"]"
    if m == 12:
        return gen_value(r)
    # 13: object with many keys of chosen lengths (key cache, 32-byte key compare)
    ks = [b'"' + bytes([97 + r.randrange(3)]) * r.choice([1, 7, 8, 16, 31, 32, 33, 64]) + b'"' for _ in range(r.randint(1, 40))]
    return b"{" + b",".join(k + b":" + gen_value(r, 3) for k in ks) + b"}"


def load_corpus():
    c = []
    for f in sorted(ROOT.glob("testdata/**/*.json")):
        try:
            if f.stat().st_size <= 64 * 1024:
                c.append(f.read_bytes())
        except OSError:
            pass
    return c or [b"{}"]


def loads_case(seed, i, corpus):
    r = random.Random("%d:loads:%d" % (seed, i))
    d = r.choice(corpus) if r.random() < 0.6 else gen_value(r)
    for _ in range(r.choice([0, 1, 1, 2, 3, 5])):
        d = mutate(r, d[:MAX_LEN], corpus)
    d = d[:MAX_LEN]
    form = r.choice(["bytes", "guard", "guard", "guard", "str", "bytearray", "memoryview"])
    return d, form


def depth_of(d):
    """Bracket depth outside strings, as orjson counts it."""
    depth = best = 0
    in_s = esc = False
    for ch in d:
        if in_s:
            if esc:
                esc = False
            elif ch == 92:
                esc = True
            elif ch == 34:
                in_s = False
        elif ch == 34:
            in_s = True
        elif ch in (91, 123):
            depth += 1
            best = max(best, depth)
        elif ch in (93, 125):
            depth -= 1
    return best


def check_loads(ox, orj, d, form, guard):
    """None if fine, else a short description of the problem."""
    if form == "str":
        try:
            arg = d.decode("utf-8")
        except UnicodeDecodeError:
            form, arg = "bytes", d
    elif form == "guard" and guard is not None:
        arg = guard.make(d)
    elif form == "bytearray":
        arg = bytearray(d)
    elif form == "memoryview":
        arg = memoryview(d)
    else:
        form, arg = "bytes", d
    try:
        a = ("ok", ox.loads(arg))
    except Exception as e:
        # the traceback holds this frame and so `arg`; a guard-page object must not
        # outlive this call, the next one is written over it
        e.__traceback__ = None
        a = ("err", e)
    del arg
    try:
        b = ("ok", orj.loads(d))
    except Exception as e:
        b = ("err", e)
    if a[0] == "err":
        e = a[1]
        if not isinstance(e, ox.JSONDecodeError) or not isinstance(e, ValueError):
            return "loads(%s) raised %s: %s" % (form, type(e).__name__, str(e)[:200])
        pos, doc = getattr(e, "pos", None), getattr(e, "doc", None)
        if not isinstance(pos, int) or not isinstance(doc, str) or not 0 <= pos <= len(doc):
            return "loads(%s) error position %r outside the document (%d chars)" % (
                form, pos, len(doc) if isinstance(doc, str) else -1)
    if a[0] != b[0]:
        if a[0] == "ok" and ("ecursion" in str(b[1]) or "depth" in str(b[1])) and depth_of(d) > 1024:
            return None  # orjson stops at 1024 levels, oxjson has no limit
        return "loads(%s): oxjson %s, orjson %s (%s)" % (
            form, a[0], b[0], str(a[1] if a[0] == "err" else b[1])[:200])
    if a[0] == "err":
        return None
    ca, cb = canon(a[1]), canon(b[1])
    if ca != cb:
        return "loads(%s): values differ from orjson" % form
    # what loads produced, dumps must write back and loads must read the same
    try:
        back = ox.dumps(a[1])
    except Exception as e:
        try:
            orj.dumps(b[1])
        except Exception:
            return None
        return "dumps of a loads result raised %s: %s" % (type(e).__name__, str(e)[:200])
    try:
        if canon(ox.loads(back)) != ca:
            return "dumps/loads round trip changed the value"
    except Exception as e:
        return "loads of own dumps output raised %s: %s" % (type(e).__name__, str(e)[:200])
    return None


# ---------------------------------------------------------------- dumps cases

class Color(enum.Enum):
    RED = 1
    NAME = "name"
    PAIR = (1, 2)


class IntE(enum.IntEnum):
    A = 7


class StrE(str, enum.Enum):
    A = "a"


@dataclasses.dataclass
class Point:
    x: object
    y: object = 0.5
    _h: int = 1


@dataclasses.dataclass(slots=True)
class Slotted:
    a: object
    b: object = None


class MyStr(str):
    pass


class MyInt(int):
    pass


class MyFloat(float):
    pass


class MyList(list):
    pass


class MyDict(dict):
    pass


class Odd:
    pass


class Mutator:
    """Placeholder that, when default() sees it, changes its parent container."""

    def __init__(self, parent, how):
        self.parent = parent
        self.how = how


UTC = dt.timezone.utc
TZS = [None, UTC, dt.timezone(dt.timedelta(hours=5, minutes=30)), dt.timezone(dt.timedelta(seconds=-1050))]


def rnd_text(r):
    n = r.choice([0, 1, 5, 15, 16, 17, 31, 32, 33, 63, 64, 65, 200])
    pool = ["a", "Z", " ", '"', "\\", "\n", "\x00", "\x1f", "\x7f", "é", "中", "😀", " ", "</", "\ud800", "\udfff"]
    plain = r.random() < 0.5
    s = "".join("a" if plain and r.random() < 0.9 else r.choice(pool) for _ in range(n))
    if r.random() < 0.85:
        s = s.encode("utf-16", "surrogatepass").decode("utf-16", "replace")  # mostly valid text
    return s


def rnd_float(r):
    return r.choice([0.0, -0.0, 0.1, 1.5, 1e-5, 1e16, 1e22, 1e23, 5e-324, 2.2250738585072014e-308,
                     1.7976931348623157e308, float("nan"), float("inf"), float("-inf"), 123456.789,
                     r.uniform(-1e3, 1e3), r.random() * 10 ** r.randint(-30, 30),
                     struct.unpack("<d", struct.pack("<Q", r.getrandbits(64)))[0]])


def rnd_int(r):
    return r.choice([0, 1, -1, 2 ** 31, 2 ** 53, 2 ** 53 + 1, 2 ** 63 - 1, 2 ** 63, 2 ** 64 - 1, 2 ** 64, -(2 ** 63),
                     -(2 ** 63) - 1, 10 ** 30, r.randint(-1000, 1000), r.getrandbits(r.randint(1, 66)) * r.choice([1, -1])])


def rnd_leaf(r):
    c = r.randrange(16)
    if c < 3:
        return rnd_text(r)
    if c < 5:
        return rnd_int(r)
    if c < 7:
        return rnd_float(r)
    if c == 7:
        return r.choice([None, True, False])
    if c == 8:
        tz = r.choice(TZS)
        return r.choice([dt.datetime(r.randint(1, 9999), r.randint(1, 12), r.randint(1, 28), r.randint(0, 23),
                                     r.randint(0, 59), r.randint(0, 59), r.choice([0, 1, 999999, r.randint(0, 999999)]),
                                     tzinfo=tz),
                         dt.date(r.randint(1, 9999), r.randint(1, 12), r.randint(1, 28)),
                         dt.time(r.randint(0, 23), r.randint(0, 59), r.randint(0, 59), r.choice([0, 5, 999999]))])
    if c == 9:
        return uuid.UUID(int=r.getrandbits(128))
    if c == 10:
        return r.choice([Color.RED, Color.NAME, Color.PAIR, IntE.A, StrE.A])
    if c == 11:
        return r.choice([MyStr(rnd_text(r)), MyInt(rnd_int(r)), MyFloat(rnd_float(r))])
    if c == 12:
        return r.choice([Odd(), {1, 2}, complex(1, 2), b"raw", object()])
    if c == 13:
        try:
            import numpy as np
        except ImportError:
            return rnd_int(r)
        return r.choice([np.array([rnd_int(r) % 1000 for _ in range(r.randint(0, 5))]),
                         np.arange(r.randint(0, 12), dtype=r.choice(["int8", "int64", "uint64", "float32", "float64"])),
                         np.zeros((2, r.randint(0, 3))), np.float64(rnd_float(r)), np.int32(5), np.bool_(True),
                         np.array([True, False]), np.array(["a"]), np.arange(6).reshape(2, 3).T])
    if c == 14:
        return Point(rnd_int(r), rnd_float(r))
    return Slotted(rnd_text(r))


def rnd_key(r, nonstr):
    if not nonstr or r.random() < 0.6:
        return rnd_text(r) if r.random() < 0.3 else "k%d" % r.randint(0, 30)
    return r.choice([rnd_int(r), rnd_float(r), True, None, dt.date(2020, 1, 2), uuid.UUID(int=r.getrandbits(128)),
                     Color.RED, IntE.A, StrE.A, MyStr("ms"), (1, 2), b"b"])


def rnd_obj(r, depth, nonstr, mutators, budget=None):
    if budget is None:
        budget = [r.choice([30, 300, 2000])]  # nodes left for the whole tree
    budget[0] -= 1
    if depth > 6 or r.random() < 0.35 or budget[0] <= 0:
        return rnd_leaf(r)
    n = r.choice([0, 1, 2, 3, 5, 9, 17, 40])
    kind = r.randrange(6)
    if kind < 3:
        out = [rnd_obj(r, depth + 1, nonstr, mutators, budget) for _ in range(n)]
        if kind == 1:
            out = tuple(out)
        elif kind == 2:
            out = MyList(out)
        elif mutators and r.random() < 0.3:
            out.append(Mutator(out, r.choice(["clear", "append", "pop"])))
        return out
    d = {} if kind < 5 else MyDict()
    for _ in range(n):
        d[rnd_key(r, nonstr)] = rnd_obj(r, depth + 1, nonstr, mutators, budget)
    if mutators and r.random() < 0.3:
        d["mut"] = Mutator(d, r.choice(["clear", "add", "pop"]))
    return d


BITS = ["OPT_INDENT_2", "OPT_NAIVE_UTC", "OPT_NON_STR_KEYS", "OPT_OMIT_MICROSECONDS", "OPT_SERIALIZE_NUMPY",
        "OPT_SORT_KEYS", "OPT_STRICT_INTEGER", "OPT_UTC_Z", "OPT_PASSTHROUGH_SUBCLASS", "OPT_PASSTHROUGH_DATETIME",
        "OPT_APPEND_NEWLINE", "OPT_PASSTHROUGH_DATACLASS"]


def dumps_case(seed, i, orj):
    r = random.Random("%d:dumps:%d" % (seed, i))
    opt = 0
    for b in BITS:
        if r.random() < 0.2:
            opt |= getattr(orj, b)
    mutators = r.random() < 0.1
    obj = rnd_obj(r, 0, bool(opt & orj.OPT_NON_STR_KEYS), mutators)
    if r.random() < 0.05:
        deep = cur = []
        for _ in range(r.choice([200, 253, 254, 255, 1000])):
            nxt = [] if r.random() < 0.8 else {}
            if isinstance(cur, list):
                cur.append(nxt)
            else:
                cur["d"] = nxt
            cur = nxt
        obj = [obj, deep]
    if r.random() < 0.03:
        obj = [obj]
        obj.append(obj)  # cycle
    mode = r.choice(["none", "default", "default", "selfref", "raise"])
    return obj, opt, mode, mutators


def make_default(mode):
    def default(o):
        if isinstance(o, Mutator):
            p = o.parent
            if o.how == "clear":
                p.clear()
            elif o.how == "append":
                p.extend(range(50))
            elif o.how == "add":
                for k in range(50):
                    p["x%d" % k] = k
            elif o.how == "pop" and p:
                if isinstance(p, dict):
                    p.pop(next(iter(p)))
                else:
                    p.pop(0)
            return "m"
        if mode == "selfref":
            return o  # orjson stops after 254 nested default() calls
        if mode == "raise":
            raise ValueError("no")
        if isinstance(o, (set, frozenset)):
            return sorted(o)
        if isinstance(o, complex):
            return [o.real, o.imag]
        if isinstance(o, Odd):
            return {"odd": [1, 2.5]}
        raise TypeError
    return default


def call_dumps(m, obj, opt, mode):
    kw = {}
    if opt:
        kw["option"] = opt
    if mode != "none":
        kw["default"] = make_default(mode)
    try:
        return ("ok", m.dumps(obj, **kw))
    except RecursionError:
        return ("err", "RecursionError")
    except Exception as e:
        return ("err", "%s: %s" % (type(e).__name__, str(e)[:150]))


def check_dumps(ox, orj, seed, i):
    obj, opt, mode, mutators = dumps_case(seed, i, orj)
    if mutators:
        call_dumps(ox, obj, opt, mode)  # only has to survive
        obj2, opt, mode, _ = dumps_case(seed, i, orj)
        call_dumps(ox, obj2, opt, mode)
        return None
    a = call_dumps(ox, obj, opt, mode)
    b = call_dumps(orj, obj, opt, mode)
    if a[0] == "err" and not (a[1].startswith("TypeError") or a[1].startswith("ValueError")
                              or a[1] == "RecursionError" or "JSONEncodeError" in a[1]):
        return "dumps raised %s" % a[1]
    if a[0] != b[0]:
        if a[0] == "ok" and "Recursion limit reached" in b[1] and not opt & (
                orj.OPT_INDENT_2 | orj.OPT_SORT_KEYS | orj.OPT_NON_STR_KEYS | orj.OPT_STRICT_INTEGER):
            return None  # the compact writer here has no depth limit, orjson stops at 254
        return "dumps opt=%d default=%s: oxjson %s, orjson %s" % (opt, mode, a[1] if a[0] == "err" else "ok",
                                                                b[1] if b[0] == "err" else "ok")
    if a[0] == "ok" and a[1] != b[1]:
        k = next((j for j in range(min(len(a[1]), len(b[1]))) if a[1][j] != b[1][j]), 0)
        return "dumps opt=%d default=%s: bytes differ at %d: oxjson %r orjson %r" % (
            opt, mode, k, a[1][max(0, k - 30):k + 40], b[1][max(0, k - 30):k + 40])
    return None


# ---------------------------------------------------------------- worker

def worker(args):
    sys.path.insert(0, str(ROOT))
    import oxjson as ox
    import orjson as orj
    sys.setrecursionlimit(10000)
    corpus = load_corpus() if args.target == "loads" else None
    guard = None
    if args.target == "loads":
        try:
            guard = GuardBuf()
            guard_selftest(guard)
        except Exception as e:
            print("guard page unavailable: %s" % e, file=sys.stderr)
            guard = None
    trace = Path(args.trace) if args.trace else None
    found = []
    # self-test of the crash path: FUZZ_SELFTEST_CRASH=<case number> kills the worker there
    crash_at = int(os.environ.get("FUZZ_SELFTEST_CRASH", "-1"))
    for i in range(args.start, args.start + args.count):
        if args.target == "loads":
            d, form = loads_case(args.seed, i, corpus)
            if trace:
                trace.write_bytes(d)
                (trace.parent / (trace.name + ".form")).write_text("%d %s" % (i, form))
            if i == crash_at:
                ctypes.string_at(0)  # access violation / SIGSEGV
            problem = check_loads(ox, orj, d, form, guard)
            if problem:
                found.append({"i": i, "problem": problem, "input": d.hex() if len(d) <= 65536 else d[:65536].hex(),
                              "form": form})
        else:
            if trace:
                trace.write_text(str(i))
            if i == crash_at:
                ctypes.string_at(0)
            problem = check_dumps(ox, orj, args.seed, i)
            if problem:
                found.append({"i": i, "problem": problem})
        if len(found) >= 20:
            break
    print("RESULT " + json.dumps({"n": args.count, "found": found}))
    return 0


# ---------------------------------------------------------------- driver

def run_batch(target, seed, start, count, portable, trace=None, timeout=BATCH_TIMEOUT):
    cmd = [sys.executable, str(Path(__file__).resolve()), "--worker", "--target", target, "--seed", str(seed),
           "--start", str(start), "--count", str(count)]
    if trace:
        cmd += ["--trace", str(trace)]
    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONFAULTHANDLER="1")
    if portable:
        env["OXJSON_NO_AVX2"] = "1"
    else:
        env.pop("OXJSON_NO_AVX2", None)
    return subprocess.Popen(cmd, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, encoding="utf-8", errors="replace"), time.time() + timeout


def save(kind, target, portable, seed, info, data=None):
    OUT.mkdir(exist_ok=True)
    path = "portable" if portable else "default"
    h = hashlib.sha1((info.get("problem", "") + (data.hex() if data else str(info.get("i")))).encode()).hexdigest()[:10]
    stem = OUT / ("%s-%s-%s-%s" % (kind, target, path, h))
    meta = dict(info, kind=kind, target=target, path=path, seed=seed, python=sys.version.split()[0],
                machine=platform.machine())
    meta.pop("input", None)
    stem.with_suffix(".txt").write_text(json.dumps(meta, indent=1, ensure_ascii=True) + "\n", encoding="utf-8")
    if data is not None:
        stem.with_suffix(".bin").write_bytes(data)
    return stem


def triage_dead(target, seed, start, count, portable, how, stderr):
    """Run the batch again, writing each input first; return what killed it."""
    OUT.mkdir(exist_ok=True)
    tr = OUT / (".trace-%s-%d-%d-%d" % (target, seed, start, os.getpid()))
    p, deadline = run_batch(target, seed, start, count, portable, trace=tr, timeout=BATCH_TIMEOUT * 4)
    try:
        out, err = p.communicate(timeout=max(1, deadline - time.time()))
        how2 = "exit code %d" % p.returncode
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
        how2 = "hang"
    if "RESULT " in (out or ""):
        tr.unlink(missing_ok=True)
        return None  # did not reproduce (flaky or timing)
    data = tr.read_bytes() if target == "loads" and tr.exists() else None
    idx = None
    form_file = tr.parent / (tr.name + ".form")
    if form_file.exists():
        idx, form = form_file.read_text().split()
        form_file.unlink()
    elif tr.exists():
        idx, form = tr.read_text(), None
    tr.unlink(missing_ok=True)
    info = {"i": int(idx) if idx else None, "problem": "process died: %s / %s" % (how, how2),
            "stderr": (err or stderr or "")[-3000:]}
    if target == "loads":
        info["form"] = form
    return save("crash", target, portable, seed, info, data)


def driver(args):
    paths = [False, True] if X86 else [True]
    targets = ["loads", "dumps"] if args.target == "all" else [args.target]
    jobs = args.jobs or max(1, (os.cpu_count() or 2) // 2)
    t_end = time.time() + args.seconds
    nxt = {(t, p): 0 for t in targets for p in paths}
    combos = list(nxt)
    running = []
    total = {c: 0 for c in combos}
    findings = []
    seen = set()
    k = 0
    print("fuzz: %s, paths %s, %d jobs, %d s, seed %d, output %s" % (
        "+".join(targets), "/".join("portable" if p else "default" for p in paths), jobs, args.seconds, args.seed, OUT))
    while True:
        while len(running) < jobs and time.time() < t_end:
            c = combos[k % len(combos)]
            k += 1
            start = nxt[c]
            nxt[c] += BATCH
            p, deadline = run_batch(c[0], args.seed, start, BATCH, c[1])
            running.append((p, deadline, c, start))
        if not running:
            break
        time.sleep(0.05)
        still = []
        for p, deadline, c, start in running:
            if p.poll() is None and time.time() < deadline:
                still.append((p, deadline, c, start))
                continue
            hung = p.poll() is None
            if hung:
                p.kill()
            out, err = p.communicate()
            res = None
            for line in (out or "").splitlines():
                if line.startswith("RESULT "):
                    res = json.loads(line[7:])
            if res is not None:
                total[c] += res["n"]
                for f in res["found"]:
                    key = (c[0], c[1], f["problem"][:60])
                    if key in seen:
                        continue
                    seen.add(key)
                    data = bytes.fromhex(f["input"]) if "input" in f else None
                    stem = save("diff", c[0], c[1], args.seed, f, data)
                    findings.append(stem)
                    print("  FOUND %s case %d: %s" % (stem.name, f["i"], f["problem"][:160]), flush=True)
                continue
            how = "hang" if hung else "exit code %d" % p.returncode
            print("  batch %s %s %d.. died (%s), finding the input..." % (
                c[0], "portable" if c[1] else "default", start, how), flush=True)
            stem = triage_dead(c[0], args.seed, start, BATCH, c[1], how, err)
            if stem:
                findings.append(stem)
                print("  FOUND %s" % stem.name, flush=True)
            else:
                print("  did not reproduce on a second run; stderr tail:\n%s" % (err or "")[-800:], flush=True)
                total[c] += BATCH
        running = still
    for c in combos:
        print("  %-5s %-8s %8d cases" % (c[0], "portable" if c[1] else "default", total[c]))
    print("RESULT %s: %d findings%s" % ("FAIL" if findings else "OK", len(findings),
                                      (" in " + str(OUT)) if findings else ""))
    return 1 if findings else 0


def replay(args):
    sys.path.insert(0, str(ROOT))
    import oxjson as ox
    import orjson as orj
    print("oxjson from", ox.__file__, "| OXJSON_NO_AVX2 =", os.environ.get("OXJSON_NO_AVX2", ""))
    if args.replay:
        d = Path(args.replay).read_bytes()
        guard = GuardBuf()
        for form in ("bytes", "guard", "str", "bytearray", "memoryview"):
            print("%-10s" % form, check_loads(ox, orj, d, form, guard) or "ok", flush=True)
        return 0
    target, seed, i = args.replay_case.split(":")
    if target == "loads":
        d, form = loads_case(int(seed), int(i), load_corpus())
        print("input (%s, %d bytes): %r" % (form, len(d), d[:300]))
        print(check_loads(ox, orj, d, form, GuardBuf()) or "ok")
    else:
        obj, opt, mode, mut = dumps_case(int(seed), int(i), orj)
        print("object:", ascii(obj)[:500], "\noption:", opt, "default:", mode, "mutators:", mut)
        print(check_dumps(ox, orj, int(seed), int(i)) or "ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--target", choices=["all", "loads", "dumps"], default="all")
    ap.add_argument("--replay", help="run one saved loads input")
    ap.add_argument("--replay-case", help="target:seed:index, e.g. dumps:1:4242")
    ap.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--start", type=int, default=0, help=argparse.SUPPRESS)
    ap.add_argument("--count", type=int, default=BATCH, help=argparse.SUPPRESS)
    ap.add_argument("--trace", help=argparse.SUPPRESS)
    args = ap.parse_args()
    if args.worker:
        return worker(args)
    if args.replay or args.replay_case:
        return replay(args)
    return driver(args)


if __name__ == "__main__":
    sys.exit(main())
