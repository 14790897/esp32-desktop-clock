# ESP32-C3 桌面时钟

合宙 AirM2M Core ESP32-C3 + ST7789 240×240 方屏做的桌面小时钟。

WiFi 自动对时、大号数字表盘、中文定时提醒，按 BOOT 键还能播一段 GIF。

## 功能

- **NTP 自动对时** —— 上电连 WiFi 后取网络时间，之后走内部 RTC；断网也继续走时
- **大号数字表盘** —— `HH:MM` 自适应缩放铺满屏宽，下方是秒和日期
- **中文定时提醒** —— 到点整屏显示提醒语，默认 11:59 提醒吃饭、17:59 提醒下班
- **BOOT 键播 GIF** —— 按一下播一遍，播完自动回到时钟
- **Claude 状态回显** —— 顶部一行实时滚动 Claude Code 的输出，颜色区分进行中/完成/出错
- **屏幕旋转可配** —— 改 `platformio.ini` 里的 `SCREEN_ROTATION` 即可

## 硬件接线

实测引脚如下（注意：这与 [gallery-pro](https://github.com/14790897/gallery-pro) 的 `env:airm2m_core_esp32c3_240x240` 原文**不一致**，DC/CS/RST 三个脚是轮换关系，照抄 gallery-pro 会黑屏）：

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

首次烧录后如果屏幕顶部在 `连热点` / `esp32-clock-XXXX` / `浏览器打开` / `192.168.4.1` 之间轮播，是正常的 —— 还没配 WiFi，见下节。

### 依赖

第三方库合计约 158MB，**未纳入版本库**。克隆后需要自行下载到 `lib/`：

| 目录 | 来源 | 版本 |
|---|---|---|
| `lib/LovyanGFX` | https://github.com/lovyan03/LovyanGFX | 1.2.31 |
| `lib/AnimatedGIF` | https://github.com/bitbank2/AnimatedGIF | 2.2.0 |
| `lib/esp_littlefs` | https://github.com/joltwallet/esp_littlefs | v1.22.3 |
| `lib/esp_button` | https://github.com/espressif/esp-iot-solution 的 `components/button` | 4.2.1 |
| `lib/esp-wifi-connect` | https://github.com/78/esp-wifi-connect | v3.3.1 |

两个容易踩的坑：

- `lib/LovyanGFX` 必须包含 `src/lgfx/Fonts/` 下的字体（中文靠自带的 efontCN）
- `lib/esp_littlefs` 的 **git 子模块 `src/littlefs/` 在 GitHub tarball 里是空目录**，需额外下载 https://github.com/littlefs-project/littlefs

**下载后还有几处本地改动要重做**（清单写在 `platformio.ini` 顶部注释里）：给 LovyanGFX 加 `srcFilter` 排除未编译的日韩台湾字体、给 AnimatedGIF 加 ESP-IDF 分支、给 esp_littlefs 排除仅 IDF 6+ 需要的源文件等。不照做的话编译不过或构建极慢。

`lib/esp-wifi-connect` 还有一处必改：原版用 CMake `EMBED_TXTFILES` 把配网页打进固件，而 **PlatformIO 不处理组件自带的 CMakeLists**，那两个 `_binary_*_start` 符号不会生成。已改成从 LittleFS 分块读（见 `wifi_configuration_ap.cc` 里的 `send_html_file()` 与 `WIFI_AP_INDEX_HTML`）。

## 配置

日常只需要改两个文件：

**`include/clock_config.h`** —— 对时与配网（不含隐私，已进版本库）

```cpp
#define NTP_SERVER_1   "203.107.6.88"     // ntp.aliyun.com
#define NTP_SERVER_2   "162.159.200.123"  // cn.pool.ntp.org
#define TZ_INFO        "CST-8"            // 东八区
#define WIFI_AP_PREFIX "esp32-clock"      // 配网热点名前缀
#define WIFI_HOSTNAME  "esp32-clock"      // 向路由器注册的主机名
```

> NTP 服务器写的是**IP 不是域名**：Windows 移动热点之类的网络里 DNS 代理常常不应答非 Windows 客户端（实测 `getaddrinfo` 返回 `EAI_FAIL`），而 SNTP 解析失败时**不报错**，只会静默地永远同步不上。

**`include/reminders.h`** —— 定时提醒

```cpp
static const Reminder kReminders[] = {
    {11, 59, "该吃饭了"},
    {17, 59, "该下班了"},
};

#define REMINDER_SHOW_SECONDS 60
```

## WiFi 配网

**WiFi 账号密码不写死在代码里**，设备自己记在 NVS 里（用 [esp-wifi-connect](https://github.com/78/esp-wifi-connect) 的配网门户，MIT）。

首次上电，或换到一个没记录过的网络：

1. 设备连不上就自己开热点，名字形如 `esp32-clock-5745`，无密码
2. 屏幕顶部显示热点名 —— 手机连上这个热点
3. 浏览器打开 **http://192.168.4.1**（页面自带 DNS 劫持，多数手机会自动弹出）
4. 页面里选一个 2.4G 网络、输密码、保存（ESP32-C3 不支持 5G）

连上后屏幕顶部会在 **`本机IP` / 拿到的 IP** 之间轮播 —— 这个地址要填给 Claude hook 用（见下）。Claude 一开始输出就会把它顶掉。

之后每次开机直接连，不再进配网。想换网络：把设备拿到没有旧网络的地方开机，连不上 90 秒后会自动重新进配网模式。

> 屏幕上所有提示都是**拆成短帧轮播**的，不是一整句 —— 字幕行只有 240px、字号 24px，放不下十来个字，而且是尾巴对齐的，超宽只会从左边把开头裁掉（写成一整句时，屏幕最后只剩个光秃秃的 `http://192.168.4.1`）。见 `showHint()` / `cycleHint()`。

> 配网页面本身也存在 LittleFS 里（`data/wifi_configuration*.html`），改完要跑 `pio run -t uploadfs` 才生效。

> **想换到「本机自己开的热点」上时有个死循环**：要访问 `192.168.4.1` 得先关掉本机热点（否则无线网卡不能同时作客户端），可时钟又必须在提交那一刻热点已经开着。这时别走门户，直接写 NVS：造一份 `ssid`/`password` 的 CSV，用 IDF 的 `nvs_partition_gen.py` 生成镜像，刷到 `0x9000`（分区表里 nvs 区，大小 `0x6000`）。凭据格式见 `lib/esp-wifi-connect/ssid_manager.cc`（命名空间 `wifi`）。清凭据回配网模式：`esptool erase_region 0x9000 0x6000`。

## 换 GIF

把 GIF 放进 `data/meme.gif` 后执行 `pio run -t uploadfs`。

建议先把 GIF **裁成 240×240**：固件运行时只取中间 240 列居中显示，320 宽的图两侧永远看不到，裁掉能省下约 1/5 的体积。

> 注意：这个 GIF 带透明通道、且用的是「透明 = 沿用上一帧」的帧间增量压缩。**重新编码时不要展开成完整帧**，那会丢掉主要的压缩来源（实测体积不减反增）。

## Claude 状态回显（可选）

屏幕顶部那行可以实时显示 Claude Code 正在输出的内容，并用颜色表示状态：

| 颜色 | 含义 |
|---|---|
| 琥珀 | Claude 正在回答 |
| 绿色 | 回答完成 |
| 红色 | 出错中断 |
| 灰色 | 还没收到过输出 |

显示方式是**尾巴对齐** —— 最新文字永远贴在右边缘，旧文字被顶出左侧。这里刻意不用
匀速跑马灯：Claude 的生成速度是每秒几十字，而可读的滚动速度只有约 6 字/秒，匀速滚
必然越积越滞后。

原理是 Claude Code 的 `MessageDisplay` 钩子在回答流式输出时**逐批**触发，一个 Python
脚本把新增文字 POST 给时钟。`Stop` 时再用完整文本兜底替换一次并标记完成。

### 启用步骤

**1. 拿到时钟的 IP。** 设备配好网后，**屏幕顶部会轮播「本机IP」和它拿到的 IP**，记下来。
（没看到就断电重开，或者去路由器后台按主机名 `esp32-clock` 查。）

**2. 建 hook 配置。** 模板在 `tools/claude-hooks.example.json`，两种装法：

**只在本项目生效**（模板默认按这个写，用 `${CLAUDE_PROJECT_DIR}` 定位脚本）：

```bash
cp tools/claude-hooks.example.json .claude/settings.local.json
```

**在任意项目都生效**（时钟放桌上时更实用 —— 你在别的仓库干活它也照样显示）。
把整段 `hooks` 合并进 `~/.claude/settings.json`，并把脚本路径改成**绝对路径**
（全局配置里 `${CLAUDE_PROJECT_DIR}` 会展开成当前项目，不是这个项目）：

```json
{ "hooks": { "MessageDisplay": [
  { "hooks": [ { "type": "command", "command": "python",
      "args": ["C:/git-program/esp32-clock/tools/claude-status-hook.py"] } ] } ] } }
```

> **两种装法只能选一个。** 同时配会让每轮推送两次。

> 项目级文件被 `.gitignore` 排除，不会随仓库分发 —— 别人克隆后不会平白多出一个
> 指向不存在设备的本地服务。要启用就得显式装一次。

**3. 填时钟地址**（第 1 步里记下的 IP），设环境变量：

```bash
CLAUDE_CLOCK_URL=http://192.168.1.77
```

**4. 重启 Claude Code 会话** —— hook 配置在会话启动时加载，改完当前会话不生效。

### 手动测试

不用等 Claude，直接 curl 就能验证链路（把 IP 换成时钟实际地址）：

```bash
curl -X POST -d '测试文字' http://192.168.1.77/say      # 整体替换
curl -X POST -d '追加的文字' http://192.168.1.77/append  # 追加（实时回显用）
curl -X POST -d working   http://192.168.1.77/state     # 状态：working/done/failed
```

### 注意

- `MessageDisplay` 是**同步**钩子，Claude 会等它返回，所以脚本的超时设得很短（0.6 秒）。
  **不要把它改成 `async: true`** —— 异步会让批次乱序到达，字幕就乱了。
- 脚本带 30 秒熔断：时钟不在线时自动跳过，不会每次都等超时拖慢对话。
- 时钟不在线时不会有任何提示，静默忽略。这是设计如此 —— 它不该干扰正常使用。

## 测试

`tests/` 里是**打真机**的 E2E 测试 —— 设备得先连上网、PC 能访问到它，地址从
`CLAUDE_CLOCK_URL` 取（和 hook 同一个变量），没设就整体跳过。

```bash
pip install -r tests/requirements.txt
set CLAUDE_CLOCK_URL=http://192.168.137.12
set ESSPORT=COM5
pytest -v
```

串口号也可以在 `pytest.ini` 里固定；不给的话 pytest-embedded 会自己探测。

三层：

| 文件 | 打什么 | 层 |
|---|---|---|
| `test_boot.py` | 复位真机，断言启动走到联网对时（官方 pytest-embedded 的 `dut` 夹具） | 串口 |
| `test_http.py` | `/say` `/append` `/state` 的**行为**，不只是状态码 | HTTP |
| `test_provision.py` | 配网全流程：擦凭据 → 进配网 → 客户端拿 DHCP → 提示不超宽 | 串口+HTTP |

**`test_provision.py` 默认不跑**，它有副作用（擦设备凭据、切 PC 网卡），要显式开：

```bash
set CLOCK_E2E_PROVISION=1
pytest tests/test_provision.py -v
```

它会先把 nvs 分区备份出来、测完写回，所以设备的 WiFi 凭据不会丢。前提是 **PC 的
移动热点必须关着** —— 热点开着时无线网卡无法作为客户端连设备热点，测试会检查并跳过。

> 串口断言只能用 **ASCII**：pexpect 的 encoding 默认是 None，中文模式会被按 ASCII
> 编码直接抛 `UnicodeEncodeError`。好在每条关键日志都有 ASCII 部分（`lcd ready`、
> `wifi up, ssid=`、`time synced:`）。固件里那条提示超宽的告警也因此带了
> `hint-frame-clipped` 前缀，给测试做机器可读标记。

**测不到的**：屏幕像素。渲染回归（「3 数字有丝线」那类）目前只能靠肉眼，
要进一步得让 `drawTicker` 能把 sprite dump 成 PPM 通过 HTTP 取回。

## 已知限制

- **提醒文字只能用 efontCN_24 覆盖到的汉字**，且字形固定、不可缩放。用了字库外的生僻字会显示空白。
- **中文点阵字库占 554KB flash**，所以分区表用了自定义的 `partitions.csv`（app 区 2MB），默认分区表 1MB 装不下。
- **GIF 播放需要 115KB 帧缓冲**，且必须 DMA 可访问 —— 实测堆里凑不出连续的 115200 字节，所以拆成了上下两块各 57600 字节。**配网模式下会主动把它还给堆**，见下条。
- **配网时 lwIP 发不出 DHCP 应答（会让客户端卡在「获取 IP 地址」）**，根因是内存不够：开机可用堆只有 107KB 左右，而 115KB 的 GIF 帧缓冲一开机就常驻、配网时毫无用处，lwIP 发 ACK 时分配不到内存就返回 `-1 (ERR_MEM)`，客户端永远等不到应答。修法是进配网前先 `gifFreeFramebuffer()`，按 BOOT 键播放时再自动分配。另外 `sdkconfig.defaults` 里关掉了 `CONFIG_LWIP_DHCPS_STATIC_ENTRIES`（让 DHCP 服务器一律广播，不依赖 ARP 表）。诊断这段需要把 IDF 源码 `components/lwip/apps/dhcpserver/dhcpserver.c` 里的 `DHCPS_DEBUG` 改成 1 重编 —— 注意它**不用 `ESP_LOG`**，默认构建下 60 条调试输出全是死的。
- GIF 画面与时钟不能同时显示，播放期间时钟暂停刷新。
- **配网页面不支持设静态 IP**（esp-wifi-connect 的 Advanced 页只有 OTA 地址、发射功率、记住 BSSID 三项），所以设备走 DHCP。不过 `sdkconfig.defaults` 里开了 `CONFIG_LWIP_DHCP_RESTORE_LAST_IP`：客户端会把上次拿到的地址存进 nvs，重连时**主动要回同一个**（实测重启两次地址不变），要不到才退回正常 DHCP。所以 `CLAUDE_CLOCK_URL` 在一次配网后是稳定的，且换网络照常能用 —— 比写死静态 IP 好，后者会让设备一到别的网络就联不上。

## 参考资料

硬件配置参考了 [14790897/gallery-pro](https://github.com/14790897/gallery-pro)，但引脚以本仓库实测值为准。
