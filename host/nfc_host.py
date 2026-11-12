#!/usr/bin/env python3
"""
nfc_host.py — 电脑端驱动 ESP32 NFC 桥接器 (PC BRIDGE 模式)

用法:
  1. 在 ESP32 OLED 菜单选 "PC BRIDGE (USB)" 进入桥接模式
  2. 电脑端运行:
        python3 nfc_host.py                 # 自动找串口, 进入交互
        python3 nfc_host.py -p /dev/ttyUSB0 # 指定串口
        python3 nfc_host.py uid             # 读一次 UID 就退出
        python3 nfc_host.py dump            # dump 整卡

依赖: pip install pyserial

交互命令(回车发送, 和固件一致):
  uid | info | dump | rblk <n> | wblk <n> <32hex> | ping | help | quit
"""
import sys
import time
import argparse

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("需要 pyserial: pip install pyserial")

BAUD = 115200


def find_port():
    """猜一个像 ESP32 的串口"""
    cands = []
    for p in list_ports.comports():
        name = (p.device or "") + " " + (p.description or "")
        if any(k in name.lower() for k in
               ("usbserial", "ttyusb", "ttyacm", "slab", "cp210", "ch340",
                "wchusb", "silicon", "usb")):
            cands.append(p.device)
    return cands[0] if cands else None


def open_serial(port):
    """打开串口但不触发 ESP32 自动复位。
    经典 ESP32 的 CP2102/CH340 auto-reset 电路由 DTR/RTS 驱动,
    打开端口若拉低 DTR/RTS 会让板子重启、退出 PC BRIDGE 模式。
    这里在 open() 前把 DTR/RTS 置 False, 保住已选的桥接模式。"""
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = BAUD
    ser.timeout = 0.3
    ser.dtr = False
    ser.rts = False
    ser.open()
    return ser


def open_bridge(port, timeout=6.0):
    """Open the port and wait until firmware answers PING.

    FT232-based ESP32 boards can reset as the port opens.  The firmware returns
    to bridge mode automatically, but it still needs a moment to boot.  Sending
    the real command immediately was the main cause of failed/empty dumps.
    """
    ser = open_serial(port)
    deadline = time.monotonic() + timeout
    try:
        time.sleep(0.25)
        while time.monotonic() < deadline:
            if any(line == "PONG" for line in send(ser, "PING", wait=0.08, timeout=0.7)):
                return ser
            time.sleep(0.15)
    except Exception:
        ser.close()
        raise
    ser.close()
    raise RuntimeError(
        "ESP32 did not enter PC BRIDGE. Select PC BRIDGE (USB) on the OLED "
        "and close serial monitors."
    )


def send(ser, cmd, wait=0.4, timeout=None):
    """发一条命令, 收集回应行"""
    command = cmd.strip().upper()
    # A full-card dump may try dozens of keys on every sector and the firmware
    # also keeps the OLED result visible briefly.  Three seconds was too short
    # and made the Linux dump launcher save an empty file.
    if timeout is None:
        timeout = 120.0 if command == "DUMP" else 5.0
    ser.reset_input_buffer()
    ser.write((cmd.strip() + "\n").encode())
    ser.flush()
    time.sleep(wait)
    out = []
    t0 = time.time()
    while time.time() - t0 < timeout:
        line = ser.readline().decode(errors="replace").strip()
        if line:
            out.append(line)
            # DUMP 以 DONE 结束; 其它命令一般一行就够
            if line == "DONE" or line.startswith("ERR"):
                break
            if command != "DUMP":
                break
        elif out and command != "DUMP":
            break
    return out


def main():
    ap = argparse.ArgumentParser(description="ESP32 NFC 桥接器电脑端")
    ap.add_argument("-p", "--port", help="串口, 如 /dev/ttyUSB0 或 COM5")
    ap.add_argument("-o", "--out", help="oneshot 结果同时写入该文件")
    ap.add_argument("oneshot", nargs="*",
                    help="一次性命令, 如 'uid' / 'dump' / 'rblk 4'")
    args = ap.parse_args()

    port = args.port or find_port()
    if not port:
        sys.exit("找不到串口, 用 -p 指定 (可用串口: " +
                 ", ".join(p.device for p in list_ports.comports()) + ")")

    ser = open_bridge(port)
    try:
        print(f"# 已连接 {port} @ {BAUD}")

        if args.oneshot:
            lines = send(ser, " ".join(args.oneshot))
            for line in lines:
                print(line)
            if not lines:
                sys.exit("ESP32 没有回应。请先在 OLED 选择 PC BRIDGE (USB)。")
            if args.oneshot[0].lower() == "dump" and (not lines or lines[-1] != "DONE"):
                sys.exit("dump 不完整: 未收到 DONE, 文件未保存")
            if args.out:
                with open(args.out, "w") as f:
                    f.write("\n".join(lines) + "\n")
                print(f"# 已保存 {args.out}")
            if any(line.startswith("ERR") for line in lines):
                sys.exit(1)
            return

        # ping 确认在桥接模式
        pong = send(ser, "ping")
        if not any("PONG" in x for x in pong):
            print("# 警告: 没收到 PONG。确认 ESP32 已进入 PC BRIDGE 模式?")
        print("# 交互模式。命令: uid info dump rblk <n> wblk <n> <32hex> quit")

        while True:
            try:
                cmd = input("nfc> ").strip()
            except (EOFError, KeyboardInterrupt):
                print()
                break
            if not cmd:
                continue
            if cmd.lower() in ("quit", "exit", "q"):
                break
            for line in send(ser, cmd):
                print(line)
    finally:
        ser.close()


if __name__ == "__main__":
    main()
