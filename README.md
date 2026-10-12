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

**1. 让时钟连上网络。** 固件默认用静态 IP `192.168.137.50`（适配 Windows 移动热点的
ICS 网段）。如果你的网络不是这个网段，把 `src/main.cpp` 里的 `#define CLOCK_USE_STATIC_IP`
注释掉退回 DHCP，烧录后从串口日志读实际 IP。

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

**3. 时钟地址不是默认值时**，设环境变量：

```bash
CLAUDE_CLOCK_URL=http://192.168.1.77
```

**4. 重启 Claude Code 会话** —— hook 配置在会话启动时加载，改完当前会话不生效。

### 手动测试

不用等 Claude，直接 curl 就能验证链路：

```bash
curl -X POST -d '测试文字' http://192.168.137.50/say      # 整体替换
curl -X POST -d '追加的文字' http://192.168.137.50/append  # 追加（实时回显用）
curl -X POST -d working   http://192.168.137.50/state     # 状态：working/done/failed
```

### 注意

- `MessageDisplay` 是**同步**钩子，Claude 会等它返回，所以脚本的超时设得很短（0.6 秒）。
  **不要把它改成 `async: true`** —— 异步会让批次乱序到达，字幕就乱了。
- 脚本带 30 秒熔断：时钟不在线时自动跳过，不会每次都等超时拖慢对话。
- 时钟不在线时不会有任何提示，静默忽略。这是设计如此 —— 它不该干扰正常使用。

## 已知限制

- **提醒文字只能用 efontCN_24 覆盖到的汉字**，且字形固定、不可缩放。用了字库外的生僻字会显示空白。
- **中文点阵字库占 554KB flash**，所以分区表用了自定义的 `partitions.csv`（app 区 2MB），默认分区表 1MB 装不下。
- **GIF 播放需要 115KB 帧缓冲**，且必须 DMA 可访问 —— 实测堆里凑不出连续的 115200 字节，所以拆成了上下两块各 57600 字节。
- GIF 画面与时钟不能同时显示，播放期间时钟暂停刷新。

## 参考资料

硬件配置参考了 [14790897/gallery-pro](https://github.com/14790897/gallery-pro)，但引脚以本仓库实测值为准。
