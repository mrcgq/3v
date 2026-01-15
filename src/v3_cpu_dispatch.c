#include "v3_cpu_dispatch.h"
#include <stdio.h>
#include <string.h>

#ifdef __x86_64__
#include <cpuid.h>
#endif

// 全局状态
static uint32_t g_cpu_features = 0;
static cpu_level_t g_cpu_level = CPU_LEVEL_GENERIC;
static bool g_detected = false;

#ifdef __x86_64__
static void detect_x86(void) {
    unsigned int eax, ebx, ecx, edx;
    
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        if (edx & bit_SSE2)  g_cpu_features |= CPU_FEATURE_SSE2;
        if (ecx & bit_AVX)   g_cpu_features |= CPU_FEATURE_AVX;
    }
    
    if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        if (ebx & bit_AVX2)     g_cpu_features |= CPU_FEATURE_AVX2;
        if (ebx & bit_AVX512F)  g_cpu_features |= CPU_FEATURE_AVX512F;
    }
    
    if (g_cpu_features & CPU_FEATURE_AVX512F) {
        g_cpu_level = CPU_LEVEL_AVX512;
    } else if (g_cpu_features & CPU_FEATURE_AVX2) {
        g_cpu_level = CPU_LEVEL_AVX2;
    } else {
        g_cpu_level = CPU_LEVEL_GENERIC;
    }
}
#endif

#ifdef __aarch64__
#include <sys/auxv.h>
#include <asm/hwcap.h>
static void detect_arm64(void) {
    g_cpu_features |= CPU_FEATURE_NEON;
    g_cpu_level = CPU_LEVEL_NEON;
}
#endif

void cpu_detect(void) {
    if (g_detected) return;
#ifdef __x86_64__
    detect_x86();
#elif defined(__aarch64__)
    detect_arm64();
#endif
    g_detected = true;
}

cpu_level_t cpu_get_level(void) {
    if (!g_detected) cpu_detect();
    return g_cpu_level;
}

const char* cpu_level_name(cpu_level_t level) {
    switch (level) {
        case CPU_LEVEL_GENERIC: return "Generic (Scalar)";
        case CPU_LEVEL_AVX2:    return "AVX2";
        case CPU_LEVEL_AVX512:  return "AVX-512";
        case CPU_LEVEL_NEON:    return "NEON (ARM64)";
        default:                return "Unknown";
    }
}

void cpu_print_info(void) {
    if (!g_detected) cpu_detect();
    printf("[CPU] Detected Level: %s\n", cpu_level_name(g_cpu_level));
}
