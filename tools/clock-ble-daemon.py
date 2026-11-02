"""时钟 BLE 守护进程 —— PC 侧常驻，维护和时钟的蓝牙连接。

为什么需要它：Claude Code 的 MessageDisplay 钩子跑在**关键路径**上（Claude 要等
它返回），超时预算只有 0.6 秒；而 BLE 建连要一两秒。所以不能让钩子自己去连。

架构：
    hook（快，只写本机 socket）
        └─> 127.0.0.1:8765  ──> 本进程（持 BLE 连接）──> 时钟

顺带它每 5 分钟把系统时间推给时钟 —— 蓝牙上不了互联网，时钟没有 NTP，
时间只能靠 PC。

本机协议：一行一条命令，前缀一个字符
    A<text>   追加文本（实时回显）
    R<text>   整体替换
    S<w|d|f>  状态：工作中/完成/失败
    I<sid>    会话标识（换了会话设备会清空字幕）
    T<epoch>  设置时间
    Q         查询，返回一行状态 JSON
回复：`ok` 或 `err <原因>`（Q 例外，直接回状态 JSON）。

跑法：
    python tools/clock-ble-daemon.py
"""

from __future__ import annotations

import asyncio
import json
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE = "7d9f0001-5a3e-4c1b-9e21-6b0c4c4f4b00"
CMD = "7d9f0002-5a3e-4c1b-9e21-6b0c4c4f4b00"
STATUS = "7d9f0003-5a3e-4c1b-9e21-6b0c4c4f4b00"
DEVICE_NAME = "esp32-clock"

LISTEN_HOST = "127.0.0.1"
LISTEN_PORT = 8765

# 单条 BLE 写最多 247-3 字节（MTU 247）。留点余量，并给命令前缀腾出 1 字节。
CHUNK = 200

# 多久推一次时间。ESP32 内部 RTC 会漂，5 分钟一次足够跟上。
TIME_PUSH_SECONDS = 300

RECONNECT_DELAY = 3.0


def log(msg: str) -> None:
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


class ClockLink:
    """维护 BLE 连接，并把本地命令转发过去。"""

    def __init__(self) -> None:
        self.client: BleakClient | None = None
        self.last_time_push = 0.0
        self.lock = asyncio.Lock()

    @property
    def connected(self) -> bool:
        return self.client is not None and self.client.is_connected

    async def connect_forever(self) -> None:
        """断线就一直重连。BLE 设备重启、PC 睡眠都会断。"""
        while True:
            if self.connected:
                await asyncio.sleep(1)
                continue

            try:
                dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=10.0)
                if dev is None:
                    log(f"没找到 {DEVICE_NAME}，{RECONNECT_DELAY:.0f}s 后重试")
                    await asyncio.sleep(RECONNECT_DELAY)
                    continue

                client = BleakClient(dev)
                await client.connect()
                self.client = client
                log(f"已连接 {dev.address}（MTU={getattr(client, 'mtu_size', '?')}）")

                await self.push_time()
            except Exception as exc:  # noqa: BLE001 —— 任何失败都只该导致重连
                log(f"连接失败: {exc}")
                self.client = None
                await asyncio.sleep(RECONNECT_DELAY)

    async def push_time(self) -> None:
        if not self.connected:
            return
        epoch = int(time.time())
        await self.client.write_gatt_char(
            CMD, b"T" + epoch.to_bytes(4, "little"), response=False
        )
        self.last_time_push = time.time()
        log(f"已推送时间 epoch={epoch}")

    async def send(self, payload: bytes) -> None:
        async with self.lock:
            if not self.connected:
                raise ConnectionError("未连接")
            await self.client.write_gatt_char(CMD, payload, response=False)

    async def send_text(self, prefix: bytes, text: bytes) -> None:
        """长文本分块发。每块都带前缀，设备端逐块追加。"""
        for i in range(0, len(text), CHUNK):
            await self.send(prefix + text[i : i + CHUNK])

    async def handle(self, line: bytes) -> bytes:
        if not line:
            return b"ok"

        op, body = line[:1], line[1:]

        if op == b"A":
            await self.send_text(b"A", body)
        elif op == b"R":
            await self.send_text(b"R", body)
        elif op == b"S":
            if not body:
                return b"err no state"
            await self.send(b"S" + body[:1])
        elif op == b"I":
            await self.send(b"I" + body)
        elif op == b"T":
            await self.push_time()
        elif op == b"Q":
            raw = await self.client.read_gatt_char(STATUS)
            return raw
        else:
            return b"err unknown op"

        return b"ok"


async def serve_client(link: ClockLink, reader: asyncio.StreamReader,
                       writer: asyncio.StreamWriter) -> None:
    peer = writer.get_extra_info("peername")
    try:
        while True:
            line = await reader.readline()
            if not line:
                break
            try:
                reply = await link.handle(line.rstrip(b"\r\n"))
            except Exception as exc:  # noqa: BLE001 —— 单条命令失败不要拖垮连接
                reply = f"err {exc}".encode()
            writer.write(reply + b"\n")
            await writer.drain()
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        writer.close()
        log(f"客户端断开 {peer}")


async def time_pusher(link: ClockLink) -> None:
    while True:
        await asyncio.sleep(5)
        if link.connected and time.time() - link.last_time_push >= TIME_PUSH_SECONDS:
            try:
                await link.push_time()
            except Exception as exc:  # noqa: BLE001
                log(f"推时间失败: {exc}")


async def main() -> None:
    link = ClockLink()
    asyncio.create_task(link.connect_forever())
    asyncio.create_task(time_pusher(link))

    server = await asyncio.start_server(
        lambda r, w: serve_client(link, r, w), LISTEN_HOST, LISTEN_PORT
    )
    log(f"本机接口 {LISTEN_HOST}:{LISTEN_PORT}，目标 BLE 设备 {DEVICE_NAME}")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        sys.exit(0)
