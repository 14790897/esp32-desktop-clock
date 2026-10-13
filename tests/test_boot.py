"""串口层 E2E —— 复位真实硬件，断言启动走到联网对时。

这是唯一能覆盖「启动 / 配网 / 对时」这几条路径的测试，而它们恰恰最容易
再次踩坏：配网那一整套（DHCP、软 AP 带宽、堆内存）之前从没有回归覆盖，
出问题是靠串口日志一行行看出来的。

跑法：

    pytest tests/test_boot.py -v --port COM5 --embedded-services serial,esp

用 Espressif 官方的 pytest-embedded：`dut` 夹具会硬复位目标并在后台收串口。

两点注意：
- **断言串只能用 ASCII**。pexpect 的 encoding 默认为 None，中文模式会被
  按 ASCII 编码直接抛 UnicodeEncodeError。好在每条关键日志都有 ASCII 部分
  （`lcd ready`、`wifi up, ssid=`、`time synced:`），够用了。
- 不需要单独写「启动不崩」的用例：崩了里程碑就永远等不到，
  `expect_exact` 自己会超时失败。
"""

import pytest
from pytest_embedded import Dut

# 设备连上网需要热点开着，超时给宽一点（扫描 + 关联 + DHCP + 首次对时）
BOOT_TIMEOUT = 20
ONLINE_TIMEOUT = 60
SYNC_TIMEOUT = 40


def test_启动能走到文件系统就绪(dut: Dut) -> None:
    """屏幕和 LittleFS 是后面一切的前提，起不来就全错。"""
    dut.expect_exact("lcd ready", timeout=BOOT_TIMEOUT)
    dut.expect_exact("littlefs", timeout=BOOT_TIMEOUT)


def test_能连上网络并完成对时(dut: Dut) -> None:
    """完整的联网链路：关联 → DHCP → SNTP 同步。

    这一条同时也是「启动没有崩」的兜底 —— 中间任何一步 assert 或重启，
    后面的里程碑就永远等不到，用例会超时失败。
    """
    dut.expect_exact("lcd ready", timeout=BOOT_TIMEOUT)
    dut.expect_exact("wifi up, ssid=", timeout=ONLINE_TIMEOUT)
    dut.expect_exact("time synced:", timeout=SYNC_TIMEOUT)
