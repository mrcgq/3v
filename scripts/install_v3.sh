#!/bin/bash
# v3 Universal Installer

# 你的 GitHub 仓库 Release 或 Artifact 下载链接前缀
# 实际使用时建议配合 GitHub Releases 功能
BASE_URL="https://github.com/mrcgq/3v/releases/download/v3/"


echo "╔═════════════════════════════════════════════════╗"
echo "║          v3 Server Universal Installer          ║"
echo "╚═════════════════════════════════════════════════╝"

# 1. 检测环境
ARCH=$(uname -m)
KERNEL=$(uname -r)
MEM=$(free -m | awk '/^Mem:/{print $2}')

echo "[Env] Arch: $ARCH | Kernel: $KERNEL | Mem: ${MEM}MB"

# 2. 智能推荐逻辑
RECOMMEND="v3_server_lite"

# 如果是 x86_64 且内存 > 512MB 且内核 > 5.10
if [[ "$ARCH" == "x86_64" ]] && [[ "$MEM" -gt 512 ]]; then
    # 简单检查内核版本 (主要看主版本号)
    MAJOR_VER=$(echo $KERNEL | cut -d. -f1)
    MINOR_VER=$(echo $KERNEL | cut -d. -f2)
    if [[ "$MAJOR_VER" -ge 5 ]] && [[ "$MINOR_VER" -ge 10 ]]; then
        RECOMMEND="v3_server_max"
    fi
fi

echo "---------------------------------------------------"
echo "Select Version to Install:"
echo "1) v3 Max  (Enterprise/Ultimate) - [High Performance]"
echo "   Requires: Linux 5.10+, >512MB RAM. Features: io_uring, FEC, Anti-QoS"
echo ""
echo "2) v3 Lite (Portable)            - [Compatibility]"
echo "   Requires: Any Linux. Features: Low RAM, Zero Dependency"
echo ""
echo "3) v3 WSS  (Rescue)              - [Anti-Censorship]"
echo "   Requires: Domain, TCP 443. Features: WebSocket+TLS, CDN Support"
echo "---------------------------------------------------"
read -p "Enter choice (Default: $RECOMMEND): " CHOICE

if [[ -z "$CHOICE" ]]; then
    TARGET=$RECOMMEND
elif [[ "$CHOICE" == "1" ]]; then
    TARGET="v3_server_max"
elif [[ "$CHOICE" == "2" ]]; then
    TARGET="v3_server_lite"
elif [[ "$CHOICE" == "3" ]]; then
    TARGET="v3_server_wss"
else
    echo "Invalid choice."
    exit 1
fi

echo "[Install] Downloading $TARGET..."

# 模拟下载 (实际需替换为真实 URL)
# curl -L -o /usr/local/bin/v3_server "$BASE_URL/$TARGET"
# 对于 Artifacts，通常是一个 zip 包，这里为了演示逻辑简化

# 假设文件已经通过某种方式传到了本地，或者 wget 下来了
if [[ ! -f "$TARGET" ]]; then
    echo "Error: Binary not found (Did you setup GitHub Releases?)"
    # 这里是一个占位符，实际你要配置 Release
    exit 1
fi

cp "$TARGET" /usr/local/bin/v3_server
chmod +x /usr/local/bin/v3_server

# 3. 创建 systemd 服务
echo "[Install] Creating Systemd Service..."

cat > /etc/systemd/system/v3.service <<EOF
[Unit]
Description=v3 Server ($TARGET)
After=network.target

[Service]
ExecStart=/usr/local/bin/v3_server
Restart=always
LimitNOFILE=1000000

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable v3
systemctl restart v3

echo "✅ Success! v3 is running."
