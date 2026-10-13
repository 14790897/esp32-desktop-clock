"""E2E 测试的公共设施。

这些用例打的是**真实硬件**：设备必须已经连上网络、且 PC 能访问到它。
地址从环境变量 CLAUDE_CLOCK_URL 取（和 hook 用同一个变量），没设就整体跳过，
这样 CI 或没接设备的机器上跑 pytest 不会红一片。
"""

import hashlib
import os
import time

import pytest
import requests

CONNECT_TIMEOUT = 5


class Clock:
    """时钟 HTTP 接口的一个薄封装。只做请求和解析，不做断言。"""

    def __init__(self, base_url: str) -> None:
        self.base = base_url.rstrip("/")
        self.session = requests.Session()

    def post(self, path: str, body: str, sid: str = "") -> requests.Response:
        url = self.base + path
        if sid:
            url += f"?sid={sid}"
        return self.session.post(url, data=body.encode("utf-8"), timeout=CONNECT_TIMEOUT)

    def say(self, body: str, sid: str = "") -> requests.Response:
        return self.post("/say", body, sid)

    def append(self, body: str, sid: str = "") -> requests.Response:
        return self.post("/append", body, sid)

    def state(self, value: str, sid: str = "") -> requests.Response:
        return self.post("/state", value, sid)

    def ticker(self) -> dict:
        r = self.session.get(self.base + "/debug/ticker", timeout=CONNECT_TIMEOUT)
        r.raise_for_status()
        return r.json()

    def wait_for_width(self, predicate, timeout: float = 3.0) -> dict:
        """等到字幕宽度的缓存值跟上内容为止。

        text/session/state 是写入瞬间就生效的，但 **width 不是** ——
        它由 clockTask 在下一轮刷新时用 lcd.textWidth() 算出来（HTTP 线程不能
        碰 lcd，会和绘制抢），所以刚写完时读到的还是上一段的宽度。
        """
        deadline = time.time() + timeout
        snap = self.ticker()
        while time.time() < deadline:
            snap = self.ticker()
            if predicate(snap["width"]):
                break
            time.sleep(0.1)
        return snap

    def write_then_read(self, action: str, body: str, sid: str, tries: int = 6) -> dict:
        """写入后用同一个 sid 读回来。

        设备是共享资源：Claude Code 的全局 hook 会在任意时刻往同一行推字，
        而设备在会话切换时**会清空字幕**，所以后台写入可能把我们的内容顶掉。
        读到别的 sid 就说明被插队了，重试即可 —— 这是真实竞态，不是测试不稳。
        """
        for _ in range(tries):
            getattr(self, action)(body, sid)
            snap = self.ticker()
            if snap["session"] == sid:
                return snap
        pytest.skip("hook 一直在抢字幕行，读不到本用例的会话")


@pytest.fixture(scope="session")
def clock() -> Clock:
    url = os.environ.get("CLAUDE_CLOCK_URL", "").strip()
    if not url:
        pytest.skip("未设置 CLAUDE_CLOCK_URL，跳过需要真机的 E2E 测试")

    c = Clock(url)
    # 要等而不是探一次就判死：test_boot 会复位设备，复位后重新关联 + 拿 DHCP +
    # 对时要好几秒，期间 HTTP 是不通的。只给 5 秒会把「还没连上」误判成「设备不在」。
    deadline = time.time() + 40
    last = None
    while time.time() < deadline:
        try:
            c.ticker()
            return c
        except requests.RequestException as exc:
            last = exc
            time.sleep(2)
    pytest.skip(f"时钟不可达（{url}，等了 40 秒）：{last}")


@pytest.fixture
def sid(request) -> str:
    """每个用例一个独立会话 ID，避免相互串台。

    必须**纯 ASCII**：设备端不做 URL 百分号解码，sid 会被原样存下来再回显。
    用例名里有中文的话（`test_say_是整体替换`），Python 侧发出去是编码后的
    字节、读回来是编码后的字符串，两边永远对不上。哈希一下就都是 ASCII 了。
    """
    digest = hashlib.sha1(request.node.name.encode("utf-8")).hexdigest()[:10]
    return f"pytest-{digest}"
