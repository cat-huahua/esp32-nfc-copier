#!/usr/bin/env python3
"""
write_from_dump.py — 把 .dump 文件写回 MIFARE Classic 卡 (经 PC BRIDGE)

用法:
  python3 host/write_from_dump.py dumps/card-XXXX.dump
  python3 host/write_from_dump.py -p /dev/ttyUSB0 file.dump
  python3 host/write_from_dump.py --block0 file.dump      # 也写块0(需 Gen2 magic 卡)
  python3 host/write_from_dump.py --trailers file.dump    # 也写密钥块(危险)

默认行为(安全): 只写数据块, 跳过块0(UID)和 sector trailer(密钥块),
并只写在 dump 里是合法 32 位 hex 的块(读失败的块自动跳过)。

限制: 串口桥接的 WBLK 用默认密钥 A 认证后写入, **不含 Gen1a 后门**。
  - 块0(UID)只有 Gen2 magic 卡能写; 普通卡会 ERR, 属正常。
  - 要连 UID 完整克隆 Gen1a 卡, 请用设备端菜单 READ + WRITE Gen1a。
"""
import os
import sys
import re
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nfc_host import open_serial, send, find_port  # noqa: E402
from serial.tools import list_ports  # noqa: E402

BLK_RE = re.compile(r'^BLK\s+(\d+)\s+([0-9a-fA-F]{32})\s*$')
KEY_RE = re.compile(r'^KEY\s+(\d+)\s+([AB])\s+([0-9a-fA-F]{12})\s*$')


# MIFARE Classic 几何(4K 后 8 扇区每个 16 块)
def sec_block_count(s): return 4 if s < 32 else 16
def sec_first_block(s): return s * 4 if s < 32 else 128 + (s - 32) * 16
def sec_trailer(s):     return sec_first_block(s) + sec_block_count(s) - 1
def block_sector(b):    return b // 4 if b < 128 else 32 + (b - 128) // 16
def is_trailer(b):      return b == sec_trailer(block_sector(b))


def parse_dump(path):
    blocks, keys = {}, {}
    with open(path) as f:
        for line in f:
            s = line.strip()
            m = BLK_RE.match(s)
            if m:
                blocks[int(m.group(1))] = m.group(2).upper()
                continue
            k = KEY_RE.match(s)
            if k:
                keys[int(k.group(1))] = (k.group(2), k.group(3).upper())
    return blocks, keys


def main():
    ap = argparse.ArgumentParser(description="把 .dump 写回 MIFARE Classic 卡")
    ap.add_argument("dumpfile", help=".dump 文件路径")
    ap.add_argument("-p", "--port", help="串口, 如 /dev/ttyUSB0")
    ap.add_argument("--block0", action="store_true", help="也写块0/UID(需 Gen2 magic)")
    ap.add_argument("--trailers", action="store_true",
                    help="也写密钥块: 用 KEY 行回填真 KeyA, 使克隆卡密钥=源卡(危险, 目标须默认密钥可认证)")
    args = ap.parse_args()

    if not os.path.isfile(args.dumpfile):
        sys.exit(f"文件不存在: {args.dumpfile}")

    blocks, keys = parse_dump(args.dumpfile)
    if not blocks:
        sys.exit("没解析到任何合法块(文件格式不对?)")

    # 尾块写入前, 用 KEY 行回填 KeyA(dump 里尾块 KeyA 恒为屏蔽的 000000)
    if args.trailers:
        for b in list(blocks):
            if is_trailer(b):
                s = block_sector(b)
                if s in keys:
                    blocks[b] = keys[s][1] + blocks[b][12:]   # KeyA=真密钥, 访问位+KeyB 保留

    # 选目标块(几何感知)
    targets = []
    skipped = []
    for b in sorted(blocks):
        if b == 0 and not args.block0:
            skipped.append((b, "块0/UID"))
            continue
        if is_trailer(b) and not args.trailers:
            skipped.append((b, "密钥块"))
            continue
        targets.append(b)

    print(f"# dump: {args.dumpfile}")
    print(f"# 解析到 {len(blocks)} 块, 将写入 {len(targets)} 块, 跳过 {len(skipped)} 块")
    if skipped:
        print("# 跳过:", ", ".join(f"{b}({why})" for b, why in skipped))

    port = args.port or find_port()
    if not port:
        sys.exit("找不到串口, 用 -p 指定 (可用: " +
                 ", ".join(p.device for p in list_ports.comports()) + ")")

    ser = open_serial(port)
    ok = fail = 0
    try:
        pong = send(ser, "ping")
        if not any("PONG" in x for x in pong):
            print("# 警告: 没收到 PONG, 确认 ESP32 已进 PC BRIDGE 且卡贴好")
        for b in targets:
            resp = send(ser, f"WBLK {b} {blocks[b]}")
            line = resp[0] if resp else "ERR NORESP"
            print(f"blk {b:2d}: {line}")
            if line.strip() == "OK":
                ok += 1
            else:
                fail += 1
    finally:
        ser.close()

    print(f"# 完成: OK {ok}, FAIL {fail}")
    sys.exit(1 if fail else 0)


if __name__ == "__main__":
    main()
