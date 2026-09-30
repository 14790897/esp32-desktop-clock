# ESP32-C3 桌面时钟

合宙 AirM2M Core ESP32-C3 + ST7789 240×240 方屏做的桌面小时钟。

WiFi 自动对时、大号数字表盘、中文定时提醒，按 BOOT 键还能播一段 GIF。

## 功能

- **NTP 自动对时** —— 上电连 WiFi 后取网络时间，之后走内部 RTC；断网也继续走时
- **大号数字表盘** —— `HH:MM` 自适应缩放铺满屏宽，下方是秒和日期
- **中文定时提醒** —— 到点整屏显示提醒语，默认 11:59 提醒吃饭、17:59 提醒下班
- **BOOT 键播 GIF** —— 按一下播一遍，播完自动回到时钟
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

首次烧录后如果屏幕一直显示 `connecting wifi...`，是正常的 —— 还没配 WiFi，见下节。

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

日常只需要改两个文件：

**`include/secrets.h`** —— WiFi 与对时（该文件已被 `.gitignore` 排除，不会进版本库）

```cpp
#define WIFI_SSID       "你的WiFi名称"
#define WIFI_PASSWORD   "你的WiFi密码"
#define NTP_SERVER_1    "203.107.6.88"   // ntp.aliyun.com
#define TZ_INFO         "CST-8"
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

## 换 GIF

把 GIF 放进 `data/meme.gif` 后执行 `pio run -t uploadfs`。

建议先把 GIF **裁成 240×240**：固件运行时只取中间 240 列居中显示，320 宽的图两侧永远看不到，裁掉能省下约 1/5 的体积。

> 注意：这个 GIF 带透明通道、且用的是「透明 = 沿用上一帧」的帧间增量压缩。**重新编码时不要展开成完整帧**，那会丢掉主要的压缩来源（实测体积不减反增）。

## 已知限制

- **提醒文字只能用 efontCN_24 覆盖到的汉字**，且字形固定、不可缩放。用了字库外的生僻字会显示空白。
- **中文点阵字库占 554KB flash**，所以分区表用了自定义的 `partitions.csv`（app 区 2MB），默认分区表 1MB 装不下。
- **GIF 播放需要 115KB 帧缓冲**，且必须 DMA 可访问 —— 实测堆里凑不出连续的 115200 字节，所以拆成了上下两块各 57600 字节。
- GIF 画面与时钟不能同时显示，播放期间时钟暂停刷新。

## 参考资料

硬件配置参考了 [14790897/gallery-pro](https://github.com/14790897/gallery-pro)，但引脚以本仓库实测值为准。
