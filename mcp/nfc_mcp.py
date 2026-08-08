#!/usr/bin/env python3
"""
nfc_mcp.py — 把 ESP32 RC522 读卡器(PC BRIDGE 模式)包成一个 MCP server。

零依赖: 手写 stdio JSON-RPC (MCP 协议), 只用 pyserial。
让 Claude / 任意 MCP 客户端 通过工具碰卡读 UID、dump、读写块。

前提: ESP32 已烧本项目固件并进入 "PC BRIDGE (USB)" 模式; Arduino 串口监视器已关。

注册到 Claude Code:
  claude mcp add nfc -- python3 /home/ma-xiuyuan/esp32-nfc-copier/mcp/nfc_mcp.py

工具:
  nfc_ping                     确认桥接在线
  nfc_read_uid                 碰卡读 UID(单次)
  nfc_wait_tap(timeout_s)      轮询直到有卡, 返回 UID
  nfc_info                     返回卡类型 + UID
  nfc_dump                     整卡 dump(UID + 每扇区 KEY + 每块数据)
  nfc_read_block(block)        读单块
  nfc_write_block(block, hex)  写单块(32 位 hex)
所有工具可选参数 port(串口, 不填自动找)。
"""
import os
import sys
import json
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "host"))
try:
    from nfc_host import open_serial, send, find_port  # noqa: E402
except Exception as e:  # pragma: no cover
    open_serial = send = find_port = None
    _IMPORT_ERR = str(e)
else:
    _IMPORT_ERR = None


def log(*a):
    print(*a, file=sys.stderr, flush=True)


# ---------- 串口封装 ----------
def _port(args):
    p = args.get("port") or (find_port() if find_port else None)
    if not p:
        raise RuntimeError("找不到串口, 请传 port 参数 (如 /dev/ttyUSB0)")
    return p


def _run(cmd, args):
    """打开桥接(防复位), 发一条命令, 返回文本行列表"""
    if open_serial is None:
        raise RuntimeError(f"pyserial/nfc_host 不可用: {_IMPORT_ERR}")
    ser = open_serial(_port(args))
    try:
        time.sleep(0.2)
        ser.reset_input_buffer()
        return send(ser, cmd)
    finally:
        ser.close()


# ---------- 工具实现 ----------
def tool_ping(args):
    lines = _run("PING", args)
    return "PONG" if any("PONG" in x for x in lines) else "无响应(确认已进 PC BRIDGE?)"


def tool_read_uid(args):
    lines = _run("UID", args)
    return lines[0] if lines else "ERR NORESP"


def tool_wait_tap(args):
    timeout = float(args.get("timeout_s", 15))
    if open_serial is None:
        raise RuntimeError(f"pyserial/nfc_host 不可用: {_IMPORT_ERR}")
    ser = open_serial(_port(args))
    try:
        time.sleep(0.2)
        ser.reset_input_buffer()
        t0 = time.time()
        while time.time() - t0 < timeout:
            lines = send(ser, "UID")
            for ln in lines:
                if ln.startswith("UID "):
                    return ln
            time.sleep(0.4)
        return "ERR TIMEOUT (没检测到卡)"
    finally:
        ser.close()


def tool_info(args):
    lines = _run("INFO", args)
    return lines[0] if lines else "ERR NORESP"


def tool_dump(args):
    lines = _run("DUMP", args)
    return "\n".join(lines) if lines else "ERR NORESP"


def tool_read_block(args):
    blk = int(args["block"])
    lines = _run(f"RBLK {blk}", args)
    return lines[0] if lines else "ERR NORESP"


def tool_write_block(args):
    blk = int(args["block"])
    data = str(args["hex"]).strip()
    if len(data) != 32:
        raise RuntimeError("hex 必须是 32 位十六进制(16 字节)")
    lines = _run(f"WBLK {blk} {data}", args)
    return lines[0] if lines else "ERR NORESP"


PORT_PROP = {"port": {"type": "string", "description": "串口, 如 /dev/ttyUSB0; 不填自动查找"}}

TOOLS = [
    {"name": "nfc_ping", "description": "确认 ESP32 桥接在线(应返回 PONG)",
     "inputSchema": {"type": "object", "properties": dict(PORT_PROP)}, "fn": tool_ping},
    {"name": "nfc_read_uid", "description": "碰卡读一次 UID",
     "inputSchema": {"type": "object", "properties": dict(PORT_PROP)}, "fn": tool_read_uid},
    {"name": "nfc_wait_tap", "description": "轮询直到有卡贴上, 返回 UID",
     "inputSchema": {"type": "object", "properties": {**PORT_PROP,
        "timeout_s": {"type": "number", "description": "超时秒数, 默认 15"}}}, "fn": tool_wait_tap},
    {"name": "nfc_info", "description": "返回卡类型 + UID",
     "inputSchema": {"type": "object", "properties": dict(PORT_PROP)}, "fn": tool_info},
    {"name": "nfc_dump", "description": "整卡 dump: UID + 每扇区 KEY + 每块数据(用字典自动找密钥)",
     "inputSchema": {"type": "object", "properties": dict(PORT_PROP)}, "fn": tool_dump},
    {"name": "nfc_read_block", "description": "读单块(默认密钥认证)",
     "inputSchema": {"type": "object", "properties": {**PORT_PROP,
        "block": {"type": "integer", "description": "块号 0..255"}}, "required": ["block"]},
     "fn": tool_read_block},
    {"name": "nfc_write_block", "description": "写单块, 32 位 hex(16 字节)",
     "inputSchema": {"type": "object", "properties": {**PORT_PROP,
        "block": {"type": "integer", "description": "块号"},
        "hex": {"type": "string", "description": "32 位十六进制"}}, "required": ["block", "hex"]},
     "fn": tool_write_block},
]
TOOL_MAP = {t["name"]: t for t in TOOLS}


# ---------- MCP (stdio JSON-RPC 2.0) ----------
PROTOCOL_VERSION = "2024-11-05"


def reply(id_, result=None, error=None):
    msg = {"jsonrpc": "2.0", "id": id_}
    if error is not None:
        msg["error"] = error
    else:
        msg["result"] = result
    sys.stdout.write(json.dumps(msg) + "\n")
    sys.stdout.flush()


def handle(msg):
    method = msg.get("method")
    id_ = msg.get("id")
    if method == "initialize":
        reply(id_, {
            "protocolVersion": PROTOCOL_VERSION,
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "esp32-nfc", "version": "1.0.0"},
        })
    elif method == "notifications/initialized":
        pass  # 通知, 无需回复
    elif method == "ping":
        reply(id_, {})
    elif method == "tools/list":
        reply(id_, {"tools": [{"name": t["name"], "description": t["description"],
                               "inputSchema": t["inputSchema"]} for t in TOOLS]})
    elif method == "tools/call":
        params = msg.get("params", {})
        name = params.get("name")
        args = params.get("arguments", {}) or {}
        t = TOOL_MAP.get(name)
        if not t:
            reply(id_, {"content": [{"type": "text", "text": f"未知工具: {name}"}], "isError": True})
            return
        try:
            out = t["fn"](args)
            reply(id_, {"content": [{"type": "text", "text": str(out)}]})
        except Exception as e:
            reply(id_, {"content": [{"type": "text", "text": f"错误: {e}"}], "isError": True})
    elif id_ is not None:
        reply(id_, error={"code": -32601, "message": f"method not found: {method}"})


def main():
    log("esp32-nfc MCP server 启动 (stdio)")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        try:
            handle(msg)
        except Exception as e:
            log("handle error:", e)


if __name__ == "__main__":
    main()
