/* 16-byte vector helpers for the portable build: NEON on AArch64, SSE2 on x86-64.
 * JV16 is defined when one of them is available; otherwise the callers use
 * their plain C loops. Both back ends give the same results (tests/vec16_selftest.c).
 */
#ifndef JSON_VEC16_H
#define JSON_VEC16_H

#include <stdint.h>

#if defined(__aarch64__) && defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>
#define JV16 1
#define JV_BITS 4 /* mask bits per byte */
#define JV_FULL (~(uint64_t)0)
typedef uint8x16_t jv16;
typedef uint64_t jv_mask_t;

static inline jv16 jv_load(const void *p) { return vld1q_u8((const uint8_t *)p); }
static inline void jv_store(void *p, jv16 v) { vst1q_u8((uint8_t *)p, v); }
static inline jv16 jv_set1(uint8_t c) { return vdupq_n_u8(c); }
static inline jv16 jv_or(jv16 a, jv16 b) { return vorrq_u8(a, b); }
static inline jv16 jv_and(jv16 a, jv16 b) { return vandq_u8(a, b); }
static inline jv16 jv_eq(jv16 a, jv16 b) { return vceqq_u8(a, b); }
static inline jv16 jv_le(jv16 a, jv16 b) { return vcleq_u8(a, b); } /* unsigned a <= b */
static inline jv16 jv_max(jv16 a, jv16 b) { return vmaxq_u8(a, b); }
static inline jv16 jv_high(jv16 v) { return vcltzq_s8(vreinterpretq_s8_u8(v)); }                     /* byte >= 0x80 */
static inline jv16 jv_cont(jv16 v) { return vcltq_s8(vreinterpretq_s8_u8(v), vdupq_n_s8(-64)); }      /* 0x80..0xBF */
/* four mask bits per byte, byte 0 in the low nibble */
static inline jv_mask_t jv_mask(jv16 m)
{
    return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(m), 4)), 0);
}
static inline unsigned jv_first(jv_mask_t m) { return (unsigned)__builtin_ctzll(m) >> 2; }
static inline unsigned jv_count(jv_mask_t m) { return (unsigned)__builtin_popcountll(m) >> 2; }
static inline unsigned jv_hmax(jv16 v) { return vmaxvq_u8(v); }
/* 16 bytes -> 16 two-byte units */
static inline void jv_widen(uint16_t *d, jv16 v)
{
    vst1q_u16(d, vmovl_u8(vget_low_u8(v)));
    vst1q_u16(d + 8, vmovl_high_u8(v));
}

#elif defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#define JV16 1
#define JV_BITS 1
#define JV_FULL 0xFFFFu
typedef __m128i jv16;
typedef uint32_t jv_mask_t;

static inline jv16 jv_load(const void *p) { return _mm_loadu_si128((const __m128i *)p); }
static inline void jv_store(void *p, jv16 v) { _mm_storeu_si128((__m128i *)p, v); }
static inline jv16 jv_set1(uint8_t c) { return _mm_set1_epi8((char)c); }
static inline jv16 jv_or(jv16 a, jv16 b) { return _mm_or_si128(a, b); }
static inline jv16 jv_and(jv16 a, jv16 b) { return _mm_and_si128(a, b); }
static inline jv16 jv_eq(jv16 a, jv16 b) { return _mm_cmpeq_epi8(a, b); }
static inline jv16 jv_le(jv16 a, jv16 b) { return _mm_cmpeq_epi8(_mm_max_epu8(a, b), b); }
static inline jv16 jv_max(jv16 a, jv16 b) { return _mm_max_epu8(a, b); }
static inline jv16 jv_high(jv16 v) { return _mm_cmplt_epi8(v, _mm_setzero_si128()); }
static inline jv16 jv_cont(jv16 v) { return _mm_cmplt_epi8(v, _mm_set1_epi8(-64)); }
static inline jv_mask_t jv_mask(jv16 m) { return (jv_mask_t)_mm_movemask_epi8(m); }
static inline unsigned jv_first(jv_mask_t m) { return (unsigned)__builtin_ctz(m); }
static inline unsigned jv_count(jv_mask_t m) { return (unsigned)__builtin_popcount(m); }
static inline unsigned jv_hmax(jv16 v)
{
    v = _mm_max_epu8(v, _mm_srli_si128(v, 8));
    v = _mm_max_epu8(v, _mm_srli_si128(v, 4));
    v = _mm_max_epu8(v, _mm_srli_si128(v, 2));
    v = _mm_max_epu8(v, _mm_srli_si128(v, 1));
    return (unsigned)_mm_cvtsi128_si32(v) & 0xFFu;
}
static inline void jv_widen(uint16_t *d, jv16 v)
{
    const __m128i z = _mm_setzero_si128();
    _mm_storeu_si128((__m128i *)d, _mm_unpacklo_epi8(v, z));
    _mm_storeu_si128((__m128i *)(d + 8), _mm_unpackhi_epi8(v, z));
}
#endif

#ifdef JV16
/* mask bits of the first k bytes, k <= 15 */
static inline jv_mask_t jv_low(unsigned k) { return ((jv_mask_t)1 << (k * JV_BITS)) - 1u; }
/* '"', '\\' or a byte below 0x20 */
static inline jv16 jv_special(jv16 v)
{
    return jv_or(jv_or(jv_eq(v, jv_set1('"')), jv_eq(v, jv_set1('\\'))), jv_le(v, jv_set1(0x1f)));
}
#endif

#endif
