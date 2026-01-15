
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <time.h>

#include "v3_fec_simd.h"
#include "v3_pacing_adaptive.h"
#include "v3_antidetect_mtu.h"

// =========================================================
// 配置
// =========================================================
typedef struct {
    // FEC
    bool        fec_enabled;
    fec_type_t  fec_type;
    uint8_t     fec_data_shards;
    uint8_t     fec_parity_shards;
    
    // Pacing
    bool        pacing_enabled;
    uint64_t    pacing_initial_bps;
    uint64_t    pacing_min_bps;
    uint64_t    pacing_max_bps;
    
    // Anti-Detect
    ad_profile_t ad_profile;
    uint16_t     mtu;
    
    // Network
    uint16_t    port;
    const char *bind_addr;
    
    // Debug
    bool        verbose;
    bool        benchmark;
} config_t;

static config_t g_config = {
    .fec_enabled = false,
    .fec_type = FEC_TYPE_AUTO,
    .fec_data_shards = 5,
    .fec_parity_shards = 2,
    
    .pacing_enabled = false,
    .pacing_initial_bps = 100 * 1000 * 1000,
    .pacing_min_bps = 1 * 1000 * 1000,
    .pacing_max_bps = 1000 * 1000 * 1000,
    
    .ad_profile = AD_PROFILE_NONE,
    .mtu = 1500,
    
    .port = 51820,
    .bind_addr = "0.0.0.0",
    
    .verbose = false,
    .benchmark = false,
};

// =========================================================
// 全局实例
// =========================================================
static fec_engine_t *g_fec = NULL;
static pacing_adaptive_t g_pacing;
static ad_mtu_ctx_t g_antidetect;
static volatile sig_atomic_t g_running = 1;

// =========================================================
// 初始化
// =========================================================
static void init_modules(void) {
    // FEC
    if (g_config.fec_enabled) {
        g_fec = fec_create(g_config.fec_type,
                           g_config.fec_data_shards,
                           g_config.fec_parity_shards);
        if (!g_fec) {
            fprintf(stderr, "Failed to create FEC engine\n");
            exit(1);
        }
        
        if (g_config.verbose) {
            const char *type_str;
            switch (fec_get_type(g_fec)) {
            case FEC_TYPE_XOR: type_str = "XOR"; break;
            case FEC_TYPE_RS_SIMPLE: type_str = "RS-Simple"; break;
            case FEC_TYPE_RS_SIMD: type_str = "RS-SIMD"; break;
            default: type_str = "Unknown"; break;
            }
            printf("[FEC] Using %s algorithm\n", type_str);
        }
    }
    
    // Pacing
    if (g_config.pacing_enabled) {
        pacing_adaptive_init(&g_pacing, g_config.pacing_initial_bps);
        pacing_adaptive_set_range(&g_pacing, 
                                   g_config.pacing_min_bps,
                                   g_config.pacing_max_bps);
        pacing_adaptive_enable_jitter(&g_pacing, 50000);  // 50µs jitter
    }
    
    // Anti-Detect
    if (g_config.ad_profile != AD_PROFILE_NONE) {
        ad_mtu_init(&g_antidetect, g_config.ad_profile, g_config.mtu);
        
        if (g_config.verbose) {
            printf("[AntiDetect] Max safe payload: %zu bytes\n",
                   ad_mtu_max_payload(&g_antidetect));
        }
    }
}

// =========================================================
// 基准测试
// =========================================================
static void run_benchmark(void) {
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║                    FEC BENCHMARK                              ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    
    printf("║  SIMD Available: %-5s                                        ║\n",
           fec_simd_available() ? "YES" : "NO");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    
    size_t test_sizes[] = {1000, 5000, 10000, 50000};
    int iterations = 10000;
    
    for (size_t i = 0; i < sizeof(test_sizes)/sizeof(test_sizes[0]); i++) {
        size_t size = test_sizes[i];
        
        double xor_speed = fec_benchmark(FEC_TYPE_XOR, size, iterations);
        double rs_simple_speed = fec_benchmark(FEC_TYPE_RS_SIMPLE, size, iterations);
        double rs_simd_speed = fec_benchmark(FEC_TYPE_RS_SIMD, size, iterations);
        
        printf("║  %5zu bytes:                                                  ║\n", size);
        printf("║    XOR:       %8.1f MB/s                                   ║\n", xor_speed);
        printf("║    RS-Simple: %8.1f MB/s                                   ║\n", rs_simple_speed);
        printf("║    RS-SIMD:   %8.1f MB/s                                   ║\n", rs_simd_speed);
        printf("╠═══════════════════════════════════════════════════════════════╣\n");
    }
    
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
}

// =========================================================
// 命令行
// =========================================================
static void usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n\n", prog);
    printf("FEC Options:\n");
    printf("  --fec[=TYPE]          Enable FEC (auto|xor|rs|rs-simd)\n");
    printf("  --fec-shards=D:P      Data:Parity shards (default: 5:2)\n");
    printf("\nPacing Options:\n");
    printf("  --pacing=MBPS         Initial pacing rate\n");
    printf("  --pacing-range=MIN:MAX  Rate range in Mbps\n");
    printf("\nAnti-Detect Options:\n");
    printf("  --profile=TYPE        https|video|voip|gaming\n");
    printf("  --mtu=SIZE            MTU size (default: 1500)\n");
    printf("\nGeneral:\n");
    printf("  -p, --port=PORT       Listen port\n");
    printf("  -b, --bind=ADDR       Bind address\n");
    printf("  -v, --verbose         Verbose output\n");
    printf("  --benchmark           Run FEC benchmark\n");
    printf("  -h, --help            Show help\n");
}

static void parse_args(int argc, char **argv) {
    static struct option long_opts[] = {
        {"fec",         optional_argument, 0, 'f'},
        {"fec-shards",  required_argument, 0, 'F'},
        {"pacing",      required_argument, 0, 'P'},
        {"pacing-range", required_argument, 0, 'R'},
        {"profile",     required_argument, 0, 'A'},
        {"mtu",         required_argument, 0, 'M'},
        {"port",        required_argument, 0, 'p'},
        {"bind",        required_argument, 0, 'b'},
        {"verbose",     no_argument,       0, 'v'},
        {"benchmark",   no_argument,       0, 'B'},
        {"help",        no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "f::F:P:R:A:M:p:b:vBh", 
                              long_opts, NULL)) != -1) {
        switch (opt) {
        case 'f':
            g_config.fec_enabled = true;
            if (optarg) {
                if (strcmp(optarg, "xor") == 0) {
                    g_config.fec_type = FEC_TYPE_XOR;
                } else if (strcmp(optarg, "rs") == 0) {
                    g_config.fec_type = FEC_TYPE_RS_SIMPLE;
                } else if (strcmp(optarg, "rs-simd") == 0) {
                    g_config.fec_type = FEC_TYPE_RS_SIMD;
                } else {
                    g_config.fec_type = FEC_TYPE_AUTO;
                }
            }
            break;
            
        case 'F':
            sscanf(optarg, "%hhu:%hhu", 
                   &g_config.fec_data_shards,
                   &g_config.fec_parity_shards);
            break;
            
        case 'P':
            g_config.pacing_enabled = true;
            g_config.pacing_initial_bps = strtoull(optarg, NULL, 10) * 1000000;
            break;
            
        case 'R': {
            uint64_t min_mbps, max_mbps;
            sscanf(optarg, "%lu:%lu", &min_mbps, &max_mbps);
            g_config.pacing_min_bps = min_mbps * 1000000;
            g_config.pacing_max_bps = max_mbps * 1000000;
            break;
        }
            
        case 'A':
            if (strcmp(optarg, "https") == 0) {
                g_config.ad_profile = AD_PROFILE_HTTPS;
            } else if (strcmp(optarg, "video") == 0) {
                g_config.ad_profile = AD_PROFILE_VIDEO;
            } else if (strcmp(optarg, "voip") == 0) {
                g_config.ad_profile = AD_PROFILE_VOIP;
            } else if (strcmp(optarg, "gaming") == 0) {
                g_config.ad_profile = AD_PROFILE_GAMING;
            }
            break;
            
        case 'M':
            g_config.mtu = atoi(optarg);
            break;
            
        case 'p':
            g_config.port = atoi(optarg);
            break;
            
        case 'b':
            g_config.bind_addr = optarg;
            break;
            
        case 'v':
            g_config.verbose = true;
            break;
            
        case 'B':
            g_config.benchmark = true;
            break;
            
        case 'h':
        default:
            usage(argv[0]);
            exit(opt == 'h' ? 0 : 1);
        }
    }
}

// =========================================================
// 信号处理
// =========================================================
static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

// =========================================================
// 主函数
// =========================================================
int main(int argc, char **argv) {
    parse_args(argc, argv);
    
    if (g_config.benchmark) {
        run_benchmark();
        return 0;
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    init_modules();
    
    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║        v3 Server - Ultimate Optimized Edition                 ║\n");
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║  Port:        %-5u                                           ║\n", g_config.port);
    printf("║  MTU:         %-5u                                           ║\n", g_config.mtu);
    printf("╠═══════════════════════════════════════════════════════════════╣\n");
    printf("║  FEC:         %-5s", g_config.fec_enabled ? "ON" : "OFF");
    if (g_config.fec_enabled) {
        printf("  (%u:%u, %s)", 
               g_config.fec_data_shards, g_config.fec_parity_shards,
               fec_simd_available() ? "SIMD" : "Scalar");
    }
    printf("                          ║\n");
    printf("║  Pacing:      %-5s", g_config.pacing_enabled ? "ON" : "OFF");
    if (g_config.pacing_enabled) {
        printf("  (Adaptive, %lu-%lu Mbps)",
               g_config.pacing_min_bps / 1000000,
               g_config.pacing_max_bps / 1000000);
    }
    printf("              ║\n");
    printf("║  Anti-Detect: %-10s",
           g_config.ad_profile == AD_PROFILE_NONE ? "OFF" :
           g_config.ad_profile == AD_PROFILE_HTTPS ? "HTTPS" :
           g_config.ad_profile == AD_PROFILE_VIDEO ? "VIDEO" :
           g_config.ad_profile == AD_PROFILE_VOIP ? "VOIP" : "GAMING");
    if (g_config.ad_profile != AD_PROFILE_NONE) {
        printf("  (MTU-Aware, max %zu B)", ad_mtu_max_payload(&g_antidetect));
    }
    printf("            ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");
    
    printf("Server ready. Press Ctrl+C to stop.\n\n");
    
    // ... [此处添加 io_uring 主循环] ...
    
    while (g_running) {
        sleep(1);
    }
    
    // 清理
    if (g_fec) fec_destroy(g_fec);
    
    printf("\nShutdown complete.\n");
    return 0;
}

