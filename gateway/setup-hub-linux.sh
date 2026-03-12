#!/bin/bash
# ═══════════════════════════════════════════════════════════════
# LoRa Mesh Hub — Linux Setup Script
# ═══════════════════════════════════════════════════════════════
# Run as your normal user (not root). Uses sudo where needed.
# What this does:
#   1. Installs Python dependencies
#   2. Installs Tailscale
#   3. Creates a systemd service so the hub auto-starts on boot
#   4. Optionally enables Tailscale Funnel for public access
# ═══════════════════════════════════════════════════════════════

set -e

CYAN='\033[0;36m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HUB_PORT=9000
SERVICE_NAME="lora-mesh-hub"
SERVICE_USER="$USER"

echo -e "${CYAN}"
echo "╔══════════════════════════════════════════╗"
echo "║     LoRa Mesh Hub — Linux Setup          ║"
echo "╚══════════════════════════════════════════╝"
echo -e "${NC}"

# ─── Gather config ───────────────────────────────────────────
echo -e "${YELLOW}Hub configuration:${NC}"
read -p "  Hub port [9000]: " INPUT_PORT
HUB_PORT="${INPUT_PORT:-9000}"

read -p "  Auth key (leave blank for no auth): " HUB_KEY

read -p "  Install Tailscale? [Y/n]: " INSTALL_TS
INSTALL_TS="${INSTALL_TS:-Y}"

echo ""

# ─── 1. Python dependencies ──────────────────────────────────
echo -e "${CYAN}[1/4] Installing Python dependencies...${NC}"
if ! command -v python3 &>/dev/null; then
    sudo apt-get update -q && sudo apt-get install -y python3 python3-pip
fi
pip3 install --user websockets

echo -e "${GREEN}      ✓ Python dependencies installed${NC}"

# ─── 2. Tailscale ────────────────────────────────────────────
if [[ "$INSTALL_TS" =~ ^[Yy] ]]; then
    echo -e "${CYAN}[2/4] Installing Tailscale...${NC}"
    if command -v tailscale &>/dev/null; then
        echo -e "${GREEN}      ✓ Tailscale already installed${NC}"
    else
        curl -fsSL https://tailscale.com/install.sh | sh
        echo -e "${GREEN}      ✓ Tailscale installed${NC}"
    fi

    echo ""
    echo -e "${YELLOW}  Connecting to Tailscale (browser will open or show a URL to visit)...${NC}"
    sudo tailscale up
    echo -e "${GREEN}      ✓ Tailscale connected${NC}"
    TAILSCALE_IP=$(tailscale ip -4 2>/dev/null || echo "unknown")
    echo -e "      Your Tailscale IP: ${CYAN}${TAILSCALE_IP}${NC}"
else
    echo -e "${YELLOW}[2/4] Skipping Tailscale${NC}"
fi

# ─── 3. systemd service ──────────────────────────────────────
echo -e "${CYAN}[3/4] Creating systemd service (auto-start on boot)...${NC}"

PYTHON_PATH=$(which python3)
HUB_SCRIPT="$SCRIPT_DIR/gateway_hub.py"

# Build ExecStart with optional key
if [ -n "$HUB_KEY" ]; then
    EXEC_START="$PYTHON_PATH $HUB_SCRIPT --listen $HUB_PORT --key $HUB_KEY"
else
    EXEC_START="$PYTHON_PATH $HUB_SCRIPT --listen $HUB_PORT"
fi

sudo tee /etc/systemd/system/${SERVICE_NAME}.service > /dev/null <<EOF
[Unit]
Description=LoRa Mesh Gateway Hub
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=${SERVICE_USER}
WorkingDirectory=${SCRIPT_DIR}
ExecStart=${EXEC_START}
Restart=always
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable ${SERVICE_NAME}
sudo systemctl restart ${SERVICE_NAME}

sleep 2
if sudo systemctl is-active --quiet ${SERVICE_NAME}; then
    echo -e "${GREEN}      ✓ Hub service running and enabled on boot${NC}"
else
    echo -e "${RED}      ✗ Service failed to start — check: sudo journalctl -u ${SERVICE_NAME} -n 30${NC}"
fi

# ─── 4. Tailscale Funnel (public URL) ────────────────────────
if [[ "$INSTALL_TS" =~ ^[Yy] ]]; then
    echo ""
    read -p "$(echo -e ${YELLOW})[4/4] Enable Tailscale Funnel (public wss:// URL for gateways)? [Y/n]: $(echo -e ${NC})" ENABLE_FUNNEL
    ENABLE_FUNNEL="${ENABLE_FUNNEL:-Y}"

    if [[ "$ENABLE_FUNNEL" =~ ^[Yy] ]]; then
        echo -e "${CYAN}      Enabling Tailscale Funnel on port ${HUB_PORT}...${NC}"
        # Run funnel in background, create a systemd service for it too
        sudo tee /etc/systemd/system/tailscale-funnel-hub.service > /dev/null <<EOF
[Unit]
Description=Tailscale Funnel for LoRa Mesh Hub
After=tailscaled.service lora-mesh-hub.service
Wants=tailscaled.service

[Service]
Type=simple
ExecStart=/usr/bin/tailscale funnel ${HUB_PORT}
Restart=always
RestartSec=15

[Install]
WantedBy=multi-user.target
EOF
        sudo systemctl daemon-reload
        sudo systemctl enable tailscale-funnel-hub
        sudo systemctl restart tailscale-funnel-hub
        sleep 2

        FUNNEL_URL=$(tailscale funnel status 2>/dev/null | grep "https://" | awk '{print $1}' || echo "")
        if [ -n "$FUNNEL_URL" ]; then
            PUBLIC_URL="${FUNNEL_URL%/}"
            WSS_URL="wss://${PUBLIC_URL#https://}"
        else
            TS_HOST=$(tailscale status --json 2>/dev/null | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('Self',{}).get('DNSName','').rstrip('.'))" 2>/dev/null || echo "your-machine.tail1234.ts.net")
            WSS_URL="wss://${TS_HOST}"
        fi

        echo -e "${GREEN}      ✓ Tailscale Funnel active${NC}"
        echo ""
        echo -e "${YELLOW}      Configure gateways with:${NC}"
        echo -e "      ${CYAN}HUBURL,${WSS_URL}${NC}"
    else
        echo -e "${YELLOW}[4/4] Skipping Funnel — gateways must be on your Tailscale network${NC}"
        echo -e "      Configure gateways with: ${CYAN}HUBURL,ws://${TAILSCALE_IP:-<tailscale-ip>}:${HUB_PORT}${NC}"
    fi
else
    echo -e "${YELLOW}[4/4] Skipping Funnel (Tailscale not installed)${NC}"
fi

# ─── Summary ─────────────────────────────────────────────────
echo ""
echo -e "${CYAN}══════════════════════════════════════════${NC}"
echo -e "${GREEN}  Setup complete!${NC}"
echo ""
echo -e "  Hub status:   ${CYAN}sudo systemctl status ${SERVICE_NAME}${NC}"
echo -e "  Hub logs:     ${CYAN}sudo journalctl -u ${SERVICE_NAME} -f${NC}"
echo -e "  Restart hub:  ${CYAN}sudo systemctl restart ${SERVICE_NAME}${NC}"
echo ""
echo -e "  To change the key or port, edit:"
echo -e "  ${CYAN}/etc/systemd/system/${SERVICE_NAME}.service${NC}"
echo -e "  then run: ${CYAN}sudo systemctl daemon-reload && sudo systemctl restart ${SERVICE_NAME}${NC}"
echo -e "${CYAN}══════════════════════════════════════════${NC}"
