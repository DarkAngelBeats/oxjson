/* Shortest round-trip binary64 -> text in orjson 3.13 layout
 * (1.0, 0.0, -0.0, 0.00001, 1e-6, 1e+16, 1234567890123456.0): Python repr
 * except that exponent -5 is written in decimal form and exponents are not
 * zero padded.
 * Digits: the Zmij algorithm (Victor Zverovich, https://github.com/vitaut/zmij),
 * ported from its Rust port by David Tolnay (zmij 1.0.23):
 * one 128x64-bit multiplication and no branch on the digit count.
 * License: MIT; copyright notices and full text in licenses/MIT-zmij.txt
 * (also listed in THIRD_PARTY.md). Table: include/json_f64_pow10.h from
 * tools/gen_pow10_table.py.
 * Text: the 16 digits are converted in one SSE pass (AVX2 build) or as two
 * SWAR words (portable build) and placed with fixed-size stores.
 * Caller handles NaN / Infinity. Needs 40 writable bytes. Returns length.
 * Header-only: static functions, no globals besides the const tables. */
#ifndef JSON_F64_FMT_H
#define JSON_F64_FMT_H

#include <stdint.h>
#include <string.h>
#include "json_f64_pow10.h"

static const char jf64_d2[200] = {
    '0','0','0','1','0','2','0','3','0','4','0','5','0','6','0','7','0','8','0','9',
    '1','0','1','1','1','2','1','3','1','4','1','5','1','6','1','7','1','8','1','9',
    '2','0','2','1','2','2','2','3','2','4','2','5','2','6','2','7','2','8','2','9',
    '3','0','3','1','3','2','3','3','3','4','3','5','3','6','3','7','3','8','3','9',
    '4','0','4','1','4','2','4','3','4','4','4','5','4','6','4','7','4','8','4','9',
    '5','0','5','1','5','2','5','3','5','4','5','5','5','6','5','7','5','8','5','9',
    '6','0','6','1','6','2','6','3','6','4','6','5','6','6','6','7','6','8','6','9',
    '7','0','7','1','7','2','7','3','7','4','7','5','7','6','7','7','7','8','7','9',
    '8','0','8','1','8','2','8','3','8','4','8','5','8','6','8','7','8','8','8','9',
    '9','0','9','1','9','2','9','3','9','4','9','5','9','6','9','7','9','8','9','9'};

/* Number of decimal digits of f, 1 <= f < 10^20. */
static inline int jf64_ndig(uint64_t f)
{
    static const uint64_t p10[20] = {
        1ULL, 10ULL, 100ULL, 1000ULL, 10000ULL, 100000ULL, 1000000ULL, 10000000ULL,
        100000000ULL, 1000000000ULL, 10000000000ULL, 100000000000ULL, 1000000000000ULL,
        10000000000000ULL, 100000000000000ULL, 1000000000000000ULL,
        10000000000000000ULL, 100000000000000000ULL, 1000000000000000000ULL,
        10000000000000000000ULL};
    int lg2 = 63 - __builtin_clzll(f | 1u);
    int d = ((lg2 + 1) * 1233) >> 12; /* floor(log10(2^(lg2+1))) */
    return d + (f >= p10[d]);
}

static inline void jf64_cpy2(char *p, unsigned r)
{
    memcpy(p, jf64_d2 + 2 * r, 2);
}

/* Exactly 8 digits of x < 10^8 at p[0..8). */
static inline void jf64_write8(char *p, uint32_t x)
{
    uint32_t a = x / 10000u;
    uint32_t b = x - a * 10000u;
    uint32_t a1 = a / 100u;
    uint32_t b1 = b / 100u;
    jf64_cpy2(p, a1);
    jf64_cpy2(p + 2, a - a1 * 100u);
    jf64_cpy2(p + 4, b1);
    jf64_cpy2(p + 6, b - b1 * 100u);
}

/* Writes the n digits of f to d[0..n). */
static inline void jf64_digits(char *d, uint64_t f, int n)
{
    char *p = d + n;
    uint32_t x;
    while (f >= 100000000u) {
        uint64_t q = f / 100000000u;
        p -= 8;
        jf64_write8(p, (uint32_t)(f - q * 100000000u));
        f = q;
    }
    x = (uint32_t)f;
    while (x >= 100u) {
        uint32_t q = x / 100u;
        p -= 2;
        jf64_cpy2(p, x - q * 100u);
        x = q;
    }
    if (x >= 10u) {
        jf64_cpy2(p - 2, x);
    } else {
        p[-1] = (char)('0' + x);
    }
}

/* x < 10^8 -> eight digit values (0..9) in bytes, most significant digit in
 * the lowest byte (little-endian memory order). No '0' bias. */
static inline uint64_t jf64_swar8(uint32_t x)
{
    uint64_t a = x / 10000u;
    uint64_t b = x - (uint32_t)a * 10000u;
    uint64_t v = a | (b << 32);
    uint64_t hi = ((v * 10486u) >> 20) & 0x0000007F0000007FULL;
    uint64_t lo = v - hi * 100u;
    v = hi | (lo << 16);
    hi = ((v * 103u) >> 10) & 0x000F000F000F000FULL;
    lo = v - hi * 10u;
    return hi | (lo << 8);
}

static inline void jf64_st8(char *p, uint64_t v)
{
    memcpy(p, &v, 8);
}

/* ---- Zmij: shortest decimal digits ---- */
typedef struct {
    uint64_t sig; /* 15 or 16 digits */
    int exp;      /* value ~ sig * 10^(exp + 1) */
    int last;     /* the digit after sig */
    int has_last; /* the shortest form needs it: value ~ (10 sig + last) * 10^exp */
} JzmDec;

static inline uint64_t jzm_mulhi_add(uint64_t x, uint64_t y, uint64_t c)
{
    return (uint64_t)(((unsigned __int128)x * y + c) >> 64);
}

/* value = bin_sig * 2^bin_exp; regular: the binary significand is not a power of two */
__attribute__((always_inline)) static inline JzmDec jzm_decimal(uint64_t bin_sig, int bin_exp, int regular)
{
    JzmDec r;
    int dec_exp = (bin_exp * 315653 - (regular ? 0 : 131072)) >> 20;
    int shift = bin_exp + ((-(dec_exp + 1) * 217707) >> 16) + 1 + 6;
    const uint64_t *pw = jzm_pow10[-dec_exp - 1 - JZM_K_MIN];
    uint64_t x = bin_sig << shift;
    unsigned __int128 ph = (unsigned __int128)pw[0] * x;
    uint64_t plo = (uint64_t)ph + (uint64_t)(((unsigned __int128)pw[1] * x) >> 64);
    uint64_t phi = (uint64_t)(ph >> 64) + (plo < (uint64_t)ph);
    uint64_t integral = phi >> 6;
    uint64_t fractional = (phi << 58) | (plo >> 6);
    uint64_t half_ulp;
    int round_up;
    int round_down;
    int digit;
    if (regular) {
        half_ulp = (pw[0] >> (7 - shift)) + (1 - (bin_sig & 1));
        round_up = fractional + half_ulp < fractional;
        round_down = half_ulp > fractional;
        digit = (int)jzm_mulhi_add(fractional, 10, (1ULL << 63) + 6);
        if (fractional == (1ULL << 62)) {
            digit = 2; /* 2.5 rounds to 2 */
        }
    } else {
        int lo;
        half_ulp = pw[0] >> (7 - shift);
        round_up = half_ulp > ~fractional;
        round_down = (half_ulp >> 1) > fractional;
        digit = (int)jzm_mulhi_add(fractional, 10, (1ULL << 63) - 1);
        lo = (int)jzm_mulhi_add(fractional - (half_ulp >> 1), 10, ~0ULL);
        if (digit < lo) {
            digit = lo;
        }
    }
    r.sig = integral + (uint64_t)round_up;
    r.exp = dec_exp;
    r.last = digit;
    r.has_last = !(round_up | round_down);
    return r;
}

#define JZM_THRESHOLD 1000000000000000ULL /* 10^15 */

/* bits: a finite nonzero double without its sign */
__attribute__((always_inline)) static inline JzmDec jzm_dec_of(uint64_t bits)
{
    int raw = (int)(bits >> 52) & 0x7ff;
    uint64_t m = bits & ((1ULL << 52) - 1);
    if (__builtin_expect(raw == 0, 0)) {
        /* subnormal: renormalize to 16 digits */
        JzmDec d = jzm_decimal(m, 1 - 1075, 1);
        uint64_t s = d.sig * 10 + (d.has_last ? (uint64_t)d.last : 0);
        int e = d.exp;
        uint64_t q;
        while (s < JZM_THRESHOLD) {
            s *= 10;
            e--;
        }
        q = s / 10;
        d.sig = q;
        d.exp = e;
        d.last = (int)(s - q * 10);
        d.has_last = d.last != 0;
        return d;
    }
    return jzm_decimal(m | (1ULL << 52), raw - 1075, m != 0);
}

/* "e+XX" / "e-X" / "e-XXX" at o, as orjson 3.13 writes it: no zero padding
 * (e-6, e-10). A positive exponent only comes after the decimal range, so it
 * always has two or three digits (e+16). Returns the end. */
static inline char *jf64_exp(char *o, int x)
{
    o[0] = 'e';
    o[1] = '+';
    if (x < 0) {
        o[1] = '-';
        x = -x;
    }
    o += 2;
    if (x >= 100) {
        o[0] = (char)('0' + x / 100);
        jf64_cpy2(o + 1, (unsigned)(x % 100));
        return o + 3;
    }
    if (x < 10) {
        o[0] = (char)('0' + x);
        return o + 1;
    }
    jf64_cpy2(o, (unsigned)x);
    return o + 2;
}

/* Decimal (not exponent) form for first-digit exponents lo..hi, as orjson
 * 3.13 writes them: binary64 -5..15 (0.00001, 1e-6, 1e+16), numpy float32 and
 * float16 values -6..12 (0.000001, 1e-7, 1e+13). */
#define JF64_LO (-5)
#define JF64_HI 15
#define JF32_LO (-6)
#define JF32_HI 12

#ifdef JF64_PORTABLE
#define JF64_ATTR __attribute__((always_inline))

/* orjson layout with decimal range lo..hi. The significant digits S (n of
 * them, then '0' bytes) sit in T, so every copy has a fixed size. */
JF64_ATTR
static inline int jf64_write_lay(char *out, double v, int lo, int hi)
{
    char T[48];
    const char *S = T + 1;
    uint64_t bits;
    JzmDec d;
    int neg;
    int has_extra;
    int x;
    int n;
    int nd;
    char *o;
    memcpy(&bits, &v, 8);
    neg = (int)(bits >> 63);
    o = out + neg;
    out[0] = '-';
    bits &= ~(1ULL << 63);
    if (bits == 0) {
        memcpy(o, "0.0", 3);
        return neg + 3;
    }
    d = jzm_dec_of(bits);
    has_extra = d.sig >= JZM_THRESHOLD;
    x = d.exp + 15 + has_extra; /* decimal exponent of the first digit */
    {
        uint32_t h = (uint32_t)(d.sig / 100000000u);
        uint32_t l = (uint32_t)(d.sig - (uint64_t)h * 100000000u);
        uint64_t w1 = jf64_swar8(h);
        uint64_t w2 = jf64_swar8(l);
        /* digits of the 16-wide field up to the last nonzero one; the last digit is the top byte of w2 */
        nd = w2 ? 16 - (__builtin_clzll(w2) >> 3) : 8 - (__builtin_clzll(w1) >> 3);
        jf64_st8(T + 16, 0x3030303030303030ULL);
        jf64_st8(T + 24, 0x3030303030303030ULL);
        jf64_st8(T + 32, 0x3030303030303030ULL);
        jf64_st8(T + 40, 0x3030303030303030ULL);
        jf64_st8(T + has_extra, w1 + 0x3030303030303030ULL);
        jf64_st8(T + 8 + has_extra, w2 + 0x3030303030303030ULL);
        T[16 + has_extra] = (char)('0' + (d.last & -d.has_last));
    }
    n = has_extra - 1 + nd + ((17 - nd) & -d.has_last);
    if (x >= lo && x <= hi) {
        if (x >= n - 1) {
            memcpy(o, S, 16);
            o[x + 1] = '.';
            o[x + 2] = '0';
            return neg + x + 3;
        }
        if (x >= 0) {
            memcpy(o, S, 16);
            memcpy(o + x + 2, S + x + 1, 16);
            o[x + 1] = '.';
            return neg + n + 1;
        }
        jf64_st8(o, 0x3030303030302e30ULL); /* "0.000000" */
        memcpy(o + 1 - x, S, 17);
        return neg + 1 - x + n;
    }
    o[0] = S[0];
    memcpy(o + 2, S + 1, 16);
    o[1] = '.';
    return (int)(jf64_exp(o + (n > 1 ? n + 1 : 1), x) - out);
}
#else
#include <immintrin.h>
#define JF64_ATTR __attribute__((target("avx2"), always_inline))

/* pshufb masks: byte i <- i + k, zero past 15 */
static const unsigned char jf64_shl[17][16] __attribute__((aligned(16))) = {
#define JF64_R(k) { (k)+0>15?0x80:(k)+0, (k)+1>15?0x80:(k)+1, (k)+2>15?0x80:(k)+2, (k)+3>15?0x80:(k)+3, \
    (k)+4>15?0x80:(k)+4, (k)+5>15?0x80:(k)+5, (k)+6>15?0x80:(k)+6, (k)+7>15?0x80:(k)+7, \
    (k)+8>15?0x80:(k)+8, (k)+9>15?0x80:(k)+9, (k)+10>15?0x80:(k)+10, (k)+11>15?0x80:(k)+11, \
    (k)+12>15?0x80:(k)+12, (k)+13>15?0x80:(k)+13, (k)+14>15?0x80:(k)+14, (k)+15>15?0x80:(k)+15 }
    JF64_R(0), JF64_R(1), JF64_R(2), JF64_R(3), JF64_R(4), JF64_R(5), JF64_R(6), JF64_R(7),
    JF64_R(8), JF64_R(9), JF64_R(10), JF64_R(11), JF64_R(12), JF64_R(13), JF64_R(14), JF64_R(15), JF64_R(16)
#undef JF64_R
};

/* 16 zero-padded ASCII digits of sig < 10^16 (Zmij's SSE4.1 steps);
 * *nd = digits up to the last nonzero one */
__attribute__((target("avx2"), always_inline))
static inline __m128i jzm_digits16(uint64_t sig, int *nd)
{
    uint64_t hi = sig / 100000000u;
    uint64_t lo = sig - hi * 100000000u;
    __m128i x = _mm_set_epi64x((long long)hi, (long long)lo);
    /* each 64-bit lane: n % 10000 | (n / 10000) << 32 */
    __m128i y = _mm_add_epi64(x, _mm_mul_epu32(_mm_set1_epi64x((1LL << 32) - 10000),
                                               _mm_srli_epi64(_mm_mul_epu32(x, _mm_set1_epi64x(109951163)), 40)));
    /* each 32-bit lane: n % 100 | (n / 100) << 16 */
    __m128i z = _mm_add_epi64(y, _mm_mullo_epi32(_mm_set1_epi32((1 << 16) - 100),
                                                 _mm_srli_epi32(_mm_mulhi_epu16(y, _mm_set1_epi32(5243)), 3)));
    /* each 16-bit lane: n % 10 | (n / 10) << 8; the bytes are least significant first */
    __m128i bcd = _mm_add_epi64(z, _mm_mullo_epi16(_mm_set1_epi16((1 << 8) - 10),
                                                   _mm_mulhi_epu16(z, _mm_set1_epi16(6554))));
    unsigned mask = (unsigned)_mm_movemask_epi8(_mm_cmpgt_epi8(bcd, _mm_setzero_si128()));
    *nd = 16 - __builtin_ctz(mask);
    return _mm_or_si128(_mm_shuffle_epi8(bcd, _mm_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)),
                        _mm_set1_epi8('0'));
}

/* orjson layout with decimal range lo..hi. W = the first 16 significant
 * digits (then '0'), s16 = the 17th. */
JF64_ATTR
static inline int jf64_write_lay(char *out, double v, int lo, int hi)
{
    uint64_t bits;
    JzmDec d;
    __m128i V;
    __m128i W;
    int neg;
    int has_extra;
    int x;
    int n;
    int nd;
    char L;
    char s16;
    char *o;
    memcpy(&bits, &v, 8);
    neg = (int)(bits >> 63);
    o = out + neg;
    out[0] = '-';
    bits &= ~(1ULL << 63);
    if (bits == 0) {
        memcpy(o, "0.0", 3);
        return neg + 3;
    }
    d = jzm_dec_of(bits);
    has_extra = d.sig >= JZM_THRESHOLD;
    x = d.exp + 15 + has_extra; /* decimal exponent of the first digit */
    /* masks, not branches: whether the last digit is needed is data-dependent */
    L = (char)('0' + (d.last & -d.has_last));
    V = jzm_digits16(d.sig, &nd);
    n = has_extra - 1 + nd + ((17 - nd) & -d.has_last);
    /* 15-digit sig: drop the pad digit, the last digit becomes the 16th */
    W = _mm_blendv_epi8(_mm_insert_epi8(_mm_srli_si128(V, 1), L, 15), V, _mm_set1_epi8((char)-has_extra));
    s16 = (char)('0' + ((L - '0') & -has_extra));
    if (x >= lo && x <= hi) {
        if (x >= n - 1) {
            _mm_storeu_si128((__m128i *)o, W);
            o[x + 1] = '.';
            o[x + 2] = '0';
            return neg + x + 3;
        }
        if (x >= 0) {
            _mm_storeu_si128((__m128i *)o, W);
            _mm_storeu_si128((__m128i *)(o + x + 2), _mm_shuffle_epi8(W, _mm_load_si128((const __m128i *)jf64_shl[x + 1])));
            o[17] = s16;
            o[x + 1] = '.';
            return neg + n + 1;
        }
        memcpy(o, "0.000000", 8);
        _mm_storeu_si128((__m128i *)(o + 1 - x), W);
        o[17 - x] = s16;
        return neg + 1 - x + n;
    }
    _mm_storeu_si128((__m128i *)(o + 1), W);
    o[17] = s16;
    o[0] = (char)_mm_cvtsi128_si32(W);
    o[1] = '.';
    return (int)(jf64_exp(o + (n > 1 ? n + 1 : 1), x) - out);
}
#endif /* JF64_PORTABLE */

/* a Python float (binary64) */
JF64_ATTR
static inline int jf64_write(char *out, double v)
{
    return jf64_write_lay(out, v, JF64_LO, JF64_HI);
}

#endif
