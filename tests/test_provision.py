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
import re
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


def _connect_device_ap() -> tuple[bool, str]:
    """把 PC 的无线挂到设备的配网热点上。返回 (是否关联上, 拿到的 IP)。

    把「关联」和「拿到 DHCP 地址」分开是有意的 —— 两者失败含义完全不同：

    - **关联不上** = 前置条件不满足（PC 的移动热点多半在共享无线，Windows 把
      STA 钉在上游链路上，切不了别的 AP）。这时该跳过。
    - **关联上了但拿不到 192.168.4.x** = 正是设备发不出 DHCP 应答的病征
      （`udp_sendto` 返回 -1 ERR_MEM）。这时**必须失败**，跳过就把 bug 藏起来了。
    """
    _netsh("wlan", "connect", f"name={DEVICE_AP}")

    deadline = time.time() + WIFI_SWITCH_TIMEOUT
    while time.time() < deadline:
        if _wifi_ssid() == DEVICE_AP:
            break
        time.sleep(1)
    else:
        return False, ""

    deadline = time.time() + WIFI_SWITCH_TIMEOUT
    while time.time() < deadline:
        ip = _wifi_ip()
        if ip.startswith("192.168.4."):
            return True, ip
        time.sleep(1)
    return True, ""


def _require_device_ap() -> str:
    associated, ip = _connect_device_ap()
    if not associated:
        pytest.skip(f"PC 连不上设备热点 {DEVICE_AP} —— 多半是移动热点在共享无线，"
                    "Windows 此时会把 STA 钉在上游链路上（热点改共享有线即可）")
    assert ip, (
        f"已关联 {DEVICE_AP} 但拿不到 192.168.4.x 的地址（当前 {_wifi_ip()!r}）—— "
        "这正是 DHCP 应答发不出去时的病征，检查配网前有没有把 GIF 帧缓冲还给堆"
    )
    return ip


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


def test_擦掉凭据后_客户端能从设备拿到_DHCP_地址(nvs_backup, wifi_restore, com_port):
    """这条就是「卡在获取 IP 地址」的回归测试。

    设备擦掉凭据后会开配网热点，PC 连上去必须**真的拿到 192.168.4.x**。
    当初设备发不出 DHCP ACK（ERR_MEM）时，症状正是这里拿不到地址。
    """
    _esptool("erase_region", hex(NVS_OFFSET), hex(NVS_SIZE))
    _reset_device()
    time.sleep(6)  # 等 AP 起来

    # 关联失败会跳过，关联成功却拿不到地址会失败 —— 区别见 _connect_device_ap
    _require_device_ap()


def test_配网时堆余量足够发得出_DHCP_应答(nvs_backup, com_port):
    """这条比断言现象更早报警。

    lwIP 发不出去 DHCP ACK 那个 bug（客户端卡在「获取 IP 地址」）根因是内存：
    115KB 的 GIF 帧缓冲开机常驻，把堆挤到分配不出发送缓冲，`udp_sendto`
    返回 -1 (ERR_MEM)。等到有人拿手机去连、发现卡住，已经绕了一大圈。

    这里直接守堆余量 —— 以后再往常驻内存里塞大块东西，这条会先红。

    基线是实测的：配网模式下（帧缓冲已释放）最大连续块 58.0 KB、Free 153.4 KB。
    联网模式下那 115KB 是按设计常驻的（留给 BOOT 键播 GIF），所以只能在
    配网模式守。
    """
    _esptool("erase_region", hex(NVS_OFFSET), hex(NVS_SIZE))
    log = _boot_log()

    m = re.search(r"heap-after-free free=(\d+) largest=(\d+) dma_largest=(\d+)", log)
    assert m, f"没抓到 heap-after-free 那行，启动日志：\n{log[-1200:]}"
    free_kb = int(m.group(1)) / 1024
    largest_kb = int(m.group(2)) / 1024

    # 基线 58.0 KB，留 18 KB 余量。真退回「帧缓冲不释放」那种状态会差 115KB，
    # 稳稳跌穿这条线。正常加功能用了十几 KB 内存也不会误报。
    assert largest_kb >= 40, (
        f"配网模式下最大连续块只有 {largest_kb:.1f} KB（基线 58.0 KB，下限 40 KB）—— "
        f"lwIP 很可能分配不出 DHCP 发送缓冲，客户端会卡在「获取 IP 地址」，"
        f"Free 堆 {free_kb:.1f} KB"
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

    _require_device_ap()

    resp = requests.post(
        f"{PORTAL}/submit",
        json={"ssid": target, "password": password},
        timeout=SUBMIT_TIMEOUT,
    ).json()
    assert resp.get("success") is True, f"提交失败：{resp}"
