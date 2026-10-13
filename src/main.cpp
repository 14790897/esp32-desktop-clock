#include <LovyanGFX.hpp>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
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

#include "button_gpio.h"
#include "clock_config.h"
#include "gif_player.h"
#include "iot_button.h"
#include "reminders.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

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
constexpr int H_STATUS = 26;
constexpr int Y_TIME = 44;
constexpr int H_TIME = 84;
constexpr int Y_SECOND = 138;
constexpr int H_SECOND = 30;
constexpr int Y_DATE = 184;
constexpr int H_DATE = 26;

// ==================== Claude 输出字幕 ====================
// 顶部那行原来是网络状态提示，收到 Claude 输出后改作字幕；没有文字时仍回退
// 显示网络状态，所以启动阶段看到的还是原来的提示。
//
// 显示方式是「尾巴对齐」：最新文字永远贴在屏幕右边缘，旧文字被顶出左侧。
// 不做匀速跑马灯 —— Claude 生成速度是每秒几十字，而可读的滚动速度只有约 6 字/秒，
// 匀速滚必然越积越滞后，就不是实时了。

constexpr int kTickerCap = 1024;        // 文字缓冲上限（字节），满了从前面丢
constexpr int kTickerIndent = 4;        // 右侧留白
constexpr uint16_t COL_BUSY = 0xFD20;   // 工作中：琥珀
constexpr uint16_t COL_DONE = 0x07E0;   // 已完成：绿
constexpr uint16_t COL_FAIL = 0xF800;   // 出错中断：红
constexpr uint16_t COL_IDLE = 0x8410;   // 还没有过输出：灰

// 字幕颜色直接反映 Claude 的状态，扫一眼就知道它是在干活还是干完了
enum class ClaudeState : uint8_t { Idle, Working, Done, Failed };

volatile ClaudeState gClaudeState = ClaudeState::Idle;

uint16_t claudeColor() {
  switch (gClaudeState) {
    case ClaudeState::Working: return COL_BUSY;
    case ClaudeState::Done: return COL_DONE;
    case ClaudeState::Failed: return COL_FAIL;
    default: return COL_IDLE;
  }
}

char gTickerText[kTickerCap] = {};   // 显示缓冲。HTTP 线程持锁写，clockTask 只读
char gTickerSession[48] = {};        // 当前内容属于哪个会话，换了会话要清空重来
volatile bool gTickerDirty = false;  // 有新内容，等 clockTask 重算宽度并重画
volatile bool gTickerActive = false;
int gTickerTextW = 0;  // 文本总宽，内容变化时重算一次

// 单行提示的轮播状态。放在这里而不是 showHint 旁边 —— /debug/ticker 要读它们，
// 那个 handler 定义得更靠前。
constexpr int kHintMaxFrames = 4;
char gHintFrames[kHintMaxFrames][96] = {};
uint16_t gHintMs[kHintMaxFrames] = {};
int16_t gHintWidths[kHintMaxFrames] = {};  // 量好的像素宽，量的时候顺手存下来
int gHintCount = 0;
int gHintIndex = 0;
uint32_t gHintAt = 0;
SemaphoreHandle_t gTickerLock = nullptr;

// 行内绘制走精灵缓冲：直接往屏幕上「先清后画」会闪，而且 drawString 往屏幕
// 写 200 个字形要几十毫秒，主循环压根跑不到目标帧率。先在 RAM 里合成再整行推出去，
// 既没有中间空白态，帧率也稳得住。
lgfx::LGFX_Sprite gTickerSprite(&lcd);
bool gTickerSpriteReady = false;

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
  // 有 Claude 文字时这行归字幕，网络状态让位
  if (gTickerActive) return;

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

// 精灵这条绘制路径在 R/B 通道上是反的。实测方式：在字幕行画 5 个已知色块
// （红 绿 蓝 白 灰），观察到的是「蓝 绿 红 白 灰」—— 红蓝对调、绿白灰正常，
// 是通道顺序问题而非字节序（灰的 R/B 值相同，若是字节序它也会变）。
// 设备直绘那条路正常（GIF 帧缓冲走 pushImageDMA 颜色是对的），只有精灵不一样，
// 所以按元件个补偿：送进去之前先把 R 和 B 换一次，显示出来就正了。
// 换屏幕/换库版本后若颜色又不对，先用 drawColorTest() 重测再改这里。
uint16_t forSprite(uint16_t rgb565) {
  return static_cast<uint16_t>(((rgb565 & 0x001F) << 11) | (rgb565 & 0x07E0) |
                               ((rgb565 & 0xF800) >> 11));
}

// 颜色校准诊断：在字幕行画 5 个纯色块，用来确定颜色管线做了什么变换。
// 正常顺序应为 红 绿 蓝 白 灰。看到别的顺序就能反推：
//   蓝 粉 绿 白 灰 → 字节序颠倒
//   蓝 绿 红 白 灰 → R/B 通道颠倒（forSprite 就是为这种补偿的）
// 换屏幕或升级 LovyanGFX 后颜色不对时，用 -DTICKER_COLOR_TEST 编译一次，
// 按实际看到的顺序调整 forSprite()。
void drawColorTest() {
  if (!gTickerSpriteReady) return;
  static const uint16_t kCols[5] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x8410};
  const int bw = W / 5;
  gTickerSprite.fillRect(0, 0, W, H_STATUS, COL_BG);
  for (int i = 0; i < 5; ++i) {
    gTickerSprite.fillRect(i * bw, 0, bw, H_STATUS, forSprite(kCols[i]));
  }
  gTickerSprite.pushSprite(0, Y_STATUS);
}

// 按当前内容绘制定长字幕。尾巴对齐 —— 右边缘位置固定，文字变长时旧内容自然被
// 顶出屏幕左侧，新内容始终可见。
void drawTicker() {
  if (!gTickerSpriteReady) return;

#ifdef TICKER_COLOR_TEST
  drawColorTest();
  return;
#endif

  gTickerSprite.fillRect(0, 0, W, H_STATUS, COL_BG);
  if (gTickerActive) {
    gTickerSprite.setFont(&fonts::efontCN_24);
    gTickerSprite.setTextSize(1.0f);
    gTickerSprite.setTextColor(forSprite(claudeColor()));
    gTickerSprite.drawString(gTickerText, W - kTickerIndent - gTickerTextW, (H_STATUS - 24) / 2);
  }
  gTickerSprite.pushSprite(0, Y_STATUS);
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
bool httpStarted = false;
httpd_handle_t gHttpServer = nullptr;
bool apHintShown = false;  // 配网提示只在进入配网模式时推一次

// 有凭据但连不上多久后改入配网模式。留足时间给路由器重启之类的短暂故障，
// 又不至于让换了网络的人一直干等。
constexpr uint32_t kConfigApFallbackMs = 90000;

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

// NTP 成功前系统时间是 1970，用这个阈值区分
bool timeIsValid() { return time(nullptr) > 1600000000; }

// WiFi 交给 78/esp-wifi-connect 组件的 WifiManager：凭据存在 NVS 里，连不上就
// 自己开热点 + 起配置网页（默认中文界面），不再需要 secrets.h 写死账号密码。
//
// 代价是没有静态 IP —— 组件不支持（高级页只有 OTA 地址、发射功率、记住 BSSID），
// 所以走 DHCP。连上后把 IP 显示在屏幕上，供设置 hook 的 CLAUDE_CLOCK_URL 用。
void initWifi() {
  WifiManagerConfig cfg;
  cfg.ssid_prefix = WIFI_AP_PREFIX;      // 配网热点名形如 esp32-clock-A1B2
  cfg.language = "zh-CN";
  cfg.station_hostname = WIFI_HOSTNAME;  // 向路由器注册的主机名，部分路由器可据此解析

  WifiManager& wifi = WifiManager::GetInstance();
  if (!wifi.Initialize(cfg)) {
    ESP_LOGE(kTag, "WiFi 初始化失败");
    return;
  }

  // 没有任何已保存的凭据就直接进配网模式。
  // 注意组件**不会**在连接失败后自动切到配网 —— 实测它只会一直重试扫描（10s/20s 退避），
  // 所以这个判断必须由应用自己做，组件 README 里也是这么示范的。
  if (SsidManager::GetInstance().GetSsidList().empty()) {
    ESP_LOGI(kTag, "没有已保存的 WiFi，进入配网模式");
    // 115KB 的 GIF 帧缓冲先还给堆：配网时用不到它，而堆被占着时 lwIP
    // 发 DHCP ACK 会分配不到内存（udp_sendto 返回 -1），客户端就卡在
    // 「获取 IP 地址」。按 BOOT 键播放时会自动重新分配。
    gifFreeFramebuffer();
    wifi.StartConfigAp();
  } else {
    wifi.StartStation();
  }
}

// ==================== HTTP 服务：接收 Claude 的输出 ====================
// 只在局域网监听，三个端点：
//   POST /say     body 是文本，整体替换（手动测试、一次性推送用）
//   POST /append  body 是新增文本，追加到末尾（实时回显用）
//   POST /state   body 为 "working" / "done" / "failed"，只决定字幕颜色
// PC 端由 Claude Code 的 hook 调用，见 tools/claude-status-hook.py

// 把请求 body 读进 buf，返回实际字节数。超出 cap-1 的部分读掉丢弃 ——
// 不排空会破坏 HTTP 连接状态，让同一连接上的后续请求失败。
int readBody(httpd_req_t* req, char* buf, int cap) {
  int total = 0;
  int remaining = req->content_len;

  while (remaining > 0) {
    const int room = cap - 1 - total;
    if (room <= 0) {
      char sink[128];
      const int want = remaining < static_cast<int>(sizeof(sink)) ? remaining
                                                                 : static_cast<int>(sizeof(sink));
      const int n = httpd_req_recv(req, sink, want);
      if (n <= 0) {
        if (n != HTTPD_SOCK_ERR_TIMEOUT) break;
        continue;
      }
      remaining -= n;
      continue;
    }

    const int want = remaining < room ? remaining : room;
    const int n = httpd_req_recv(req, buf + total, want);
    if (n <= 0) {
      if (n != HTTPD_SOCK_ERR_TIMEOUT) break;  // 收完或出错
      continue;                                 // 超时则继续等
    }
    total += n;
    remaining -= n;
  }
  buf[total] = '\0';

  // 截断可能正好切在汉字中间，退回到最后一个完整字符，否则屏幕上会多出乱码
  int cut = total;
  while (cut > 0 && (static_cast<unsigned char>(buf[cut - 1]) & 0xC0) == 0x80) --cut;
  if (cut > 0) {
    const unsigned char lead = static_cast<unsigned char>(buf[cut - 1]);
    int need = 0;
    if ((lead & 0x80) == 0x00) need = 1;
    else if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    if (need == 0 || total - (cut - 1) < need) total = cut - 1;
  }
  return total;
}

// 换了会话就把缓冲清掉。多开会话时它们都往同一个字幕推，不清的话
// A 的输出会接上 B 的输出，屏幕上是一串拼接起来的话。调用方须已持锁。
void switchSessionIfNeeded(const char* sid) {
  if (sid == nullptr || sid[0] == '\0') return;
  if (strncmp(sid, gTickerSession, sizeof(gTickerSession) - 1) == 0) return;

  strncpy(gTickerSession, sid, sizeof(gTickerSession) - 1);
  gTickerSession[sizeof(gTickerSession) - 1] = '\0';
  gTickerText[0] = '\0';
}

void replaceTickerText(const char* sid, const char* s, int len) {
  if (len <= 0 || xSemaphoreTake(gTickerLock, pdMS_TO_TICKS(50)) != pdTRUE) return;
  switchSessionIfNeeded(sid);
  if (len > kTickerCap - 1) len = kTickerCap - 1;
  memcpy(gTickerText, s, len);
  gTickerText[len] = '\0';
  gTickerDirty = true;  // 先写内容再置脏，clockTask 看到脏标志时数据必然已完整
  xSemaphoreGive(gTickerLock);
}

// 追加到末尾。满了从前面丢，保证新内容一定进得来 —— 尾巴对齐的显示方式下，
// 被丢掉的老内容本来就已经滚出屏幕左侧了。
void appendTickerText(const char* sid, const char* s, int len) {
  if (len <= 0 || xSemaphoreTake(gTickerLock, pdMS_TO_TICKS(50)) != pdTRUE) return;

  switchSessionIfNeeded(sid);

  int cur = static_cast<int>(strlen(gTickerText));
  if (cur + len > kTickerCap - 1) {
    int drop = cur + len - (kTickerCap - 1);
    // 对齐到 UTF-8 字符边界，别把汉字劈成半个
    while (drop < cur && (static_cast<unsigned char>(gTickerText[drop]) & 0xC0) == 0x80) ++drop;
    memmove(gTickerText, gTickerText + drop, cur - drop);
    cur -= drop;
  }
  memcpy(gTickerText + cur, s, len);
  gTickerText[cur + len] = '\0';
  // 有文字流进来就说明这个会话正在工作 —— 状态由数据流推导，
  // 不依赖单独的「开始工作」信号。那个信号走的是异步 hook，可能比第一批
  // 文字到得还晚，结果就是工作中却显示着上一轮的「已完成」绿色。
  gClaudeState = ClaudeState::Working;
  gTickerDirty = true;
  xSemaphoreGive(gTickerLock);
}

// 取 URL 里的 sid 查询参数（?sid=xxx）。没有就返回空串。
void querySession(httpd_req_t* req, char* out, size_t outLen) {
  out[0] = '\0';
  char q[128];
  if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return;
  if (httpd_query_key_value(q, "sid", out, outLen) != ESP_OK) out[0] = '\0';
}

esp_err_t handleSay(httpd_req_t* req) {
  char buf[kTickerCap];
  char sid[sizeof(gTickerSession)];
  querySession(req, sid, sizeof(sid));
  replaceTickerText(sid, buf, readBody(req, buf, sizeof(buf)));
  httpd_resp_sendstr(req, "ok");
  return ESP_OK;
}

esp_err_t handleAppend(httpd_req_t* req) {
  char buf[kTickerCap];
  char sid[sizeof(gTickerSession)];
  querySession(req, sid, sizeof(sid));
  appendTickerText(sid, buf, readBody(req, buf, sizeof(buf)));
  httpd_resp_sendstr(req, "ok");
  return ESP_OK;
}

esp_err_t handleState(httpd_req_t* req) {
  char buf[16] = {};
  const int n = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (n <= 0) {
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
  }
  buf[n] = '\0';

  ClaudeState st = ClaudeState::Done;  // "done" 及任何其它值都视为已完成
  if (strncmp(buf, "working", 7) == 0) {
    st = ClaudeState::Working;
  } else if (strncmp(buf, "failed", 6) == 0) {
    st = ClaudeState::Failed;
  }

  if (xSemaphoreTake(gTickerLock, pdMS_TO_TICKS(50)) == pdTRUE) {
    // 状态也要认会话：A 还在输出时，B 的一句「完成」不该把颜色改绿。
    // 顺带切换当前会话 —— 新一轮开始时清掉上一条，随后流式追加的内容才干净。
    char sid[sizeof(gTickerSession)];
    querySession(req, sid, sizeof(sid));
    switchSessionIfNeeded(sid);

    gClaudeState = st;
    // 状态只影响颜色不改文字，但同样得触发重画，否则颜色会停在旧值
    if (gTickerActive) gTickerDirty = true;
    xSemaphoreGive(gTickerLock);
  }

  httpd_resp_sendstr(req, "ok");
  return ESP_OK;
}

// 转义成 JSON 字符串内容。字幕里可能出现引号、反斜杠、控制字符
void jsonEscape(const char* s, char* out, size_t outLen) {
  size_t o = 0;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p != '\0'; ++p) {
    char tmp[8];
    const char* piece = tmp;
    switch (*p) {
      case '"': piece = "\\\""; break;
      case '\\': piece = "\\\\"; break;
      case '\n': piece = "\\n"; break;
      case '\r': piece = "\\r"; break;
      case '\t': piece = "\\t"; break;
      default:
        if (*p < 0x20) {
          snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
        } else {
          tmp[0] = static_cast<char>(*p);
          tmp[1] = '\0';
        }
    }
    const size_t n = strlen(piece);
    if (o + n >= outLen) break;
    memcpy(out + o, piece, n);
    o += n;
  }
  out[o] = '\0';
}

const char* claudeStateName(ClaudeState s) {
  switch (s) {
    case ClaudeState::Working: return "working";
    case ClaudeState::Done: return "done";
    case ClaudeState::Failed: return "failed";
    default: return "idle";
  }
}

// 把字幕的当前状态以 JSON 吐出来，给 tests/ 里的 E2E 用例断言用。
// 没有这个端点，HTTP 测试就只能断言「返回 200」—— 而「提示被裁成裸 IP」
// 「颜色状态不对」「多会话串台」这些真踩过的坑一个都测不到。
esp_err_t handleDebugTicker(httpd_req_t* req) {
  // 快照必须在锁内取，否则可能读到写到一半的缓冲
  char text[kTickerCap];
  char session[sizeof(gTickerSession)];
  int width = 0;
  ClaudeState state = ClaudeState::Idle;
  bool active = false;

  if (xSemaphoreTake(gTickerLock, pdMS_TO_TICKS(100)) != pdTRUE) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "lock busy");
    return ESP_FAIL;
  }
  snprintf(text, sizeof(text), "%s", gTickerText);
  snprintf(session, sizeof(session), "%s", gTickerSession);
  width = gTickerTextW;
  state = gClaudeState;
  active = gTickerActive;
  xSemaphoreGive(gTickerLock);

  // 转义后最大是原文两倍，全放栈上会顶爆 HTTP 任务的 4KB 栈，所以走堆
  const size_t cap = strlen(text) * 2 + strlen(session) * 2 +
                     static_cast<size_t>(kHintMaxFrames) * sizeof(gHintFrames[0]) * 2 + 512;
  char* escText = static_cast<char*>(malloc(cap));
  char* escSess = static_cast<char*>(malloc(sizeof(session) * 2));
  char* escHint = static_cast<char*>(malloc(static_cast<size_t>(kHintMaxFrames) *
                                            sizeof(gHintFrames[0]) * 2));
  char* body = static_cast<char*>(malloc(cap + 256));
  if (escText == nullptr || escSess == nullptr || escHint == nullptr || body == nullptr) {
    free(escText); free(escSess); free(escHint); free(body);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    return ESP_FAIL;
  }

  jsonEscape(text, escText, cap);
  jsonEscape(session, escSess, sizeof(session) * 2);

  size_t o = 0;
  o += snprintf(body + o, cap + 256 - o,
                "{\"text\":\"%s\",\"state\":\"%s\",\"session\":\"%s\",\"active\":%s,"
                "\"width\":%d,\"limit\":%d,\"hints\":[",
                escText, claudeStateName(state), escSess, active ? "true" : "false", width,
                W - kTickerIndent);

  for (int i = 0; i < gHintCount && i < kHintMaxFrames; ++i) {
    jsonEscape(gHintFrames[i], escHint, static_cast<size_t>(sizeof(gHintFrames[0])) * 2);
    o += snprintf(body + o, cap + 256 - o, "%s{\"text\":\"%s\",\"width\":%d}",
                  i == 0 ? "" : ",", escHint, gHintWidths[i]);
  }
  snprintf(body + o, cap + 256 - o, "]}");

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, body);

  free(escText); free(escSess); free(escHint); free(body);
  return ESP_OK;
}

bool startHttpServer() {
  if (gTickerLock == nullptr) {
    ESP_LOGE(kTag, "字幕锁未初始化");
    return false;
  }

  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 5;
  cfg.lru_purge_enable = true;  // 连接堆积时回收最久未用的，避免句柄耗尽

  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &cfg) != ESP_OK) {
    ESP_LOGE(kTag, "HTTP 服务启动失败");
    return false;
  }
  gHttpServer = server;

  httpd_uri_t sayUri = {};
  sayUri.uri = "/say";
  sayUri.method = HTTP_POST;
  sayUri.handler = handleSay;
  httpd_register_uri_handler(server, &sayUri);

  httpd_uri_t appendUri = {};
  appendUri.uri = "/append";
  appendUri.method = HTTP_POST;
  appendUri.handler = handleAppend;
  httpd_register_uri_handler(server, &appendUri);

  httpd_uri_t stateUri = {};
  stateUri.uri = "/state";
  stateUri.method = HTTP_POST;
  stateUri.handler = handleState;
  httpd_register_uri_handler(server, &stateUri);

  // 只读的诊断端点，给 tests/ 用。不参与正常业务，也不改任何状态。
  httpd_uri_t debugUri = {};
  debugUri.uri = "/debug/ticker";
  debugUri.method = HTTP_GET;
  debugUri.handler = handleDebugTicker;
  httpd_register_uri_handler(server, &debugUri);

  ESP_LOGI(kTag, "HTTP 服务已启动，等待 Claude 输出");
  return true;
}

// 配网页也要绑 80 端口，而组件的 httpd_start 用的是 ESP_ERROR_CHECK —— 端口被占时
// 它直接 abort 而不是返回错误。所以进配网前必须先把我们这个服务停掉。
void stopHttpServer() {
  if (gHttpServer == nullptr) return;
  httpd_stop(gHttpServer);
  gHttpServer = nullptr;
  httpStarted = false;
  ESP_LOGI(kTag, "HTTP 服务已停止，让出 80 端口给配网页面");
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

// ==================== 单行提示轮播 ====================
// 字幕行只有 240px、字号 24px，放不下十来个字；而它是尾巴对齐的（见 drawTicker），
// 超宽只会从左边裁。实测「本机 192.168.1.77」宽 276px、「连热点 A 后浏览器打开 B」
// 更宽 —— 两种情况下屏幕上都只剩个裸 IP 或网址，最该看的那半句反而没了。
// 整个屏幕又只有这一条消息行（下面几行是时钟和日期），所以统一拆成
// 「标签 / 取值」交替的短帧轮播：每帧都短到放得下，连起来读正好是一句完整提示。

void showHint(const char* const* texts, const uint16_t* durations, int count) {
  if (count > kHintMaxFrames) count = kHintMaxFrames;
  for (int i = 0; i < count; ++i) {
    snprintf(gHintFrames[i], sizeof(gHintFrames[i]), "%s", texts[i]);
    gHintMs[i] = durations[i];
  }
  gHintCount = count;
  gHintIndex = 0;
  gHintAt = nowMs();

  // 宽度是量出来的不是猜的：WIFI_AP_PREFIX 改长、或 IP 段变长都会静默裁字。
  // 前缀 `hint-frame-clipped` 是给测试用的机器可读标记（tests/test_provision.py），
  // 串口断言只能用 ASCII，而后面这句是中文。
  lcd.setFont(&fonts::efontCN_24);
  lcd.setTextSize(1.0f);
  for (int i = 0; i < count; ++i) {
    const int w = lcd.textWidth(gHintFrames[i]);
    gHintWidths[i] = static_cast<int16_t>(w);  // 存下来，HTTP 线程读它就不用碰 lcd
    if (w > W - kTickerIndent) {
      ESP_LOGW(kTag, "hint-frame-clipped: 提示第 %d 帧「%s」宽 %dpx，超出可显示的 %dpx，会被裁掉",
               i, gHintFrames[i], w, W - kTickerIndent);
    }
  }

  replaceTickerText("boot", gHintFrames[0], static_cast<int>(strlen(gHintFrames[0])));
}

void stopHint() { gHintCount = 0; }

// 只在 clockTask 里调用。
void cycleHint() {
  if (gHintCount == 0) return;
  if (nowMs() - gHintAt < gHintMs[gHintIndex]) return;

  // 这一行已经被别人（Claude 的输出）改写了，提示就到此为止，别再抢回来
  bool preempted = false;
  if (xSemaphoreTake(gTickerLock, pdMS_TO_TICKS(50)) != pdTRUE) return;
  preempted = strcmp(gTickerText, gHintFrames[gHintIndex]) != 0;
  xSemaphoreGive(gTickerLock);
  if (preempted) {
    stopHint();
    return;
  }

  gHintAt = nowMs();
  gHintIndex = (gHintIndex + 1) % gHintCount;
  replaceTickerText("boot", gHintFrames[gHintIndex],
                    static_cast<int>(strlen(gHintFrames[gHintIndex])));
}

// 把设备 IP 推到字幕行显示。Claude 一有输出就会顶掉它，所以不做超时清除 ——
// 顺带的好处是没跑 Claude 时屏幕上一直能看到本机地址。
void showIpOnTicker(const char* ip) {
  const char* texts[] = {"本机IP", ip};
  const uint16_t ms[] = {2000, 4000};
  showHint(texts, ms, 2);
}

// 配网模式时把热点名和配置网址显示出来。有屏就不必去翻串口日志。
void showConfigPortalHint(const char* ap, const char* url) {
  const char* texts[] = {"连热点", ap, "浏览器打开", url};
  const uint16_t ms[] = {1500, 3000, 1500, 3000};
  showHint(texts, ms, 4);
}

void serviceNetwork() {
  WifiManager& wifi = WifiManager::GetInstance();
  static uint32_t tryingSince = 0;

  // ---- 配网模式：设备自己开着热点等配置 ----
  if (wifi.IsConfigMode()) {
    if (!apHintShown) {
      apHintShown = true;
      const std::string ap = wifi.GetApSsid();
      const std::string url = wifi.GetApWebUrl();
      ESP_LOGI(kTag, "进入配网模式：热点 %s，浏览器打开 %s", ap.c_str(), url.c_str());
      showConfigPortalHint(ap.c_str(), url.c_str());
    }
    netState = Net::Wifi;
    return;
  }
  apHintShown = false;
  stopHint();

  // ---- 还没连上 ----
  if (!wifi.IsConnected()) {
    if (netState != Net::Wifi) {
      netState = Net::Wifi;
      drawStatus(COL_WAIT, "wifi lost, retrying...");
      ESP_LOGW(kTag, "wifi disconnected");
    }
    // 有凭据却一直连不上（换了网络、改了密码、路由器没开）：超时后自动进配网，
    // 否则设备就再也配不了了，只能重新烧录固件
    if (tryingSince == 0) tryingSince = nowMs();
    if (nowMs() - tryingSince >= kConfigApFallbackMs) {
      ESP_LOGW(kTag, "连接超时，改入配网模式");
      tryingSince = 0;
      stopHttpServer();
      gifFreeFramebuffer();  // 同上：配网要那 115KB 才发得出 DHCP ACK
      wifi.StartConfigAp();
    }
    return;
  }
  tryingSince = 0;

  // ---- 刚连上：记录 IP、起服务、开始对时 ----
  if (netState == Net::Wifi) {
    const std::string ip = wifi.GetIpAddress();
    ESP_LOGI(kTag, "wifi up, ssid=%s ip=%s", wifi.GetSsid().c_str(), ip.c_str());
    showIpOnTicker(ip.c_str());

    // 只在成功时置位：配网刚结束时组件的 httpd 可能还没释放 80 端口，
    // 这一轮起不来下一轮还会再试
    if (!httpStarted) httpStarted = startHttpServer();
    if (!sntpStarted) startSntp();  // 掉线重连时不要重复初始化 SNTP
    netState = Net::Syncing;
    return;
  }

  // ---- 等对时 ----
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

// 顶部那行的统一重画入口。有 Claude 文字时画字幕并复位滚动位置（从右边重新进入），
// 否则回退到网络状态提示。GIF 播完、提醒结束后都要走这里。
void redrawTopRow() {
  if (gTickerActive) {
    // 尾巴对齐，位置由内容宽度决定，GIF/提醒占屏后不需要复位什么，直接重画
    drawTicker();
  } else if (netState == Net::Ready) {
    drawStatus(COL_OK, nullptr);
  } else {
    drawStatus(COL_WAIT, "connecting wifi...");
  }
}

// 内容有更新时重算宽度并重画。只在 clockTask 里调用。
// 宽度算法放在锁内，和 HTTP 线程的追加互斥；drawTicker 在锁外，避免持锁做绘制。
void refreshTicker() {
#ifdef TICKER_COLOR_TEST
  drawTicker();  // 诊断模式：每轮都重画色块，压过网络状态提示
  return;
#endif

  if (!gTickerDirty || gTickerLock == nullptr) return;

  xSemaphoreTake(gTickerLock, portMAX_DELAY);
  lcd.setFont(&fonts::efontCN_24);
  lcd.setTextSize(1.0f);
  gTickerTextW = lcd.textWidth(gTickerText);
  gTickerDirty = false;
  xSemaphoreGive(gTickerLock);

  gTickerActive = true;
  drawTicker();
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
    cycleHint();  // 配网提示和开机 IP 都靠它轮播
    refreshTicker();

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
        redrawTopRow();
      }
    }

    if (reminderActive) {
      if (nowMs() >= reminderUntil) {
        reminderActive = false;
        lcd.fillScreen(COL_BG);
        lastHour = lastMinute = lastSecond = lastDay = -1;  // 强制整屏重画
        invalidateClockText();
        redrawTopRow();
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

    // 字幕内容由 HTTP 线程置的脏标志驱动刷新（见 refreshTicker），
    // 尾巴对齐不需要逐帧推进，所以这里保持省电节奏
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

}  // namespace

extern "C" void app_main(void) {
  // 临时诊断：手机能关联却卡在「获取 IP 地址」。
  // 注意 ESP-IDF 的 DHCP 服务器**不用 ESP_LOG** —— 它的 60 条日志全包在
  // `#if DHCPS_DEBUG` 里，而该宏默认为 0，release 构建下一条都不存在，
  // 所以在这里设 esp_log_level_set("dhcps", ...) 是没用的（试过，白搭）。
  // 真正的开关在 components/lwip/apps/dhcpserver/dhcpserver.c 里的
  // DHCPS_DEBUG，已临时改成 1，改完要一起还原。

  // 不主动驱动 TFT_BL：gallery-pro 全程未使用该引脚，屏幕仍正常点亮，
  // 说明背光由硬件自行处理（接 VCC 或默认导通），软件干预反而可能关掉它
  ESP_LOGI(kTag, "TFT_BL=gpio%d 交由硬件处理，软件不驱动", TFT_BL);

  lcd.setColorDepth(16);
  lcd.init();
  lcd.setRotation(SCREEN_ROTATION);
  ESP_LOGI(kTag, "lcd ready, %dx%d", lcd.width(), lcd.height());

  // 字幕行缓冲，在起 WiFi 之前建，堆还干净。建不起来只是没有字幕，别的功能不受影响。
  //
  // setSwapBytes(true)：精灵默认字节序和屏幕一致（都是 false），但实测通过
  // pushSprite 推出去颜色会高低字节颠倒 —— 琥珀色会显示成蓝色。精灵和屏幕各自
  // 的写转换在这里对不上，翻转精灵一侧即可。若哪天颜色又不对，先怀疑这一行。
  gTickerSprite.setColorDepth(16);
  gTickerSprite.setSwapBytes(true);
  gTickerSpriteReady = (gTickerSprite.createSprite(W, H_STATUS) != nullptr);
  if (!gTickerSpriteReady) {
    ESP_LOGE(kTag, "字幕行缓冲创建失败，滚动字幕不可用");
  }

  // 字幕锁要在这里就建好，不能等到起 HTTP 服务时 —— 配网模式下 HTTP 服务不会启动，
  // 而配网提示要往字幕里写字；那时锁还是 NULL，xSemaphoreTake(NULL) 会直接 assert 重启
  gTickerLock = xSemaphoreCreateMutex();
  if (gTickerLock == nullptr) {
    ESP_LOGE(kTag, "创建字幕锁失败");
  }

  // 先挂载文件系统再起 WiFi：littlefs 挂载要分配缓存，而 WiFi 栈启动后会
  // 吃掉大量堆内存，放在后面挂载会因分配失败而挂不上
  initStorage();
  initButton();
  initBoardLeds();
  gifInitFramebuffer();

  drawStatus(COL_WAIT, "connecting wifi...");
  initWifi();

  // 栈开到 8K：播放 GIF 时要在本任务里跑文件 IO 和逐行解码
  xTaskCreate(clockTask, "clock", 8192, nullptr, 4, nullptr);
}
