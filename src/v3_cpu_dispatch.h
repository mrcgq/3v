#ifndef V3_CPU_DISPATCH_H
#define V3_CPU_DISPATCH_H

#include <stdint.h>
#include <stdbool.h>

// CPU 特性标志
typedef enum {
    CPU_FEATURE_SSE2      = (1 << 0),
    CPU_FEATURE_AVX       = (1 << 5),
    CPU_FEATURE_AVX2      = (1 << 6),
    CPU_FEATURE_AVX512F   = (1 << 7),
    CPU_FEATURE_NEON      = (1 << 9),
} cpu_feature_t;

// CPU 级别
typedef enum {
    CPU_LEVEL_GENERIC = 0,
    CPU_LEVEL_AVX2,
    CPU_LEVEL_AVX512,
    CPU_LEVEL_NEON,
} cpu_level_t;

// API
void cpu_detect(void);
cpu_level_t cpu_get_level(void);
const char* cpu_level_name(cpu_level_t level);
void cpu_print_info(void);

#endif // V3_CPU_DISPATCH_H
