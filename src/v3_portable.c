/*
 * v3 Server Lite (Portable Edition)
 * 
 * [特性]
 * - 单文件无依赖 (Zero Dependency)
 * - 内嵌 ChaCha20-Poly1305
 * - epoll 后端 (兼容老内核)
 * - 静态编译体积 < 500KB
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define V3_PORT 51820
#define BUF_SIZE 2048
#define MAX_EVENTS 64

static uint8_t g_key[32] = { /* ... 32 bytes key ... */ 0x01 };

// --- 1. 内嵌加密 (ChaCha20) ---
// (此处填入之前提供的 chacha20_block, chacha20_xor, poly1305 实现)
// 为了节省篇幅，这里用占位符，实际编译时需完整填入
void chacha20_xor(uint8_t *out, const uint8_t *in, size_t len, const uint8_t *k, const uint8_t *n, uint32_t c) {
    // ... Implementation ...
}
void aead_decrypt_mock(const uint8_t *in, size_t len, uint8_t *out) {
    // 实际应包含完整 AEAD 解密
    memcpy(out, in, len); 
}

// --- 2. 协议头 ---
#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint8_t  nonce[12];
    uint8_t  enc[16];
    uint8_t  tag[16];
    uint16_t len;
    uint16_t pad;
} header_t;
#pragma pack(pop)

// --- 3. 核心逻辑 ---
int main(int argc, char **argv) {
    int port = V3_PORT;
    if (argc > 1) port = atoi(argv[1]);

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = 0, .sin_port = htons(port) };
    bind(udp_fd, (struct sockaddr*)&addr, sizeof(addr));
    
    int epfd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = udp_fd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, udp_fd, &ev);
    
    printf("v3 Lite running on %d\n", port);
    
    struct epoll_event events[MAX_EVENTS];
    uint8_t buf[BUF_SIZE];
    
    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == udp_fd) {
                struct sockaddr_in client;
                socklen_t clen = sizeof(client);
                int n = recvfrom(udp_fd, buf, BUF_SIZE, 0, (struct sockaddr*)&client, &clen);
                
                if (n > (int)sizeof(header_t)) {
                    // 解密、验证、转发逻辑
                    // ... 
                    // 简单 Echo 演示连通性
                    // header_t *h = (header_t*)buf;
                    // decrypt...
                    // sendto(udp_fd, buf, n, 0, (struct sockaddr*)&client, clen);
                }
            }
        }
    }
    return 0;
}
