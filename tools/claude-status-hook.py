#!/usr/bin/env python3
"""把 Claude Code 的输出实时回显到桌面时钟上（走蓝牙）。

作为 Claude Code 的 hook 使用，两个事件：

  MessageDisplay  回答流式输出过程中按「完成的整行」分批触发，delta 是新增文字
                  → 追加到时钟（设备端尾巴对齐，新字始终贴右边）
  Stop            一轮回答结束
                  → 用完整文本兜底替换一次，再标记「已完成」（字幕转绿）

字幕颜色由设备端从数据流推导：收到文字就是「正在工作」（琥珀），收到 Stop 就是
「已完成」（绿）。因此**不需要** UserPromptSubmit 之类的开工信号 —— 那个 hook 只能
异步跑，可能比第一批文字到得还晚，反而会把颜色卡在上一轮的状态上。

**为什么不是直接连蓝牙**：MessageDisplay 是同步钩子，Claude 会等它返回，超时预算
只有 0.6 秒；而 BLE 建连要一两秒。所以这里只往本机 socket 写一行，真正的 BLE
连接由常驻的 tools/clock-ble-daemon.py 维护。

    本 hook ──> 127.0.0.1:8765 ──> 守护进程 ──BLE──> 时钟

守护进程没跑时连接会立刻失败，这时按熔断逻辑跳过，不拖慢对话。
"""

import json
import os
import socket
import sys
import tempfile
import time

# 守护进程的本机地址。端口要和 tools/clock-ble-daemon.py 一致。
HOST = os.environ.get("CLAUDE_CLOCK_HOST", "127.0.0.1")
PORT = int(os.environ.get("CLAUDE_CLOCK_PORT", "8765"))

# 兜底替换时的文字上限（字符数）。设备端的显示缓冲只有 1024 字节，
# 而一个汉字占 3 字节 —— 留出余量，别让尾部被截掉。
MAX_CHARS = 300

# 超时必须短：MessageDisplay 在关键路径上，Claude 会等它返回
TIMEOUT = 0.6

# 熔断：守护进程/时钟不在线时不要每批都等超时，否则整段输出会被拖慢。
# 文件 mtime 就是上次失败时刻。
FAIL_MARK = os.path.join(tempfile.gettempdir(), "claude-clock-offline")
FAIL_COOLDOWN = 30


def clock_offline() -> bool:
    try:
        return (time.time() - os.path.getmtime(FAIL_MARK)) < FAIL_COOLDOWN
    except OSError:
        return False


def mark(offline: bool) -> None:
    try:
        if offline:
            open(FAIL_MARK, "w").close()
        else:
            os.remove(FAIL_MARK)
    except OSError:
        pass


def send(*lines: str) -> None:
    """把若干条命令发给守护进程。失败时记下时间触发熔断，不抛异常给调用方。

    每条命令一行，前缀一个字符，见守护进程的协议说明。
    """
    if clock_offline():
        return
    try:
        with socket.create_connection((HOST, PORT), timeout=TIMEOUT) as sock:
            sock.sendall(("\n".join(lines) + "\n").encode("utf-8"))
            sock.recv(64)  # 等一个 ok，确认真的送到了
        mark(offline=False)
    except OSError:
        mark(offline=True)


def flatten(text: str) -> str:
    """折叠所有空白 —— 字幕是单行的，换行会把排版打乱。"""
    return " ".join(text.split())


def tail(text: str, limit: int) -> str:
    """取结尾而不是开头。屏幕尾巴对齐，开头那部分根本显示不出来。"""
    if len(text) <= limit:
        return text
    return "…" + text[-(limit - 1):]


def main() -> None:
    try:
        payload = json.load(sys.stdin)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return

    event = payload.get("hook_event_name", "")

    # 会话标识。多开会话时它们都往同一个字幕推，设备靠这个区分：
    # 换了会话就清空重来，否则两个会话的输出会接成一句读不通的话。
    # 每条消息都带上，钩子就不用自己记状态。
    sid = payload.get("session_id") or ""

    # 每条命令前都带上会话标识（空则省略）。钩子不记状态，设备自己判断变没变。
    prefix = [f"I{sid}"] if sid else []

    if event == "MessageDisplay":
        # 逐批追加，不截断 —— 设备端自己会丢掉超出缓冲的老内容。
        # 设备收到文字就认为「正在工作」，所以不需要单独的开工信号。
        delta = flatten(payload.get("delta") or "")
        if delta:
            send(*prefix, "A" + delta)

    elif event == "Stop":
        # 流式过程中已经追加过了，这里用完整文本兜底替换一次，
        # 保证最终内容完整（万一有批次丢了）
        text = flatten(payload.get("last_assistant_message") or "")
        lines = list(prefix)
        if text:
            lines.append("R" + tail(text, MAX_CHARS))
        lines.append("Sd")
        send(*lines)

    elif event == "StopFailure":
        send(*prefix, "Sf")


if __name__ == "__main__":
    main()
