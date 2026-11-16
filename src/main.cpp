#include <LovyanGFX.hpp>

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <lgfx/Fonts/efont/lgfx_efont_cn.h>

#include <sys/time.h>

#include <cstring>
#include <ctime>

#include "ble_clock.h"
#include "button_gpio.h"
#include "clock_config.h"
#include "gif_player.h"
#include "iot_button.h"
#include "reminders.h"

static const char* kTag = "clock";

namespace {

constexpr int W = SCREEN_WIDTH;
constexpr int H = SCREEN_HEIGHT;

// ==================== 显示 ====================
// ST7789 240x240。引脚与面板参数均为本机实测，不是照抄现成配置 ——
// 见 README「硬件接线」开头那条警告（DC/CS/RST 是轮换关系，照抄会黑屏）。
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

// ==================== 显示风格 ====================

// 两种表盘风格。像素风只作用于大号的时间和秒 —— 日期是细笔画小字、字幕是中文
// 点阵字，改成像素风收益低、可读性反而变差。
enum class DisplayStyle : uint8_t { Classic, Pixel };

constexpr int kStyleCount = 2;

volatile DisplayStyle gStyle = DisplayStyle::Classic;
// 风格刚变，clockTask 下一轮要整屏重画 —— 两种风格的字号差很多，
// 只重画变化的字位会留下旧字形的残影
volatile bool gStyleDirty = false;

// 风格存 NVS，重启后保持。命名空间和键名都很短，NVS 里每个键都占空间。
constexpr const char* kNvsNamespace = "clock";
constexpr const char* kNvsStyleKey = "style";

void loadStyle() {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
  uint8_t v = 0;
  if (nvs_get_u8(h, kNvsStyleKey, &v) == ESP_OK && v < kStyleCount) {
    gStyle = v == 0 ? DisplayStyle::Classic : DisplayStyle::Pixel;
  }
  nvs_close(h);
}

void saveStyle() {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u8(h, kNvsStyleKey, gStyle == DisplayStyle::Classic ? 0 : 1);
  nvs_commit(h);
  nvs_close(h);
}

void setStyle(DisplayStyle s) {
  if (s == gStyle) return;
  gStyle = s;
  saveStyle();
  gStyleDirty = true;
  ESP_LOGI(kTag, "显示风格切到 %s", s == DisplayStyle::Classic ? "classic" : "pixel");
}

void cycleStyle() {
  setStyle(gStyle == DisplayStyle::Classic ? DisplayStyle::Pixel : DisplayStyle::Classic);
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

// ==================== 像素风字形 ====================
// 5×7 点阵，每行一个字节、低 5 位有效、bit4 在最左。
// 只收时间用得到的字符：0-9 和冒号。
//
// 为什么自己画字模而不是把矢量字体放大：矢量字体放大出来是「圆润的大字」，
// 不是像素风 —— 像素风的关键是方块边缘和整格对齐。
static const uint8_t kPixelGlyphs[][7] = {
    {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110},  // 0
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},  // 1
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111},  // 2
    {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110},  // 3
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010},  // 4
    {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110},  // 5
    {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110},  // 6
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000},  // 7
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110},  // 8
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100},  // 9
    {0b11000, 0b11000, 0b00000, 0b00000, 0b11000, 0b11000, 0b00000},  // :
};

// 每个字形的实际列数。冒号只要 2 列 —— 按 5 列算会留下两大块空白，
// 时间被挤窄。2×2 的方点也更像像素画里的冒号。
static const uint8_t kPixelWidths[] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 2};

const uint8_t* pixelGlyph(char c, int* width) {
  if (c >= '0' && c <= '9') {
    *width = kPixelWidths[c - '0'];
    return kPixelGlyphs[c - '0'];
  }
  if (c == ':') {
    *width = kPixelWidths[10];
    return kPixelGlyphs[10];
  }
  return nullptr;
}

// 用点阵画定长字符串。block = 一个像素方块的边长。
//
// 和 drawSlotString 一样只重画**变化的字位**：整块清空再重画，每秒会闪一次。
// 也只有这样，"11:11" 这种宽度不同的串才不会左右跳。
void drawPixelString(const char* text, int y, int block, uint16_t fg, char* prev) {
  const int len = static_cast<int>(strlen(text));
  if (len <= 0 || block <= 0) return;

  int widths[8];
  const int maxLen = static_cast<int>(sizeof(widths) / sizeof(widths[0]));
  if (len > maxLen) return;

  int cells = 0;
  for (int i = 0; i < len; ++i) {
    int w = 0;
    pixelGlyph(text[i], &w);
    if (w == 0) w = 5;  // 不认识的字符按 5 列留白
    widths[i] = static_cast<int8_t>(w);
    cells += w;
  }

  const int gap = block;  // 字间留一个方块宽
  const int totalW = cells * block + gap * (len - 1);
  const int x0 = (W - totalW) / 2;
  const int h = 7 * block;

  int x = x0;
  for (int i = 0; i < len; ++i) {
    const int slotW = widths[i] * block;
    if (text[i] != prev[i]) {
      lcd.fillRect(x, y, slotW, h, COL_BG);

      int w = 0;
      const uint8_t* g = pixelGlyph(text[i], &w);
      if (g != nullptr) {
        for (int row = 0; row < 7; ++row) {
          for (int col = 0; col < w; ++col) {
            if (g[row] & (1 << (4 - col))) {
              lcd.fillRect(x + col * block, y + row * block, block, block, fg);
            }
          }
        }
      }
      prev[i] = text[i];
    }
    x += slotW + gap;
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
  if (gStyle == DisplayStyle::Pixel) {
    // "HH:MM" 一共 22 列 + 4 个间隔 = 26 个方块宽；留 4px 边距 → 9px 一块。
    // 7 行高 = 63px，比分配给时间的 84px 矮，垂直居中。
    constexpr int kBlock = 9;
    const int h = 7 * kBlock;
    drawPixelString(buf, Y_TIME + (H_TIME - h) / 2, kBlock, COL_TIME, gPrevTime);
    return;
  }
  drawSlotString(&fonts::FreeSansBold24pt7b, buf, "0123456789:", Y_TIME, H_TIME, COL_TIME,
                 W - 16, gPrevTime);
}

void drawSecond(int sec) {
  char buf[8];
  snprintf(buf, sizeof(buf), ":%02d", sec);
  if (gStyle == DisplayStyle::Pixel) {
    constexpr int kBlock = 4;  // 7 行 × 4 = 28px，正好放进 30px 的秒行
    const int h = 7 * kBlock;
    drawPixelString(buf, Y_SECOND + (H_SECOND - h) / 2, kBlock, COL_SECOND, gPrevSecond);
    return;
  }
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

// ==================== 状态与字幕 ====================

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

// 时间还没被 PC 通过 BLE 推过来时，系统时间是 1970，用这个阈值区分
bool timeIsValid() { return time(nullptr) > 1600000000; }

// 当前会话标识。BLE 用 'I' 指令设置它，之后收到的文本都归这个会话。
char gBleSession[48] = {};

// 换了会话就把缓冲清掉。多开会话时它们都往同一行推，不清的话
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
  // 不依赖单独的「开始工作」信号，否则工作中会显示着上一轮的绿色。
  gClaudeState = ClaudeState::Working;
  gTickerDirty = true;
  xSemaphoreGive(gTickerLock);
}

// ==================== BLE 命令回调 ====================

void bleOnAppend(const char* s, int len) { appendTickerText(gBleSession, s, len); }

void bleOnReplace(const char* s, int len) { replaceTickerText(gBleSession, s, len); }

void bleOnSession(const char* sid, int len) {
  if (len <= 0) {
    gBleSession[0] = '\0';
    return;
  }
  if (len > static_cast<int>(sizeof(gBleSession)) - 1) len = sizeof(gBleSession) - 1;
  memcpy(gBleSession, sid, len);
  gBleSession[len] = '\0';
}

void bleOnStyle(char c) {
  if (c == '1') setStyle(DisplayStyle::Pixel);
  else if (c == '0') setStyle(DisplayStyle::Classic);
}

void bleOnState(char c) {
  ClaudeState st = ClaudeState::Done;  // 和原来的 HTTP /state 保持一致：认不出的值算完成
  if (c == 'w') st = ClaudeState::Working;
  else if (c == 'f') st = ClaudeState::Failed;

  if (xSemaphoreTake(gTickerLock, pdMS_TO_TICKS(50)) == pdTRUE) {
    gClaudeState = st;
    // 状态只影响颜色不改文字，但同样得触发重画，否则颜色会停在旧值
    if (gTickerActive) gTickerDirty = true;
    xSemaphoreGive(gTickerLock);
  }
}

void bleOnTime(uint32_t epoch) {
  if (epoch < 1600000000UL) return;  // 明显不合理的时间不设
  struct timeval tv = {};
  tv.tv_sec = static_cast<time_t>(epoch);
  tv.tv_usec = 0;
  if (settimeofday(&tv, nullptr) == 0) {
    ESP_LOGI(kTag, "时间已由 PC 同步: epoch=%lu", static_cast<unsigned long>(epoch));
  } else {
    ESP_LOGW(kTag, "settimeofday 失败");
  }
}

const char* claudeStateName(ClaudeState s) {
  switch (s) {
    case ClaudeState::Working: return "working";
    case ClaudeState::Done: return "done";
    case ClaudeState::Failed: return "failed";
    default: return "idle";
  }
}

// 状态快照。BLE 的 read 响应受 MTU 限制（协商后通常 ~180 字节），所以只带
// 字幕的**尾部** —— 屏幕本来就是尾巴对齐的，尾部才是看得见的部分。
int bleStatus(char* buf, int cap) {
  char text[kTickerCap];
  char session[sizeof(gTickerSession)];
  int width = 0;
  ClaudeState st = ClaudeState::Idle;
  bool active = false;
  bool connected = bleClockConnected();

  if (xSemaphoreTake(gTickerLock, pdMS_TO_TICKS(100)) != pdTRUE) return 0;
  snprintf(text, sizeof(text), "%s", gTickerText);
  snprintf(session, sizeof(session), "%s", gTickerSession);
  width = gTickerTextW;
  st = gClaudeState;
  active = gTickerActive;
  xSemaphoreGive(gTickerLock);

  const int fullLen = static_cast<int>(strlen(text));

  // 取尾部，并且从 UTF-8 字符边界开始，别把汉字劈成半个。
  // 56 而不是 80：JSON 里字段多了，MTU 247 时 read 最多回 ~243 字节，
  // 尾部太长会把整个响应截断。
  constexpr int kTailMax = 56;
  int tailStart = fullLen > kTailMax ? fullLen - kTailMax : 0;
  while (tailStart < fullLen && (static_cast<unsigned char>(text[tailStart]) & 0xC0) == 0x80) {
    ++tailStart;
  }
  const char* tail = text + tailStart;

  return snprintf(buf, cap,
                  "{\"state\":\"%s\",\"session\":\"%s\",\"active\":%s,\"connected\":%s,"
                  "\"style\":\"%s\",\"len\":%d,\"width\":%d,\"limit\":%d,\"free_heap\":%u,"
                  "\"largest_block\":%u,\"tail\":\"%s\"}",
                  claudeStateName(st), session, active ? "true" : "false",
                  connected ? "true" : "false",
                  gStyle == DisplayStyle::Pixel ? "pixel" : "classic", fullLen, width,
                  W - kTickerIndent, static_cast<unsigned>(esp_get_free_heap_size()),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                  tail);
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
// 回调在库的任务上下文里跑，这里只累加计数 / 置标志，实际动作交给 clockTask。
volatile uint32_t gButtonClicks = 0;

// 长按 3 秒切换显示风格（经典 / 像素）。3 秒而不是库默认的 1.5 秒 ——
// 切换虽然无害，但也不该那么容易误触。
constexpr uint16_t kLongPressStyleMs = 3000;

void onButtonSingleClick(void*, void*) {
  // volatile 上的 ++ 在 C++20 已废弃，显式读改写
  gButtonClicks = gButtonClicks + 1;
}

void onButtonLongPress(void*, void*) { cycleStyle(); }

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
  cfg.long_press_time = kLongPressStyleMs;
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
  iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, nullptr, onButtonLongPress, nullptr);
  ESP_LOGI(kTag, "BOOT 键 gpio%d 就绪（短按播放 GIF，长按 %ums 切换显示风格）",
           static_cast<int>(kBootPin), kLongPressStyleMs);
}

// ==================== 定时提醒 ====================
const char* matchReminder(const struct tm& t) {
  for (const Reminder& r : kReminders) {
    if (r.hour == t.tm_hour && r.minute == t.tm_min) return r.text;
  }
  return nullptr;
}

// 顶部那行的统一重画入口。有 Claude 文字时画字幕，否则回退到状态提示 ——
// 时间还没被 PC 推过来之前显示等待提示。GIF 播完、提醒结束后都要走这里。
void redrawTopRow() {
  if (gTickerActive) {
    // 尾巴对齐，位置由内容宽度决定，GIF/提醒占屏后不需要复位什么，直接重画
    drawTicker();
  } else if (timeIsValid()) {
    drawStatus(COL_OK, nullptr);
  } else {
    drawStatus(COL_WAIT, "waiting for BLE...");
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
    refreshTicker();

    // 时间由 PC 通过 BLE 推过来（蓝牙上不了互联网，没有 NTP）。
    // 还没收到时系统时间是 1970，先只跑字幕、不画时钟。
    if (!timeIsValid()) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);

    // 显示风格刚被切换（BOOT 长按或 BLE 指令）：整屏清掉重画。
    // 两种风格的字号差很多，只重画变化的字位会留下旧字形的残影。
    if (gStyleDirty) {
      gStyleDirty = false;
      lcd.fillScreen(COL_BG);
      invalidateClockText();
      lastHour = lastMinute = lastSecond = lastDay = -1;
      redrawTopRow();
    }

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
  // NVS 要最先初始化：显示风格存在里面，NimBLE 内部也可能用
  esp_err_t nvsErr = nvs_flash_init();
  if (nvsErr == ESP_ERR_NVS_NO_FREE_PAGES || nvsErr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvsErr = nvs_flash_init();
  }
  if (nvsErr != ESP_OK) {
    ESP_LOGW(kTag, "NVS 初始化失败（%s），显示风格将不会保存", esp_err_to_name(nvsErr));
  }
  loadStyle();

  // 不主动驱动 TFT_BL：不接该引脚屏幕也正常点亮，说明背光由硬件自行处理
  // （接 VCC 或默认导通），软件干预反而可能关掉它（实测拉高后全黑）
  ESP_LOGI(kTag, "TFT_BL=gpio%d 交由硬件处理，软件不驱动", TFT_BL);

  lcd.setColorDepth(16);
  lcd.init();
  lcd.setRotation(SCREEN_ROTATION);
  ESP_LOGI(kTag, "lcd ready, %dx%d", lcd.width(), lcd.height());

  // 字幕行缓冲，在起蓝牙之前建，堆还干净。
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

  // 字幕锁要在这里就建好：BLE 回调可能在任意时刻往字幕里写字，
  // 锁是 NULL 的话 xSemaphoreTake(NULL) 会直接 assert 重启
  gTickerLock = xSemaphoreCreateMutex();
  if (gTickerLock == nullptr) {
    ESP_LOGE(kTag, "创建字幕锁失败");
  }

  // 先挂载文件系统再起蓝牙：littlefs 挂载要分配缓存，而蓝牙栈启动后会
  // 吃掉大量堆内存，放在后面挂载会因分配失败而挂不上
  initStorage();
  initButton();
  initBoardLeds();
  gifInitFramebuffer();

  BleClockCallbacks cb = {};
  cb.append = bleOnAppend;
  cb.replace = bleOnReplace;
  cb.state = bleOnState;
  cb.time = bleOnTime;
  cb.session = bleOnSession;
  cb.style = bleOnStyle;
  cb.status = bleStatus;
  bleClockSetCallbacks(cb);

  drawStatus(COL_WAIT, "waiting for BLE...");
  if (!bleClockStart()) {
    ESP_LOGE(kTag, "蓝牙启动失败 —— 这个固件没有 WiFi 兜底，通信不会工作");
    drawStatus(COL_FAIL, "BLE failed");
  }

  // 栈开到 8K：播放 GIF 时要在本任务里跑文件 IO 和逐行解码
  xTaskCreate(clockTask, "clock", 8192, nullptr, 4, nullptr);
}
