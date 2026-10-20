/* Production PyObject dumps (oxjson.pyd): Python object -> bytes.
 *
 * Output text is json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
 * encoded as UTF-8, except non-str dict keys are a TypeError and ints are
 * limited to -2^63 .. 2^64-1 (the range orjson writes).
 *
 * Design:
 * - The output buffer is the result PyBytes itself. It grows with
 *   _PyBytes_Resize (realloc) and is trimmed once at the end, so there is no
 *   final copy and no calloc zeroing.
 * - One iterative walk with a small explicit stack. Each list/dict frame
 *   writes consecutive leaves in a tight loop and only pushes for a nested
 *   container. Empty containers never push.
 * - Every leaf reserves its worst case once, then writes with raw stores.
 * - str: compact ASCII data is copied 32 bytes at a time with AVX2 while the
 *   same load is checked for '"', '\\' and bytes < 0x20. Non-ASCII uses the
 *   cached UTF-8 (PyUnicode_AsUTF8AndSize) and the same copy.
 * - int: compact value -> two-digit table. float: Schubfach shortest digits in
 *   Python repr layout (include/json_f64_fmt.h).
 * - Cycle check only once the stack is 64 deep (a cycle is unbounded depth),
 *   so ordinary trees pay nothing.
 * - default= is called at most 254 times per value (unchanged contract).
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <float.h>
#ifndef JD_PORTABLE
#include <immintrin.h>
#else
/* Portable build (src/json_dumps_portable.c): C with NEON or SSE2 helpers where available. The only
 * dumps on a non-x86 target and the fallback on x86 without AVX2. */
#define JF64_PORTABLE 1
#include "json_vec16.h"
#define JsonDumps_py_dumps JsonDumps_py_dumps_portable
#define JsonDumps_py_dumps_fast JsonDumps_py_dumps_fast_portable
#endif
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "json_dumps_walk.h"
#include "json_f64_fmt.h"

void JsonDetectAvx2(void);
extern int g_jsonHasAvx2;

#define DUMPS_INIT_CAP 4096
#define DEFAULT_RECURSE_LIMIT 254
#define STACK_INIT 64
#define CYCLE_CHECK_DEPTH 64
#define SLACK 64 /* writable bytes beyond any reservation (SIMD over-store) */

enum { K_LIST = 0, K_TUPLE = 1, K_DICT = 2 };

typedef struct {
    PyObject *obj;
    Py_ssize_t pos;
    Py_ssize_t cnt;
    int kind;
    int owned;
} Ent;

typedef struct {
    char *cur;
    char *end; /* base + cap - SLACK */
    char *base;
    int scratch;     /* base is the shared warm buffer */
    PyObject *bytes; /* non-NULL once the output lives in the result bytes */
    char *priv;      /* private malloc buffer we own (re-entrant call), or NULL */
    Ent *stk;
    Py_ssize_t sp;
    Py_ssize_t scap;
    PyObject *default_fn;
    int allow_nan;
    int avx2;
    unsigned opt; /* orjson OPT_* bits (src/json_dumps_ext.inc) */
    Ent small[STACK_INIT];
} Ctx;

static int g_avx_ready = 0;

/* ------------------------------------------------------------------ buffer */

/* Warm write buffer shared by calls (GIL-serialized). A call that finds it
 * busy (default= re-entering dumps, or another thread while default= runs)
 * writes into its own malloc buffer instead. The scratch stays at most
 * DIRECT_SWITCH (64 KiB): a longer output moves into the result bytes object
 * (sized by the last long output) and is written there directly. */
static char *g_scratch = NULL;
static size_t g_scratch_cap = 0;
static int g_scratch_busy = 0;

#define DIRECT_SWITCH (64u << 10)
static size_t g_size_hint = 0; /* length of the last output that went direct */

static int w_grow(Ctx *restrict c, size_t need)
{
    size_t used = (size_t)(c->cur - c->base);
    size_t cap = (size_t)(c->end - c->base) + SLACK;
    size_t ncap = cap * 2;
    char *nb;
    if (ncap < used + need + SLACK) {
        ncap = used + need + SLACK;
    }
    if (ncap > (size_t)PY_SSIZE_T_MAX) {
        PyErr_NoMemory();
        return -1;
    }
    if (c->bytes) {
        if (_PyBytes_Resize(&c->bytes, (Py_ssize_t)ncap) < 0) {
            c->bytes = NULL;
            return -1;
        }
        nb = PyBytes_AS_STRING(c->bytes);
    } else if (ncap > DIRECT_SWITCH) {
        /* large output: continue in the result object itself */
        size_t want = g_size_hint + g_size_hint / 8 + SLACK;
        if (want > ncap) {
            ncap = want;
        }
        c->bytes = PyBytes_FromStringAndSize(NULL, (Py_ssize_t)ncap);
        if (!c->bytes) {
            return -1;
        }
        nb = PyBytes_AS_STRING(c->bytes);
        memcpy(nb, c->base, used);
        if (c->priv) {
            free(c->priv);
            c->priv = NULL;
        }
    } else {
        nb = (char *)realloc(c->base, ncap);
        if (!nb) {
            PyErr_NoMemory();
            return -1;
        }
        if (c->scratch) {
            g_scratch = nb;
            g_scratch_cap = ncap;
        } else {
            c->priv = nb;
        }
    }
    c->base = nb;
    c->cur = nb + used;
    c->end = nb + ncap - SLACK;
    return 0;
}

#define RESERVE(c, n)                                                                  \
    do {                                                                               \
        if ((size_t)((c)->end - (c)->cur) < (size_t)(n) && w_grow((c), (size_t)(n)) < 0) \
            return -1;                                                                 \
    } while (0)
/* same, with the caller's own way out (must leave the enclosing block) */
#define RESERVE_OR(c, n, fail)                                                           \
    do {                                                                               \
        if ((size_t)((c)->end - (c)->cur) < (size_t)(n) && w_grow((c), (size_t)(n)) < 0) { \
            fail;                                                                      \
        }                                                                              \
    } while (0)

/* ------------------------------------------------------------------ str */

static const unsigned char ESC[256] = {
    /* 0 = copy, else the second byte of a two-byte escape, 'u' = \u00XX */
    'u','u','u','u','u','u','u','u','b','t','n','u','f','r','u','u',
    'u','u','u','u','u','u','u','u','u','u','u','u','u','u','u','u',
    0,0,'"',0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,'\\',0,0,0,
};

static const char HEX[16] = "0123456789abcdef";

static inline char *esc_one(char *d, unsigned char ch)
{
    unsigned char e = ESC[ch];
    d[0] = '\\';
    d[1] = (char)e;
    if (e != 'u') {
        return d + 2;
    }
    d[2] = '0';
    d[3] = '0';
    d[4] = HEX[ch >> 4];
    d[5] = HEX[ch & 15u];
    return d + 6;
}

#ifdef JD_PORTABLE
static char *esc_copy_scalar(char *d, const unsigned char *s, size_t n);
#define esc_copy_avx2(d, s, n) esc_copy_scalar((d), (s), (n))
#define esc_fast64(d, s, n) ((void)(s), (char *)NULL)
#ifdef JV16
/* n <= 32, source readable for 32 bytes (same page), no byte needs escaping:
 * returns the new write pointer, else NULL. Destination has 32 bytes of room. */
static inline char *esc_fast32(char *d, const unsigned char *s, size_t n)
{
    jv16 a;
    jv16 b;
    jv_mask_t ma;
    jv_mask_t mb;
    if (n > 32 || (((uintptr_t)s) & 4095u) > 4096u - 32u) {
        return NULL;
    }
    if (n <= 8) {
        /* one word: short keys and values never touch a vector */
        const uint64_t ones = 0x0101010101010101ULL;
        const uint64_t high = 0x8080808080808080ULL;
        uint64_t v;
        uint64_t q;
        uint64_t bs;
        uint64_t sp;
        memcpy(&v, s, 8);
        memcpy(d, &v, 8);
        q = v ^ (ones * '"');
        bs = v ^ (ones * '\\');
        sp = (((v - ones * 0x20u) & ~v) | ((q - ones) & ~q) | ((bs - ones) & ~bs)) & high;
        /* a borrow only travels upward, so bits of the first n bytes are exact
         * unless a lower byte already matched; either way a set bit means "not clean" */
        if (n < 8) {
            sp &= ((uint64_t)1 << (8 * n)) - 1u;
        }
        return sp ? NULL : d + n;
    }
    a = jv_load(s);
    b = jv_load(s + 16);
    jv_store(d, a);
    jv_store(d + 16, b);
    ma = jv_mask(jv_special(a));
    mb = jv_mask(jv_special(b));
    if (n < 16) {
        ma &= jv_low((unsigned)n);
        mb = 0;
    } else if (n < 32) {
        mb &= jv_low((unsigned)n - 16u);
    }
    return (ma | mb) ? NULL : d + n;
}
#else
#define esc_fast32(d, s, n) ((void)(s), (char *)NULL)
#endif
#else
/* Copy n bytes of UTF-8/ASCII text with JSON escaping. Destination has room
 * for 6*n + SLACK. Returns the new write pointer. */
__attribute__((target("avx2,bmi")))
static char *esc_copy_avx2(char *d, const unsigned char *s, size_t n)
{
    const __m256i q = _mm256_set1_epi8('"');
    const __m256i bs = _mm256_set1_epi8('\\');
    const __m256i c1f = _mm256_set1_epi8(0x1f);
    if (n <= 16 && (((uintptr_t)s) & 4095u) <= 4096u - 16u) {
        /* short string: one 16-byte load, store and check */
        __m128i v = _mm_loadu_si128((const __m128i *)s);
        uint32_t m;
        _mm_storeu_si128((__m128i *)d, v);
        m = (uint32_t)_mm_movemask_epi8(_mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(v, _mm256_castsi256_si128(q)), _mm_cmpeq_epi8(v, _mm256_castsi256_si128(bs))),
            _mm_cmpeq_epi8(_mm_max_epu8(v, _mm256_castsi256_si128(c1f)), _mm256_castsi256_si128(c1f))));
        m &= (uint32_t)((1u << n) - 1u);
        if (!m) {
            return d + n;
        }
    }
    for (;;) {
        __m256i v;
        uint32_t m;
        if (n >= 32) {
            v = _mm256_loadu_si256((const __m256i *)s);
        } else {
            if (n == 0) {
                return d;
            }
            if ((((uintptr_t)s) & 4095u) <= 4096u - 32u) {
                v = _mm256_loadu_si256((const __m256i *)s); /* same page: no fault */
            } else {
                /* near a page end: scalar tail */
                while (n) {
                    unsigned char ch = *s++;
                    n--;
                    if (ESC[ch]) {
                        d = esc_one(d, ch);
                    } else {
                        *d++ = (char)ch;
                    }
                }
                return d;
            }
        }
        _mm256_storeu_si256((__m256i *)d, v);
        m = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, bs)),
            _mm256_cmpeq_epi8(_mm256_max_epu8(v, c1f), c1f)));
        if (n < 32) {
            m &= (uint32_t)((1ull << n) - 1u);
            if (!m) {
                return d + n;
            }
        } else if (!m) {
            d += 32;
            s += 32;
            n -= 32;
            continue;
        }
        {
            unsigned off = (unsigned)_tzcnt_u32(m);
            d += off;
            s += off;
            n -= off;
            d = esc_one(d, *s);
            s++;
            n--;
        }
    }
}

#endif /* !JD_PORTABLE */
static char *esc_copy_scalar(char *d, const unsigned char *s, size_t n)
{
#if defined(JD_PORTABLE) && defined(JV16)
    /* 16 bytes at a time: copy, then look for a byte that needs an escape */
    while (n >= 16) {
        jv16 v = jv_load(s);
        jv_mask_t m;
        jv_store(d, v);
        m = jv_mask(jv_special(v));
        if (!m) {
            d += 16;
            s += 16;
            n -= 16;
        } else {
            unsigned off = jv_first(m);
            d = esc_one(d + off, s[off]);
            s += off + 1;
            n -= off + 1;
        }
    }
#endif
    while (n) {
        unsigned char ch = *s++;
        n--;
        if (ESC[ch]) {
            d = esc_one(d, ch);
        } else {
            *d++ = (char)ch;
        }
    }
    return d;
}

#ifndef JD_PORTABLE
/* Inline fast path: n <= 32, source readable for 32 bytes (same page), no
 * byte needs escaping. Returns the new write pointer, or NULL to take the
 * general copy. Destination has 32 bytes of room. */
__attribute__((target("avx2,bmi")))
static inline char *esc_fast32(char *d, const unsigned char *s, size_t n)
{
    __m256i v;
    __m256i m;
    uint32_t bits;
    if (n > 32 || (((uintptr_t)s) & 4095u) > 4096u - 32u) {
        return NULL;
    }
    v = _mm256_loadu_si256((const __m256i *)s);
    _mm256_storeu_si256((__m256i *)d, v);
    m = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('"')),
                                        _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\\'))),
                        _mm256_cmpeq_epi8(_mm256_max_epu8(v, _mm256_set1_epi8(0x1f)), _mm256_set1_epi8(0x1f)));
    bits = (uint32_t)_mm256_movemask_epi8(m) & (uint32_t)((1ull << n) - 1u);
    return bits ? NULL : d + n;
}

/* 33..64 bytes: two loads, the second one ending at the last byte. */
__attribute__((target("avx2,bmi")))
static inline char *esc_fast64(char *d, const unsigned char *s, size_t n)
{
    __m256i a;
    __m256i b;
    __m256i q = _mm256_set1_epi8('"');
    __m256i bs = _mm256_set1_epi8('\\');
    __m256i c1f = _mm256_set1_epi8(0x1f);
    uint64_t m;
    if (n <= 32 || n > 64) {
        return NULL;
    }
    a = _mm256_loadu_si256((const __m256i *)s);
    b = _mm256_loadu_si256((const __m256i *)(s + n - 32));
    m = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(a, q), _mm256_cmpeq_epi8(a, bs)),
            _mm256_cmpeq_epi8(_mm256_max_epu8(a, c1f), c1f))) |
        (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(b, q), _mm256_cmpeq_epi8(b, bs)),
            _mm256_cmpeq_epi8(_mm256_max_epu8(b, c1f), c1f)));
    if (m) {
        return NULL;
    }
    _mm256_storeu_si256((__m256i *)d, a);
    _mm256_storeu_si256((__m256i *)(d + n - 32), b);
    return d + n;
}
#endif

static inline char *esc_any(Ctx *restrict c, char *d, const unsigned char *p, size_t n)
{
    if (c->avx2) {
        char *r = n <= 32 ? esc_fast32(d, p, n) : esc_fast64(d, p, n);
        if (r) {
            return r;
        }
        return esc_copy_avx2(d, p, n);
    }
    return esc_copy_scalar(d, p, n);
}

static int write_str_slow(Ctx *restrict c, PyObject *s)
{
    const unsigned char *p;
    Py_ssize_t n;
    if (PyUnicode_IS_COMPACT_ASCII(s)) {
        p = (const unsigned char *)(((PyASCIIObject *)s) + 1);
        n = ((PyASCIIObject *)s)->length;
    } else {
        p = (const unsigned char *)PyUnicode_AsUTF8AndSize(s, &n);
        if (!p) {
            if (PyErr_ExceptionMatches(PyExc_UnicodeEncodeError)) {
                PyErr_Clear();
                PyErr_SetString(PyExc_TypeError, "str is not valid UTF-8: surrogates not allowed");
            }
            return -1;
        }
    }
    if ((size_t)n > ((size_t)PY_SSIZE_T_MAX - 2 * SLACK) / 6u) {
        PyErr_NoMemory();
        return -1;
    }
    RESERVE(c, (size_t)n * 6u + 2u);
    *c->cur++ = '"';
    c->cur = esc_any(c, c->cur, p, (size_t)n);
    *c->cur++ = '"';
    return 0;
}

/* Fast path: exact layout reads, short clean text inline. */
static inline __attribute__((always_inline)) int write_str(Ctx *restrict c, PyObject *s)
{
    const unsigned char *p;
    size_t n;
    if (PyUnicode_IS_COMPACT_ASCII(s)) {
        p = (const unsigned char *)(((PyASCIIObject *)s) + 1);
        n = (size_t)((PyASCIIObject *)s)->length;
    }
#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030E0000
    else if (PyUnicode_IS_COMPACT(s) && ((PyCompactUnicodeObject *)s)->utf8) {
        p = (const unsigned char *)((PyCompactUnicodeObject *)s)->utf8;
        n = (size_t)((PyCompactUnicodeObject *)s)->utf8_length;
    }
#endif
    else {
        return write_str_slow(c, s);
    }
    if (n <= 64 && c->avx2 && (size_t)(c->end - c->cur) >= 72) {
        char *r = n <= 32 ? esc_fast32(c->cur + 1, p, n) : esc_fast64(c->cur + 1, p, n);
        if (r) {
            c->cur[0] = '"';
            r[0] = '"';
            c->cur = r + 1;
            return 0;
        }
    }
    return write_str_slow(c, s);
}

/* ------------------------------------------------------------------ int */

static inline char *write_u64(char *d, uint64_t u)
{
    int n;
    if (u < 10u) {
        *d = (char)('0' + u);
        return d + 1;
    }
    n = jf64_ndig(u);
    jf64_digits(d, u, n);
    return d + n;
}

static inline char *write_i64(char *d, int64_t v)
{
    if (v < 0) {
        *d++ = '-';
        return write_u64(d, (uint64_t)0 - (uint64_t)v);
    }
    return write_u64(d, (uint64_t)v);
}

/* u < 2^32: SWAR eight digits (no division chain), leading zeros shifted out. */
static inline char *write_u32(char *d, uint32_t u)
{
    uint64_t w;
    int n;
    if (u < 10u) {
        d[0] = (char)('0' + u);
        return d + 1;
    }
    if (u < 100u) {
        memcpy(d, jf64_d2 + 2 * u, 2);
        return d + 2;
    }
    if (u >= 100000000u) {
        uint32_t hi = u / 100000000u;
        u -= hi * 100000000u;
        if (hi < 10u) {
            *d++ = (char)('0' + hi);
        } else {
            memcpy(d, jf64_d2 + 2 * hi, 2);
            d += 2;
        }
        w = jf64_swar8(u) + 0x3030303030303030ULL;
        memcpy(d, &w, 8);
        return d + 8;
    }
    n = u < 1000u ? 3 : u < 10000u ? 4 : u < 100000u ? 5 : u < 1000000u ? 6 : u < 10000000u ? 7 : 8;
    w = (jf64_swar8(u) + 0x3030303030303030ULL) >> (8 * (8 - n));
    memcpy(d, &w, 8);
    return d + n;
}

/* Compact int (|v| < 2^30 on 3.12) written into reserved room (>= 24).
 * Returns 0 when o is not compact. */
static inline int put_compact_int(Ctx *restrict c, PyObject *o)
{
#if PY_VERSION_HEX >= 0x030C0000
    if (PyUnstable_Long_IsCompact((PyLongObject *)o)) {
        Py_ssize_t v = PyUnstable_Long_CompactValue((PyLongObject *)o);
        char *d = c->cur;
        if (v < 0) {
            *d++ = '-';
            v = -v;
        }
        c->cur = write_u32(d, (uint32_t)v);
        return 1;
    }
#else
    (void)c;
    (void)o;
#endif
    return 0;
}

static int write_int(Ctx *restrict c, PyObject *o)
{
    int64_t v;
#if PY_VERSION_HEX >= 0x030C0000
    if (PyUnstable_Long_IsCompact((PyLongObject *)o)) {
        v = (int64_t)PyUnstable_Long_CompactValue((PyLongObject *)o);
    } else
#endif
    {
        int overflow = 0;
        long long ll = PyLong_AsLongLongAndOverflow(o, &overflow);
        if (overflow > 0) {
            unsigned long long u = PyLong_AsUnsignedLongLong(o);
            if (u != (unsigned long long)-1 || !PyErr_Occurred()) {
                RESERVE(c, 24);
                c->cur = write_u64(c->cur, (uint64_t)u);
                return 0;
            }
            PyErr_Clear();
        }
        if (overflow) {
            PyErr_SetString(PyExc_TypeError, "Integer exceeds 64-bit range");
            return -1;
        }
        if (ll == -1 && PyErr_Occurred()) {
            return -1;
        }
        v = (int64_t)ll;
    }
    RESERVE(c, 24);
    c->cur = write_i64(c->cur, v);
    return 0;
}

/* ------------------------------------------------------------------ float */

static int write_float(Ctx *restrict c, double d)
{
    RESERVE(c, 48);
    if (!isfinite(d)) {
        /* not JSON: written as null, the way orjson does (loads rejects NaN too) */
        const char *t = "null";
        size_t n = 4;
        if (!c->allow_nan) {
            PyErr_Format(PyExc_ValueError, "Out of range float values are not JSON compliant: %s",
                         d != d ? "nan" : (d > 0 ? "inf" : "-inf"));
            return -1;
        }
        memcpy(c->cur, t, n);
        c->cur += n;
        return 0;
    }
    c->cur += jf64_write(c->cur, d);
    return 0;
}

/* ------------------------------------------------------------------ walk */

static int stack_push(Ctx *restrict c, PyObject *o, int kind, int owned)
{
    Ent *e;
    if (c->sp == c->scap) {
        Py_ssize_t ncap = c->scap * 2;
        Ent *nt;
        if (c->stk == c->small) {
            nt = (Ent *)PyMem_Malloc((size_t)ncap * sizeof(Ent));
            if (nt) {
                memcpy(nt, c->small, (size_t)c->sp * sizeof(Ent));
            }
        } else {
            nt = (Ent *)PyMem_Realloc(c->stk, (size_t)ncap * sizeof(Ent));
        }
        if (!nt) {
            PyErr_NoMemory();
            return -1;
        }
        c->stk = nt;
        c->scap = ncap;
    }
    if (c->sp >= CYCLE_CHECK_DEPTH) {
        Py_ssize_t i;
        for (i = 0; i < c->sp; i++) {
            if (c->stk[i].obj == o) {
                PyErr_SetString(PyExc_ValueError, "Circular reference detected");
                return -1;
            }
        }
    }
    e = &c->stk[c->sp++];
    e->obj = o;
    e->pos = 0;
    e->cnt = 0;
    e->kind = kind;
    e->owned = owned;
    return 0;
}

static void type_not_serializable(PyObject *obj)
{
    PyErr_Format(PyExc_TypeError, "Object of type %.200s is not JSON serializable",
                 Py_TYPE(obj)->tp_name);
}

/* Write a leaf or open a container.
 * Returns 0 done, -1 error. `owned` = we hold a new ref to o. */
static int write_any(Ctx *restrict c, PyObject *o, int owned);

static int open_container(Ctx *restrict c, PyObject *o, int kind, int owned)
{
    Py_ssize_t n;
    if (kind == K_DICT) {
        n = PyDict_GET_SIZE(o);
    } else if (kind == K_LIST) {
        n = PyList_GET_SIZE(o);
    } else {
        n = PyTuple_GET_SIZE(o);
    }
    RESERVE(c, 2);
    if (n == 0) {
        c->cur[0] = kind == K_DICT ? '{' : '[';
        c->cur[1] = kind == K_DICT ? '}' : ']';
        c->cur += 2;
        if (owned) {
            Py_DECREF(o);
        }
        return 0;
    }
    *c->cur++ = kind == K_DICT ? '{' : '[';
    if (stack_push(c, o, kind, owned) < 0) {
        if (owned) {
            Py_DECREF(o);
        }
        return -1;
    }
    return 0;
}

#include "json_dumps_ext.inc"

static int write_slow(Ctx *restrict c, PyObject *o, int owned)
{
    int n_default = 0;
    for (;;) {
        PyTypeObject *t = Py_TYPE(o);
        int rc;
        int exact = t == &PyUnicode_Type || t == &PyLong_Type || t == &PyFloat_Type || t == &PyBool_Type ||
                    o == Py_None || t == &PyList_Type || t == &PyTuple_Type || t == &PyDict_Type;
        int sub = exact || !(c->opt & JOPT_PASSTHROUGH_SUBCLASS);
        if (!exact) {
            /* enum, datetime, dataclass, UUID, numpy */
            PyObject *conv = NULL;
            rc = ext_try(c, o, &conv);
            if (rc == 2 || rc == 3) {
                if (owned) {
                    Py_DECREF(o);
                }
                o = conv;
                owned = 1;
                continue;
            }
            if (rc == 4) {
                /* numpy content: one value through the general writer */
                unsigned save = c->opt;
                c->opt &= ~(JOPT_STRICT_INTEGER | JOPT_PASSTHROUGH_DATETIME);
                rc = opt_value(c, conv, 0);
                c->opt = save;
                Py_DECREF(conv);
                if (owned) {
                    Py_DECREF(o);
                }
                return rc;
            }
            if (rc != 0) {
                if (owned) {
                    Py_DECREF(o);
                }
                return rc < 0 ? -1 : 0;
            }
        }
        if (t == &PyUnicode_Type || (sub && PyUnicode_Check(o))) {
            rc = write_str(c, o);
        } else if (o == Py_True || o == Py_False) {
            RESERVE(c, 8);
            if (o == Py_True) {
                memcpy(c->cur, "true", 4);
                c->cur += 4;
            } else {
                memcpy(c->cur, "false", 5);
                c->cur += 5;
            }
            rc = 0;
        } else if (t == &PyLong_Type || (sub && PyLong_Check(o))) {
            rc = write_int(c, o);
        } else if (o == Py_None) {
            RESERVE(c, 4);
            memcpy(c->cur, "null", 4);
            c->cur += 4;
            rc = 0;
        } else if (t == &PyFloat_Type) {
            rc = write_float(c, PyFloat_AS_DOUBLE(o));
        } else if (t == &PyList_Type || (sub && PyList_Check(o))) {
            return open_container(c, o, K_LIST, owned);
        } else if (t == &PyTuple_Type) {
            return open_container(c, o, K_TUPLE, owned);
        } else if (t == &PyDict_Type || (sub && PyDict_Check(o))) {
            return open_container(c, o, K_DICT, owned);
        } else {
            PyObject *conv;
            if (!c->default_fn) {
                type_not_serializable(o);
                rc = -1;
            } else if (n_default >= DEFAULT_RECURSE_LIMIT) {
                PyErr_SetString(PyExc_TypeError, "default serializer exceeds recursion limit");
                rc = -1;
            } else {
                conv = PyObject_CallOneArg(c->default_fn, o);
                if (owned) {
                    Py_DECREF(o);
                }
                if (!conv) {
                    return -1;
                }
                o = conv;
                owned = 1;
                n_default++;
                continue;
            }
        }
        if (owned) {
            Py_DECREF(o);
        }
        return rc;
    }
}

/* Exact-type fast dispatch. */
static inline int write_any(Ctx *restrict c, PyObject *o, int owned)
{
    PyTypeObject *t = Py_TYPE(o);
    if (t == &PyUnicode_Type) {
        if (!owned) {
            return write_str(c, o);
        }
    } else if (t == &PyLong_Type) {
        if (!owned) {
            return write_int(c, o);
        }
    } else if (t == &PyFloat_Type) {
        if (!owned) {
            return write_float(c, PyFloat_AS_DOUBLE(o));
        }
    } else if (t == &PyDict_Type) {
        return open_container(c, o, K_DICT, owned);
    } else if (t == &PyList_Type) {
        return open_container(c, o, K_LIST, owned);
    } else if (o == Py_None && !owned) {
        RESERVE(c, 4);
        memcpy(c->cur, "null", 4);
        c->cur += 4;
        return 0;
    } else if (t == &PyBool_Type && !owned) {
        RESERVE(c, 8);
        if (o == Py_True) {
            memcpy(c->cur, "true", 4);
            c->cur += 4;
        } else {
            memcpy(c->cur, "false", 5);
            c->cur += 5;
        }
        return 0;
    } else if (t == &PyTuple_Type) {
        return open_container(c, o, K_TUPLE, owned);
    }
    return write_slow(c, o, owned);
}

static int write_key(Ctx *restrict c, PyObject *k)
{
    if (Py_TYPE(k) == &PyUnicode_Type && PyUnicode_IS_COMPACT_ASCII(k)) {
        const unsigned char *p = (const unsigned char *)(((PyASCIIObject *)k) + 1);
        Py_ssize_t n = ((PyASCIIObject *)k)->length;
        RESERVE(c, (size_t)n * 6u + 3u);
        *c->cur++ = '"';
        c->cur = esc_any(c, c->cur, p, (size_t)n);
        c->cur[0] = '"';
        c->cur[1] = ':';
        c->cur += 2;
        return 0;
    }
    if (Py_TYPE(k) != &PyUnicode_Type) {
        /* a subclass of str is a key only with OPT_NON_STR_KEYS, as in orjson */
        PyErr_SetString(PyExc_TypeError, "Dict key must be str");
        return -1;
    }
    if (write_str(c, k) < 0) {
        return -1;
    }
    RESERVE(c, 1);
    *c->cur++ = ':';
    return 0;
}

static int walk(Ctx *restrict c)
{
    while (c->sp > 0) {
        Ent *e = &c->stk[c->sp - 1];
        PyObject *o = e->obj;
        Py_ssize_t base_sp = c->sp;
        if (e->kind == K_DICT) {
            PyObject *k;
            PyObject *v;
            Py_ssize_t pos = e->pos;
            while (PyDict_Next(o, &pos, &k, &v)) {
                if (e->cnt++) {
                    RESERVE(c, 1);
                    *c->cur++ = ',';
                }
                if (write_key(c, k) < 0) {
                    return -1;
                }
                if (write_any(c, v, 0) < 0) {
                    return -1;
                }
                if (c->sp != base_sp) {
                    break; /* a nested container was pushed */
                }
                e = &c->stk[c->sp - 1];
            }
            if (c->sp != base_sp) {
                c->stk[base_sp - 1].pos = pos;
                continue;
            }
            RESERVE(c, 1);
            *c->cur++ = '}';
        } else {
            PyObject **items;
            Py_ssize_t i = e->pos;
            int is_list = e->kind == K_LIST;
            for (;;) {
                Py_ssize_t n = is_list ? PyList_GET_SIZE(o) : PyTuple_GET_SIZE(o);
                PyObject *v;
                if (i >= n) {
                    break;
                }
                items = is_list ? ((PyListObject *)o)->ob_item : ((PyTupleObject *)o)->ob_item;
                v = items[i++];
                if (i > 1) {
                    RESERVE(c, 1);
                    *c->cur++ = ',';
                }
                if (write_any(c, v, 0) < 0) {
                    return -1;
                }
                if (c->sp != base_sp) {
                    break;
                }
            }
            if (c->sp != base_sp) {
                c->stk[base_sp - 1].pos = i;
                continue;
            }
            RESERVE(c, 1);
            *c->cur++ = ']';
        }
        /* pop */
        c->sp--;
        if (c->stk[c->sp].owned) {
            Py_DECREF(c->stk[c->sp].obj);
        }
    }
    return 0;
}

/* ------------------------------------------------------------ recursive
 * Depth < REC_LIMIT is plain C recursion (state in registers, no frame
 * push/pop per container). Deeper values go to the iterative walk above,
 * which also carries the cycle check. */
#define REC_LIMIT 200

static int rec_value(Ctx *restrict c, PyObject *o, int depth);

static int rec_deep(Ctx *restrict c, PyObject *o)
{
    Py_ssize_t base = c->sp;
    if (write_any(c, o, 0) < 0) {
        return -1;
    }
    /* walk() runs until the stack is empty; it only ever sees frames above base (base is 0 here) */
    (void)base;
    return walk(c);
}

static inline int rec_true_false_none(Ctx *restrict c, PyObject *o)
{
    if (o == Py_True) {
        memcpy(c->cur, "true", 4);
        c->cur += 4;
    } else if (o == Py_False) {
        memcpy(c->cur, "false", 5);
        c->cur += 5;
    } else {
        memcpy(c->cur, "null", 4);
        c->cur += 4;
    }
    return 0;
}

/* Short list of exact float / compact int written in place (coordinates,
 * pairs, small vectors). Returns 1 when written, 0 to take rec_list. The
 * caller reserved SMALL_SEQ_ROOM bytes. */
#define SMALL_SEQ_MAX 8
#define SMALL_SEQ_ROOM (2 + SMALL_SEQ_MAX * 49)
static inline __attribute__((always_inline)) int put_small_numeric_list(Ctx *restrict c, PyObject *v)
{
    Py_ssize_t n = PyList_GET_SIZE(v);
    PyObject **it = ((PyListObject *)v)->ob_item;
    char *start = c->cur;
    char *d = start;
    Py_ssize_t i;
    *d++ = '[';
    for (i = 0; i < n; i++) {
        PyObject *x = it[i];
        PyTypeObject *t = Py_TYPE(x);
        if (i) {
            *d++ = ',';
        }
        if (t == &PyFloat_Type) {
            double f = PyFloat_AS_DOUBLE(x);
            if (!isfinite(f)) {
                return 0;
            }
            d += jf64_write(d, f);
        } else if (t == &PyLong_Type) {
            c->cur = d;
            if (!put_compact_int(c, x)) {
                c->cur = start;
                return 0;
            }
            d = c->cur;
        } else {
            c->cur = start;
            return 0;
        }
    }
    *d++ = ']';
    c->cur = d;
    return 1;
}

static int rec_list(Ctx *restrict c, PyObject *o, int is_list, int depth)
{
    Py_ssize_t i;
    RESERVE(c, 2);
    *c->cur++ = '[';
    for (i = 0;; i++) {
        PyObject *v;
        PyTypeObject *t;
        Py_ssize_t n = is_list ? PyList_GET_SIZE(o) : PyTuple_GET_SIZE(o);
        if (i >= n) {
            break;
        }
        v = is_list ? ((PyListObject *)o)->ob_item[i] : ((PyTupleObject *)o)->ob_item[i];
        RESERVE(c, 64);
        if (i) {
            *c->cur++ = ',';
        }
        t = Py_TYPE(v);
        if (t == &PyFloat_Type) {
            double d = PyFloat_AS_DOUBLE(v);
            if (isfinite(d)) {
                c->cur += jf64_write(c->cur, d);
                continue;
            }
            if (write_float(c, d) < 0) {
                return -1;
            }
        } else if (t == &PyUnicode_Type) {
            if (write_str(c, v) < 0) {
                return -1;
            }
        } else if (t == &PyLong_Type) {
            if (!put_compact_int(c, v) && write_int(c, v) < 0) {
                return -1;
            }
        } else if (t == &PyList_Type) {
            if (PyList_GET_SIZE(v) <= SMALL_SEQ_MAX) {
                RESERVE(c, SMALL_SEQ_ROOM);
                if (put_small_numeric_list(c, v)) {
                    continue;
                }
            }
            if (depth >= REC_LIMIT) {
                if (rec_deep(c, v) < 0) {
                    return -1;
                }
            } else if (rec_list(c, v, 1, depth + 1) < 0) {
                return -1;
            }
        } else if (t == &PyDict_Type || t == &PyTuple_Type || v == Py_None || t == &PyBool_Type) {
            if (v == Py_None || t == &PyBool_Type) {
                rec_true_false_none(c, v);
            } else if (rec_value(c, v, depth) < 0) {
                return -1;
            }
        } else if (rec_value(c, v, depth) < 0) {
            return -1;
        }
    }
    RESERVE(c, 1);
    *c->cur++ = ']';
    return 0;
}

#if PY_VERSION_HEX >= 0x030C0000 && PY_VERSION_HEX < 0x030D0000 && !defined(Py_GIL_DISABLED)
/* CPython 3.12 combined-table layout (Include/internal/pycore_dict.h). */
#define JD_DIRECT 1
typedef struct {
    Py_ssize_t dk_refcnt;
    uint8_t dk_log2_size;
    uint8_t dk_log2_index_bytes;
    uint8_t dk_kind;
    uint32_t dk_version;
    Py_ssize_t dk_usable;
    Py_ssize_t dk_nentries;
    char dk_indices[];
} JDictKeys;
typedef struct {
    PyObject *k;
    PyObject *v;
} JUEnt;
typedef struct {
    Py_hash_t h;
    PyObject *k;
    PyObject *v;
} JGEnt;
#endif

/* One dict item value (key already written). */
static inline int rec_item(Ctx *restrict c, PyObject *v, int depth)
{
    PyTypeObject *t = Py_TYPE(v);
    if (t == &PyUnicode_Type) {
        return write_str(c, v);
    }
    if (t == &PyLong_Type) {
        RESERVE(c, 24);
        if (put_compact_int(c, v)) {
            return 0;
        }
        return write_int(c, v);
    }
    if (t == &PyFloat_Type) {
        double d = PyFloat_AS_DOUBLE(v);
        if (isfinite(d)) {
            RESERVE(c, 48);
            c->cur += jf64_write(c->cur, d);
            return 0;
        }
        return write_float(c, d);
    }
    if (v == Py_None || t == &PyBool_Type) {
        RESERVE(c, 8);
        return rec_true_false_none(c, v);
    }
    if (t == &PyList_Type && PyList_GET_SIZE(v) <= SMALL_SEQ_MAX) {
        RESERVE(c, SMALL_SEQ_ROOM);
        if (put_small_numeric_list(c, v)) {
            return 0;
        }
    }
    return rec_value(c, v, depth);
}

/* ,"key": for an exact-str key (comma optional). */
static inline __attribute__((always_inline)) int rec_key(Ctx *restrict c, PyObject *k, int comma)
{
    if (PyUnicode_IS_COMPACT_ASCII(k)) {
        const unsigned char *p = (const unsigned char *)(((PyASCIIObject *)k) + 1);
        Py_ssize_t n = ((PyASCIIObject *)k)->length;
        if (n <= 32 && c->avx2 && (c->end - c->cur) >= 40) {
            char *d = c->cur;
            char *r;
            d[0] = ',';
            d += comma;
            r = esc_fast32(d + 1, p, (size_t)n);
            if (r) {
                d[0] = '"';
                r[0] = '"';
                r[1] = ':';
                c->cur = r + 2;
                return 0;
            }
        }
        RESERVE(c, (size_t)n * 6u + 4u);
        if (comma) {
            *c->cur++ = ',';
        }
        *c->cur++ = '"';
        c->cur = esc_any(c, c->cur, p, (size_t)n);
        c->cur[0] = '"';
        c->cur[1] = ':';
        c->cur += 2;
        return 0;
    }
    if (comma) {
        RESERVE(c, 1);
        *c->cur++ = ',';
    }
    return write_key(c, k);
}

static int rec_dict(Ctx *restrict c, PyObject *o, int depth)
{
    PyObject *k;
    PyObject *v;
    Py_ssize_t pos = 0;
    int first = 1;
    RESERVE(c, 2);
    *c->cur++ = '{';
#ifdef JD_DIRECT
    if (!c->default_fn && ((PyDictObject *)o)->ma_values == NULL &&
        ((JDictKeys *)((PyDictObject *)o)->ma_keys)->dk_kind != 0) {
        /* Unicode-key combined table and no default=: no Python code can run
         * while this dict is written, so its entry table stays put. */
        JDictKeys *dk = (JDictKeys *)((PyDictObject *)o)->ma_keys;
        JUEnt *ent = (JUEnt *)(dk->dk_indices + ((size_t)1 << dk->dk_log2_index_bytes));
        JUEnt *eend = ent + dk->dk_nentries;
        for (; ent < eend; ent++) {
            v = ent->v;
            if (!v) {
                continue;
            }
            if (rec_key(c, ent->k, !first) < 0) {
                return -1;
            }
            first = 0;
            if (rec_item(c, v, depth) < 0) {
                return -1;
            }
        }
        RESERVE(c, 1);
        *c->cur++ = '}';
        return 0;
    }
    if (((PyDictObject *)o)->ma_values == NULL && ((JDictKeys *)((PyDictObject *)o)->ma_keys)->dk_kind != 0) {
        /* unicode-key combined table: keys are exact str */
        Py_ssize_t i;
        for (i = 0;; i++) {
            JDictKeys *dk = (JDictKeys *)((PyDictObject *)o)->ma_keys;
            JUEnt *ent;
            if (((PyDictObject *)o)->ma_values != NULL || dk->dk_kind == 0) {
                pos = i; /* table changed shape under a default= call: finish generically */
                goto generic;
            }
            if (i >= dk->dk_nentries) {
                break;
            }
            ent = (JUEnt *)(dk->dk_indices + ((size_t)1 << dk->dk_log2_index_bytes)) + i;
            v = ent->v;
            if (!v) {
                continue;
            }
            if (rec_key(c, ent->k, !first) < 0) {
                return -1;
            }
            first = 0;
            if (rec_item(c, v, depth) < 0) {
                return -1;
            }
        }
        RESERVE(c, 1);
        *c->cur++ = '}';
        return 0;
    }
generic:
#endif
    while (PyDict_Next(o, &pos, &k, &v)) {
        if (Py_TYPE(k) == &PyUnicode_Type) {
            if (rec_key(c, k, !first) < 0) {
                return -1;
            }
        } else {
            if (!first) {
                RESERVE(c, 1);
                *c->cur++ = ',';
            }
            if (write_key(c, k) < 0) {
                return -1;
            }
        }
        first = 0;
        if (rec_item(c, v, depth) < 0) {
            return -1;
        }
    }
    RESERVE(c, 1);
    *c->cur++ = '}';
    return 0;
}

/* Any value at nesting `depth` (containers recurse with depth + 1). */
static int rec_value(Ctx *restrict c, PyObject *o, int depth)
{
    PyTypeObject *t = Py_TYPE(o);
    if (t == &PyDict_Type) {
        if (depth >= REC_LIMIT) {
            return rec_deep(c, o);
        }
        return rec_dict(c, o, depth + 1);
    }
    if (t == &PyList_Type || t == &PyTuple_Type) {
        if (depth >= REC_LIMIT) {
            return rec_deep(c, o);
        }
        return rec_list(c, o, t == &PyList_Type, depth + 1);
    }
    if (t == &PyUnicode_Type) {
        return write_str(c, o);
    }
    if (t == &PyLong_Type) {
        return write_int(c, o);
    }
    if (t == &PyFloat_Type) {
        return write_float(c, PyFloat_AS_DOUBLE(o));
    }
    if (o == Py_None || t == &PyBool_Type) {
        RESERVE(c, 8);
        return rec_true_false_none(c, o);
    }
    /* subclasses and default=: one value through the iterative machinery */
    return rec_deep(c, o);
}

static int dumps_tail(Ctx *restrict c)
{
    if (c->opt & JOPT_APPEND_NEWLINE) {
        RESERVE(c, 1);
        *c->cur++ = '\n';
    }
    return 0;
}

static PyObject *dumps_obj(PyObject *obj, PyObject *default_fn, int allow_nan, unsigned opt)
{
    Ctx c;
    PyObject *out = NULL;
    if (!g_avx_ready) {
        JsonDetectAvx2();
        g_avx_ready = 1;
    }
    c.bytes = NULL;
    c.priv = NULL;
    if (!g_scratch_busy) {
        if (!g_scratch) {
            g_scratch = (char *)malloc(DUMPS_INIT_CAP);
            if (!g_scratch) {
                return PyErr_NoMemory();
            }
            g_scratch_cap = DUMPS_INIT_CAP;
        }
        g_scratch_busy = 1;
        c.scratch = 1;
        c.base = g_scratch;
        c.end = g_scratch + g_scratch_cap - SLACK;
    } else {
        c.scratch = 0;
        c.priv = (char *)malloc(DUMPS_INIT_CAP);
        if (!c.priv) {
            return PyErr_NoMemory();
        }
        c.base = c.priv;
        c.end = c.base + DUMPS_INIT_CAP - SLACK;
    }
    c.cur = c.base;
    c.stk = c.small;
    c.sp = 0;
    c.scap = STACK_INIT;
    c.default_fn = (default_fn && default_fn != Py_None) ? default_fn : NULL;
    c.allow_nan = allow_nan;
    c.opt = opt;
#ifdef JD_PORTABLE
#ifdef JV16
    c.avx2 = 1; /* the short-string paths exist in their 16-byte form */
#else
    c.avx2 = 0;
#endif
#else
    c.avx2 = g_jsonHasAvx2;
#endif

    if (((opt & JOPT_GENERAL) ? opt_value(&c, obj, 0) : rec_value(&c, obj, 0)) == 0 && dumps_tail(&c) == 0) {
        size_t len = (size_t)(c.cur - c.base);
        if (c.bytes) {
            size_t cap = (size_t)PyBytes_GET_SIZE(c.bytes);
            if (cap - len <= len / 4u + 4096u) {
                /* Small tail: keep the block as allocated (no realloc). A shrink
                 * would make glibc set its mmap threshold below the next
                 * request, so every call would mmap fresh pages again. */
                Py_SET_SIZE(c.bytes, (Py_ssize_t)len);
                PyBytes_AS_STRING(c.bytes)[len] = '\0';
                out = c.bytes;
                g_size_hint = len;
            } else if (_PyBytes_Resize(&c.bytes, (Py_ssize_t)len) == 0) {
                out = c.bytes;
                g_size_hint = len;
            }
            c.bytes = NULL;
        } else {
            out = PyBytes_FromStringAndSize(c.base, (Py_ssize_t)len);
        }
    }
    Py_XDECREF(c.bytes);
    while (c.sp > 0) {
        c.sp--;
        if (c.stk[c.sp].owned) {
            Py_DECREF(c.stk[c.sp].obj);
        }
    }
    if (c.stk != c.small) {
        PyMem_Free(c.stk);
    }
    if (c.scratch) {
        g_scratch_busy = 0;
    }
    free(c.priv);
    return out;
}

static int parse_allow_nan(PyObject *allow_obj, int *allow_nan)
{
    if (allow_obj == NULL || allow_obj == Py_True) {
        *allow_nan = 1;
    } else if (allow_obj == Py_False || allow_obj == Py_None) {
        *allow_nan = 0;
    } else if (PyLong_Check(allow_obj)) {
        int overflow = 0;
        long long v = PyLong_AsLongLongAndOverflow(allow_obj, &overflow);
        if (v == -1 && PyErr_Occurred()) {
            return -1;
        }
        *allow_nan = (overflow != 0 || v != 0);
    } else {
        PyErr_SetString(PyExc_TypeError, "allow_nan must be bool");
        return -1;
    }
    return 0;
}

/* option=None or an int made of OPT_* bits */
static int parse_option(PyObject *o, unsigned *opt)
{
    long v;
    *opt = 0;
    if (!o || o == Py_None) {
        return 0;
    }
    if (!PyLong_CheckExact(o)) {
        PyErr_SetString(PyExc_TypeError, "Invalid opts");
        return -1;
    }
    v = PyLong_AsLong(o);
    if (v < 0 || v > (long)JOPT_ALL) {
        PyErr_Clear();
        PyErr_SetString(PyExc_TypeError, "Invalid opts");
        return -1;
    }
    *opt = (unsigned)v;
    return 0;
}

PyObject *JsonDumps_py_dumps(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"obj", "default", "option", "allow_nan", NULL};
    PyObject *obj = NULL;
    PyObject *default_fn = Py_None;
    PyObject *option = Py_None;
    PyObject *allow_obj = Py_True;
    int allow_nan;
    unsigned opt;
    (void)self;
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|OO$O", kwlist, &obj, &default_fn, &option, &allow_obj)) {
        return NULL;
    }
    if (parse_allow_nan(allow_obj, &allow_nan) < 0 || parse_option(option, &opt) < 0) {
        return NULL;
    }
    return dumps_obj(obj, default_fn, allow_nan, opt);
}

/* METH_FASTCALL | METH_KEYWORDS: dumps(obj, default=None, option=None, *, allow_nan=True) */
PyObject *JsonDumps_py_dumps_fast(PyObject *self, PyObject *const *args, Py_ssize_t nargs,
                                  PyObject *kwnames)
{
    PyObject *default_fn = NULL;
    PyObject *option = NULL;
    PyObject *allow_obj = NULL;
    int allow_nan = 1;
    unsigned opt = 0;
    (void)self;
    if (nargs < 1 || nargs > 3) {
        PyErr_Format(PyExc_TypeError, "dumps() takes 1 to 3 positional arguments (%zd given)", nargs);
        return NULL;
    }
    if (nargs > 1) {
        default_fn = args[1];
        if (nargs > 2) {
            option = args[2];
        }
    }
    if (kwnames) {
        Py_ssize_t i;
        Py_ssize_t nk = PyTuple_GET_SIZE(kwnames);
        for (i = 0; i < nk; i++) {
            PyObject *name = PyTuple_GET_ITEM(kwnames, i);
            if (PyUnicode_CompareWithASCIIString(name, "default") == 0) {
                default_fn = args[nargs + i];
            } else if (PyUnicode_CompareWithASCIIString(name, "option") == 0) {
                option = args[nargs + i];
            } else if (PyUnicode_CompareWithASCIIString(name, "allow_nan") == 0) {
                allow_obj = args[nargs + i];
            } else {
                PyErr_Format(PyExc_TypeError, "dumps() got an unexpected keyword argument '%U'", name);
                return NULL;
            }
        }
        if (parse_allow_nan(allow_obj, &allow_nan) < 0) {
            return NULL;
        }
    }
    if (option && parse_option(option, &opt) < 0) {
        return NULL;
    }
    return dumps_obj(args[0], default_fn, allow_nan, opt);
}

/* Module wrappers live in json_module.c. */
