#!/usr/bin/env bash
# AGV Wi-Fi Bridge 전체 이미지 굽기 (Linux/macOS)
# 사용: ./flash.sh /dev/ttyUSB0 [merged.bin]
set -e
PORT="${1:?usage: flash.sh <port> [merged.bin]}"
BIN="${2:-$(dirname "$0")/agv_wifi_bridge_merged.bin}"
[ -f "$BIN" ] || { echo "not found: $BIN"; exit 1; }
if command -v esptool >/dev/null 2>&1; then ESPTOOL=esptool
elif command -v esptool.py >/dev/null 2>&1; then ESPTOOL=esptool.py
else ESPTOOL="python3 -m esptool"; fi
$ESPTOOL --chip esp32s3 --port "$PORT" --baud 921600 --before default_reset --after hard_reset write_flash 0x0 "$BIN"
