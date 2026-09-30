#pragma once

#include <LovyanGFX.hpp>

// 预分配整屏帧缓冲（240x240x2 = 115KB，需 DMA 可访问）。
// 必须在 WiFi 启动前调用，否则 DMA 内存不够可能分配失败。
// 分配失败会返回 false，此时 gifPlay 退回较慢的逐段推送，仍可播放。
bool gifInitFramebuffer();

// 播放 GIF 一遍。画布比屏幕宽时居中裁切；透明像素跳过以保留上一帧内容。
// 文件打不开或解码失败返回 false。
bool gifPlay(lgfx::LGFX_Device& lcd, const char* path);
