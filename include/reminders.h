#pragma once

#include <stdint.h>

// ============ 定时提醒配置 ============
// 到点后全屏显示提醒文字，持续 REMINDER_SHOW_SECONDS 秒，然后自动回到时钟。
// 增删改直接编辑下面的表，时间用 24 小时制，文字支持中文。
//
// 注意：中文由 LovyanGFX 自带的 efontCN 点阵字库渲染，字形固定不可缩放，
// 用 efontCN_24（约 24px 高）。24px 下 240 宽的屏幕一行约放 9 个汉字，
// 文字超长会被截断，所以提醒语尽量控制在 8 字以内。

struct Reminder {
  uint8_t hour;
  uint8_t minute;
  const char* text;
};

static const Reminder kReminders[] = {
    {11, 59, "该吃饭了"},
    {17, 59, "该下班了"},
};

// 提醒在屏幕上停留的秒数
#define REMINDER_SHOW_SECONDS 60
