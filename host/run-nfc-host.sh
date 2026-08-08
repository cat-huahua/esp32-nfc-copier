#!/usr/bin/env bash
# 在终端里启动 nfc_host.py, 退出后停住等按键(防止窗口秒关)
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
echo "== ESP32 NFC 桥接器 =="
echo "提示: 先在 ESP32 OLED 菜单选 PC BRIDGE (USB), 并关掉 Arduino 串口监视器"
echo
python3 host/nfc_host.py "$@"
echo
read -rp "按回车关闭窗口..."
