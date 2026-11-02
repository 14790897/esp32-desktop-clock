"""BLE 链路的行为测试 —— 打真机，路径和 Claude 的 hook 完全一样。

要跑：
    python tools/clock-ble-daemon.py      # 另开一个终端
    pytest tests/test_ble.py -v

守护进程没跑就整体跳过（见 conftest.py）。

注意设备端的 STATUS 只回**字幕尾部**（BLE 的 read 受 MTU 限制），所以这里
断言的是 `tail` 而不全文；`len` 用来验证长度。
"""


def test_能读回状态(clock):
    snap = clock.status()
    for key in ("state", "session", "active", "connected", "len", "width", "tail"):
        assert key in snap, f"状态里缺字段 {key}: {snap}"
    assert snap["connected"] is True


def test_替换是整体覆盖(clock, setup_session):
    snap = clock.write_then_read("replace", "第一段", setup_session)
    assert snap["tail"] == "第一段"

    snap = clock.write_then_read("replace", "第二段", setup_session)
    assert snap["tail"] == "第二段"


def test_追加是接在末尾(clock, setup_session):
    clock.replace("开头")
    snap = clock.write_then_read("append", "结尾", setup_session)
    assert snap["tail"] == "开头结尾"


def test_追加会把状态推成_working(clock, setup_session):
    """状态由数据流推导，不依赖单独的开工信号。

    这里是被踩过的坑：`Stop` 钩子走异步，可能比下一轮的第一批文字到得还晚，
    屏幕就会停在上一轮的绿色上。所以收到文字必须自己把状态置成 working。
    """
    clock.session(setup_session)
    clock.state("d")
    assert clock.status()["state"] == "done"

    snap = clock.write_then_read("append", "新内容", setup_session)
    assert snap["state"] == "working"


def test_状态取值(clock, setup_session):
    clock.session(setup_session)
    for value, expected in (("w", "working"), ("d", "done"), ("f", "failed")):
        clock.state(value)
        assert clock.status()["state"] == expected


def test_换会话会清空字幕(clock, sid):
    """多开会话时它们都往同一行推，不清的话两段话会拼在一起读不通。"""
    clock.session(sid)
    clock.write_then_read("replace", "A 会话的内容", sid)

    other = "p-other"
    snap = clock.write_then_read("replace", "B 会话的内容", other)
    assert snap["tail"] == "B 会话的内容"
    assert snap["session"] == other


def test_字幕宽度跟着内容更新(clock, setup_session):
    """宽度决定尾巴对齐的起点，错了整行会偏。

    设备端是 clockTask 异步算的，写完不能立刻读，要等一拍 —— 见
    Clock.wait_for_width 的说明。
    """
    clock.write_then_read("replace", "短", setup_session)
    short_w = clock.wait_for_width(lambda w: w < 100)["width"]

    clock.write_then_read("replace", "字" * 40, setup_session)
    long_w = clock.wait_for_width(lambda w: w > 300)["width"]

    assert short_w < 100, f"一个汉字竟有 {short_w}px 宽"
    assert long_w > short_w, f"40 个字没比 1 个字宽（{long_w} vs {short_w}）"


def test_堆余量没有被饿死(clock):
    """去掉 WiFi 之后堆宽裕多了，这条守一个下限。

    实测基线：free_heap ≈ 35KB、最大连续块 ≈ 22KB。往常驻内存里再塞大块
    东西会把这条顶红 —— 早期 WiFi 版本就是被 115KB 的 GIF 帧缓冲挤到 lwIP
    发不出 DHCP 应答的。
    """
    snap = clock.status()
    assert snap["largest_block"] >= 8192, (
        f"最大连续块只剩 {snap['largest_block'] / 1024:.1f} KB，堆被饿死了"
    )
