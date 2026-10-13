"""HTTP 接口的 E2E 测试 —— 打真实硬件，断言的是设备的实际状态而不只是状态码。

要跑：

    pip install -r tests/requirements.txt
    set CLAUDE_CLOCK_URL=http://192.168.137.45
    pytest tests/ -v

没设 CLAUDE_CLOCK_URL 就整体跳过（见 conftest.py）。
"""

import pytest

# 设备端 kTickerCap，见 src/main.cpp
TICKER_CAP = 1024


def test_三个端点都返回_ok(clock, sid):
    assert clock.say("测试", sid).text == "ok"
    assert clock.append("追加", sid).text == "ok"
    assert clock.state("done", sid).text == "ok"


def test_say_是整体替换(clock, sid):
    clock.say("第一段", sid)
    snap = clock.write_then_read("say", "第二段", sid)
    assert snap["text"] == "第二段"


def test_append_是追加(clock, sid):
    clock.say("开头", sid)
    snap = clock.write_then_read("append", "结尾", sid)
    assert snap["text"] == "开头结尾"


def test_append_会把状态推成_working(clock, sid):
    """状态由数据流推导，不依赖单独的开工信号。

    这里是被踩过的坑：`stop` hook 走异步，可能比下一轮的第一批文字到得还晚，
    屏幕就会停在上一轮的绿色上。所以 /append 必须自己把状态置成 working。
    """
    clock.state("done", sid)
    assert clock.ticker()["state"] == "done"

    snap = clock.write_then_read("append", "新内容", sid)
    assert snap["state"] == "working"


@pytest.mark.parametrize("value,expected", [
    ("working", "working"),
    ("done", "done"),
    ("failed", "failed"),
    ("随便什么值", "done"),   # 未识别的值按已完成处理，见 handleState
])
def test_state_取值与映射(clock, sid, value, expected):
    clock.state(value, sid)
    assert clock.ticker()["state"] == expected


def test_换会话会清空字幕(clock, sid):
    """多开会话时它们都往同一行推，不清的话两段话会拼在一起读不通。"""
    clock.say("A 会话的内容", sid)
    assert clock.write_then_read("say", "还是 A", sid)["text"] == "还是 A"

    other = "pytest-other-session"
    snap = clock.write_then_read("say", "B 会话的内容", other)
    assert snap["text"] == "B 会话的内容"
    assert snap["session"] == other


def test_字幕宽度跟着内容更新(clock, sid):
    """宽度决定尾巴对齐的起点，错了整行会偏。

    注意宽度是 clockTask 异步算的，写完不能立刻读（见 Clock.wait_for_width）。
    """
    clock.write_then_read("say", "短", sid)
    short_w = clock.wait_for_width(lambda w: w < 100)["width"]

    clock.write_then_read("say", "字" * 40, sid)
    long_w = clock.wait_for_width(lambda w: w > 300)["width"]

    assert short_w < 100, f"一个汉字竟有 {short_w}px 宽"
    assert long_w > short_w, f"40 个字({long_w}px)没有比 1 个字({short_w}px)宽"


def test_超长文字被截断且不切坏汉字(clock, sid):
    """设备端缓冲上限 1024 字节。

    这里钉住的是**实际行为**：`/say` 只保留请求体的**前面** 1023 字节，
    多出来的直接读掉丢弃（见 src/main.cpp 的 readBody）。也就是说尾巴对齐的
    显示拿不到结尾 —— 所以 PC 侧的 hook 是**先截好再发**的
    （tools/claude-status-hook.py 里的 tail()）。要改这个行为得两边一起改。
    """
    body = "字" * 800          # 2400 字节，远超上限
    snap = clock.write_then_read("say", body, sid)

    assert len(snap["text"].encode("utf-8")) <= TICKER_CAP - 1
    assert body.startswith(snap["text"]), "保留的应该是开头那一段"
    assert "�" not in snap["text"], "有汉字被切成了半个，屏幕会出乱码"


def test_提示帧宽度不超出可显示范围(clock):
    """提示帧（配网提示、开机 IP）写成整句会被尾巴对齐裁掉开头。

    实测踩过：屏幕上只剩个裸 http://192.168.4.1，最该看的热点名被切没了。
    这个断言就是那次事故的回归测试 —— 设备只有在提示生效时才有帧可查
    （配网模式下才有），所以其余时候跳过。
    """
    snap = clock.ticker()
    hints = snap["hints"]
    if not hints:
        pytest.skip("当前没有轮播提示（设备已联网），无法检查帧宽")

    for frame in hints:
        assert frame["width"] <= snap["limit"], (
            f"提示帧「{frame['text']}」宽 {frame['width']}px，"
            f"超出可显示的 {snap['limit']}px，会被裁掉"
        )
