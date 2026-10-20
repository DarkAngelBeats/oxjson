/* CPU detection: sets g_jsonHasAvx2 once. OXJSON_NO_AVX2=1 forces the portable path. */
#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#define JSON_X86_64 1
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(JSON_X86_64)
#include <cpuid.h>
#endif
#else
#include <stdlib.h>
#if defined(JSON_X86_64)
#include <cpuid.h>
#endif
#endif

int g_jsonHasAvx2 = 0;

static int env_no_avx2(void)
{
#if defined(_WIN32)
    char env[4];
    DWORD n = GetEnvironmentVariableA("OXJSON_NO_AVX2", env, (DWORD)sizeof(env));
    return n == 1 && env[0] == '1';
#else
    const char *e = getenv("OXJSON_NO_AVX2");
    return e && e[0] == '1' && e[1] == 0;
#endif
}

#if !defined(JSON_X86_64)
/* not x86-64: the portable code is the only path */
void JsonDetectAvx2(void)
{
    (void)env_no_avx2;
}
#elif defined(_MSC_VER)
void JsonDetectAvx2(void)
{
    static int probed;
    int cpu[4];
    unsigned long long xcr0;

    if (probed)
        return;
    probed = 1;
    if (env_no_avx2())
        return;

    __cpuid(cpu, 1);
    if ((cpu[2] & ((1 << 27) | (1 << 28))) != ((1 << 27) | (1 << 28)))
        return;
    xcr0 = _xgetbv(0);
    if ((xcr0 & 6ull) != 6ull)
        return;
    __cpuidex(cpu, 7, 0);
    if (cpu[1] & (1 << 5))
        g_jsonHasAvx2 = 1;
}
#else
static unsigned long long json_xgetbv(unsigned idx)
{
    unsigned eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(idx));
    return ((unsigned long long)edx << 32) | eax;
}

void JsonDetectAvx2(void)
{
    static int probed;
    unsigned eax, ebx, ecx, edx;
    unsigned long long xcr0;

    if (probed)
        return;
    probed = 1;
    if (env_no_avx2())
        return;

    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return;
    if ((ecx & ((1u << 27) | (1u << 28))) != ((1u << 27) | (1u << 28)))
        return;
    xcr0 = json_xgetbv(0);
    if ((xcr0 & 6ull) != 6ull)
        return;
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx))
        return;
    if (ebx & (1u << 5))
        g_jsonHasAvx2 = 1;
}
#endif
