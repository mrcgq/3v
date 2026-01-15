
## 文件 2：v3_fec_simd.c


#define _GNU_SOURCE
#include "v3_fec_simd.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __x86_64__
#include <cpuid.h>
#include <immintrin.h>
#define HAVE_AVX2 1
#endif

#ifdef __aarch64__
#include <arm_neon.h>
#define HAVE_NEON 1
#endif

// =========================================================
// GF(2^8) 基础
// =========================================================
static uint8_t gf_exp[512];
static uint8_t gf_log[256];
static uint8_t gf_mul_table[256][256];  // 完整乘法表（空间换时间）
static int gf_initialized = 0;

static void gf_init(void) {
    if (gf_initialized) return;
    
    // 生成 exp/log 表
    int x = 1;
    for (int i = 0; i < 255; i++) {
        gf_exp[i] = x;
        gf_log[x] = i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11d;
    }
    for (int i = 255; i < 512; i++) {
        gf_exp[i] = gf_exp[i - 255];
    }
    gf_log[0] = 0;
    
    // 预计算完整乘法表
    for (int a = 0; a < 256; a++) {
        for (int b = 0; b < 256; b++) {
            if (a == 0 || b == 0) {
                gf_mul_table[a][b] = 0;
            } else {
                gf_mul_table[a][b] = gf_exp[gf_log[a] + gf_log[b]];
            }
        }
    }
    
    gf_initialized = 1;
}

// =========================================================
// CPU 能力检测
// =========================================================
bool fec_simd_available(void) {
#ifdef HAVE_AVX2
    unsigned int eax, ebx, ecx, edx;
    if (__get_cpuid(7, &eax, &ebx, &ecx, &edx)) {
        return (ebx & bit_AVX2) != 0;
    }
    return false;
#elif defined(HAVE_NEON)
    return true;  // ARM64 总是有 NEON
#else
    return false;
#endif
}

// =========================================================
// XOR FEC 实现（极简高速）
// =========================================================
typedef struct {
    uint32_t next_group_id;
    uint8_t  group_size;
    
    // 解码缓存
    struct {
        uint32_t group_id;
        uint8_t  shards[FEC_XOR_GROUP_SIZE + 1][FEC_SHARD_SIZE];
        bool     present[FEC_XOR_GROUP_SIZE + 1];
        size_t   shard_len;
        uint64_t create_time;
    } decode_cache[32];
    int cache_count;
} xor_fec_t;

static int xor_encode(xor_fec_t *ctx,
                      const uint8_t *data, size_t len,
                      uint8_t out[][FEC_SHARD_SIZE],
                      size_t out_lens[],
                      uint32_t *group_id) {
    uint8_t gs = ctx->group_size;
    *group_id = ctx->next_group_id++;
    
    // 分割数据
    size_t shard_size = (len + gs - 1) / gs;
    if (shard_size > FEC_SHARD_SIZE - 8) shard_size = FEC_SHARD_SIZE - 8;
    
    // Header: group_id(4) + shard_idx(1) + group_size(1) + shard_len(2)
    for (int i = 0; i < gs; i++) {
        out[i][0] = (*group_id >> 24) & 0xFF;
        out[i][1] = (*group_id >> 16) & 0xFF;
        out[i][2] = (*group_id >> 8) & 0xFF;
        out[i][3] = *group_id & 0xFF;
        out[i][4] = i;
        out[i][5] = gs;
        out[i][6] = (shard_size >> 8) & 0xFF;
        out[i][7] = shard_size & 0xFF;
        
        size_t offset = i * shard_size;
        size_t copy_len = (offset + shard_size <= len) ? shard_size : 
                          (offset < len ? len - offset : 0);
        if (copy_len > 0) {
            memcpy(out[i] + 8, data + offset, copy_len);
        }
        if (copy_len < shard_size) {
            memset(out[i] + 8 + copy_len, 0, shard_size - copy_len);
        }
        out_lens[i] = shard_size + 8;
    }
    
    // XOR 校验分片
    out[gs][0] = (*group_id >> 24) & 0xFF;
    out[gs][1] = (*group_id >> 16) & 0xFF;
    out[gs][2] = (*group_id >> 8) & 0xFF;
    out[gs][3] = *group_id & 0xFF;
    out[gs][4] = gs;  // parity shard index
    out[gs][5] = gs;
    out[gs][6] = (shard_size >> 8) & 0xFF;
    out[gs][7] = shard_size & 0xFF;
    
    // XOR 所有数据分片
    memset(out[gs] + 8, 0, shard_size);
    for (int i = 0; i < gs; i++) {
        for (size_t j = 0; j < shard_size; j++) {
            out[gs][8 + j] ^= out[i][8 + j];
        }
    }
    out_lens[gs] = shard_size + 8;
    
    return gs + 1;
}

static int xor_decode(xor_fec_t *ctx,
                      uint32_t group_id,
                      uint8_t shard_idx,
                      const uint8_t *data, size_t len,
                      uint8_t *out_data, size_t *out_len) {
    if (len < 8) return -1;
    
    uint8_t gs = data[5];
    size_t shard_size = (data[6] << 8) | data[7];
    
    // 查找或创建缓存
    int cache_idx = -1;
    for (int i = 0; i < ctx->cache_count; i++) {
        if (ctx->decode_cache[i].group_id == group_id) {
            cache_idx = i;
            break;
        }
    }
    
    if (cache_idx < 0) {
        if (ctx->cache_count >= 32) {
            // 淘汰最旧的
            memmove(&ctx->decode_cache[0], &ctx->decode_cache[1], 
                    31 * sizeof(ctx->decode_cache[0]));
            ctx->cache_count = 31;
        }
        cache_idx = ctx->cache_count++;
        memset(&ctx->decode_cache[cache_idx], 0, sizeof(ctx->decode_cache[0]));
        ctx->decode_cache[cache_idx].group_id = group_id;
        ctx->decode_cache[cache_idx].shard_len = shard_size;
    }
    
    // 保存分片
    if (shard_idx <= gs) {
        memcpy(ctx->decode_cache[cache_idx].shards[shard_idx], data + 8, shard_size);
        ctx->decode_cache[cache_idx].present[shard_idx] = true;
    }
    
    // 检查是否能恢复
    int present_count = 0;
    int missing_idx = -1;
    for (int i = 0; i <= gs; i++) {
        if (ctx->decode_cache[cache_idx].present[i]) {
            present_count++;
        } else {
            missing_idx = i;
        }
    }
    
    // XOR FEC 只能恢复 1 个丢失
    if (present_count < gs) {
        return 0;  // 继续等待
    }
    
    if (present_count == gs && missing_idx >= 0 && missing_idx < gs) {
        // 恢复丢失的数据分片
        memset(ctx->decode_cache[cache_idx].shards[missing_idx], 0, shard_size);
        for (int i = 0; i <= gs; i++) {
            if (i != missing_idx && ctx->decode_cache[cache_idx].present[i]) {
                for (size_t j = 0; j < shard_size; j++) {
                    ctx->decode_cache[cache_idx].shards[missing_idx][j] ^= 
                        ctx->decode_cache[cache_idx].shards[i][j];
                }
            }
        }
        ctx->decode_cache[cache_idx].present[missing_idx] = true;
    }
    
    // 拼接数据
    *out_len = 0;
    for (int i = 0; i < gs; i++) {
        memcpy(out_data + *out_len, ctx->decode_cache[cache_idx].shards[i], shard_size);
        *out_len += shard_size;
    }
    
    // 清理缓存
    ctx->decode_cache[cache_idx].group_id = 0;
    
    return 1;
}

// =========================================================
// RS SIMD 实现
// =========================================================
#ifdef HAVE_AVX2

// AVX2 加速的 GF 乘法（一次处理 32 字节）
static inline __m256i gf_mul_avx2(__m256i a, uint8_t b) {
    if (b == 0) return _mm256_setzero_si256();
    if (b == 1) return a;
    
    __m256i result = _mm256_setzero_si256();
    __m256i log_b = _mm256_set1_epi8(gf_log[b]);
    
    // 对每个字节进行 GF 乘法
    // 这里使用查表法的 SIMD 版本
    uint8_t temp_a[32], temp_r[32];
    _mm256_storeu_si256((__m256i*)temp_a, a);
    
    for (int i = 0; i < 32; i++) {
        temp_r[i] = gf_mul_table[temp_a[i]][b];
    }
    
    return _mm256_loadu_si256((__m256i*)temp_r);
}

static void rs_encode_avx2(const uint8_t data[][FEC_SHARD_SIZE],
                           int data_count,
                           uint8_t parity[][FEC_SHARD_SIZE],
                           int parity_count,
                           int shard_size) {
    gf_init();
    
    // 生成 Vandermonde 矩阵行
    uint8_t matrix[FEC_MAX_PARITY_SHARDS][FEC_MAX_DATA_SHARDS];
    for (int p = 0; p < parity_count; p++) {
        uint8_t x = data_count + p + 1;
        matrix[p][0] = 1;
        for (int j = 1; j < data_count; j++) {
            matrix[p][j] = gf_mul_table[matrix[p][j-1]][x];
        }
    }
    
    // 使用 AVX2 计算 parity
    for (int p = 0; p < parity_count; p++) {
        memset(parity[p], 0, shard_size);
        
        for (int d = 0; d < data_count; d++) {
            uint8_t coef = matrix[p][d];
            if (coef == 0) continue;
            
            int i = 0;
            // AVX2 处理（32 字节一组）
            for (; i + 32 <= shard_size; i += 32) {
                __m256i src = _mm256_loadu_si256((__m256i*)(data[d] + i));
                __m256i dst = _mm256_loadu_si256((__m256i*)(parity[p] + i));
                __m256i mul = gf_mul_avx2(src, coef);
                dst = _mm256_xor_si256(dst, mul);
                _mm256_storeu_si256((__m256i*)(parity[p] + i), dst);
            }
            // 处理剩余
            for (; i < shard_size; i++) {
                parity[p][i] ^= gf_mul_table[data[d][i]][coef];
            }
        }
    }
}

#endif // HAVE_AVX2

#ifdef HAVE_NEON

// NEON 加速版本
static void rs_encode_neon(const uint8_t data[][FEC_SHARD_SIZE],
                           int data_count,
                           uint8_t parity[][FEC_SHARD_SIZE],
                           int parity_count,
                           int shard_size) {
    gf_init();
    
    uint8_t matrix[FEC_MAX_PARITY_SHARDS][FEC_MAX_DATA_SHARDS];
    for (int p = 0; p < parity_count; p++) {
        uint8_t x = data_count + p + 1;
        matrix[p][0] = 1;
        for (int j = 1; j < data_count; j++) {
            matrix[p][j] = gf_mul_table[matrix[p][j-1]][x];
        }
    }
    
    for (int p = 0; p < parity_count; p++) {
        memset(parity[p], 0, shard_size);
        
        for (int d = 0; d < data_count; d++) {
            uint8_t coef = matrix[p][d];
            if (coef == 0) continue;
            
            int i = 0;
            // NEON 处理（16 字节一组）
            for (; i + 16 <= shard_size; i += 16) {
                uint8x16_t src = vld1q_u8(data[d] + i);
                uint8x16_t dst = vld1q_u8(parity[p] + i);
                
                // NEON GF 乘法（使用查表）
                uint8_t temp_src[16], temp_mul[16];
                vst1q_u8(temp_src, src);
                for (int k = 0; k < 16; k++) {
                    temp_mul[k] = gf_mul_table[temp_src[k]][coef];
                }
                uint8x16_t mul = vld1q_u8(temp_mul);
                
                dst = veorq_u8(dst, mul);
                vst1q_u8(parity[p] + i, dst);
            }
            for (; i < shard_size; i++) {
                parity[p][i] ^= gf_mul_table[data[d][i]][coef];
            }
        }
    }
}

#endif // HAVE_NEON

// =========================================================
// RS 简单实现（无 SIMD）
// =========================================================
static void rs_encode_simple(const uint8_t data[][FEC_SHARD_SIZE],
                             int data_count,
                             uint8_t parity[][FEC_SHARD_SIZE],
                             int parity_count,
                             int shard_size) {
    gf_init();
    
    uint8_t matrix[FEC_MAX_PARITY_SHARDS][FEC_MAX_DATA_SHARDS];
    for (int p = 0; p < parity_count; p++) {
        uint8_t x = data_count + p + 1;
        matrix[p][0] = 1;
        for (int j = 1; j < data_count; j++) {
            matrix[p][j] = gf_mul_table[matrix[p][j-1]][x];
        }
    }
    
    for (int p = 0; p < parity_count; p++) {
        memset(parity[p], 0, shard_size);
        for (int byte = 0; byte < shard_size; byte++) {
            for (int d = 0; d < data_count; d++) {
                parity[p][byte] ^= gf_mul_table[data[d][byte]][matrix[p][d]];
            }
        }
    }
}

// =========================================================
// RS 解码（高斯消元）
// =========================================================
static int rs_decode_common(uint8_t shards[][FEC_SHARD_SIZE],
                            bool *present,
                            int data_count,
                            int total_count,
                            int shard_size) {
    gf_init();
    
    int available = 0;
    for (int i = 0; i < total_count; i++) {
        if (present[i]) available++;
    }
    if (available < data_count) return -1;
    
    // 构建矩阵
    uint8_t matrix[FEC_MAX_DATA_SHARDS][FEC_MAX_DATA_SHARDS];
    uint8_t *shard_ptrs[FEC_MAX_DATA_SHARDS];
    
    int idx = 0;
    for (int i = 0; i < total_count && idx < data_count; i++) {
        if (present[i]) {
            // Vandermonde 行
            uint8_t x = i + 1;
            matrix[idx][0] = 1;
            for (int j = 1; j < data_count; j++) {
                matrix[idx][j] = gf_mul_table[matrix[idx][j-1]][x];
            }
            shard_ptrs[idx] = shards[i];
            idx++;
        }
    }
    
    // 高斯消元
    uint8_t inv[FEC_MAX_DATA_SHARDS][FEC_MAX_DATA_SHARDS];
    memset(inv, 0, sizeof(inv));
    for (int i = 0; i < data_count; i++) inv[i][i] = 1;
    
    for (int col = 0; col < data_count; col++) {
        // 找主元
        int pivot = -1;
        for (int row = col; row < data_count; row++) {
            if (matrix[row][col] != 0) {
                pivot = row;
                break;
            }
        }
        if (pivot < 0) return -1;
        
        // 交换
        if (pivot != col) {
            for (int j = 0; j < data_count; j++) {
                uint8_t t = matrix[col][j]; 
                matrix[col][j] = matrix[pivot][j]; 
                matrix[pivot][j] = t;
                t = inv[col][j]; 
                inv[col][j] = inv[pivot][j]; 
                inv[pivot][j] = t;
            }
            uint8_t *t = shard_ptrs[col];
            shard_ptrs[col] = shard_ptrs[pivot];
            shard_ptrs[pivot] = t;
        }
        
        // 归一化
        uint8_t scale = gf_exp[255 - gf_log[matrix[col][col]]];
        for (int j = 0; j < data_count; j++) {
            matrix[col][j] = gf_mul_table[matrix[col][j]][scale];
            inv[col][j] = gf_mul_table[inv[col][j]][scale];
        }
        
        // 消元
        for (int row = 0; row < data_count; row++) {
            if (row != col && matrix[row][col] != 0) {
                uint8_t factor = matrix[row][col];
                for (int j = 0; j < data_count; j++) {
                    matrix[row][j] ^= gf_mul_table[matrix[col][j]][factor];
                    inv[row][j] ^= gf_mul_table[inv[col][j]][factor];
                }
            }
        }
    }
    
    // 恢复丢失分片
    for (int i = 0; i < data_count; i++) {
        if (!present[i]) {
            memset(shards[i], 0, shard_size);
            for (int byte = 0; byte < shard_size; byte++) {
                for (int j = 0; j < data_count; j++) {
                    shards[i][byte] ^= gf_mul_table[shard_ptrs[j][byte]][inv[i][j]];
                }
            }
            present[i] = true;
        }
    }
    
    return 0;
}

// =========================================================
// 统一 FEC 引擎
// =========================================================
struct fec_engine_s {
    fec_type_t type;
    uint8_t    data_shards;
    uint8_t    parity_shards;
    float      loss_rate;
    uint32_t   next_group_id;
    
    union {
        xor_fec_t xor_ctx;
        struct {
            struct {
                uint32_t group_id;
                uint8_t  shards[FEC_MAX_TOTAL_SHARDS][FEC_SHARD_SIZE];
                bool     present[FEC_MAX_TOTAL_SHARDS];
                size_t   shard_size;
                uint8_t  data_count;
                uint8_t  parity_count;
            } cache[64];
            int cache_count;
        } rs_ctx;
    };
};

fec_engine_t* fec_create(fec_type_t type, uint8_t data_shards, uint8_t parity_shards) {
    fec_engine_t *e = calloc(1, sizeof(fec_engine_t));
    if (!e) return NULL;
    
    // 自动选择
    if (type == FEC_TYPE_AUTO) {
        if (fec_simd_available()) {
            type = FEC_TYPE_RS_SIMD;
        } else if (data_shards <= 4 && parity_shards == 1) {
            type = FEC_TYPE_XOR;
        } else {
            type = FEC_TYPE_RS_SIMPLE;
        }
    }
    
    e->type = type;
    e->data_shards = data_shards > 0 ? data_shards : 5;
    e->parity_shards = parity_shards > 0 ? parity_shards : 2;
    
    if (type == FEC_TYPE_XOR) {
        e->xor_ctx.group_size = e->data_shards;
    }
    
    gf_init();
    
    return e;
}

void fec_destroy(fec_engine_t *e) {
    if (e) free(e);
}

int fec_encode(fec_engine_t *e,
               const uint8_t *data, size_t len,
               uint8_t out_shards[][FEC_SHARD_SIZE],
               size_t out_lens[],
               uint32_t *group_id) {
    
    if (e->type == FEC_TYPE_XOR) {
        return xor_encode(&e->xor_ctx, data, len, out_shards, out_lens, group_id);
    }
    
    // RS 编码
    *group_id = e->next_group_id++;
    
    uint8_t ds = e->data_shards;
    uint8_t ps = e->parity_shards;
    
    size_t shard_size = (len + ds - 1) / ds;
    if (shard_size > FEC_SHARD_SIZE - 8) shard_size = FEC_SHARD_SIZE - 8;
    
    // 分割数据
    uint8_t data_buf[FEC_MAX_DATA_SHARDS][FEC_SHARD_SIZE];
    memset(data_buf, 0, sizeof(data_buf));
    
    size_t offset = 0;
    for (int i = 0; i < ds && offset < len; i++) {
        size_t copy = (len - offset > shard_size) ? shard_size : (len - offset);
        memcpy(data_buf[i], data + offset, copy);
        offset += copy;
    }
    
    // 生成校验
    uint8_t parity_buf[FEC_MAX_PARITY_SHARDS][FEC_SHARD_SIZE];
    
#ifdef HAVE_AVX2
    if (e->type == FEC_TYPE_RS_SIMD) {
        rs_encode_avx2(data_buf, ds, parity_buf, ps, shard_size);
    } else
#endif
#ifdef HAVE_NEON
    if (e->type == FEC_TYPE_RS_SIMD) {
        rs_encode_neon(data_buf, ds, parity_buf, ps, shard_size);
    } else
#endif
    {
        rs_encode_simple(data_buf, ds, parity_buf, ps, shard_size);
    }
    
    // 打包输出
    int total = ds + ps;
    for (int i = 0; i < ds; i++) {
        out_shards[i][0] = (*group_id >> 24) & 0xFF;
        out_shards[i][1] = (*group_id >> 16) & 0xFF;
        out_shards[i][2] = (*group_id >> 8) & 0xFF;
        out_shards[i][3] = *group_id & 0xFF;
        out_shards[i][4] = i;
        out_shards[i][5] = ds;
        out_shards[i][6] = ps;
        out_shards[i][7] = (shard_size >> 4) & 0xFF;
        memcpy(out_shards[i] + 8, data_buf[i], shard_size);
        out_lens[i] = shard_size + 8;
    }
    
    for (int i = 0; i < ps; i++) {
        int idx = ds + i;
        out_shards[idx][0] = (*group_id >> 24) & 0xFF;
        out_shards[idx][1] = (*group_id >> 16) & 0xFF;
        out_shards[idx][2] = (*group_id >> 8) & 0xFF;
        out_shards[idx][3] = *group_id & 0xFF;
        out_shards[idx][4] = idx;
        out_shards[idx][5] = ds;
        out_shards[idx][6] = ps;
        out_shards[idx][7] = (shard_size >> 4) & 0xFF;
        memcpy(out_shards[idx] + 8, parity_buf[i], shard_size);
        out_lens[idx] = shard_size + 8;
    }
    
    return total;
}

int fec_decode(fec_engine_t *e,
               uint32_t group_id,
               uint8_t shard_idx,
               const uint8_t *shard_data, size_t shard_len,
               uint8_t *out_data, size_t *out_len) {
    
    if (shard_len < 8) return -1;
    
    if (e->type == FEC_TYPE_XOR) {
        return xor_decode(&e->xor_ctx, group_id, shard_idx, 
                          shard_data, shard_len, out_data, out_len);
    }
    
    // RS 解码
    uint8_t ds = shard_data[5];
    uint8_t ps = shard_data[6];
    size_t shard_size = shard_data[7] << 4;
    int total = ds + ps;
    
    // 查找缓存
    int cache_idx = -1;
    for (int i = 0; i < e->rs_ctx.cache_count; i++) {
        if (e->rs_ctx.cache[i].group_id == group_id) {
            cache_idx = i;
            break;
        }
    }
    
    if (cache_idx < 0) {
        if (e->rs_ctx.cache_count >= 64) {
            memmove(&e->rs_ctx.cache[0], &e->rs_ctx.cache[1],
                    63 * sizeof(e->rs_ctx.cache[0]));
            e->rs_ctx.cache_count = 63;
        }
        cache_idx = e->rs_ctx.cache_count++;
        memset(&e->rs_ctx.cache[cache_idx], 0, sizeof(e->rs_ctx.cache[0]));
        e->rs_ctx.cache[cache_idx].group_id = group_id;
        e->rs_ctx.cache[cache_idx].shard_size = shard_size;
        e->rs_ctx.cache[cache_idx].data_count = ds;
        e->rs_ctx.cache[cache_idx].parity_count = ps;
    }
    
    // 保存分片
    if (shard_idx < total) {
        memcpy(e->rs_ctx.cache[cache_idx].shards[shard_idx], 
               shard_data + 8, shard_size);
        e->rs_ctx.cache[cache_idx].present[shard_idx] = true;
    }
    
    // 检查
    int present_count = 0;
    for (int i = 0; i < total; i++) {
        if (e->rs_ctx.cache[cache_idx].present[i]) present_count++;
    }
    
    if (present_count < ds) return 0;
    
    // 恢复
    if (rs_decode_common(e->rs_ctx.cache[cache_idx].shards,
                         e->rs_ctx.cache[cache_idx].present,
                         ds, total, shard_size) < 0) {
        return -1;
    }
    
    // 拼接
    *out_len = 0;
    for (int i = 0; i < ds; i++) {
        memcpy(out_data + *out_len, e->rs_ctx.cache[cache_idx].shards[i], shard_size);
        *out_len += shard_size;
    }
    
    e->rs_ctx.cache[cache_idx].group_id = 0;
    return 1;
}

void fec_set_loss_rate(fec_engine_t *e, float loss_rate) {
    e->loss_rate = loss_rate;
    
    if (e->type == FEC_TYPE_XOR) {
        // XOR 不支持动态调整
        return;
    }
    
    // RS 动态调整
    if (loss_rate < 0.05f) {
        e->parity_shards = 2;
    } else if (loss_rate < 0.10f) {
        e->parity_shards = 3;
    } else if (loss_rate < 0.20f) {
        e->parity_shards = 4;
    } else if (loss_rate < 0.30f) {
        e->parity_shards = 5;
    } else {
        e->parity_shards = e->data_shards;  // 最大 100% 冗余
    }
}

fec_type_t fec_get_type(fec_engine_t *e) {
    return e->type;
}

// =========================================================
// 基准测试
// =========================================================
double fec_benchmark(fec_type_t type, size_t data_size, int iterations) {
    fec_engine_t *e = fec_create(type, 5, 2);
    if (!e) return -1;
    
    uint8_t *data = malloc(data_size);
    uint8_t shards[FEC_MAX_TOTAL_SHARDS][FEC_SHARD_SIZE];
    size_t lens[FEC_MAX_TOTAL_SHARDS];
    uint32_t gid;
    
    for (size_t i = 0; i < data_size; i++) {
        data[i] = (uint8_t)(i & 0xFF);
    }
    
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    for (int i = 0; i < iterations; i++) {
        fec_encode(e, data, data_size, shards, lens, &gid);
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end);
    
    double elapsed = (end.tv_sec - start.tv_sec) + 
                     (end.tv_nsec - start.tv_nsec) / 1e9;
    double throughput = (data_size * iterations) / elapsed / (1024 * 1024);
    
    free(data);
    fec_destroy(e);
    
    return throughput;  // MB/s
}
