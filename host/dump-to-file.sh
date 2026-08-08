#!/usr/bin/env bash
# 一键把整卡 dump 到文件, 可自选保存位置(kdialog), 无 kdialog 时落到 dumps/
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
TS="$(date +%Y%m%d-%H%M%S)"
# 默认目录: 环境变量 NFC_DUMP_DIR 优先, 否则项目内 dumps/
DUMPDIR="${NFC_DUMP_DIR:-$PWD/dumps}"
mkdir -p "$DUMPDIR"

# 选保存位置: 优先 kdialog 保存对话框, 默认目录 + 时间戳名
if command -v kdialog >/dev/null; then
  OUT="$(kdialog --title "dump 保存到哪里?" \
                 --getsavefilename "$DUMPDIR/card-$TS.dump" "*.dump" 2>/dev/null)"
  [ -z "$OUT" ] && { echo "已取消"; exit 0; }
else
  OUT="$DUMPDIR/card-$TS.dump"
  echo "无 kdialog, 保存到: $OUT"
fi
mkdir -p "$(dirname "$OUT")"

echo "== ESP32 NFC 一键 dump =="
echo "确保: ESP32 已进 PC BRIDGE 模式, 卡贴好, Arduino 串口监视器已关"
read -rp "按回车开始 dump..."
echo

python3 host/nfc_host.py -o "$OUT" dump
rc=$?

echo
if [ $rc -eq 0 ] && [ -s "$OUT" ]; then
  echo "已保存: $OUT"
else
  echo "dump 失败(没卡 / 没进 bridge / 串口被占?)"
fi
read -rp "按回车关闭窗口..."
