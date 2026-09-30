#include <LovyanGFX.hpp>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <lgfx/Fonts/efont/lgfx_efont_cn.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <cstring>
#include <ctime>

#include "gif_player.h"
#include "button_gpio.h"
#include "iot_button.h"
#include "reminders.h"
#include "secrets.h"

static const char* kTag = "clock";

namespace {

constexpr int W = SCREEN_WIDTH;
constexpr int H = SCREEN_HEIGHT;

// ==================== 显示 ====================
// ST7789 240x240，总线与面板参数沿用 gallery-pro 的实测配置
class ClockDisplay : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel_;
  lgfx::Bus_SPI bus_;

public:
  ClockDisplay() {
    auto b = bus_.config();
    b.freq_write = 40000000;
    // 启用 DMA：整屏推送 240x240x2=115KB，走 DMA 免得 CPU 全程轮询
    b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = TFT_SCLK;
    b.pin_mosi = TFT_MOSI;
    b.pin_miso = TFT_MISO;
    b.pin_dc = TFT_DC;
    bus_.config(b);
    panel_.setBus(&bus_);

    auto p = panel_.config();
    p.pin_cs = TFT_CS;
    p.pin_rst = TFT_RST;
    p.panel_width = W;
    p.panel_height = H;
    p.offset_x = 0;
    p.offset_y = 0;
    p.offset_rotation = 0;
    p.dummy_read_pixel = 8;
    p.dummy_read_bits = 1;
    p.readable = false;
    p.invert = true;
    p.rgb_order = true;
    p.dlen_16bit = false;
    p.bus_shared = true;
    panel_.config(p);

    setPanel(&panel_);
  }
};

ClockDisplay lcd;

// ==================== 配色 (RGB565) ====================
constexpr uint16_t COL_BG = 0x0000;
constexpr uint16_t COL_TIME = 0xFFFF;
constexpr uint16_t COL_SECOND = 0x8410;
constexpr uint16_t COL_DATE = 0xC618;
constexpr uint16_t COL_OK = 0x07E0;
constexpr uint16_t COL_WAIT = 0xFD20;

// ==================== 版面 ====================
constexpr int Y_STATUS = 10;
constexpr int H_STATUS = 20;
constexpr int Y_TIME = 44;
constexpr int H_TIME = 84;
constexpr int Y_SECOND = 138;
constexpr int H_SECOND = 30;
constexpr int Y_DATE = 184;
constexpr int H_DATE = 26;

// 上一帧画过的内容，用于比对出变化的字位
char gPrevTime[8] = {};
char gPrevSecond[8] = {};

// 清空比对缓存，强制下一次整行重画（切回时钟、GIF 播完等场景用）
void invalidateClockText() {
  memset(gPrevTime, 0, sizeof(gPrevTime));
  memset(gPrevSecond, 0, sizeof(gPrevSecond));
}

// 定长字符串按固定字位绘制，且只重画真正变化的字位。
//
// 原来每次都是「整块 fillRect 清空 → 重绘」，清完到画完之间有肉眼可见的黑屏，
// 每走一秒闪一次。改成按字位比对后，每秒只动一个字符，每分钟只动一两个。
//
// 固定字位还顺带解决另一个问题：FreeSansBold24pt7b 是比例字体，原先用
// drawCenterString 按实际宽度居中，"11:11" 比 "00:00" 窄，位置会左右抖。
// 现在字位宽度固定，数字变化时不会移动。
//
// 字位宽度必须取「字符集里最宽的字形」，不能取整串平均值 —— 比例字体里冒号
// 远窄于数字，平均值会小于数字实际宽度，数字就溢出到相邻字位，被对方的清屏
// 切掉一两列，屏幕上表现为数字边缘出现一道细线。
//
// targetW > 0 时把整串缩放到该宽度，否则用字体原始尺寸。
void drawSlotString(const lgfx::IFont* font, const char* text, const char* charset, int y,
                    int h, uint16_t fg, int targetW, char* prev) {
  const int len = static_cast<int>(strlen(text));
  if (len <= 0) return;

  lcd.setFont(font);
  lcd.setTextSize(1.0f);

  int32_t advance = 0;  // 字符集内最宽的字形推进量
  for (const char* p = charset; *p != '\0'; ++p) {
    const char ch[2] = {*p, '\0'};
    const int32_t w = lcd.textWidth(ch);
    if (w > advance) advance = w;
  }
  if (advance <= 0) return;

  int slotW = advance;
  if (targetW > 0) {
    const float scale = static_cast<float>(targetW) / static_cast<float>(advance * len);
    lcd.setTextSize(scale, scale);
    slotW = static_cast<int>(static_cast<float>(advance) * scale);
  }
  if (slotW <= 0) return;
  const int x0 = (W - slotW * len) / 2;

  lcd.setTextColor(fg);
  for (int i = 0; i < len; ++i) {
    if (text[i] == prev[i]) continue;

    const int x = x0 + i * slotW;
    lcd.fillRect(x, y, slotW, h, COL_BG);

    // 每个字符在自己的字位里居中，冒号这类窄字符才不会挤向一侧
    const char ch[2] = {text[i], '\0'};
    const int glyphW = lcd.textWidth(ch);
    lcd.drawString(ch, x + (slotW - glyphW) / 2, y);

    prev[i] = text[i];
  }
}

void drawStatus(uint16_t color, const char* msg) {
  lcd.fillRect(0, Y_STATUS, W, H_STATUS, COL_BG);
  if (msg) {
    lcd.setFont(&fonts::FreeSans9pt7b);
    lcd.setTextSize(1.0f);
    lcd.setTextColor(color, COL_BG);
    lcd.drawCenterString(msg, W / 2, Y_STATUS);
  } else {
    lcd.fillCircle(W / 2, Y_STATUS + H_STATUS / 2, 4, color);
  }
}

void drawTime(int hour, int minute) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
  drawSlotString(&fonts::FreeSansBold24pt7b, buf, "0123456789:", Y_TIME, H_TIME, COL_TIME,
                 W - 16, gPrevTime);
}

void drawSecond(int sec) {
  char buf[8];
  snprintf(buf, sizeof(buf), ":%02d", sec);
  drawSlotString(&fonts::FreeSans12pt7b, buf, "0123456789:", Y_SECOND, H_SECOND, COL_SECOND, 0,
                 gPrevSecond);
}

void drawDate(const struct tm& t) {
  static const char* kWeekday[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d  %s", t.tm_year + 1900, t.tm_mon + 1,
           t.tm_mday, kWeekday[t.tm_wday]);
  lcd.fillRect(0, Y_DATE, W, H_DATE, COL_BG);
  lcd.setFont(&fonts::FreeSans9pt7b);
  lcd.setTextSize(1.0f);
  lcd.setTextColor(COL_DATE, COL_BG);
  lcd.drawCenterString(buf, W / 2, Y_DATE);
}

// 提醒画面：整屏黑底，中文居中。中文走 efontCN 点阵字库（不可缩放）
void drawReminder(const char* text, const struct tm& t) {
  lcd.fillScreen(COL_BG);

  lcd.setFont(&fonts::efontCN_24);
  lcd.setTextSize(1.0f);
  lcd.setTextColor(COL_WAIT, COL_BG);
  lcd.drawCenterString(text, W / 2, H / 2 - 38);

  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  lcd.setFont(&fonts::FreeSans12pt7b);
  lcd.setTextColor(COL_SECOND, COL_BG);
  lcd.drawCenterString(buf, W / 2, H / 2 + 12);
}

// ==================== 联网与对时 ====================
enum class Net : uint8_t { Wifi, Syncing, Ready };

Net netState = Net::Wifi;
bool sntpStarted = false;
volatile bool reconnectPending = false;
constexpr uint32_t kRetryIntervalMs = 5000;

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

// NTP 成功前系统时间是 1970，用这个阈值区分
bool timeIsValid() { return time(nullptr) > 1600000000; }

bool wifiConnected() {
  wifi_ap_record_t ap;
  return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

// 关联成功不等于能上网，DHCP 拿到地址前启动 SNTP 会连不上服务器
bool wifiHasIp() {
  esp_netif_ip_info_t ip = {};
  if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip) != ESP_OK) {
    return false;
  }
  return ip.ip.addr != 0;
}

void onWifiEvent(void*, esp_event_base_t base, int32_t id, void*) {
  if (base != WIFI_EVENT) return;
  if (id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
    // 只置标志，实际重连交给 clockTask 限速执行，
    // 避免在握手进行中再次调用 esp_wifi_connect 把连接打断
    reconnectPending = true;
  }
}

bool wifiStart() {
  if (nvs_flash_init() != ESP_OK) return false;
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&initCfg));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, nullptr));

  wifi_config_t staCfg = {};
  snprintf(reinterpret_cast<char*>(staCfg.sta.ssid), sizeof(staCfg.sta.ssid), "%s", WIFI_SSID);
  snprintf(reinterpret_cast<char*>(staCfg.sta.password), sizeof(staCfg.sta.password), "%s",
           WIFI_PASSWORD);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &staCfg));
  ESP_ERROR_CHECK(esp_wifi_start());
  return true;
}

void startSntp() {
  // 先自己解析一次主机名：ICS 这类网络的 DNS 代理常常是 NTP 静默失败的根因，
  // 而 esp_netif_sntp 失败时不报错，只能靠这一步区分「DNS 挂了」和「UDP 不通」
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo* res = nullptr;
  const int rc = getaddrinfo(NTP_SERVER_1, "123", &hints, &res);
  if (rc == 0 && res != nullptr) {
    char ip[16] = {};
    inet_ntoa_r(reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr, ip, sizeof(ip));
    ESP_LOGI(kTag, "DNS ok: %s -> %s", NTP_SERVER_1, ip);
    freeaddrinfo(res);
  } else {
    ESP_LOGE(kTag, "DNS failed for %s (rc=%d)", NTP_SERVER_1, rc);
  }

  // 这台设备 sdkconfig 里 CONFIG_LWIP_SNTP_MAX_SERVERS=1，只能配一个服务器
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(NTP_SERVER_1);
  cfg.start = true;
  const esp_err_t err = esp_netif_sntp_init(&cfg);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "sntp init failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(kTag, "sntp started, server=%s", NTP_SERVER_1);
  }
  sntpStarted = true;
  netState = Net::Syncing;
  drawStatus(COL_WAIT, "syncing time...");
}

void serviceNetwork() {
  static uint32_t lastRetry = 0;

  if (wifiConnected()) {
    reconnectPending = false;
    if (!wifiHasIp()) return;  // 关联上了但 DHCP 还没下发地址，继续等
    if (!sntpStarted) {
      esp_netif_ip_info_t ip = {};
      esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
      ESP_LOGI(kTag, "wifi up, ip=" IPSTR, IP2STR(&ip.ip));
      startSntp();
      return;
    }
    if (netState == Net::Syncing) {
      if (timeIsValid()) {
        netState = Net::Ready;
        drawStatus(COL_OK, nullptr);
        const time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        ESP_LOGI(kTag, "time synced: %04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900,
                 t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
      } else {
        static uint32_t lastLog = 0;
        if (nowMs() - lastLog >= 5000) {
          lastLog = nowMs();
          ESP_LOGW(kTag, "still syncing, epoch=%lld", static_cast<long long>(time(nullptr)));
        }
      }
    }
    return;
  }

  if (netState != Net::Wifi) {
    netState = Net::Wifi;
    drawStatus(COL_WAIT, "wifi lost, retrying...");
    ESP_LOGW(kTag, "wifi disconnected");
  }
  if (reconnectPending && nowMs() - lastRetry >= kRetryIntervalMs) {
    reconnectPending = false;
    lastRetry = nowMs();
    ESP_LOGI(kTag, "reconnecting to %s", WIFI_SSID);
    esp_wifi_connect();
  }
}

// ==================== 存储与按键 ====================
// 按 BOOT（GPIO9，按下拉低）播放 SPIFFS 里的 GIF
constexpr gpio_num_t kBootPin = GPIO_NUM_9;
constexpr const char* kGifPath = "/littlefs/meme.gif";
constexpr const char* kStoragePartition = "storage";

bool gStorageReady = false;

void initStorage() {
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = "/littlefs";
  conf.partition_label = kStoragePartition;
  conf.format_if_mount_failed = false;
  conf.dont_mount = false;

  const esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "littlefs 挂载失败: %s (剩余堆 %u 字节)", esp_err_to_name(err),
             static_cast<unsigned>(esp_get_free_heap_size()));
    return;
  }
  size_t total = 0, used = 0;
  esp_littlefs_info(kStoragePartition, &total, &used);
  ESP_LOGI(kTag, "littlefs 就绪 %u/%u 字节", static_cast<unsigned>(used),
           static_cast<unsigned>(total));
  gStorageReady = true;
}

// BOOT 键交给 ESP-IDF 官方按键驱动 iot_button：去抖、单击/长按/连击判定
// 都由库负责，比自己写中断计数可靠，短按也不会漏。
// 回调在库的任务上下文里跑，这里只累加计数，实际动作交给 clockTask。
volatile uint32_t gButtonClicks = 0;

void onButtonSingleClick(void*, void*) {
  // volatile 上的 ++ 在 C++20 已废弃，显式读改写
  gButtonClicks = gButtonClicks + 1;
}

// 板子上 GPIO12/13 接着指示灯。这两脚在 ESP32-C3 上默认是内部 flash 的
// SPIHD/SPIWP，但本板 flash 跑在 DIO 模式（启动日志 "SPI Mode : DIO"，只用
// SPID/SPIQ 两根数据线），所以 12/13 空闲，可以当普通输出用。
// 拉低关灯；若板上是高电平点亮则说明接法不同，改成 1 即可。
void initBoardLeds() {
  gpio_config_t io = {};
  io.pin_bit_mask = (1ULL << 12) | (1ULL << 13);
  io.mode = GPIO_MODE_OUTPUT;
  gpio_config(&io);
  gpio_set_level(GPIO_NUM_12, 0);
  gpio_set_level(GPIO_NUM_13, 0);
  ESP_LOGI(kTag, "板载指示灯 gpio12/13 已拉低");
}

void initButton() {
  button_config_t cfg = {};
  cfg.long_press_time = 0;   // 0 = 用 Kconfig 默认值
  cfg.short_press_time = 0;

  button_gpio_config_t gpioCfg = {};
  gpioCfg.gpio_num = kBootPin;
  gpioCfg.active_level = 0;  // 按下为低电平
  gpioCfg.enable_power_save = false;
  gpioCfg.disable_pull = false;

  button_handle_t btn = nullptr;
  if (iot_button_new_gpio_device(&cfg, &gpioCfg, &btn) != ESP_OK) {
    ESP_LOGE(kTag, "BOOT 键初始化失败");
    return;
  }
  iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, nullptr, onButtonSingleClick, nullptr);
  ESP_LOGI(kTag, "BOOT 键 gpio%d 就绪（iot_button）", static_cast<int>(kBootPin));
}

// ==================== 定时提醒 ====================
const char* matchReminder(const struct tm& t) {
  for (const Reminder& r : kReminders) {
    if (r.hour == t.tm_hour && r.minute == t.tm_min) return r.text;
  }
  return nullptr;
}

void redrawStatusIndicator() {
  if (netState == Net::Ready) {
    drawStatus(COL_OK, nullptr);
  } else {
    drawStatus(COL_WAIT, "connecting wifi...");
  }
}

// ==================== 主循环 ====================
void clockTask(void*) {
  setenv("TZ", TZ_INFO, 1);
  tzset();

  int lastHour = -1, lastMinute = -1, lastSecond = -1, lastDay = -1;
  int lastCheckedMinute = -1;
  bool reminderActive = false;
  uint32_t reminderUntil = 0;

  for (;;) {
    serviceNetwork();

    // 只要对过时就走内部 RTC，断网也继续显示
    if (!timeIsValid()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);

    // BOOT 键：库回调累加计数，这里消费
    static uint32_t handledClicks = 0;
    if (gButtonClicks != handledClicks) {
      handledClicks = gButtonClicks;
      if (gStorageReady) {
        gifPlay(lcd, kGifPath);
        lcd.fillScreen(COL_BG);
        lastHour = lastMinute = lastSecond = lastDay = -1;  // 强制整屏重画时钟
        invalidateClockText();
        redrawStatusIndicator();
      }
    }

    if (reminderActive) {
      if (nowMs() >= reminderUntil) {
        reminderActive = false;
        lcd.fillScreen(COL_BG);
        lastHour = lastMinute = lastSecond = lastDay = -1;  // 强制整屏重画
        invalidateClockText();
        redrawStatusIndicator();
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // 每分钟只在首次进入时查一次，避免提醒结束后在同一分钟内被重复触发
    const int minuteKey = t.tm_hour * 60 + t.tm_min;
    if (minuteKey != lastCheckedMinute) {
      lastCheckedMinute = minuteKey;
      if (const char* msg = matchReminder(t)) {
        ESP_LOGI(kTag, "reminder: %s", msg);
        reminderActive = true;
        reminderUntil = nowMs() + REMINDER_SHOW_SECONDS * 1000UL;
        drawReminder(msg, t);
        vTaskDelay(pdMS_TO_TICKS(200));
        continue;
      }
    }

    if (t.tm_hour != lastHour || t.tm_min != lastMinute) {
      drawTime(t.tm_hour, t.tm_min);
      lastHour = t.tm_hour;
      lastMinute = t.tm_min;
    }
    if (t.tm_sec != lastSecond) {
      drawSecond(t.tm_sec);
      lastSecond = t.tm_sec;
    }
    if (t.tm_mday != lastDay) {
      drawDate(t);
      lastDay = t.tm_mday;
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

}  // namespace

extern "C" void app_main(void) {
  // 不主动驱动 TFT_BL：gallery-pro 全程未使用该引脚，屏幕仍正常点亮，
  // 说明背光由硬件自行处理（接 VCC 或默认导通），软件干预反而可能关掉它
  ESP_LOGI(kTag, "TFT_BL=gpio%d 交由硬件处理，软件不驱动", TFT_BL);

  lcd.setColorDepth(16);
  lcd.init();
  lcd.setRotation(SCREEN_ROTATION);
  ESP_LOGI(kTag, "lcd ready, %dx%d", lcd.width(), lcd.height());

  // 先挂载文件系统再起 WiFi：littlefs 挂载要分配缓存，而 WiFi 栈启动后会
  // 吃掉大量堆内存，放在后面挂载会因分配失败而挂不上
  initStorage();
  initButton();
  initBoardLeds();
  gifInitFramebuffer();

  drawStatus(COL_WAIT, "connecting wifi...");
  wifiStart();

  // 栈开到 8K：播放 GIF 时要在本任务里跑文件 IO 和逐行解码
  xTaskCreate(clockTask, "clock", 8192, nullptr, 4, nullptr);
}
