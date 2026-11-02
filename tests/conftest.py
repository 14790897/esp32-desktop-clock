"""E2E 测试的公共设施。

打的是**真实硬件**，路径和 Claude 的 hook 完全一样：

    测试 ──> 127.0.0.1:8765（守护进程）──BLE──> 时钟

所以先要跑起 tools/clock-ble-daemon.py、设备在蓝牙范围内。
连不上就整体跳过，不会在没接设备的机器上红一片。
"""

import hashlib
import json
import socket
import time

import pytest

HOST = "127.0.0.1"
PORT = 8765
TIMEOUT = 5.0


class Clock:
    """守护进程本机接口的薄封装。只做收发和解析，不做断言。"""

    def __init__(self, host: str = HOST, port: int = PORT) -> None:
        self.host = host
        self.port = port

    def send(self, *lines: str) -> str:
        """发若干条命令，返回守护进程的回执。"""
        with socket.create_connection((self.host, self.port), timeout=TIMEOUT) as s:
            s.sendall(("\n".join(lines) + "\n").encode("utf-8"))
            return s.recv(4096).decode("utf-8", "replace").strip()

    def status(self) -> dict:
        """读设备当前的字幕状态。"""
        raw = self.send("Q")
        return json.loads(raw)

    def session(self, sid: str) -> str:
        return self.send("I" + sid)

    def append(self, text: str) -> str:
        return self.send("A" + text)

    def replace(self, text: str) -> str:
        return self.send("R" + text)

    def state(self, value: str) -> str:
        return self.send("S" + value)

    def write_then_read(self, action: str, text: str, sid: str) -> dict:
        """写入后用同一个会话读回来。

        设备是共享资源：Claude Code 的全局 hook 会随时往同一行推字，而设备在
        会话切换时会**清空字幕**，所以后台写入可能把我们的内容顶掉。读到别的
        会话就说明被插队了，重试即可 —— 这是真实竞态，不是测试不稳。
        """
        write = self.append if action == "append" else self.replace
        for _ in range(6):
            write(text)
            snap = self.status()
            if snap["session"] == sid:
                return snap
        pytest.skip("hook 一直在抢字幕行，读不到本用例的会话")


    def wait_for_width(self, predicate, timeout: float = 3.0) -> dict:
        """等到字幕宽度的缓存值跟上内容为止。

        text/session/state 是写入瞬间就生效的，但 **width 不是** ——
        它由 clockTask 在下一轮刷新时用 lcd.textWidth() 算出来（BLE 回调不能
        碰 lcd，会和绘制抢），所以刚写完时读到的还是上一段的宽度。
        """
        deadline = time.time() + timeout
        snap = self.status()
        while time.time() < deadline and not predicate(snap["width"]):
            time.sleep(0.1)
            snap = self.status()
        return snap


@pytest.fixture(scope="session")
def clock() -> Clock:
    c = Clock()
    try:
        c.status()
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        pytest.skip(f"守护进程没跑或时钟不可达（{HOST}:{PORT}）：{exc}")
    return c


@pytest.fixture
def sid(request) -> str:
    """每个用例一个独立会话 ID。

    设备端只回文字尾部，会话名也要能放进 48 字节的缓冲，所以保持简短。
    """
    return "p" + hashlib.sha1(request.node.name.encode("utf-8")).hexdigest()[:10]


@pytest.fixture
def setup_session(clock, sid):
    """把设备切到本用例的会话，避免上一条用例的残留干扰。"""
    clock.session(sid)
    return sid
