#!/usr/bin/env bash
# 一键: 选一个 .dump 文件, 写回卡 (数据块; 跳过 UID 和密钥块, 安全)
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1

# 默认目录: 环境变量 NFC_DUMP_DIR 优先, 否则项目内 dumps/
DUMPDIR="${NFC_DUMP_DIR:-$PWD/dumps}"
mkdir -p "$DUMPDIR"

# 选文件: 优先 kdialog 图形选择, 否则用最近的 dump
if command -v kdialog >/dev/null; then
  FILE="$(kdialog --title "选择要写回的 .dump 文件" \
                  --getopenfilename "$DUMPDIR" "*.dump" 2>/dev/null)"
  [ -z "$FILE" ] && { echo "已取消"; exit 0; }
else
  FILE="$(ls -t "$DUMPDIR"/*.dump 2>/dev/null | head -1)"
  [ -z "$FILE" ] && { echo "dumps/ 里没有 .dump 文件"; read -rp "回车关闭"; exit 1; }
  echo "用最近的文件: $FILE"
fi

echo "== ESP32 NFC 写回卡 =="
echo "文件: $FILE"
echo "目标卡将被写入! 确保: ESP32 已进 PC BRIDGE, 目标卡贴好, Arduino 监视器已关"
echo "(默认只写数据块, 跳过 UID/密钥块; 要写块0 请命令行加 --block0)"
read -rp "确认写入? 回车继续, Ctrl-C 取消..."
echo

python3 host/write_from_dump.py "$FILE"
echo
read -rp "按回车关闭窗口..."
