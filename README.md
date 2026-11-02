# ESP32-C3 桌面时钟

合宙 AirM2M Core ESP32-C3 + ST7789 240×240 方屏做的桌面小时钟。

**通信全走蓝牙**：PC 连上它推时间、推 Claude Code 的输出。没有 WiFi，也就没有配网、DHCP、IP 地址漂移这一整套问题。

> **代价**：蓝牙上不了互联网，所以时钟的时间来自 PC。PC 关机或不在蓝牙范围内时，时钟靠内部 RTC 继续走（会慢慢漂），PC 一回来就自动校正。

## 功能

- **时间由 PC 推送** —— 连上时推一次，之后每 5 分钟一次
- **大号数字表盘** —— `HH:MM` 自适应缩放铺满屏宽，下方是秒和日期
- **中文定时提醒** —— 到点整屏显示提醒语，默认 11:59 提醒吃饭、17:59 提醒下班
- **BOOT 键播 GIF** —— 按一下播一遍，播完自动回到时钟
- **Claude 状态回显** —— 顶部一行实时滚动 Claude Code 的输出，颜色区分进行中/完成/出错
- **屏幕旋转可配** —— 改 `platformio.ini` 里的 `SCREEN_ROTATION` 即可

## 硬件接线

实测引脚如下（注意：常见的 `airm2m_core_esp32c3_240x240` 配置写的是 **DC=6 / CS=7 / RST=10**，与本机实测**不一致** —— 这三个脚是轮换关系，照抄那套会黑屏）：

| 屏幕 | ESP32-C3 |
|---|---|
| SCL / SCK | GPIO2 |
| SDA / MOSI | GPIO3 |
| CS | GPIO6 |
| DC | GPIO10 |
| RES / RST | GPIO7 |
| VCC | 3.3V |
| GND | GND |
| BLK / LED | 不接（背光由硬件处理，软件不要驱动 GPIO11） |

另外用到的引脚：

- **BOOT 键** = GPIO9（内部上拉，按下为低）
- **板载指示灯** = GPIO12/13，代码里拉低关掉

> GPIO12/13 在 ESP32-C3 上默认是内部 flash 的 SPIHD/SPIWP。本板 flash 跑 **DIO 模式**（启动日志 `SPI Mode : DIO`），只用两根数据线，所以这两个脚空闲可用。**若把 flash 改成 QIO 模式，必须去掉 `initBoardLeds()`**，否则会直接崩。

## 编译烧录

```bash
pio run -t upload      # 编译并烧固件
pio run -t uploadfs    # 把 data/ 打包成 LittleFS 镜像烧进去（首次必须执行）
pio monitor            # 看串口日志
```

烧完后设备会广播名为 **`esp32-clock`** 的 BLE 外设。这时屏幕显示 `waiting for BLE...`，等 PC 连上推时间。

### 依赖

第三方库合计约 158MB，**未纳入版本库**。克隆后需要自行下载到 `lib/`：

| 目录 | 来源 | 版本 |
|---|---|---|
| `lib/LovyanGFX` | https://github.com/lovyan03/LovyanGFX | 1.2.31 |
| `lib/AnimatedGIF` | https://github.com/bitbank2/AnimatedGIF | 2.2.0 |
| `lib/esp_littlefs` | https://github.com/joltwallet/esp_littlefs | v1.22.3 |
| `lib/esp_button` | https://github.com/espressif/esp-iot-solution 的 `components/button` | 4.2.1 |

两个容易踩的坑：

- `lib/LovyanGFX` 必须包含 `src/lgfx/Fonts/` 下的字体（中文靠自带的 efontCN）
- `lib/esp_littlefs` 的 **git 子模块 `src/littlefs/` 在 GitHub tarball 里是空目录**，需额外下载 https://github.com/littlefs-project/littlefs

**下载后还有几处本地改动要重做**（清单写在 `platformio.ini` 顶部注释里）：给 LovyanGFX 加 `srcFilter` 排除未编译的日韩台湾字体、给 AnimatedGIF 加 ESP-IDF 分支、给 esp_littlefs 排除仅 IDF 6+ 需要的源文件等。不照做的话编译不过或构建极慢。

## 配置

**`include/clock_config.h`** —— 只放不含隐私的配置，已进版本库

```cpp
#define TZ_INFO "CST-8"   // POSIX 时区，东八区。只影响 localtime_r 的换算
```

时间本身不在这里配 —— 由 PC 通过蓝牙推送。

**`include/reminders.h`** —— 定时提醒

```cpp
static const Reminder kReminders[] = {
    {11, 59, "该吃饭了"},
    {17, 59, "该下班了"},
};

#define REMINDER_SHOW_SECONDS 60
```

**`sdkconfig.defaults`** —— 蓝牙/无线配置

```ini
# CONFIG_ESP_WIFI_ENABLED is not set   # 不用 WiFi，省 flash 和 RAM
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=247
```

> lwIP 不能一起关：`esp-tls` 等组件依赖它的 `sys/socket.h`，关掉会编译失败。没有 netif 在用，它基本只是躺在那儿。

## 连接与时间同步

设备是 BLE 外设，广播名 `esp32-clock`。PC 侧跑一个常驻守护进程：

```bash
pip install bleak
python tools/clock-ble-daemon.py
```

它做三件事：

1. 扫描并连上 `esp32-clock`（断了自动重连）
2. 连上时推一次时间，之后每 5 分钟一次
3. 在 `127.0.0.1:8765` 上收本机命令，转发到 BLE

**为什么需要守护进程**：Claude 的 `MessageDisplay` 钩子在关键路径上，超时预算只有 0.6 秒；而 BLE 建连要一两秒。所以钩子只往本机 socket 写一行，真正的连接由守护进程长期持有。

### 开机自启

守护进程不开机自启的话，每次重启电脑都要手动跑一次。放一个快捷方式到启动文件夹即可（不需要管理员）：

```
Win+R → shell:startup → 在里面新建快捷方式，目标：
pythonw.exe "C:\git-program\esp32-clock\tools\clock-ble-daemon.py"
```

## BLE 协议

服务和特征都是 128 位 UUID（`BLE_UUID128_INIT` 是小端，见 `src/ble_clock.cpp` 顶部的说明）：

```
服务   7d9f0001-5a3e-4c1b-9e21-6b0c4c4f4b00
  ├─ CMD    7d9f0002-…   write        指令
  └─ STATUS 7d9f0003-…   read/notify  当前字幕状态（JSON）
```

CMD 负载是 `[opcode][body]`：

| op | body | 作用 |
|---|---|---|
| `A` | 文本 | 追加（实时回显） |
| `R` | 文本 | 整体替换 |
| `S` | `w`/`d`/`f` | 工作中 / 完成 / 失败 |
| `T` | 4 字节小端 Unix epoch | 设置时间 |
| `I` | 会话 ID | 换会话时清空字幕 |

长文本分块发送，每块都带前缀；MTU 协商到 247 时每块约 200 字节。

## 换 GIF

把 GIF 放进 `data/meme.gif` 后执行 `pio run -t uploadfs`。

建议先把 GIF **裁成 240×240**：固件运行时只取中间 240 列居中显示，320 宽的图两侧永远看不到，裁掉能省下约 1/5 的体积。

> 注意：这个 GIF 带透明通道、且用的是「透明 = 沿用上一帧」的帧间增量压缩。**重新编码时不要展开成完整帧**，那会丢掉主要的压缩来源（实测体积不减反增）。

## Claude 状态回显（可选）

原理是 Claude Code 的 `MessageDisplay` 钩子在回答流式输出时**逐批**触发，脚本把新增文字发给守护进程，后者经 BLE 送到时钟。`Stop` 时再用完整文本兜底替换一次并标记完成。

### 启用步骤

**1. 让守护进程跑起来**（见上节）。

**2. 建 hook 配置。** 模板在 `tools/claude-hooks.example.json`，两种装法：

**只在本项目生效**（模板默认按这个写，用 `${CLAUDE_PROJECT_DIR}` 定位脚本）：

```bash
cp tools/claude-hooks.example.json .claude/settings.local.json
```

**在任意项目都生效**（时钟放桌上时更实用）。把整段 `hooks` 合并进 `~/.claude/settings.json`，并把脚本路径改成**绝对路径**（全局配置里 `${CLAUDE_PROJECT_DIR}` 会展开成当前项目，不是这个项目）：

```json
{ "hooks": { "MessageDisplay": [
  { "hooks": [ { "type": "command", "command": "python",
      "args": ["C:/git-program/esp32-clock/tools/claude-status-hook.py"] } ] } ] } }
```

> **两种装法只能选一个。** 同时配会让每轮推送两次。

**3. 重启 Claude Code 会话** —— hook 配置在会话启动时加载。

### 手动测试

不用等 Claude，直接往守护进程发命令就能验证链路：

```bash
echo "R测试文字" | python -c "import socket,sys;s=socket.create_connection(('127.0.0.1',8765));s.sendall(sys.stdin.buffer.read());print(s.recv(64))"
```

## 测试

`tests/` 里是**打真机**的 E2E 测试。

```bash
pip install -r tests/requirements.txt
set ESSPORT=COM5
pytest -v
```

| 文件 | 打什么 |
|---|---|
| `test_boot.py` | 复位真机，断言启动走到蓝牙开始广播（官方 pytest-embedded 的 `dut` 夹具） |
| `test_ble.py` | 通过守护进程发指令，读回设备状态断言（文本、状态、会话、时间） |

守护进程没跑或设备不可达时整体跳过，不会红一片。

> 串口断言只能用 **ASCII**：pexpect 的 encoding 默认是 None，中文模式会被按 ASCII 编码直接抛 `UnicodeEncodeError`。好在关键日志都有 ASCII 部分（`lcd ready`、`advertise`）。

**测不到的**：屏幕像素。渲染回归（「3 数字有丝线」那类）目前只能靠肉眼。

## 已知限制

- **提醒文字只能用 efontCN_24 覆盖到的汉字**，且字形固定、不可缩放。用了字库外的生僻字会显示空白。
- **中文点阵字库占 554KB flash**，所以分区表用了自定义的 `partitions.csv`（app 区 2MB），默认分区表 1MB 装不下。
- **GIF 播放需要 115KB 帧缓冲**，且必须 DMA 可访问 —— 实测堆里凑不出连续的 115200 字节，所以拆成了上下两块各 57600 字节。
- GIF 画面与时钟不能同时显示，播放期间时钟暂停刷新。
- **时间依赖 PC**：蓝牙不通互联网。PC 关机时靠内部 RTC 走，几小时会漂几秒到几十秒，重连后自动校正。
- **蓝牙范围约 10 米**，隔墙会明显缩水。时钟和 PC 最好在同一房间。
- **PC 必须开着蓝牙**。适配器存在但无线电关着时，守护进程会连不上（`Bluetooth radio is not powered on`）。

## 参考资料

- [espressif/esp-idf#12580](https://github.com/espressif/esp-idf/issues/12580) —— 早期 WiFi 版本踩过的软 AP DHCP 缺陷记录。现在这条路径已经整个删掉了，留作背景。
- 引脚与面板参数**均为本机实测**，不是照抄现成配置 —— 见「硬件接线」开头那条警告。
