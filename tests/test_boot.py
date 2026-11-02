"""串口层 E2E —— 复位真实硬件，断言启动走到蓝牙开始广播。

这是唯一能覆盖「启动 / 文件系统 / 蓝牙初始化」这条链的测试，而它出问题时的
表现就是设备完全不可达。

跑法：

    pytest tests/test_boot.py -v --port COM5 --embedded-services serial,esp

用 Espressif 官方的 pytest-embedded：`dut` 夹具会硬复位目标并在后台收串口。

两点注意：
- **断言串只能用 ASCII**。pexpect 的 encoding 默认为 None，中文模式会被
  按 ASCII 编码直接抛 UnicodeEncodeError。好在关键日志都有 ASCII 部分。
- 不需要单独写「启动不崩」的用例：崩了里程碑就永远等不到，
  `expect_exact` 自己会超时失败。
"""

import pytest
from pytest_embedded import Dut

BOOT_TIMEOUT = 20
BLE_TIMEOUT = 20


def test_启动能走到文件系统就绪(dut: Dut) -> None:
    """屏幕和 LittleFS 是后面一切的前提，起不来就全错。"""
    dut.expect_exact("lcd ready", timeout=BOOT_TIMEOUT)
    dut.expect_exact("littlefs", timeout=BOOT_TIMEOUT)


def test_蓝牙控制器初始化成功(dut: Dut) -> None:
    dut.expect_exact("lcd ready", timeout=BOOT_TIMEOUT)
    dut.expect_exact("BLE_INIT: Bluetooth MAC", timeout=BLE_TIMEOUT)


def test_开始广播(dut: Dut) -> None:
    """没有 WiFi 兜底了，广播起不来就意味着设备连不上 —— 屏幕之外唯一的路。"""
    dut.expect_exact("lcd ready", timeout=BOOT_TIMEOUT)
    dut.expect_exact("advertise;", timeout=BLE_TIMEOUT)
