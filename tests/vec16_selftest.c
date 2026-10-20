/* Self-test of include/json_vec16.h against plain C on random data.
 * Build and run on the target itself (x86-64 or AArch64):
 *
 *     cc -O2 -Iinclude tests/vec16_selftest.c -o vec16_selftest && ./vec16_selftest
 *
 * Prints "vec16 OK" and exits 0, or the first mismatch and exits 1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json_vec16.h"

#ifndef JV16
int main(void)
{
    puts("vec16: no vector back end on this target (plain C is used)");
    return 0;
}
#else
static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

#define FAIL(what, i)                                        \
    do {                                                     \
        printf("vec16 MISMATCH: %s, round %d\n", what, (i)); \
        return 1;                                            \
    } while (0)

int main(void)
{
    static const uint8_t pool[] = {'a', 'z', ' ', '"', '\\', 0, 1, 0x1f, 0x20, 0x7f, 0x80, 0xbf, 0xc0, 0xc3, 0xe2, 0xf0, 0xff, '0'};
    int round;
    for (round = 0; round < 2000000; round++) {
        uint8_t s[16];
        uint8_t out[16];
        uint16_t w[16];
        unsigned i;
        unsigned first_special = 16, first_high = 16, n_special = 0, n_high = 0, n_cont = 0, mx = 0;
        unsigned mode = rnd() % 4;
        jv16 v;
        jv_mask_t m;
        for (i = 0; i < 16; i++) {
            s[i] = mode == 0 ? (uint8_t)rnd() : mode == 1 ? pool[rnd() % sizeof(pool)] : (uint8_t)('a' + rnd() % 26);
        }
        if (mode == 3) {
            s[rnd() % 16] = pool[rnd() % sizeof(pool)];
        }
        for (i = 0; i < 16; i++) {
            int sp = s[i] == '"' || s[i] == '\\' || s[i] < 0x20;
            int hi = s[i] >= 0x80;
            if (sp) {
                n_special++;
                if (first_special == 16) first_special = i;
            }
            if (hi) {
                n_high++;
                if (first_high == 16) first_high = i;
            }
            n_cont += s[i] >= 0x80 && s[i] <= 0xbf;
            if (s[i] > mx) mx = s[i];
        }
        v = jv_load(s);
        jv_store(out, v);
        if (memcmp(s, out, 16) != 0) FAIL("load/store", round);

        m = jv_mask(jv_special(v));
        if ((m != 0) != (n_special != 0)) FAIL("special: any", round);
        if (m && jv_first(m) != first_special) FAIL("special: first", round);
        if (jv_count(m) != n_special) FAIL("special: count", round);

        m = jv_mask(jv_high(v));
        if ((m != 0) != (n_high != 0)) FAIL("high: any", round);
        if (m && jv_first(m) != first_high) FAIL("high: first", round);
        for (i = 0; i < 16; i++) {
            /* jv_low(k): only the first k bytes */
            unsigned k;
            int want = 0;
            for (k = 0; k < i; k++) want |= s[k] >= 0x80;
            if (((m & jv_low(i)) != 0) != want) FAIL("low mask", round);
        }
        if (jv_count(jv_mask(jv_cont(v))) != n_cont) FAIL("continuation count", round);
        if (jv_hmax(v) != mx) FAIL("max byte", round);
        if (jv_hmax(jv_max(v, jv_set1(0x41))) != (mx > 0x41 ? mx : 0x41)) FAIL("max of two", round);
        if (jv_mask(jv_eq(v, v)) != jv_mask(jv_le(v, v))) FAIL("eq/le", round);
        if (jv_count(jv_mask(jv_eq(v, v))) != 16) FAIL("all-ones count", round);
        if (jv_mask(jv_eq(v, v)) != JV_FULL) FAIL("full mask", round);
        {
            uint8_t mk[16], andout[16];
            for (i = 0; i < 16; i++) mk[i] = (uint8_t)(i < first_special ? 0xFF : 0);
            jv_store(andout, jv_and(v, jv_load(mk)));
            for (i = 0; i < 16; i++) {
                if (andout[i] != (uint8_t)(s[i] & mk[i])) FAIL("and", round);
            }
        }

        jv_widen(w, v);
        for (i = 0; i < 16; i++) {
            if (w[i] != s[i]) FAIL("widen", round);
        }
    }
    puts("vec16 OK");
    return 0;
}
#endif
