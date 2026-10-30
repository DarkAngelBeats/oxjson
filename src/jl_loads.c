/* loads: the sink that builds Python objects for the kernel, and the entry point.
 *
 *   kernel  jl_run()     bytes -> calls into the sink; knows nothing of Python.
 *                        NASM (src/jl_kernel.asm), or the C version below when
 *                        JL_KERNEL_C is set (the portable build).
 *   sink    jl_mk_*()    creates str, int, float, list and dict objects.
 *
 * The kernel is iterative: a frame stack and a value stack; a list or dict is
 * built once at its closing bracket from the top of the value stack.
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#ifndef JL_PORTABLE
#include <immintrin.h>
#define JL_AVX2 __attribute__((target("avx2")))
#define JL_AVX2_BMI __attribute__((target("avx2,bmi")))
#else
/* Portable build (src/jl_portable.c): the C kernel, with NEON or SSE2 helpers where available. It is
 * the only loads on a non-x86 target and the fallback on x86 without AVX2,
 * where it is linked next to the AVX2 copy of this file under other names. */
#include "json_vec16.h"
#define JL_KERNEL_C 1
#define JL_AVX2
#define JL_AVX2_BMI
#define jl_mk_i64 jl_mk_i64_portable
#define jl_mk_u64 jl_mk_u64_portable
#define jl_mk_f64 jl_mk_f64_portable
#define jl_mk_lit jl_mk_lit_portable
#define jl_mk_list jl_mk_list_portable
#define jl_mk_dict jl_mk_dict_portable
#define jl_drop jl_drop_portable
#define jl_mk_str jl_mk_str_portable
#define jl_mk_key jl_mk_key_portable
#define jl_strtod jl_strtod_portable
#define jl_grow_vs jl_grow_vs_portable
#define jl_grow_fs jl_grow_fs_portable
#define jl_str_escaped jl_str_escaped_portable
#define jl_run jl_run_portable
#define JlLoads JlLoads_portable
#define JsonLoads_set_error_type JsonLoads_set_error_type_portable
#define JsonLoads_set_json_error_type JsonLoads_set_json_error_type_portable
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "jl_pow5.h"
#include "jl_ctx.h"

#if PY_VERSION_HEX >= 0x030D0000
/* 3.13 moved the declaration to an internal header; the symbol is still exported. */
PyAPI_FUNC(int) _PyDict_SetItem_KnownHash(PyObject *mp, PyObject *key, PyObject *item, Py_hash_t hash);
#endif

_Static_assert(offsetof(JlCtx, start) == 0, "jl_ctx.inc");
_Static_assert(offsetof(JlCtx, end) == 8, "jl_ctx.inc");
_Static_assert(offsetof(JlCtx, vs) == 16, "jl_ctx.inc");
_Static_assert(offsetof(JlCtx, vs_end) == 24, "jl_ctx.inc");
_Static_assert(offsetof(JlCtx, vsp) == 32, "jl_ctx.inc");
_Static_assert(offsetof(JlCtx, fs) == 40, "jl_ctx.inc");
_Static_assert(offsetof(JlCtx, fs_end) == 48, "jl_ctx.inc");
_Static_assert(offsetof(JlCtx, fsp) == 56, "jl_ctx.inc");
_Static_assert(offsetof(JlCtx, err) == 64, "jl_ctx.inc");
_Static_assert(offsetof(JlCtx, err_pos) == 72, "jl_ctx.inc");
_Static_assert(offsetof(JlCtx, pos) == 80, "jl_ctx.inc");
_Static_assert(offsetof(JlCtx, tmpd) == 88, "jl_ctx.inc");
_Static_assert(offsetof(JlCtx, kc) == 112, "jl_ctx.inc");
_Static_assert(sizeof(JlFrame) == 16, "jl_ctx.inc");

/* ------------------------------------------------------------------ sink (Python) */
void *jl_mk_i64(int64_t v) { return PyLong_FromLongLong(v); }
void *jl_mk_u64(uint64_t v) { return PyLong_FromUnsignedLongLong(v); }
void *jl_mk_f64(double d) { return PyFloat_FromDouble(d); }

void *jl_mk_lit(uint32_t which)
{
    switch (which) {
    case JL_LIT_NULL: return Py_NewRef(Py_None);
    case JL_LIT_TRUE: return Py_NewRef(Py_True);
    case JL_LIT_FALSE: return Py_NewRef(Py_False);
    case JL_LIT_EMPTY_DICT: return PyDict_New();
    default: return PyList_New(0);
    }
}

static void *jl_mk_ascii(const uint8_t *p, size_t n)
{
    PyObject *s;
    if (n == 1) {
        /* one-character strings are shared objects in CPython: no allocation */
        return PyUnicode_FromOrdinal(*p);
    }
    s = PyUnicode_New((Py_ssize_t)n, 127);
    if (s) {
        memcpy(PyUnicode_1BYTE_DATA(s), p, n);
    }
    return s;
}

/* UTF-8 with at least one byte >= 0x80. Validates (strict, no surrogates).
 * Pass 1 (AVX2): code points = bytes - continuation bytes; the widest lead
 * byte picks the kind (only C2/C3 leads -> latin-1, E0..EF -> UCS2, F0.. -> UCS4).
 * Pass 2: decode and validate; ASCII runs widen 16 bytes at a time.
 * Returns NULL without an exception for invalid UTF-8. */
JL_AVX2
static void *jl_mk_utf8(const uint8_t *p, size_t n)
{
    const uint8_t *s = p;
    const uint8_t *end = p + n;
    size_t cont = 0;
    uint8_t maxb = 0;
    Py_UCS4 maxc;
    PyObject *u;
    int kind;
    {
        size_t i = 0;
#ifndef JL_PORTABLE
        __m256i vmax = _mm256_setzero_si256();
        __m256i acc = _mm256_setzero_si256();
        int iters = 0;
        for (; i + 32 <= n; i += 32) {
            __m256i v = _mm256_loadu_si256((const __m256i *)(p + i));
            /* continuation: 0x80..0xBF  <=>  (int8)v < -64 */
            __m256i isc = _mm256_cmpgt_epi8(_mm256_set1_epi8(-64), v);
            acc = _mm256_sub_epi8(acc, isc);
            vmax = _mm256_max_epu8(vmax, v);
            if (++iters == 255) {
                __m256i sad = _mm256_sad_epu8(acc, _mm256_setzero_si256());
                cont += (size_t)_mm256_extract_epi64(sad, 0) + (size_t)_mm256_extract_epi64(sad, 1) +
                        (size_t)_mm256_extract_epi64(sad, 2) + (size_t)_mm256_extract_epi64(sad, 3);
                acc = _mm256_setzero_si256();
                iters = 0;
            }
        }
        {
            __m256i sad = _mm256_sad_epu8(acc, _mm256_setzero_si256());
            cont += (size_t)_mm256_extract_epi64(sad, 0) + (size_t)_mm256_extract_epi64(sad, 1) +
                    (size_t)_mm256_extract_epi64(sad, 2) + (size_t)_mm256_extract_epi64(sad, 3);
        }
        {
            __m128i m = _mm_max_epu8(_mm256_castsi256_si128(vmax), _mm256_extracti128_si256(vmax, 1));
            m = _mm_max_epu8(m, _mm_srli_si128(m, 8));
            m = _mm_max_epu8(m, _mm_srli_si128(m, 4));
            m = _mm_max_epu8(m, _mm_srli_si128(m, 2));
            m = _mm_max_epu8(m, _mm_srli_si128(m, 1));
            maxb = (uint8_t)_mm_cvtsi128_si32(m);
        }
#elif defined(JV16)
        {
            jv16 vmax = jv_set1(0);
            for (; i + 16 <= n; i += 16) {
                jv16 v = jv_load(p + i);
                cont += jv_count(jv_mask(jv_cont(v)));
                vmax = jv_max(vmax, v);
            }
            maxb = (uint8_t)jv_hmax(vmax);
        }
#endif
        for (; i < n; i++) {
            uint8_t c = p[i];
            cont += (c & 0xC0u) == 0x80u;
            if (c > maxb) {
                maxb = c;
            }
        }
    }
    if (maxb >= 0xF0u) {
        maxc = 0x10000u;
    } else if (maxb >= 0xC4u) {
        maxc = 0x100u;
    } else {
        maxc = 0xFFu;
    }
    u = PyUnicode_New((Py_ssize_t)(n - cont), maxc);
    if (!u) {
        return NULL;
    }
    kind = PyUnicode_KIND(u);
    if (kind == PyUnicode_1BYTE_KIND) {
        Py_UCS1 *d = PyUnicode_1BYTE_DATA(u);
        while (s < end) {
            uint8_t c = *s;
            if (c < 0x80u) {
                *d++ = c;
                s++;
                continue;
            }
            /* only C2/C3 leads possible here (maxb < C4) */
            if (c < 0xC2u || end - s < 2 || (s[1] & 0xC0u) != 0x80u) {
                goto bad;
            }
            *d++ = (Py_UCS1)(((c & 0x1Fu) << 6) | (s[1] & 0x3Fu));
            s += 2;
        }
        if (d != PyUnicode_1BYTE_DATA(u) + PyUnicode_GET_LENGTH(u)) {
            goto bad;
        }
    } else if (kind == PyUnicode_2BYTE_KIND) {
        Py_UCS2 *d = PyUnicode_2BYTE_DATA(u);
        Py_UCS2 *dend = d + PyUnicode_GET_LENGTH(u);
        while (s < end) {
            uint8_t c = *s;
            if (c < 0x80u) {
#ifndef JL_PORTABLE
                if (end - s >= 16 && dend - d >= 16) {
                    __m128i v = _mm_loadu_si128((const __m128i *)s);
                    uint32_t hm = (uint32_t)_mm_movemask_epi8(v);
                    if (!hm) {
                        _mm256_storeu_si256((__m256i *)d, _mm256_cvtepu8_epi16(v));
                        d += 16;
                        s += 16;
                        continue;
                    }
                    {
                        uint32_t k = (uint32_t)__builtin_ctz(hm);
                        _mm256_storeu_si256((__m256i *)d, _mm256_cvtepu8_epi16(v));
                        d += k;
                        s += k;
                        continue;
                    }
                }
#elif defined(JV16)
                if (end - s >= 16 && dend - d >= 16) {
                    jv16 v = jv_load(s);
                    jv_mask_t hm = jv_mask(jv_high(v));
                    unsigned k = hm ? jv_first(hm) : 16u;
                    jv_widen((uint16_t *)d, v);
                    d += k;
                    s += k;
                    continue;
                }
#endif
                *d++ = c;
                s++;
                continue;
            }
            if (c < 0xC2u) {
                goto bad;
            } else if (c < 0xE0u) {
                if (end - s < 2 || (s[1] & 0xC0u) != 0x80u) {
                    goto bad;
                }
                *d++ = (Py_UCS2)(((c & 0x1Fu) << 6) | (s[1] & 0x3Fu));
                s += 2;
            } else {
                Py_UCS4 cp;
                if (c >= 0xF0u || end - s < 3 || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u) {
                    goto bad;
                }
                cp = ((Py_UCS4)(c & 0x0Fu) << 12) | ((Py_UCS4)(s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
                if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
                    goto bad;
                }
                *d++ = (Py_UCS2)cp;
                s += 3;
            }
            if (d > dend) {
                goto bad;
            }
        }
        if (d != dend) {
            goto bad;
        }
    } else {
        Py_UCS4 *d = PyUnicode_4BYTE_DATA(u);
        Py_UCS4 *dend = d + PyUnicode_GET_LENGTH(u);
        while (s < end) {
            uint8_t c = *s;
            Py_UCS4 cp;
            if (d >= dend) {
                goto bad;
            }
            if (c < 0x80u) {
                *d++ = c;
                s++;
                continue;
            }
            if (c < 0xC2u) {
                goto bad;
            } else if (c < 0xE0u) {
                if (end - s < 2 || (s[1] & 0xC0u) != 0x80u) {
                    goto bad;
                }
                cp = ((Py_UCS4)(c & 0x1Fu) << 6) | (s[1] & 0x3Fu);
                s += 2;
            } else if (c < 0xF0u) {
                if (end - s < 3 || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u) {
                    goto bad;
                }
                cp = ((Py_UCS4)(c & 0x0Fu) << 12) | ((Py_UCS4)(s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
                if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
                    goto bad;
                }
                s += 3;
            } else if (c < 0xF5u) {
                if (end - s < 4 || (s[1] & 0xC0u) != 0x80u || (s[2] & 0xC0u) != 0x80u ||
                    (s[3] & 0xC0u) != 0x80u) {
                    goto bad;
                }
                cp = ((Py_UCS4)(c & 0x07u) << 18) | ((Py_UCS4)(s[1] & 0x3Fu) << 12) |
                     ((Py_UCS4)(s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
                if (cp < 0x10000u || cp > 0x10FFFFu) {
                    goto bad;
                }
                s += 4;
            } else {
                goto bad;
            }
            *d++ = cp;
        }
        if (d != dend) {
            goto bad;
        }
    }
    return u;
bad:
    Py_DECREF(u);
    return NULL;
}

/* ---- key cache: ASCII keys up to 32 bytes, compared as one 32-byte vector */
#define KC_BITS 11
#define KC_SIZE (1u << KC_BITS)
typedef struct {
    uint8_t text[32]; /* key bytes, zero padded */
    PyObject *obj;
    uint32_t len;
    uint32_t pad;
    uint64_t pad2[2];
} KcEnt; /* 64 bytes */
static KcEnt g_kc[KC_SIZE] __attribute__((aligned(64)));
_Static_assert(sizeof(KcEnt) == 64, "kernel indexes 64-byte key-cache entries");
_Static_assert(offsetof(KcEnt, obj) == 32 && offsetof(KcEnt, len) == 40, "jl_kernel.asm key cache");

/* The kernel pushes cache hits as borrowed pointers. An entry evicted while
 * a parse may still hold it is parked here and released when the outermost
 * loads() returns (a nested loads from a finalizer cannot free it early). */
static PyObject **g_grave = NULL;
static size_t g_grave_n = 0;
static size_t g_grave_cap = 0;
static int g_depth = 0;

static void jl_evict(PyObject *o)
{
    if (g_grave_n == g_grave_cap) {
        size_t ncap = g_grave_cap ? g_grave_cap * 2 : 64;
        PyObject **n = (PyObject **)realloc(g_grave, ncap * sizeof(PyObject *));
        if (!n) {
            /* cannot park it: keep the old entry alive forever rather than free it early */
            return;
        }
        g_grave = n;
        g_grave_cap = ncap;
    }
    g_grave[g_grave_n++] = o;
}

static void jl_grave_flush(void)
{
    while (g_grave_n) {
        PyObject *o = g_grave[--g_grave_n];
        Py_DECREF(o);
    }
}

static const uint8_t jl_ones32[64] = {
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};

static void *jl_mk_key_slow(const uint8_t *p, size_t n, int ascii)
{
    PyObject *k = ascii ? (PyObject *)jl_mk_ascii(p, n) : (PyObject *)jl_mk_utf8(p, n);
    if (k && PyObject_Hash(k) == -1) {
        Py_DECREF(k);
        return NULL;
    }
    return k;
}

#ifdef JL_PORTABLE
/* n <= 32; ASCII. Same cache as the vector version. `wide` = 32 bytes at p are readable. */
static inline void *jl_mk_key32p(const uint8_t *p, size_t n, int wide)
{
    uint8_t k[32];
    uint64_t w[4];
    uint64_t h;
    KcEnt *e;
#ifdef JV16
    if (wide) {
        /* key bytes, zero padded, without a variable-length copy */
        jv_store(k, jv_and(jv_load(p), jv_load(jl_ones32 + 32 - n)));
        jv_store(k + 16, jv_and(jv_load(p + 16), jv_load(jl_ones32 + 48 - n)));
    } else
#endif
    {
        (void)wide;
        memset(k, 0, 32);
        memcpy(k, p, n);
    }
    memcpy(w, k, 32);
    h = (w[0] * 0x9E3779B97F4A7C15ULL) ^ (w[1] * 0xC2B2AE3D27D4EB4FULL) ^ (n * 0x165667B19E3779F9ULL);
    if (n > 16) {
        h ^= (w[2] * 0x85EBCA77C2B2AE63ULL) ^ (w[3] * 0x27D4EB2F165667C5ULL);
    }
    h ^= h >> 29;
    e = &g_kc[(h * 0x9E3779B97F4A7C15ULL) >> (64 - KC_BITS)];
    if (e->obj && e->len == n && memcmp(w, e->text, 32) == 0) {
        Py_INCREF(e->obj);
        return e->obj;
    }
    {
        PyObject *o = (PyObject *)jl_mk_key_slow(p, n, 1);
        if (!o) {
            return NULL;
        }
        if (e->obj) {
            jl_evict(e->obj);
        }
        memcpy(e->text, k, 32);
        e->len = (uint32_t)n;
        e->obj = o;
        Py_INCREF(o);
        return o;
    }
}
#define JL_KEY_ASCII(p, n) \
    ((n) <= 32 ? jl_mk_key32p((p), (n), end - (p) >= 32 || (((uintptr_t)(p)) & 4095u) <= 4096u - 32u) \
               : jl_mk_key_slow((p), (n), 1))
#else
#define JL_KEY_ASCII(p, n) jl_mk_key_slow((p), (n), 1)
/* v = the 32 bytes at p (caller loaded them safely); n <= 32; ASCII. */
JL_AVX2
static inline void *jl_mk_key32(const uint8_t *p, size_t n, __m256i v)
{
    __m256i m = _mm256_loadu_si256((const __m256i *)(jl_ones32 + 32 - n));
    __m256i k = _mm256_and_si256(v, m);
    /* hash: fold the four lanes */
    uint64_t a = (uint64_t)_mm256_extract_epi64(k, 0);
    uint64_t b = (uint64_t)_mm256_extract_epi64(k, 1);
    uint64_t h = (a * 0x9E3779B97F4A7C15ULL) ^ (b * 0xC2B2AE3D27D4EB4FULL) ^ (n * 0x165667B19E3779F9ULL);
    KcEnt *e;
    if (n > 16) {
        uint64_t c = (uint64_t)_mm256_extract_epi64(k, 2);
        uint64_t d = (uint64_t)_mm256_extract_epi64(k, 3);
        h ^= (c * 0x85EBCA77C2B2AE63ULL) ^ (d * 0x27D4EB2F165667C5ULL);
    }
    h ^= h >> 29;
    e = &g_kc[(h * 0x9E3779B97F4A7C15ULL) >> (64 - KC_BITS)];
    if (e->obj && e->len == n &&
        (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(k, _mm256_loadu_si256((const __m256i *)e->text))) == 0xFFFFFFFFu) {
        Py_INCREF(e->obj);
        return e->obj;
    }
    {
        PyObject *o = (PyObject *)jl_mk_key_slow(p, n, 1);
        if (!o) {
            return NULL;
        }
        if (e->obj) {
            jl_evict(e->obj);
        }
        _mm256_storeu_si256((__m256i *)e->text, k);
        e->len = (uint32_t)n;
        e->obj = o;
        Py_INCREF(o);
        return o;
    }
}
#endif

void *jl_mk_list(void **items, size_t n)
{
    PyObject *l;
    l = PyList_New((Py_ssize_t)n);
    if (l && n) {
        memcpy(((PyListObject *)l)->ob_item, items, n * sizeof(void *));
    }
    return l;
}

/* kv = key0, val0, key1, val1 ...; steals all. */
void *jl_mk_dict(void **kv, size_t npairs)
{
    PyObject *d = npairs > 5 ? _PyDict_NewPresized((Py_ssize_t)npairs) : PyDict_New();
    size_t i;
    if (!d) {
        return NULL;
    }
    for (i = 0; i < npairs; i++) {
        uintptr_t kt = (uintptr_t)kv[2 * i];
        PyObject *k = (PyObject *)(kt & ~(uintptr_t)1);
        PyObject *v = (PyObject *)kv[2 * i + 1];
        Py_hash_t h = ((PyASCIIObject *)k)->hash;
        if (_PyDict_SetItem_KnownHash(d, k, v, h) < 0) {
            /* release what is left; caller frees nothing after a NULL */
            size_t j;
            for (j = i; j < npairs; j++) {
                if (!((uintptr_t)kv[2 * j] & 1u)) {
                    Py_DECREF((PyObject *)kv[2 * j]);
                }
                Py_DECREF((PyObject *)kv[2 * j + 1]);
            }
            Py_DECREF(d);
            return NULL;
        }
        if (!(kt & 1u)) {
            Py_DECREF(k);
        }
        Py_DECREF(v);
    }
    return d;
}

void jl_drop(void *o) { Py_DECREF((PyObject *)o); }

void *jl_mk_str(const uint8_t *p, size_t n, uint32_t flags)
{
    return (flags & 1u) ? jl_mk_utf8(p, n) : jl_mk_ascii(p, n);
}

JL_AVX2
void *jl_mk_key(const uint8_t *p, size_t n, uint32_t flags)
{
#ifdef JL_PORTABLE
    if (!(flags & 1u) && n <= 32) {
        return jl_mk_key32p(p, n, (flags & 2u) != 0);
    }
#else
    if (!(flags & 1u) && n <= 32 && (flags & 2u)) {
        return jl_mk_key32(p, n, _mm256_loadu_si256((const __m256i *)p));
    }
#endif
    return jl_mk_key_slow(p, n, !(flags & 1u));
}

/* exact decimal -> double for an ambiguous token (text is the token) */
int jl_strtod(const uint8_t *p, size_t n, double *out)
{
    char tmp[128];
    char *buf = tmp;
    char *endp = NULL;
    double d;
    if (n + 1 > sizeof(tmp)) {
        buf = (char *)PyMem_Malloc(n + 1);
        if (!buf) {
            return -1;
        }
    }
    memcpy(buf, p, n);
    buf[n] = 0;
    d = PyOS_string_to_double(buf, &endp, NULL);
    if (buf != tmp) {
        PyMem_Free(buf);
    }
    if (d == -1.0 && PyErr_Occurred()) {
        PyErr_Clear();
        return -1;
    }
    *out = d;
    return 0;
}

/* ------------------------------------------------------------------ kernel */

#define JL_LIKELY(x) __builtin_expect(!!(x), 1)
#define JL_UNLIKELY(x) __builtin_expect(!!(x), 0)

int jl_grow_vs(JlCtx *c)
{
    size_t used = (size_t)(c->vsp - c->vs);
    size_t cap = (size_t)(c->vs_end - c->vs);
    size_t ncap = cap * 2;
    void **n;
    if (c->vs == c->small_vs) {
        n = (void **)malloc(ncap * sizeof(void *));
        if (n) {
            memcpy(n, c->vs, used * sizeof(void *));
        }
    } else {
        n = (void **)realloc(c->vs, ncap * sizeof(void *));
    }
    if (!n) {
        PyErr_NoMemory();
        return -1;
    }
    c->vs = n;
    c->vs_end = n + ncap;
    c->vsp = n + used;
    return 0;
}

int jl_grow_fs(JlCtx *c)
{
    size_t used = (size_t)(c->fsp - c->fs);
    size_t cap = (size_t)(c->fs_end - c->fs);
    size_t ncap = cap * 2;
    JlFrame *n;
    if (c->fs == c->small_fs) {
        n = (JlFrame *)malloc(ncap * sizeof(JlFrame));
        if (n) {
            memcpy(n, c->fs, used * sizeof(JlFrame));
        }
    } else {
        n = (JlFrame *)realloc(c->fs, ncap * sizeof(JlFrame));
    }
    if (!n) {
        PyErr_NoMemory();
        return -1;
    }
    c->fs = n;
    c->fs_end = n + ncap;
    c->fsp = n + used;
    return 0;
}

static inline int jl_is_ws(uint8_t ch)
{
    return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
}

/* Skip whitespace. The buffer ends with a NUL sentinel (not whitespace). */
static inline const uint8_t *jl_skip(const uint8_t *p)
{
    if (JL_LIKELY(*p > ' ')) {
        return p;
    }
    while (jl_is_ws(*p)) {
        p++;
    }
    return p;
}

static const uint8_t jl_esc_tab[256] = {
    ['"'] = '"', ['\\'] = '\\', ['/'] = '/', ['b'] = '\b', ['f'] = '\f',
    ['n'] = '\n', ['r'] = '\r', ['t'] = '\t'};

static inline int jl_hex4(const uint8_t *p, uint32_t *out)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        uint8_t ch = p[i];
        uint32_t d;
        if (ch >= '0' && ch <= '9') {
            d = ch - '0';
        } else if ((ch | 0x20u) >= 'a' && (ch | 0x20u) <= 'f') {
            d = (ch | 0x20u) - 'a' + 10;
        } else {
            return -1;
        }
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

static int jl_sbuf_need(JlCtx *c, size_t need)
{
    size_t ncap;
    uint8_t *n;
    if (need <= c->scap) {
        return 0;
    }
    ncap = c->scap ? c->scap : 256;
    while (ncap < need) {
        ncap *= 2;
    }
    n = (uint8_t *)realloc(c->sbuf, ncap);
    if (!n) {
        return -1;
    }
    c->sbuf = n;
    c->scap = ncap;
    return 0;
}

/* Scan string body from p (after the opening quote). Clean span: returns a
 * pointer to the closing quote, *hi = any byte >= 0x80. Stops early at '\\'
 * or a control byte and returns that position. */
#ifdef JL_PORTABLE
static inline const uint8_t *jl_scan(const uint8_t *p, const uint8_t *end, uint32_t *hi)
{
    const uint64_t ones = 0x0101010101010101ULL;
    const uint64_t high = 0x8080808080808080ULL;
    uint32_t h = 0;
#ifdef JV16
    /* most strings end within eight bytes: look at one word before any vector */
    if (end - p >= 8) {
        uint64_t v;
        uint64_t q;
        uint64_t b;
        uint64_t sp;
        memcpy(&v, p, 8);
        q = v ^ (ones * '"');
        b = v ^ (ones * '\\');
        sp = (((v - ones * 0x20u) & ~v) | ((q - ones) & ~q) | ((b - ones) & ~b)) & high;
        if (sp) {
            /* the lowest set bit is exact; bits above it may be borrows */
            unsigned off = (unsigned)__builtin_ctzll(sp) >> 3;
            *hi = off ? ((v & high) << (64 - 8 * off)) != 0 : 0;
            return p + off;
        }
        h = (v & high) != 0;
        p += 8;
    }
    while (end - p >= 16) {
        jv16 v = jv_load(p);
        jv16 sv = jv_special(v);
        jv16 hv = jv_high(v);
        if (jv_mask(jv_or(sv, hv))) {
            /* something in this block: a special byte, a non-ASCII byte or both */
            jv_mask_t m = jv_mask(sv);
            jv_mask_t hm = jv_mask(hv);
            if (m) {
                unsigned off = jv_first(m);
                h |= (hm & jv_low(off)) != 0;
                *hi = h;
                return p + off;
            }
            h = 1;
        }
        p += 16;
    }
#endif
    /* eight bytes at a time while none is '"', '\\' or below 0x20 */
    while (end - p >= 8) {
        uint64_t v;
        uint64_t q;
        uint64_t b;
        memcpy(&v, p, 8);
        q = v ^ (ones * '"');
        b = v ^ (ones * '\\');
        if ((((v - ones * 0x20u) & ~v) | ((q - ones) & ~q) | ((b - ones) & ~b)) & high) {
            break;
        }
        h |= (v & high) != 0;
        p += 8;
    }
    while (p < end) {
        uint8_t ch = *p;
        if (ch == '"' || ch == '\\' || ch < 0x20u) {
            break;
        }
        h |= ch >> 7;
        p++;
    }
    *hi = h;
    return p;
}
#else
JL_AVX2_BMI
static inline const uint8_t *jl_scan(const uint8_t *p, const uint8_t *end, uint32_t *hi)
{
    const __m256i q = _mm256_set1_epi8('"');
    const __m256i bs = _mm256_set1_epi8('\\');
    const __m256i c1f = _mm256_set1_epi8(0x1f);
    uint32_t h = 0;
    for (;;) {
        __m256i v;
        uint32_t m;
        uint32_t hm;
        if (JL_LIKELY(end - p >= 32)) {
            v = _mm256_loadu_si256((const __m256i *)p);
        } else if ((((uintptr_t)p) & 4095u) <= 4096u - 32u) {
            v = _mm256_loadu_si256((const __m256i *)p);
        } else {
            /* scalar tail near a page end */
            while (p < end) {
                uint8_t ch = *p;
                if (ch == '"' || ch == '\\' || ch < 0x20u) {
                    *hi = h;
                    return p;
                }
                h |= ch >> 7;
                p++;
            }
            *hi = h;
            return p;
        }
        m = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(v, q), _mm256_cmpeq_epi8(v, bs)),
            _mm256_cmpeq_epi8(_mm256_max_epu8(v, c1f), c1f)));
        hm = (uint32_t)_mm256_movemask_epi8(v);
        if (m) {
            uint32_t off = _tzcnt_u32(m);
            h |= hm & (uint32_t)((1ull << off) - 1u);
            *hi = h;
            return p + off;
        }
        h |= hm;
        p += 32;
        if (p >= end) {
            *hi = h;
            return end;
        }
    }
}
#endif

/* Decode a string with escapes into c->sbuf. p points after the opening
 * quote; seg is the first special byte. Returns the position after the
 * closing quote, or NULL on error (c->err/err_pos set). *outn = length,
 * *hi = any byte >= 0x80 in the output. */
static const uint8_t *jl_unescape(JlCtx *c, const uint8_t *p, const uint8_t *seg, size_t *outn, uint32_t *hi)
{
    size_t n = 0;
    uint32_t h = *hi;
    const uint8_t *end = c->end;
    for (;;) {
        size_t run = (size_t)(seg - p);
        uint8_t ch;
        if (jl_sbuf_need(c, n + run + 8) < 0) {
            c->err = JL_E_MEMORY;
            c->err_pos = (size_t)(seg - c->start);
            return NULL;
        }
        memcpy(c->sbuf + n, p, run);
        n += run;
        p = seg;
        if (p >= end) {
            c->err = JL_E_INVALID;
            c->err_pos = (size_t)(p - c->start);
            return NULL;
        }
        ch = *p;
        if (ch == '"') {
            *outn = n;
            *hi = h;
            return p + 1;
        }
        if (ch < 0x20u) {
            c->err = JL_E_INVALID;
            c->err_pos = (size_t)(p - c->start);
            return NULL;
        }
        /* backslash */
        if (end - p < 2) {
            c->err = JL_E_INVALID;
            c->err_pos = (size_t)(p + 1 - c->start);
            return NULL;
        }
        ch = p[1];
        if (ch != 'u') {
            uint8_t e = jl_esc_tab[ch];
            if (!e) {
                c->err = JL_E_INVALID;
                c->err_pos = (size_t)(p + 1 - c->start);
                return NULL;
            }
            c->sbuf[n++] = e;
            p += 2;
        } else {
            uint32_t cp;
            if (end - p < 6 || jl_hex4(p + 2, &cp) < 0) {
                c->err = JL_E_INVALID;
                c->err_pos = (size_t)(p + 2 - c->start);
                return NULL;
            }
            p += 6;
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
                uint32_t lo;
                if (end - p < 6 || p[0] != '\\' || p[1] != 'u' || jl_hex4(p + 2, &lo) < 0 ||
                    lo < 0xDC00u || lo > 0xDFFFu) {
                    c->err = JL_E_INVALID;
                    c->err_pos = (size_t)(p - c->start);
                    return NULL;
                }
                p += 6;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                c->err = JL_E_INVALID;
                c->err_pos = (size_t)(p - c->start);
                return NULL;
            }
            if (cp < 0x80u) {
                c->sbuf[n++] = (uint8_t)cp;
            } else if (cp < 0x800u) {
                c->sbuf[n++] = (uint8_t)(0xC0u | (cp >> 6));
                c->sbuf[n++] = (uint8_t)(0x80u | (cp & 0x3Fu));
                h = 1;
            } else if (cp < 0x10000u) {
                c->sbuf[n++] = (uint8_t)(0xE0u | (cp >> 12));
                c->sbuf[n++] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
                c->sbuf[n++] = (uint8_t)(0x80u | (cp & 0x3Fu));
                h = 1;
            } else {
                c->sbuf[n++] = (uint8_t)(0xF0u | (cp >> 18));
                c->sbuf[n++] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
                c->sbuf[n++] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
                c->sbuf[n++] = (uint8_t)(0x80u | (cp & 0x3Fu));
                h = 1;
            }
        }
        {
            uint32_t h2 = 0;
            seg = jl_scan(p, end, &h2);
            h |= h2;
        }
    }
}

void *jl_str_escaped(JlCtx *c, const uint8_t *s, const uint8_t *seg, uint32_t flags)
{
    size_t n = 0;
    uint32_t hi = flags & 1u;
    int is_key = (flags & 2u) != 0;
    const uint8_t *after = jl_unescape(c, s, seg, &n, &hi);
    void *o;
    if (!after) {
        return NULL;
    }
    if (!hi) {
        o = is_key ? jl_mk_key_slow(c->sbuf, n, 1) : jl_mk_ascii(c->sbuf, n);
    } else {
        o = is_key ? jl_mk_key_slow(c->sbuf, n, 0) : jl_mk_utf8(c->sbuf, n);
    }
    if (!o) {
        c->err = PyErr_Occurred() ? JL_E_MEMORY : JL_E_INVALID;
        c->err_pos = (size_t)(s - c->start);
        return NULL;
    }
    c->pos = after;
    return o;
}

/* String value or key at p (p at the opening quote). On success *out is the
 * object and the return value is the position after the closing quote. */
JL_AVX2_BMI
static inline const uint8_t *jl_string(JlCtx *c, const uint8_t *p, void **out, int is_key)
{
    const uint8_t *s = p + 1;
    const uint8_t *end = c->end;
    uint32_t hi = 0;
    const uint8_t *q;
    void *o;
#ifndef JL_PORTABLE
    /* short string: one vector, quote inside, nothing special before it */
    if (JL_LIKELY(end - s >= 32 || (((uintptr_t)s) & 4095u) <= 4096u - 32u)) {
        __m256i v = _mm256_loadu_si256((const __m256i *)s);
        uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(v, _mm256_set1_epi8('"')), _mm256_cmpeq_epi8(v, _mm256_set1_epi8('\\'))),
            _mm256_cmpeq_epi8(_mm256_max_epu8(v, _mm256_set1_epi8(0x1f)), _mm256_set1_epi8(0x1f))));
        if (m) {
            uint32_t off = _tzcnt_u32(m);
            if (JL_LIKELY(s + off < end && s[off] == '"')) {
                uint32_t hm = (uint32_t)_mm256_movemask_epi8(v) & (uint32_t)((1ull << off) - 1u);
                if (!hm) {
                    o = is_key ? jl_mk_key32(s, off, v) : jl_mk_ascii(s, off);
                } else {
                    o = is_key ? jl_mk_key_slow(s, off, 0) : jl_mk_utf8(s, off);
                    if (!o && !PyErr_Occurred()) {
                        c->err = JL_E_INVALID;
                        c->err_pos = (size_t)(s - c->start);
                        return NULL;
                    }
                }
                if (!o) {
                    c->err = JL_E_MEMORY;
                    c->err_pos = (size_t)(s - c->start);
                    return NULL;
                }
                *out = o;
                return s + off + 1;
            }
        }
    }
#endif
    q = jl_scan(s, end, &hi);
    if (JL_LIKELY(q < end && *q == '"')) {
        size_t n = (size_t)(q - s);
        if (!hi) {
            o = is_key ? JL_KEY_ASCII(s, n) : jl_mk_ascii(s, n);
        } else {
            o = is_key ? jl_mk_key_slow(s, n, 0) : jl_mk_utf8(s, n);
            if (!o && !PyErr_Occurred()) {
                c->err = JL_E_INVALID;
                c->err_pos = (size_t)(s - c->start);
                return NULL;
            }
        }
        if (!o) {
            c->err = JL_E_MEMORY;
            c->err_pos = (size_t)(s - c->start);
            return NULL;
        }
        *out = o;
        return q + 1;
    }
    if (q >= end) {
        /* unterminated: same position as the NASM kernel reports */
        c->err = JL_E_INVALID;
        c->err_pos = (size_t)(s - c->start);
        return NULL;
    }
    {
        size_t n = 0;
        const uint8_t *after = jl_unescape(c, s, q, &n, &hi);
        if (!after) {
            return NULL;
        }
        if (!hi) {
            o = is_key ? jl_mk_key_slow(c->sbuf, n, 1) : jl_mk_ascii(c->sbuf, n);
        } else {
            o = is_key ? jl_mk_key_slow(c->sbuf, n, 0) : jl_mk_utf8(c->sbuf, n);
            if (!o && !PyErr_Occurred()) {
                c->err = JL_E_INVALID;
                c->err_pos = (size_t)(s - c->start);
                return NULL;
            }
        }
        if (!o) {
            c->err = JL_E_MEMORY;
            c->err_pos = (size_t)(s - c->start);
            return NULL;
        }
        *out = o;
        return after;
    }
}

/* ---- numbers */

static inline int jl_is_digit(uint8_t ch) { return (uint8_t)(ch - '0') < 10u; }

static inline uint32_t jl_parse8x(uint64_t v)
{
    /* eight digit values 0..9, little-endian -> value */
    v = (v * 10) + (v >> 8);
    v = (((v & 0x000000FF000000FFULL) * (100 + (1000000ULL << 32))) +
         (((v >> 16) & 0x000000FF000000FFULL) * (1 + (10000ULL << 32)))) >> 32;
    return (uint32_t)v;
}

static const uint64_t jl_p10w[20] = {1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL,
                                     10000000ULL, 100000000ULL, 1000000000ULL, 10000000000ULL,
                                     100000000000ULL, 1000000000000ULL, 10000000000000ULL,
                                     100000000000000ULL, 1000000000000000ULL, 10000000000000000ULL,
                                     100000000000000000ULL, 1000000000000000000ULL,
                                     10000000000000000000ULL};

/* eight bytes at p, digits -> 0..9, everything else >= 10 */
static inline uint64_t jl_load8x(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v ^ 0x3030303030303030ULL;
}

/* high bit of every byte of x (from jl_load8x) that is not a digit */
static inline uint64_t jl_nondigit8(uint64_t x)
{
    /* exact per byte: (x & 0x7F) + 0x76 does not carry into the next byte */
    return (((x & 0x7F7F7F7F7F7F7F7FULL) + 0x7676767676767676ULL) | x) & 0x8080808080808080ULL;
}

/* Digit run at p, eight bytes at a time: w = w * 10^len + run (mod 2^64).
 * Returns the end of the run, or NULL when a load would pass end. */
static inline const uint8_t *jl_digits(const uint8_t *p, const uint8_t *end, uint64_t *w)
{
    uint64_t acc = *w;
    for (;;) {
        uint64_t x;
        uint64_t nd;
        if (JL_UNLIKELY(end - p < 8)) {
            return NULL;
        }
        x = jl_load8x(p);
        nd = jl_nondigit8(x);
        if (nd == 0) {
            acc = acc * 100000000u + jl_parse8x(x);
            p += 8;
            continue;
        }
        {
            unsigned k = (unsigned)__builtin_ctzll(nd) >> 3; /* digits before the first non-digit */
            if (k) {
                /* the k digits to the top bytes, zero digits below them */
                acc = acc * jl_p10w[k] + jl_parse8x(x << (64 - 8 * k));
                p += k;
            }
        }
        *w = acc;
        return p;
    }
}

static const double jl_p10[23] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                  1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

/* Eisel-Lemire: w != 0 exact 64-bit significand, value = w * 10^q.
 * Returns 1 and the double, or 0 when it cannot decide (caller falls back). */
static inline int jl_eisel_lemire(uint64_t w, int64_t q, int neg, double *out)
{
    unsigned __int128 prod;
    uint64_t hi;
    uint64_t lo;
    int lz;
    uint64_t upper;
    uint64_t mant;
    int64_t p2;
    uint64_t bits;
    if (q < JL_POW5_QMIN) {
        *out = neg ? -0.0 : 0.0;
        return 1;
    }
    if (q > JL_POW5_QMAX) {
        return -1; /* overflow */
    }
    lz = __builtin_clzll(w);
    w <<= lz;
    prod = (unsigned __int128)w * jl_pow5[q - JL_POW5_QMIN][0];
    hi = (uint64_t)(prod >> 64);
    lo = (uint64_t)prod;
    if ((hi & 0x1FFu) == 0x1FFu) {
        unsigned __int128 p2x = (unsigned __int128)w * jl_pow5[q - JL_POW5_QMIN][1];
        uint64_t add = (uint64_t)(p2x >> 64);
        lo += add;
        if (lo < add) {
            hi++;
        }
    }
    upper = hi >> 63;
    mant = hi >> (upper + 9);
    p2 = ((((152170 + 65536) * q) >> 16) + 63) + (int64_t)upper - lz + 1023;
    if (p2 <= 0) {
        if (-p2 + 1 >= 64) {
            *out = neg ? -0.0 : 0.0;
            return 1;
        }
        mant >>= -p2 + 1;
        mant += mant & 1u;
        mant >>= 1;
        p2 = (mant < (1ULL << 52)) ? 0 : 1;
        bits = ((uint64_t)p2 << 52) | (mant & ((1ULL << 52) - 1));
        bits |= (uint64_t)neg << 63;
        memcpy(out, &bits, 8);
        return 1;
    }
    if (lo <= 1 && q >= -4 && q <= 23 && (mant & 3u) == 1u) {
        if ((mant << (upper + 9)) == hi) {
            mant &= ~1ULL;
        }
    }
    mant += mant & 1u;
    mant >>= 1;
    if (mant >= (2ULL << 52)) {
        mant = 1ULL << 52;
        p2++;
    }
    if (p2 >= 0x7FF) {
        return -1;
    }
    bits = ((uint64_t)p2 << 52) | (mant & ((1ULL << 52) - 1));
    bits |= (uint64_t)neg << 63;
    memcpy(out, &bits, 8);
    return 1;
}

/* Float from the exact significand w (or a truncated one, trunc) and exp10;
 * start..p is the token for the jl_strtod fallback. */
static inline __attribute__((always_inline)) const uint8_t *jl_float(JlCtx *c, const uint8_t *start, const uint8_t *p, uint64_t w,
                                      int64_t exp10, int neg, int trunc, void **out)
{
    double d;
    int r;
    if (w == 0) {
        d = neg ? -0.0 : 0.0;
        *out = jl_mk_f64(d);
        return *out ? p : NULL;
    }
    if (!trunc && w <= (1ULL << 53) && exp10 >= -22 && exp10 <= 22) {
        d = (double)w;
        if (exp10 < 0) {
            d /= jl_p10[-exp10];
        } else {
            d *= jl_p10[exp10];
        }
        *out = jl_mk_f64(neg ? -d : d);
        return *out ? p : NULL;
    }
    r = jl_eisel_lemire(w, exp10, neg, &d);
    if (r == 1 && trunc) {
        /* w+1 must round the same way */
        double d2;
        if (jl_eisel_lemire(w + 1, exp10, neg, &d2) != 1 || d2 != d) {
            r = 0;
        }
    }
    if (r == 0) {
        if (jl_strtod(start, (size_t)(p - start), &d) < 0) {
            c->err = JL_E_NUMBER;
            c->err_pos = (size_t)(p - c->start);
            return NULL;
        }
        r = 1;
    }
    if (r < 0 || d - d != 0) {
        c->err = JL_E_NUMBER;
        c->err_pos = (size_t)(p - c->start);
        return NULL;
    }
    *out = jl_mk_f64(d);
    return *out ? p : NULL;
}

/* Number token at p, any form: the long-number and error path of jl_number. */
static __attribute__((noinline)) const uint8_t *jl_number_slow(JlCtx *c, const uint8_t *p, void **out)
{
    const uint8_t *start = p;
    int neg = 0;
    uint64_t w = 0;
    int64_t exp10 = 0;
    int ndig = 0; /* significant digits taken into w */
    int trunc = 0;
    int is_float = 0;
    if (*p == '-') {
        neg = 1;
        p++;
    }
    if (*p == '0') {
        p++;
        if (jl_is_digit(*p)) {
            c->err = JL_E_NUMBER;
            c->err_pos = (size_t)(p - c->start);
            return NULL;
        }
    } else if (jl_is_digit(*p)) {
        while (c->end - p >= 8 && !jl_nondigit8(jl_load8x(p))) {
            if (ndig + 8 <= 19) {
                w = w * 100000000u + jl_parse8x(jl_load8x(p));
                ndig += 8;
                p += 8;
            } else {
                break;
            }
        }
        while (jl_is_digit(*p)) {
            if (ndig < 19) {
                w = w * 10 + (uint64_t)(*p - '0');
                ndig++;
            } else {
                exp10++;
                trunc |= *p != '0';
            }
            p++;
        }
    } else {
        c->err = JL_E_NUMBER;
        c->err_pos = (size_t)(p - c->start);
        return NULL;
    }
    if (*p == '.') {
        p++;
        is_float = 1;
        if (!jl_is_digit(*p)) {
            c->err = JL_E_NUMBER;
            c->err_pos = (size_t)(p - c->start);
            return NULL;
        }
        if (ndig == 0) {
            /* leading zeros of 0.000ddd do not count */
            while (*p == '0') {
                exp10--;
                p++;
            }
        }
        while (c->end - p >= 8 && ndig + 8 <= 19 && !jl_nondigit8(jl_load8x(p))) {
            w = w * 100000000u + jl_parse8x(jl_load8x(p));
            ndig += 8;
            exp10 -= 8;
            p += 8;
        }
        while (jl_is_digit(*p)) {
            if (ndig < 19) {
                w = w * 10 + (uint64_t)(*p - '0');
                ndig++;
                exp10--;
            } else {
                trunc |= *p != '0';
            }
            p++;
        }
    }
    if ((*p | 0x20u) == 'e') {
        int eneg = 0;
        int64_t e = 0;
        p++;
        is_float = 1;
        if (*p == '+' || *p == '-') {
            eneg = *p == '-';
            p++;
        }
        if (!jl_is_digit(*p)) {
            c->err = JL_E_NUMBER;
            c->err_pos = (size_t)(p - c->start);
            return NULL;
        }
        while (jl_is_digit(*p)) {
            if (e < 100000) {
                e = e * 10 + (*p - '0');
            }
            p++;
        }
        exp10 += eneg ? -e : e;
    }
    if (!is_float) {
        if (JL_LIKELY(!trunc && exp10 == 0 && ndig <= 18)) {
            int64_t v = neg ? -(int64_t)w : (int64_t)w;
            *out = jl_mk_i64(v);
            return *out ? p : NULL;
        }
        if (!trunc && exp10 == 0 && ndig == 19) {
            if (!neg) {
                *out = jl_mk_u64(w);
                return *out ? p : NULL;
            }
            if (w <= (uint64_t)INT64_MAX + 1u) {
                *out = jl_mk_i64((int64_t)(0 - w));
                return *out ? p : NULL;
            }
        }
        if (!neg && exp10 == 1 && ndig == 19) {
            /* 20 digits: p[-1] is the one digit that did not go into w */
            uint64_t d = (uint64_t)(p[-1] - '0');
            if (w <= (UINT64_MAX - d) / 10) {
                *out = jl_mk_u64(w * 10 + d);
                return *out ? p : NULL;
            }
        }
        /* below INT64_MIN or above UINT64_MAX: float (same as orjson) */
    }
    return jl_float(c, start, p, w, exp10, neg, trunc, out);
}

/* Number token at p. Fast path for up to 19 significant digits: an integer part
 * of 0, 1 or 2 digits takes a predicted branch, a longer one and the fraction
 * end at a bit scan of an eight- or sixteen-byte non-digit mask, so the position
 * of the next token does not wait on a digit-by-digit loop (whose exit
 * mispredicts on varying lengths); the digit values are computed off that chain.
 * Everything else (long numbers, errors, the last bytes of the input) goes to
 * jl_number_slow, which gives the same results. */
static inline const uint8_t *jl_number(JlCtx *c, const uint8_t *p, void **out)
{
    const uint8_t *start = p;
    const uint8_t *end = c->end;
    int neg = *p == '-';
    uint64_t w;
    int64_t exp10 = 0;
    int64_t ndig;
    int is_float = 0;
    uint64_t x0;
    uint64_t m0;
    p += neg;
    if (*p == '0') {
        /* 0 and 0.ddd: the next position does not wait on a load */
        p++;
        if (JL_UNLIKELY(jl_is_digit(*p))) {
            goto slow; /* leading zero */
        }
        w = 0;
        ndig = 1;
        goto int_done;
    }
    if (JL_UNLIKELY(end - p < 8)) {
        goto slow;
    }
    x0 = jl_load8x(p);
    m0 = jl_nondigit8(x0);
    if (JL_UNLIKELY(m0 & 0x80u)) {
        goto slow; /* no digit */
    }
    /* one or two digits (coordinates, small ints): a predicted branch, so the
     * next position does not wait on the bit scan */
    if (m0 & 0x8000u) {
        w = x0 & 0xFFu;
        ndig = 1;
        p += 1;
        goto int_done;
    }
    if (m0 & 0x800000u) {
        w = (x0 & 0xFFu) * 10 + ((x0 >> 8) & 0xFFu);
        ndig = 2;
        p += 2;
        goto int_done;
    }
    if (JL_LIKELY(m0 != 0)) {
        unsigned k = (unsigned)__builtin_ctzll(m0) >> 3; /* 3..7 */
        w = jl_parse8x(x0 << (64 - 8 * k));
        ndig = k;
        p += k;
    } else {
        const uint8_t *q;
        w = jl_parse8x(x0);
        q = jl_digits(p + 8, end, &w);
        if (JL_UNLIKELY(!q)) {
            goto slow;
        }
        ndig = q - p;
        p = q;
    }
int_done:
    if (*p == '.') {
        const uint8_t *f = p + 1;
        uint64_t xa;
        uint64_t xb;
        uint64_t ma;
        uint64_t mb;
        uint64_t fv;
        int64_t fl;
        if (JL_UNLIKELY(end - f < 16)) {
            goto slow;
        }
        xa = jl_load8x(f);
        xb = jl_load8x(f + 8);
        ma = jl_nondigit8(xa);
        mb = jl_nondigit8(xb);
        if (ma) {
            unsigned k = (unsigned)__builtin_ctzll(ma) >> 3;
            if (JL_UNLIKELY(k == 0)) {
                goto slow; /* no digit after '.' */
            }
            fl = k;
            fv = jl_parse8x(xa << (64 - 8 * k));
        } else if (mb) {
            unsigned k = (unsigned)__builtin_ctzll(mb) >> 3;
            fl = 8 + k;
            /* (x << (63 - 8k)) << 1 is x << (64 - 8k), and 0 for k == 0 */
            fv = (uint64_t)jl_parse8x(xa) * jl_p10w[k] + jl_parse8x((xb << (63 - 8 * k)) << 1);
        } else {
            const uint8_t *q;
            fv = (uint64_t)jl_parse8x(xa) * 100000000u + jl_parse8x(xb);
            q = jl_digits(f + 16, end, &fv);
            if (JL_UNLIKELY(!q)) {
                goto slow;
            }
            fl = q - f;
        }
        ndig += fl;
        if (JL_UNLIKELY(ndig > 19)) {
            goto slow;
        }
        w = w * jl_p10w[fl] + fv;
        exp10 = -fl;
        is_float = 1;
        p = f + fl;
    }
    if (JL_UNLIKELY((*p | 0x20u) == 'e')) {
        int eneg = 0;
        int64_t e = 0;
        p++;
        is_float = 1;
        if (*p == '+' || *p == '-') {
            eneg = *p == '-';
            p++;
        }
        if (!jl_is_digit(*p)) {
            goto slow;
        }
        while (jl_is_digit(*p)) {
            if (e < 100000) {
                e = e * 10 + (*p - '0');
            }
            p++;
        }
        exp10 += eneg ? -e : e;
    }
    if (!is_float) {
        if (JL_UNLIKELY(ndig > 18)) {
            goto slow;
        }
        *out = jl_mk_i64(neg ? -(int64_t)w : (int64_t)w);
        return *out ? p : NULL;
    }
    if (JL_UNLIKELY(ndig > 19)) {
        goto slow;
    }
    return jl_float(c, start, p, w, exp10, neg, 0, out);
slow:
    return jl_number_slow(c, start, out);
}

#ifdef JL_KERNEL_C
int jl_run(JlCtx *c)
{
    const uint8_t *p = c->start;
    const uint8_t *end = c->end;
    void *o;
    p = jl_skip(p);
    if (p >= end) {
        c->err = JL_E_EMPTY;
        c->err_pos = (size_t)(p - c->start);
        return 0;
    }

value:
    p = jl_skip(p);
    switch (*p) {
    case '"':
        p = jl_string(c, p, &o, 0);
        if (!p) {
            return 0;
        }
        goto have;
    case '{':
        p = jl_skip(p + 1);
        if (*p == '}') {
            p++;
            o = PyDict_New();
            if (!o) {
                goto oom;
            }
            goto have;
        }
        if (JL_UNLIKELY(c->fsp == c->fs_end) && jl_grow_fs(c) < 0) {
            goto oom;
        }
        c->fsp->kind = 1;
        c->fsp->vstart = (size_t)((char *)c->vsp - (char *)c->vs);
        c->fsp++;
        goto key;
    case '[':
        p = jl_skip(p + 1);
        if (*p == ']') {
            p++;
            o = PyList_New(0);
            if (!o) {
                goto oom;
            }
            goto have;
        }
        if (JL_UNLIKELY(c->fsp == c->fs_end) && jl_grow_fs(c) < 0) {
            goto oom;
        }
        c->fsp->kind = 0;
        c->fsp->vstart = (size_t)((char *)c->vsp - (char *)c->vs);
        c->fsp++;
        goto value;
    case '-':
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        p = jl_number(c, p, &o);
        if (!p) {
            if (!c->err) {
                goto oom;
            }
            return 0;
        }
        goto have;
    case 't':
        if (end - p >= 4 && memcmp(p, "true", 4) == 0) {
            p += 4;
            o = jl_mk_lit(JL_LIT_TRUE);
            goto have;
        }
        goto unexpected;
    case 'f':
        if (end - p >= 5 && memcmp(p, "false", 5) == 0) {
            p += 5;
            o = jl_mk_lit(JL_LIT_FALSE);
            goto have;
        }
        goto unexpected;
    case 'n':
        if (end - p >= 4 && memcmp(p, "null", 4) == 0) {
            p += 4;
            o = jl_mk_lit(JL_LIT_NULL);
            goto have;
        }
        goto unexpected;
    default:
        goto invalid;
    }

have:
    if (JL_UNLIKELY(!o)) {
        goto oom;
    }
    if (JL_UNLIKELY(c->vsp == c->vs_end) && jl_grow_vs(c) < 0) {
        jl_drop(o);
        goto oom;
    }
    *c->vsp++ = o;
    if (JL_UNLIKELY(c->fsp == c->fs)) {
        goto done;
    }
    p = jl_skip(p);
    {
        JlFrame *fr = c->fsp - 1;
        void **first = (void **)((char *)c->vs + fr->vstart);
        if (fr->kind == 0) {
            if (*p == ',') {
                p++;
                goto value;
            }
            if (*p == ']') {
                size_t n = (size_t)(c->vsp - first);
                p++;
                c->vsp = first;
                o = jl_mk_list(first, n);
                if (!o) {
                    goto oom;
                }
                c->fsp--;
                goto have;
            }
            goto invalid;
        }
        if (*p == ',') {
            p++;
            goto key;
        }
        if (*p == '}') {
            size_t n = (size_t)(c->vsp - first);
            p++;
            c->vsp = first; /* stolen even on failure */
            o = jl_mk_dict(first, n / 2);
            if (!o) {
                goto oom;
            }
            c->fsp--;
            goto have;
        }
        goto invalid;
    }

key:
    p = jl_skip(p);
    if (JL_UNLIKELY(*p != '"')) {
        goto invalid;
    }
    p = jl_string(c, p, &o, 1);
    if (!p) {
        return 0;
    }
    if (JL_UNLIKELY(c->vsp == c->vs_end) && jl_grow_vs(c) < 0) {
        jl_drop(o);
        goto oom;
    }
    *c->vsp++ = o;
    p = jl_skip(p);
    if (JL_UNLIKELY(*p != ':')) {
        goto invalid;
    }
    p++;
    goto value;

done:
    p = jl_skip(p);
    if (p != end) {
        c->err = JL_E_UNEXPECTED;
        c->err_pos = (size_t)(p - c->start);
        return 0;
    }
    return 1;

unexpected:
    c->err = JL_E_UNEXPECTED;
    c->err_pos = (size_t)(p - c->start);
    return 0;
invalid:
    c->err = JL_E_INVALID;
    c->err_pos = (size_t)(p - c->start);
    return 0;
oom:
    c->err = JL_E_MEMORY;
    c->err_pos = (size_t)(p - c->start);
    return 0;
}

#endif /* JL_KERNEL_C */

/* ------------------------------------------------------------------ entry */

/* oxjson.JSONDecodeError (json.JSONDecodeError + JSONError), set at module init */
static PyObject *g_loads_error = NULL;
static PyObject *g_json_error = NULL;

void JsonLoads_set_error_type(PyObject *exc_type)
{
    Py_XINCREF(exc_type);
    Py_XDECREF(g_loads_error);
    g_loads_error = exc_type;
}

void JsonLoads_set_json_error_type(PyObject *exc_type)
{
    Py_XINCREF(exc_type);
    Py_XDECREF(g_json_error);
    g_json_error = exc_type;
}

/* the document as str for the exception (json.JSONDecodeError takes doc, pos) */
static PyObject *jl_error_doc(PyObject *obj)
{
    if (PyUnicode_Check(obj)) {
        return Py_NewRef(obj);
    }
    if (PyBytes_Check(obj)) {
        return PyUnicode_DecodeUTF8(PyBytes_AS_STRING(obj), PyBytes_GET_SIZE(obj), "surrogateescape");
    }
    if (PyByteArray_Check(obj)) {
        return PyUnicode_DecodeUTF8(PyByteArray_AS_STRING(obj), PyByteArray_GET_SIZE(obj), "surrogateescape");
    }
    if (PyMemoryView_Check(obj)) {
        Py_buffer view;
        PyObject *doc;
        if (PyObject_GetBuffer(obj, &view, PyBUF_SIMPLE) < 0) {
            PyErr_Clear();
            return PyUnicode_FromStringAndSize("", 0);
        }
        doc = PyUnicode_DecodeUTF8((const char *)view.buf, view.len, "surrogateescape");
        PyBuffer_Release(&view);
        return doc;
    }
    return PyUnicode_FromStringAndSize("", 0);
}

static void jl_raise_at(int code, uint32_t pos, const char *msg, PyObject *data)
{
    PyObject *doc;
    PyObject *exc;
    PyObject *c;
    PyObject *p;
    if (!g_loads_error) {
        PyErr_Format(PyExc_ValueError, "%s (code %d pos %u)", msg, code, (unsigned)pos);
        return;
    }
    doc = jl_error_doc(data);
    if (!doc) {
        PyErr_Clear();
        doc = PyUnicode_FromStringAndSize("", 0);
        if (!doc) {
            return;
        }
    }
    exc = PyObject_CallFunction(g_loads_error, "isOn", code, msg, doc, (Py_ssize_t)pos);
    Py_DECREF(doc);
    if (!exc) {
        return;
    }
    c = PyLong_FromLong(code);
    p = PyLong_FromUnsignedLong(pos);
    if (c) {
        PyObject_SetAttrString(exc, "code", c);
    }
    if (p) {
        PyObject_SetAttrString(exc, "pos", p);
    }
    Py_XDECREF(c);
    Py_XDECREF(p);
    PyErr_SetObject(g_loads_error, exc);
    Py_DECREF(exc);
}

/* the kernels report a byte offset; JSONDecodeError wants an index into the
 * str doc built by jl_error_doc, so decode the prefix the same way */
static size_t jl_char_pos(const char *buf, size_t len, size_t pos)
{
    PyObject *s;
    size_t n;
    if (pos > len) {
        pos = len;
    }
    s = PyUnicode_DecodeUTF8(buf, (Py_ssize_t)pos, "surrogateescape");
    if (!s) {
        PyErr_Clear();
        return pos;
    }
    n = (size_t)PyUnicode_GET_LENGTH(s);
    Py_DECREF(s);
    return n;
}

static void jl_raise(int code, size_t pos, PyObject *data)
{
    const char *msg;
    switch (code) {
    case JL_E_EMPTY: msg = "Input is empty or whitespace-only"; break;
    case JL_E_NUMBER: msg = "Invalid number format in JSON"; break;
    case JL_E_UNEXPECTED: msg = "Unexpected character in JSON"; break;
    case JL_E_MEMORY: msg = "Memory allocation failed for JSON parsing"; break;
    default: msg = "Invalid JSON format"; break;
    }
    jl_raise_at(code, (uint32_t)pos, msg, data);
}

PyObject *JlLoads(PyObject *self, PyObject *obj)
{
    JlCtx c;
    const char *buf;
    Py_ssize_t len;
    PyObject *res = NULL;
    char *owned = NULL;
    (void)self;
    if (PyBytes_Check(obj)) {
        buf = PyBytes_AS_STRING(obj);
        len = PyBytes_GET_SIZE(obj);
    } else if (PyUnicode_Check(obj)) {
        buf = PyUnicode_AsUTF8AndSize(obj, &len);
        if (!buf) {
            return NULL;
        }
    } else if (PyByteArray_Check(obj)) {
        /* bytearray keeps a NUL after its data */
        buf = PyByteArray_AS_STRING(obj);
        len = PyByteArray_GET_SIZE(obj);
    } else if (PyMemoryView_Check(obj)) {
        Py_buffer view;
        if (PyMemoryView_GET_BUFFER(obj)->ndim > 1) {
            PyErr_SetString(PyExc_TypeError, "loads() memoryview must be one-dimensional");
            return NULL;
        }
        if (PyObject_GetBuffer(obj, &view, PyBUF_SIMPLE) < 0) {
            return NULL;
        }
        owned = (char *)malloc((size_t)view.len + 1);
        if (!owned) {
            PyBuffer_Release(&view);
            return PyErr_NoMemory();
        }
        memcpy(owned, view.buf, (size_t)view.len);
        owned[view.len] = 0; /* the kernel relies on a NUL sentinel */
        len = view.len;
        buf = owned;
        PyBuffer_Release(&view);
    } else {
        PyErr_SetString(PyExc_TypeError, "loads() expects str, bytes, bytearray, or memoryview");
        return NULL;
    }
    if ((size_t)len > 0xFFFFFFFFu) {
        free(owned);
        PyErr_SetString(PyExc_OverflowError, "JSON input longer than 4 GiB");
        return NULL;
    }
    c.start = (const uint8_t *)buf;
    c.end = c.start + len;
    c.vs = c.small_vs;
    c.vsp = c.vs;
    c.vs_end = c.vs + sizeof(c.small_vs) / sizeof(c.small_vs[0]);
    c.fs = c.small_fs;
    c.fsp = c.fs;
    c.fs_end = c.fs + sizeof(c.small_fs) / sizeof(c.small_fs[0]);
    c.sbuf = NULL;
    c.scap = 0;
    c.err = 0;
    c.err_pos = 0;
    c.pos = NULL;
    c.tmpd = 0.0;
    c.kc = g_kc;
    c.pad1 = 0;
    g_depth++;
    if (jl_run(&c)) {
        res = (PyObject *)c.vs[0];
    } else {
        size_t i;
        for (i = 0; c.vs + i < c.vsp; i++) {
            if (!((uintptr_t)c.vs[i] & 1u)) {
                Py_DECREF((PyObject *)c.vs[i]);
            }
        }
        if (!PyErr_Occurred()) {
            /* a sink NULL without an exception is invalid UTF-8 */
            jl_raise(c.err && c.err != JL_E_MEMORY ? c.err : JL_E_INVALID,
                     jl_char_pos(buf, (size_t)len, c.err_pos), obj);
        }
    }
    if (c.vs != c.small_vs) {
        free(c.vs);
    }
    if (c.fs != c.small_fs) {
        free(c.fs);
    }
    free(c.sbuf);
    free(owned);
    if (--g_depth == 0 && g_grave_n) {
        jl_grave_flush();
    }
    return res;
}
