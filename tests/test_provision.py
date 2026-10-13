"""配网全流程 E2E —— 唯一能覆盖「设备给客户端发 DHCP 应答」的测试。

今天踩的三个坑全在这条路径上，而它此前零覆盖：
  1. 软 AP 默认跑 HT40，2.4GHz 上横跨 1~5 信道，客户端容易在握手时丢包
  2. 115KB 的 GIF 帧缓冲开机常驻，把堆挤到 lwIP 发 ACK 时分配不到内存
     （udp_sendto 返回 -1 ERR_MEM），客户端卡在「获取 IP 地址」
  3. 配网提示写成整句会被尾巴对齐裁掉开头，屏幕只剩个裸 IP

**默认不跑**，因为它有副作用：要擦掉设备的 WiFi 凭据、还要临时切换 PC 的
无线网卡。用完两者都会恢复，但仍必须显式开启：

    set CLOCK_E2E_PROVISION=1
    pytest tests/test_provision.py -v

前提：PC 的「移动热点」必须是关的 —— 热点开着时无线网卡无法作为客户端
连别的 AP（这条踩了很久）。测试会检查并直接跳过。
"""

import os
import subprocess
import sys
import time

import pytest
import requests
import serial

# 分区表里的 nvs 区，见 partitions.csv
NVS_OFFSET = 0x9000
NVS_SIZE = 0x6000

DEVICE_AP = "esp32-clock-5745"
PORTAL = "http://192.168.4.1"
PORTAL_TIMEOUT = 10

# 切网卡 + DHCP + 提交 + 恢复，给足余量
WIFI_SWITCH_TIMEOUT = 30
SUBMIT_TIMEOUT = 150


def _run(*args: str, timeout: int = 120) -> subprocess.CompletedProcess:
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def _netsh(*args: str) -> str:
    return _run("netsh", *args).stdout


def _esptool(*args: str) -> subprocess.CompletedProcess:
    return _run(sys.executable, "-m", "esptool", "--chip", "esp32c3",
                "--port", os.environ.get("ESPPORT", "COM5"), "--baud", "460800", *args)


def _wifi_ssid() -> str:
    for line in _netsh("wlan", "show", "interfaces").splitlines():
        stripped = line.strip()
        if stripped.startswith("SSID") and not stripped.startswith("BSSID"):
            return stripped.split(":", 1)[1].strip()
    return ""


def _wifi_ip() -> str:
    import re
    m = re.search(r"IP Address:\s+(\S+)", _netsh("int", "ip", "show", "addresses", "name=WLAN"))
    return m.group(1) if m else ""


def _hotspot_on() -> bool:
    out = _run("powershell", "-NoProfile", "-Command",
               "Get-NetAdapter | Where-Object {$_.InterfaceDescription -like '*Wi-Fi Direct*'} "
               "| Select-Object -ExpandProperty Status").stdout
    return "Up" in out


def _reset_device() -> None:
    """用 DTR/RTS 复位。开串口时别动这两根线的话设备不会重启。"""
    with serial.Serial(os.environ.get("ESPPORT", "COM5"), 115200, timeout=1) as s:
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.1)
        s.setRTS(False)


def _boot_log(seconds: float = 14) -> str:
    """复位设备并收一段启动日志（UTF-8 解码）。

    没用 pytest-embedded 的 `dut`：pexpect 的 encoding 默认是 None，模式会被按
    ASCII 编码，中文断言直接抛 UnicodeEncodeError；而这里的告警是中文的。
    另外 `dut` 会一直占着串口，测试代码自己就开不了口去复位。
    """
    port = os.environ.get("ESPPORT", "COM5")
    with serial.Serial(port, 115200, timeout=1) as s:
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.1)
        s.setRTS(False)
        deadline = time.time() + seconds
        chunks = []
        while time.time() < deadline:
            line = s.readline()
            if line:
                chunks.append(line.decode("utf-8", "replace"))
    return "".join(chunks)


def _wait_wifi(expected_ssid: str, want_ip_prefix: str = "", timeout: float = WIFI_SWITCH_TIMEOUT) -> str:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if _wifi_ssid() == expected_ssid:
            ip = _wifi_ip()
            if not want_ip_prefix or ip.startswith(want_ip_prefix):
                return ip
        time.sleep(1)
    return ""


@pytest.fixture
def com_port() -> str:
    return os.environ.get("ESPPORT", "COM5")


@pytest.fixture
def nvs_backup(com_port):
    """备份 nvs 分区，测完无条件写回 —— 这个测试会擦掉设备已保存的 WiFi。"""
    import tempfile
    path = os.path.join(tempfile.gettempdir(), "clock_nvs_backup.bin")

    r = _esptool("read_flash", hex(NVS_OFFSET), hex(NVS_SIZE), path)
    if r.returncode != 0:
        pytest.skip(f"读不出 nvs 分区，跳过：{r.stderr.strip()[:200]}")

    yield path

    r = _esptool("write_flash", hex(NVS_OFFSET), path)
    assert r.returncode == 0, f"nvs 恢复失败，设备的 WiFi 凭据可能丢了：{r.stderr[:300]}"
    _reset_device()


@pytest.fixture
def wifi_restore():
    """记下当前连的无线，测完连回去。"""
    original = _wifi_ssid()
    yield original
    if original:
        _netsh("wlan", "connect", f"name={original}")
        _wait_wifi(original, timeout=60)


@pytest.fixture(autouse=True)
def require_opt_in():
    if os.environ.get("CLOCK_E2E_PROVISION") != "1":
        pytest.skip("配网全流程测试会擦设备凭据并切换 PC 网卡，"
                    "要显式设置 CLOCK_E2E_PROVISION=1 才跑")
    if _hotspot_on():
        pytest.skip("PC 的移动热点开着 —— 此时无线网卡无法作为客户端连设备热点，先关掉它")


def test_擦掉凭据后_客户端能从设备拿到_DHCP_地址(nvs_backup, wifi_restore, com_port):
    """这条就是「卡在获取 IP 地址」的回归测试。

    设备擦掉凭据后会开配网热点，PC 连上去必须**真的拿到 192.168.4.x**。
    当初设备发不出 DHCP ACK（ERR_MEM）时，症状正是这里拿不到地址。
    """
    _esptool("erase_region", hex(NVS_OFFSET), hex(NVS_SIZE))
    _reset_device()
    time.sleep(6)  # 等 AP 起来

    _netsh("wlan", "connect", f"name={DEVICE_AP}")
    ip = _wait_wifi(DEVICE_AP, want_ip_prefix="192.168.4.")

    assert ip, (
        f"连上 {DEVICE_AP} 但没拿到 192.168.4.x 的地址（当前 {_wifi_ip()!r}）—— "
        "这正是 DHCP 应答发不出去时的症状，检查配网前有没有把 GIF 帧缓冲还给堆"
    )


def test_配网提示的每一帧都不超出屏幕(nvs_backup, com_port):
    """「提示被裁成裸 IP」的回归测试。

    设备只有一行 240px、字号 24px 的消息位，而且是尾巴对齐的 —— 超宽只从
    左边裁。写成一整句时屏幕上只剩个 http://192.168.4.1，最该看的热点名没了。

    断言走串口日志而不是 /debug/ticker：**配网模式下我们的 HTTP 服务还没起**
    （80 端口是门户占着），那个端点此刻并不存在。
    """
    _esptool("erase_region", hex(NVS_OFFSET), hex(NVS_SIZE))
    log = _boot_log()

    assert "Access Point started with SSID" in log, (
        f"擦掉凭据后设备没进配网模式。启动日志：\n{log[-1500:]}"
    )
    assert "hint-frame-clipped" not in log, (
        f"有提示帧超出屏幕宽度会被裁掉（固件已打告警）：\n"
        + "\n".join(l for l in log.splitlines() if "hint-frame-clipped" in l)
    )


def test_提交凭据后设备能连上并继续对时(nvs_backup, wifi_restore, com_port):
    """完整往返：进配网 → 提交 → 连上 → 对时。

    目标网络沿用测试机当前连着的那个（wifi_restore 记下来的），所以设备最后
    会跟 PC 处在同一网段。
    """
    target = wifi_restore
    password = os.environ.get("CLOCK_E2E_TARGET_PASSWORD", "")
    if not password:
        pytest.skip("没给目标网络密码，设 CLOCK_E2E_TARGET_PASSWORD 后再跑")

    _esptool("erase_region", hex(NVS_OFFSET), hex(NVS_SIZE))
    _reset_device()
    time.sleep(6)

    _netsh("wlan", "connect", f"name={DEVICE_AP}")
    assert _wait_wifi(DEVICE_AP, want_ip_prefix="192.168.4."), "拿不到设备地址"

    resp = requests.post(
        f"{PORTAL}/submit",
        json={"ssid": target, "password": password},
        timeout=SUBMIT_TIMEOUT,
    ).json()
    assert resp.get("success") is True, f"提交失败：{resp}"
