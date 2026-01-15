// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include "v3_common.h"

// =========================================================
// 1. 配置常量 (内核态)
// =========================================================
#define BLACKLIST_THRESHOLD   100      // 失败次数超过此值则拉黑
#define RATE_LIMIT_PPS        10000    // 每个源 IP 每秒最大包数
#define RATE_WINDOW_NS        1000000000ULL  // 1秒 (纳秒)
#define DECAY_INTERVAL_NS     60000000000ULL // 60秒衰减周期

// =========================================================
// 2. BPF Maps (内核与用户态的共享内存)
// =========================================================

// Magic 表 (由用户态 loader 定时更新)
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 3);
    __type(key, __u32);
    __type(value, __u32);
} valid_magics SEC(".maps");

// 统计计数器 (Per-CPU, 高性能)
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, STAT_MAX);
    __type(key, __u32);
    __type(value, __u64);
} stats SEC(".maps");

// 黑名单 (LRU 哈希表，自动淘汰冷数据)
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 100000);
    __type(key, __u32);
    __type(value, struct blacklist_entry);
} blacklist SEC(".maps");

// 速率限制表
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 100000);
    __type(key, __u32);
    __type(value, struct rate_entry);
} rate_limit SEC(".maps");

// 连接缓存表 (快速路径)
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 50000);
    __type(key, __u64); // key = src_ip << 32 | src_port
    __type(value, struct conn_cache_entry);
} conn_cache SEC(".maps");

// =========================================================
// 3. 内联辅助函数 (高性能)
// =========================================================

static __always_inline void stats_increment(__u32 key) {
    __u64 *count = bpf_map_lookup_elem(&stats, &key);
    if (count) {
        __sync_fetch_and_add(count, 1);
    }
}

// =========================================================
// 4. XDP 主程序
// =========================================================
SEC("xdp")
int v3_filter(struct xdp_md *ctx) {
    void *data = (void *)(long)ctx->data;
    void *data_end = (void *)(long)ctx->data_end;
    __u64 now_ns = bpf_ktime_get_ns();

    stats_increment(STAT_TOTAL_PROCESSED);

    // --- L2: Ethernet ---
    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end || eth->h_proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    // --- L3: IP ---
    struct iphdr *ip = (void *)(eth + 1);
    if ((void *)(ip + 1) > data_end || ip->protocol != IPPROTO_UDP) {
        if (ip->protocol != IPPROTO_UDP) stats_increment(STAT_DROPPED_NOT_UDP);
        return XDP_PASS;
    }
    __u32 src_ip = ip->saddr;

    // --- L4: UDP ---
    struct udphdr *udp = (void *)ip + (ip->ihl * 4);
    if ((void *)(udp + 1) > data_end || udp->dest != bpf_htons(V3_PORT))
        return XDP_PASS;

    // --- Check 1: Blacklist (带衰减) ---
    struct blacklist_entry *bl_entry = bpf_map_lookup_elem(&blacklist, &src_ip);
    if (bl_entry) {
        __u64 decay_periods = (now_ns - bl_entry->last_fail_ns) / DECAY_INTERVAL_NS;
        if (decay_periods > 0) {
            bl_entry->fail_count >>= decay_periods; // 指数衰减
            bl_entry->last_fail_ns = now_ns;
        }
        if (bl_entry->fail_count >= BLACKLIST_THRESHOLD) {
            stats_increment(STAT_DROPPED_BLACKLIST);
            return XDP_DROP;
        }
    }

    // --- Check 2: Rate Limit ---
    struct rate_entry *rl_entry = bpf_map_lookup_elem(&rate_limit, &src_ip);
    if (!rl_entry) {
        struct rate_entry new_rl = {.window_start_ns = now_ns, .packet_count = 1};
        bpf_map_update_elem(&rate_limit, &src_ip, &new_rl, BPF_NOEXIST);
    } else {
        if (now_ns - rl_entry->window_start_ns < RATE_WINDOW_NS) {
            if (rl_entry->packet_count >= RATE_LIMIT_PPS) {
                stats_increment(STAT_DROPPED_RATELIMIT);
                return XDP_DROP;
            }
            __sync_fetch_and_add(&rl_entry->packet_count, 1);
        } else {
            rl_entry->window_start_ns = now_ns;
            rl_entry->packet_count = 1;
        }
    }

    // --- L7: v3 Protocol Header ---
    void *payload = (void *)(udp + 1);
    if (payload + sizeof(struct v3_header) > data_end) {
        stats_increment(STAT_DROPPED_TOO_SHORT);
        return XDP_DROP;
    }

    __u32 received_magic = ((struct v3_header *)payload)->magic_derived;
    __u64 conn_key = ((__u64)src_ip << 32) | bpf_ntohs(udp->source);

    // --- Check 3: Connection Cache (Fast Path) ---
    struct conn_cache_entry *cache = bpf_map_lookup_elem(&conn_cache, &conn_key);
    if (cache && cache->magic == received_magic) {
        cache->last_seen_ns = now_ns;
        stats_increment(STAT_PASSED);
        return XDP_PASS;
    }

    // --- Check 4: Full Magic Verification (Slow Path) ---
    int magic_valid = 0;
    #pragma unroll
    for (__u32 i = 0; i < 3; i++) {
        __u32 *valid = bpf_map_lookup_elem(&valid_magics, &i);
        if (valid && *valid == received_magic) {
            magic_valid = 1;
            break;
        }
    }

    if (!magic_valid) {
        if (bl_entry) {
            __sync_fetch_and_add(&bl_entry->fail_count, 1);
            bl_entry->last_fail_ns = now_ns;
        } else {
            struct blacklist_entry new_bl = {.fail_count = 1, .last_fail_ns = now_ns};
            bpf_map_update_elem(&blacklist, &src_ip, &new_bl, BPF_NOEXIST);
        }
        stats_increment(STAT_DROPPED_INVALID_MAGIC);
        return XDP_DROP;
    }

    // --- Success: Update Cache & Pass ---
    struct conn_cache_entry new_cache = {.last_seen_ns = now_ns, .magic = received_magic};
    bpf_map_update_elem(&conn_cache, &conn_key, &new_cache, BPF_ANY);
    stats_increment(STAT_PASSED);
    
    return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
