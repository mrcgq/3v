#!/bin/bash
# v3 Universal Installer - Production Ready

set -e

# =========================================================
# 1. 配置
# =========================================================
BASE_URL="https://github.com/mrcgq/3v/releases/download/v3"
INSTALL_PATH="/usr/local/bin/v3_server"
XDP_PATH="/usr/local/etc/v3_xdp.o"
SERVICE_NAME="v3-server"

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# =========================================================
# 2. 检查与准备
# =========================================================
check_root() {
    if [[ $EUID -ne 0 ]]; then
        log_error "This script must be run as root. Please use sudo."
        exit 1
    fi
}

cleanup_old() {
    if systemctl is-active --quiet $SERVICE_NAME; then
        log_info "Stopping existing v3 service..."
        systemctl stop $SERVICE_NAME
    fi
    rm -f $INSTALL_PATH $XDP_PATH
}

# =========================================================
# 3. 核心逻辑
# =========================================================
main() {
    check_root
    
    echo -e "${BLUE}╔═════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║          v3 Server Universal Installer          ║${NC}"
    echo -e "${BLUE}╚═════════════════════════════════════════════════╝${NC}"

    # 检测环境
    ARCH=$(uname -m)
    KERNEL=$(uname -r)
    MEM=$(free -m | awk '/^Mem:/{print $2}')
    echo "[Env] Arch: $ARCH | Kernel: $KERNEL | Mem: ${MEM}MB"

    # 智能推荐
    RECOMMEND="v3_server_lite"
    MAJOR_VER=$(echo $KERNEL | cut -d. -f1)
    if [[ "$ARCH" == "x86_64" ]] && [[ "$MEM" -gt 512 ]] && [[ "$MAJOR_VER" -ge 5 ]]; then
        RECOMMEND="v3_server_max"
    fi

    echo "---------------------------------------------------"
    echo "1) v3_server_max  (High Performance - io_uring + XDP)"
    echo "2) v3_server_lite (Compatibility - epoll)"
    echo "3) v3_server_wss  (Rescue - WebSocket + TLS)"
    echo "---------------------------------------------------"
    read -p "Enter choice (Default: recommended [$RECOMMEND]): " CHOICE

    local TARGET
    case "$CHOICE" in
        1) TARGET="v3_server_max" ;;
        2) TARGET="v3_server_lite" ;;
        3) TARGET="v3_server_wss" ;;
        "") TARGET="$RECOMMEND" ;;
        *) log_error "Invalid choice."; exit 1 ;;
    esac

    cleanup_old

    # [修复] 真正的下载逻辑
    log_info "Downloading $TARGET..."
    if ! curl -L -o "$INSTALL_PATH" "$BASE_URL/$TARGET"; then
        log_error "Download failed. Please check your network or the URL."
        exit 1
    fi
    chmod +x "$INSTALL_PATH"

    # 如果是 Max 版本，下载并加载 XDP
    if [[ "$TARGET" == "v3_server_max" ]]; then
        log_info "Downloading v3_xdp.o..."
        if curl -L -o "$XDP_PATH" "$BASE_URL/v3_xdp.o"; then
            chmod 644 "$XDP_PATH"
            
            # 尝试加载 XDP (需要 root)
            read -p "Enter your primary network interface (e.g., eth0): " IFACE
            if [[ -n "$IFACE" ]]; then
                log_info "Attaching XDP filter to $IFACE..."
                # 先卸载旧的
                ip link set dev "$IFACE" xdpgeneric off 2>/dev/null || true
                # 加载新的
                if ip link set dev "$IFACE" xdpgeneric obj "$XDP_PATH" sec xdp; then
                    log_info "XDP filter attached successfully."
                else
                    log_warn "Failed to attach XDP. Server will run without kernel-level protection."
                fi
            fi
        else
            log_warn "Failed to download v3_xdp.o. Skipping XDP setup."
        fi
    fi

    # 创建 systemd 服务
    log_info "Creating systemd service..."
    cat > /etc/systemd/system/$SERVICE_NAME.service <<EOF
[Unit]
Description=v3 Server ($TARGET)
After=network.target

[Service]
ExecStart=$INSTALL_PATH
Restart=always
LimitNOFILE=1000000
LimitMEMLOCK=infinity

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable $SERVICE_NAME
    systemctl start $SERVICE_NAME

    log_info "Waiting for service to start..."
    sleep 3

    if systemctl is-active --quiet $SERVICE_NAME; then
        echo -e "${GREEN}✅ Success! v3 is running.${NC}"
        echo "---------------------------------------------------"
        echo "To check status: systemctl status $SERVICE_NAME"
        echo "To view logs:    journalctl -u $SERVICE_NAME -f"
    else
        log_error "Service failed to start. Please check logs:"
        journalctl -u $SERVICE_NAME -n 20 --no-pager
    fi
}

main "$@"
