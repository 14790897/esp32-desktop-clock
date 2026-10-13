#pragma once

#include <LovyanGFX.hpp>

// 预分配整屏帧缓冲（240x240x2 = 115KB，需 DMA 可访问）。
// 必须在 WiFi 启动前调用，否则 DMA 内存不够可能分配失败。
// 分配失败会返回 false，此时 gifPlay 退回较慢的逐段推送，仍可播放。
bool gifInitFramebuffer();

// 释放帧缓冲，把 115KB 还给堆。配网模式下必须还 —— 实测堆本来就只剩
// 100KB 出头，被它占着时 lwIP 发 DHCP ACK 会因分配不到内存而失败
// （udp_sendto 返回 -1 ERR_MEM），客户端就卡在「获取 IP 地址」。
// gifPlay 内部会自己重新分配，所以调用方不用管配对。
void gifFreeFramebuffer();

// 帧缓冲当前是不是空的（被 gifFreeFramebuffer 释放过、还没补回来）。
// 联网后据此决定要不要补 —— 不补的话 GIF 会一直走逐段推送那条慢路。
bool gifFramebufferMissing();

// 播放 GIF 一遍。画布比屏幕宽时居中裁切；透明像素跳过以保留上一帧内容。
// 文件打不开或解码失败返回 false。
bool gifPlay(lgfx::LGFX_Device& lcd, const char* path);
