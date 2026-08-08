#!/usr/bin/env bash
# 一键把整卡 dump 到带时间戳的文件, 无需进交互
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
mkdir -p dumps
TS="$(date +%Y%m%d-%H%M%S)"
OUT="dumps/card-$TS.dump"

echo "== ESP32 NFC 一键 dump =="
echo "确保: ESP32 已进 PC BRIDGE 模式, 卡贴好, Arduino 串口监视器已关"
read -rp "按回车开始 dump..."
echo

python3 host/nfc_host.py -o "$OUT" dump
rc=$?

echo
if [ $rc -eq 0 ] && [ -s "$OUT" ]; then
  echo "已保存: $PWD/$OUT"
else
  echo "dump 失败(没卡 / 没进 bridge / 串口被占?)"
fi
read -rp "按回车关闭窗口..."
