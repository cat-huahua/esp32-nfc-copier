#!/usr/bin/env bash
# Konsole fallback for users who prefer the command-line bridge.
if [ ! -t 0 ] && command -v konsole >/dev/null; then
  exec konsole --hold -e "$0" "$@"
fi
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
echo "== ESP32 NFC 桥接器 =="
echo "提示: 先在 ESP32 OLED 菜单选 PC BRIDGE (USB), 并关掉 Arduino 串口监视器"
echo
python3 host/nfc_host.py "$@"
echo
read -rp "按回车关闭窗口..."
