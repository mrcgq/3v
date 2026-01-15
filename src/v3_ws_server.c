/*
 * v3 Server WSS (Rescue Edition)
 * 
 * [功能]
 * - 监听 TCP 443
 * - 处理 TLS 握手 (OpenSSL)
 * - 处理 WebSocket Upgrade
 * - 解包 WS Frame -> 转发给本地 UDP 51820
 * 
 * [编译] gcc -O3 -o v3_server_wss v3_ws_server.c -lssl -lcrypto
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#define WS_PORT 443
#define V3_LOCAL_PORT 51820

void handle_client(int client_fd, SSL_CTX *ctx) {
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, client_fd);
    
    if (SSL_accept(ssl) <= 0) {
        ERR_print_errors_fp(stderr);
    } else {
        char buf[4096];
        int n = SSL_read(ssl, buf, sizeof(buf)-1);
        buf[n] = 0;
        
        // 简单的 WS 握手识别
        if (strstr(buf, "Upgrade: websocket")) {
            char *key_ptr = strstr(buf, "Sec-WebSocket-Key: ");
            // ... 计算 Accept Key (省略 SHA1/Base64 细节) ...
            char *accept_key = "MOCK_KEY"; 
            
            char resp[512];
            sprintf(resp, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept_key);
            SSL_write(ssl, resp, strlen(resp));
            
            // 隧道建立：SSL <-> Local UDP
            int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in v3_addr = {
                .sin_family = AF_INET,
                .sin_port = htons(V3_LOCAL_PORT),
                .sin_addr.s_addr = inet_addr("127.0.0.1")
            };
            
            while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) {
                // 解 WebSocket Frame (去掉头部的 Mask 等)
                // 发送给本地 v3
                sendto(udp_fd, buf, n, 0, (struct sockaddr*)&v3_addr, sizeof(v3_addr));
                
                // 接收 v3 回包 -> 封包 WS -> SSL_write
                // ...
            }
            close(udp_fd);
        }
    }
    
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(client_fd);
}

int main(int argc, char **argv) {
    // OpenSSL Init
    SSL_library_init();
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    // 加载证书 (生产环境需替换路径)
    SSL_CTX_use_certificate_file(ctx, "server.crt", SSL_FILETYPE_PEM);
    SSL_CTX_use_PrivateKey_file(ctx, "server.key", SSL_FILETYPE_PEM);
    
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(WS_PORT), .sin_addr.s_addr = 0 };
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 10);
    
    printf("v3 Rescue WSS listening on %d\n", WS_PORT);
    
    while(1) {
        int client = accept(listen_fd, NULL, NULL);
        if (fork() == 0) {
            close(listen_fd);
            handle_client(client, ctx);
            exit(0);
        }
        close(client);
    }
}
