"""看门狗 —— 守护进程挂了就把它拉起来。

为什么需要：`clock-ble-daemon.py` 曾经在没有任何征兆的情况下退出过（pythonw
下异常堆栈也没有），结果时钟静默地停止更新，只能靠人发现「钟不走了」。

判断依据有两条，缺一不可：
  1. 子进程退出了          —— 最常见的
  2. 子进程活着但端口没人听 —— 卡死的情况，poll() 是看不出来的

用法：开机自启指向本文件，而不是守护进程本身。
"""

from __future__ import annotations

import os
import socket
import subprocess
import sys
import tempfile
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
DAEMON = os.path.join(HERE, "clock-ble-daemon.py")

PORT = 8765
CHECK_SECONDS = 10
RESTART_DELAY = 5
# 连着这么多次检查都没在听，就当它卡死了
DEAD_CHECKS = 3

LOG_PATH = os.path.join(tempfile.gettempdir(), "clock-ble-daemon.log")


def log(msg: str) -> None:
    line = f"{time.strftime('%H:%M:%S')} [watchdog] {msg}"
    try:
        print(line, flush=True)
    except Exception:  # noqa: BLE001 —— pythonw 下没有 stdout
        pass
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except OSError:
        pass


def port_listening() -> bool:
    try:
        with socket.create_connection(("127.0.0.1", PORT), timeout=1):
            return True
    except OSError:
        return False


def spawn() -> subprocess.Popen:
    log(f"启动守护进程 {os.path.basename(sys.executable)} {DAEMON}")
    return subprocess.Popen([sys.executable, DAEMON], cwd=HERE)


def main() -> None:
    log("看门狗启动")
    proc = spawn()
    dead = 0

    while True:
        time.sleep(CHECK_SECONDS)

        rc = proc.poll()
        if rc is not None:
            log(f"守护进程退出了（code={rc}），{RESTART_DELAY}s 后重启")
            time.sleep(RESTART_DELAY)
            proc = spawn()
            dead = 0
            continue

        if port_listening():
            dead = 0
            continue

        dead += 1
        log(f"进程活着但 {PORT} 端口没人听（第 {dead}/{DEAD_CHECKS} 次）")
        if dead >= DEAD_CHECKS:
            log("判定为卡死，杀掉重启")
            proc.kill()
            proc.wait(timeout=10)
            time.sleep(RESTART_DELAY)
            proc = spawn()
            dead = 0


if __name__ == "__main__":
    try:
        sys.stderr = open(LOG_PATH, "a", encoding="utf-8", buffering=1)
    except OSError:
        pass
    try:
        main()
    except KeyboardInterrupt:
        log("收到 Ctrl-C，退出")
        sys.exit(0)
    except BaseException:
        log("看门狗异常退出：")
        traceback.print_exc()
        raise
