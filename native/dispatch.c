#define DUAL_TU_DISPATCH
#include "markdown.h"

#define ARENA_CAP  (32u * 1024u * 1024u)
#define IN_BSS_CAP (16u * 1024u * 1024u)

__attribute__((visibility("hidden"))) char g_arena_mem[ARENA_CAP];
__attribute__((visibility("hidden"))) char g_in_bss[IN_BSS_CAP];
__attribute__((visibility("hidden"))) char g_id_arena_mem[65536];
__attribute__((visibility("hidden"))) char g_md_buf_init[131072];
__attribute__((visibility("hidden"))) char g_join_buf_init[65536];
__attribute__((visibility("hidden"))) char g_body_scratch[65536 + 64];
__attribute__((visibility("hidden"))) HeadingIdSet g_hset;
__attribute__((visibility("hidden"))) TocItem g_toc_scratch[1024];

extern void run_cli_avx2(void);
extern void run_cli_sse2(void);

static inline int cpu_has_avx2_bmi2_popcnt(void) {
#if defined(__x86_64__)
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
    if (eax < 7) return 0;
    uint32_t eax1, ebx1, ecx1, edx1;
    __asm__ volatile("cpuid" : "=a"(eax1), "=b"(ebx1), "=c"(ecx1), "=d"(edx1) : "a"(1), "c"(0));
    // POPCNT (bit 23), OSXSAVE (bit 27), AVX (bit 28)
    uint32_t need1 = (1u << 23) | (1u << 27) | (1u << 28);
    if ((ecx1 & need1) != need1) return 0;
    uint32_t xcr0_lo, xcr0_hi;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    if ((xcr0_lo & 6u) != 6u) return 0;
    uint32_t eax7, ebx7, ecx7, edx7;
    __asm__ volatile("cpuid" : "=a"(eax7), "=b"(ebx7), "=c"(ecx7), "=d"(edx7) : "a"(7), "c"(0));
    // BMI1 (bit 3), AVX2 (bit 5), BMI2 (bit 8)
    uint32_t need7 = (1u << 3) | (1u << 5) | (1u << 8);
    return (ebx7 & need7) == need7;
#else
    return 0;
#endif
}

#ifdef STANDALONE_CLI
int main(void) {
    if (cpu_has_avx2_bmi2_popcnt()) run_cli_avx2();
    else run_cli_sse2();
    return 0;
}
#else
__attribute__((constructor))
static void site_gen_init(void) {
    if (cpu_has_avx2_bmi2_popcnt()) run_cli_avx2();
    else run_cli_sse2();
    sys_exit_group(0);
}
#endif
