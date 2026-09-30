#include "gif_player.h"

#include <AnimatedGIF.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

namespace {

constexpr int kScreenW = SCREEN_WIDTH;
constexpr int kScreenH = SCREEN_HEIGHT;
constexpr const char* kTag = "gif";

lgfx::LGFX_Device* gLcd = nullptr;
AnimatedGIF gGif;

int64_t gDrawUs = 0;

// 整屏帧缓冲，分上下两半。透明像素要透出上一帧的内容，画布必须持久保留；
// 顺带把「每行多次小 SPI 推送」合并成「每帧两次整屏推送」。
//
// 为什么分两半：实测堆里凑不出连续的 115200 字节（最大连续块只有 114688），
// 拆成两个 57600 字节的块就能稳定分配。
constexpr int kFbHalves = 2;
constexpr int kFbRows = kScreenH / kFbHalves;

uint16_t* gFb[kFbHalves] = {nullptr, nullptr};
bool gFbIsDma = false;

inline uint16_t* fbRow(int y) {
  return gFb[y / kFbRows] + static_cast<size_t>(y % kFbRows) * kScreenW;
}

// 单行像素缓冲，只在没有帧缓冲时使用
uint16_t gLineBuf[kScreenW];

// ---- 文件回调：走 ESP-IDF 的 VFS，直接用 stdio ----
// 读/定位回调必须自己维护 pFile->iPos —— 解码器靠它跟踪解析位置，
// 不更新的话每帧都会重新读取同一段数据，表现就是只出一帧且循环不结束
void* fileOpen(const char* filename, int32_t* size) {
  FILE* f = fopen(filename, "rb");
  if (f == nullptr) return nullptr;
  fseek(f, 0, SEEK_END);
  *size = static_cast<int32_t>(ftell(f));
  fseek(f, 0, SEEK_SET);
  return f;
}

void fileClose(void* handle) {
  if (handle != nullptr) fclose(static_cast<FILE*>(handle));
}

int32_t fileRead(GIFFILE* pFile, uint8_t* buf, int32_t len) {
  int32_t want = len;
  const int32_t remain = pFile->iSize - pFile->iPos;
  if (remain < want) want = remain;
  if (want <= 0) return 0;

  const int32_t got = static_cast<int32_t>(fread(buf, 1, want, static_cast<FILE*>(pFile->fHandle)));
  pFile->iPos += got;
  return got;
}

int32_t fileSeek(GIFFILE* pFile, int32_t pos) {
  if (fseek(static_cast<FILE*>(pFile->fHandle), pos, SEEK_SET) != 0) return -1;
  pFile->iPos = pos;
  return pos;
}

// 把本行像素写进帧缓冲。透明像素原样保留（即上一帧的内容）。
void blitLineToFb(GIFDRAW* pDraw, int from, int to, int screenX0) {
  const uint16_t* pal = pDraw->pPalette;
  const uint8_t* src = pDraw->pPixels;
  const bool hasAlpha = pDraw->ucHasTransparency != 0;
  const int dstX = pDraw->iX + from - screenX0;
  uint16_t* dst = fbRow(pDraw->iY + pDraw->y) + dstX;

  for (int k = from; k < to; ++k, ++dst) {
    const uint8_t idx = src[k];
    if (hasAlpha && idx == pDraw->ucTransparent) continue;
    *dst = pal[idx];
  }
}

void pushHalf(int half, uint16_t* buf) {
  gLcd->startWrite();
  if (gFbIsDma) {
    gLcd->pushImageDMA(0, half * kFbRows, kScreenW, kFbRows, buf);
    gLcd->waitDMA();
  } else {
    gLcd->pushImage(0, half * kFbRows, kScreenW, kFbRows, buf);
  }
  gLcd->endWrite();
}

// ---- 每行绘制回调 ----
void drawLine(GIFDRAW* pDraw) {
  const int64_t t0 = esp_timer_get_time();

  const int cropX = (pDraw->iCanvasWidth - kScreenW) / 2;  // 宽画布居中裁切
  const int canvasY = pDraw->iY + pDraw->y;
  if (canvasY < 0 || canvasY >= kScreenH) return;

  // 本行在画布上的可见列区间，换算成帧内下标 [from, to)
  const int from = cropX - pDraw->iX > 0 ? cropX - pDraw->iX : 0;
  int to = cropX + kScreenW - pDraw->iX;
  if (to > pDraw->iWidth) to = pDraw->iWidth;
  if (from >= to) return;

  if (gFb[0] != nullptr) {
    blitLineToFb(pDraw, from, to, cropX);
    // 最后一行的到达即表示这一帧画完了，把整屏推出去
    if (pDraw->y == pDraw->iHeight - 1) {
      for (int half = 0; half < kFbHalves; ++half) pushHalf(half, gFb[half]);
    }
    gDrawUs += esp_timer_get_time() - t0;
    return;
  }

  // 无帧缓冲的退路：只推不透明的连续段
  const uint16_t* pal = pDraw->pPalette;
  const uint8_t* src = pDraw->pPixels;
  const bool hasAlpha = pDraw->ucHasTransparency != 0;

  int k = from;
  while (k < to) {
    while (k < to && hasAlpha && src[k] == pDraw->ucTransparent) ++k;
    const int runStart = k;
    while (k < to && !(hasAlpha && src[k] == pDraw->ucTransparent)) ++k;
    const int runLen = k - runStart;
    if (runLen <= 0) continue;

    for (int i = 0; i < runLen; ++i) gLineBuf[i] = pal[src[runStart + i]];
    gLcd->pushImage(pDraw->iX + runStart - cropX, canvasY, runLen, 1, gLineBuf);
  }

  gDrawUs += esp_timer_get_time() - t0;
}

}  // namespace

bool gifInitFramebuffer() {
  if (gFb[0] != nullptr) return true;

  const size_t halfBytes = static_cast<size_t>(kScreenW) * kFbRows * sizeof(uint16_t);
  for (int half = 0; half < kFbHalves; ++half) {
    gFb[half] = static_cast<uint16_t*>(heap_caps_malloc(halfBytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (gFb[half] != nullptr) {
      gFbIsDma = true;
      continue;
    }
    gFbIsDma = false;
    gFb[half] = static_cast<uint16_t*>(heap_caps_malloc(halfBytes, MALLOC_CAP_8BIT));
    if (gFb[half] == nullptr) {
      ESP_LOGW(kTag, "帧缓冲分配失败（每半需 %u 字节，最大连续块 %u），退回逐段推送",
               static_cast<unsigned>(halfBytes),
               static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
      for (int i = 0; i < half; ++i) {
        free(gFb[i]);
        gFb[i] = nullptr;
      }
      return false;
    }
  }

  ESP_LOGI(kTag, "帧缓冲就绪 %d×%u 字节分 %d 块，推送方式=整屏%s", kFbHalves,
           static_cast<unsigned>(halfBytes), kFbHalves, gFbIsDma ? " DMA" : " CPU");
  return true;
}

bool gifPlay(lgfx::LGFX_Device& lcd, const char* path) {
  gLcd = &lcd;

  if (!gGif.open(path, fileOpen, fileClose, fileRead, fileSeek, drawLine)) {
    ESP_LOGE(kTag, "打不开或不是合法的 GIF: %s (err=%d)", path, gGif.getLastError());
    return false;
  }
  ESP_LOGI(kTag, "播放 %s  画布 %dx%d", path, gGif.getCanvasWidth(), gGif.getCanvasHeight());

  const int64_t tStart = esp_timer_get_time();
  gDrawUs = 0;
  // 帧缓冲留着上一帧的内容，透明像素才能正确透出来；起播前清成黑底
  if (gFb[0] != nullptr) {
    const size_t halfPixels = static_cast<size_t>(kScreenW) * kFbRows;
    for (int half = 0; half < kFbHalves; ++half) {
      for (size_t i = 0; i < halfPixels; ++i) gFb[half][i] = 0;
    }
    lcd.fillScreen(0);
  }

  int delay = 0;
  int frames = 0;
  for (;;) {
    const int64_t tFrame = esp_timer_get_time();
    if (!gGif.playFrame(false, &delay, nullptr)) break;
    ++frames;

    // 按 GIF 的帧间隔校准：扣掉本帧解码+绘制已花掉的时间。
    // 不扣的话渲染耗时会叠加到帧延迟上，整段动画就会比原速慢。
    const int elapsedMs = static_cast<int>((esp_timer_get_time() - tFrame) / 1000);
    int waitMs = delay - elapsedMs;
    if (waitMs < 1) waitMs = 1;
    vTaskDelay(pdMS_TO_TICKS(waitMs));
  }
  gGif.close();

  const int64_t totalMs = (esp_timer_get_time() - tStart) / 1000;
  ESP_LOGI(kTag, "播放结束，共 %d 帧，耗时 %lld ms，其中绘制 %lld ms", frames,
           static_cast<long long>(totalMs), static_cast<long long>(gDrawUs / 1000));
  return true;
}
