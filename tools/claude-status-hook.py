#!/usr/bin/env python3
"""把 Claude Code 的输出实时回显到桌面时钟上。

作为 Claude Code 的 hook 使用，两个事件：

  MessageDisplay  回答流式输出过程中按「完成的整行」分批触发，delta 是新增文字
                  → 追加到时钟（设备端尾巴对齐，新字始终贴右边）
  Stop            一轮回答结束
                  → 用完整文本兜底替换一次，再标记「已完成」（字幕转绿）

设计约束：
- MessageDisplay 是**同步**的 —— Claude 会等钩子返回，所以超时必须短。
  用 async 反而会让批次乱序到达，字幕就乱了。
- 这台机器没有 jq，用 Python 解析 JSON 最省事。
- 时钟可能不在线（关机 / 换网络），失败是常态，不能因此拖慢对话。
"""

import json
import os
import sys
import tempfile
import time
import urllib.error
import urllib.request

# 时钟地址。默认值对应固件里 CLOCK_USE_STATIC_IP 配的固定 IP；
# 如果你的网络不是 192.168.137.x 网段，改固件的静态 IP 配置并在这里设环境变量：
#     CLAUDE_CLOCK_URL=http://<时钟地址>
DEVICE = os.environ.get("CLAUDE_CLOCK_URL", "http://192.168.137.50")

# 兜底替换时的文字上限（字符数）。设备端的显示缓冲只有 1KB，
# 而且屏幕是尾巴对齐的，多送的部分根本显示不出来。
MAX_CHARS = 400

# 超时必须短：MessageDisplay 在关键路径上，Claude 会等它返回
TIMEOUT = 0.6

# 熔断：设备不在线时不要每批都等超时，否则整段输出会被拖慢。
# 文件 mtime 就是上次失败时刻。
FAIL_MARK = os.path.join(tempfile.gettempdir(), "claude-clock-offline")
FAIL_COOLDOWN = 30


def clock_offline() -> bool:
    try:
        return (time.time() - os.path.getmtime(FAIL_MARK)) < FAIL_COOLDOWN
    except OSError:
        return False


def post(path: str, body: str) -> None:
    """发一个请求。失败时记下时间触发熔断，不抛异常给调用方。"""
    if clock_offline():
        return
    try:
        req = urllib.request.Request(
            DEVICE + path,
            data=body.encode("utf-8"),
            method="POST",
            headers={"Content-Type": "text/plain; charset=utf-8"},
        )
        with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
            resp.read()
        try:
            os.remove(FAIL_MARK)  # 通了就把熔断解除
        except OSError:
            pass
    except (urllib.error.URLError, OSError, TimeoutError):
        try:
            open(FAIL_MARK, "w").close()
        except OSError:
            pass


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

    if event == "MessageDisplay":
        # 逐批追加，不截断 —— 设备端自己会丢掉超出缓冲的老内容
        delta = flatten(payload.get("delta") or "")
        if delta:
            post("/append", delta)

    elif event == "UserPromptSubmit":
        post("/state", "working")

    elif event == "Stop":
        # 流式过程中已经追加过了，这里用完整文本兜底替换一次，
        # 保证最终内容完整（万一有批次丢了）
        text = flatten(payload.get("last_assistant_message") or "")
        if text:
            post("/say", tail(text, MAX_CHARS))
        post("/state", "done")

    elif event == "StopFailure":
        post("/state", "failed")


if __name__ == "__main__":
    main()
