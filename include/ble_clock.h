#pragma once

#include <cstdint>

// 时钟的 BLE 外设（NimBLE）。设备只作 peripheral，PC 作 central 连上来推数据。
//
// 之所以整个通信走 BLE 而不是 WiFi：WiFi 那条路要配网、要 DHCP、地址会漂，
// 出问题时排查成本极高；蓝牙没有这些问题，代价是上不了互联网 —— 所以时间
// 由 PC 通过 BLE 推过来（见 time 回调）。
//
// GATT 结构：
//   服务 7d9f0001-5a3e-4c1b-9e21-6b0c4c4f4b00
//     ├─ CMD    7d9f0002-…   write        指令（见 BleClockCallbacks 的 opcode）
//     └─ STATUS 7d9f0003-…   read/notify  当前字幕状态（JSON）

struct BleClockCallbacks {
  // 追加文本（实时回显）。len 是字节数，不保证以 '\0' 结尾。
  void (*append)(const char* s, int len);
  // 整体替换文本
  void (*replace)(const char* s, int len);
  // 状态：'w' 工作中 / 'd' 完成 / 'f' 失败
  void (*state)(char c);
  // 设置系统时间（Unix epoch 秒，UTC）
  void (*time)(uint32_t epoch);
  // 会话标识。换了会话字幕要清空重来。
  void (*session)(const char* sid, int len);
  // 把状态快照写成 JSON 放进 buf，返回写入长度；<=0 表示失败。
  int (*status)(char* buf, int cap);
};

// 注册回调。必须在 bleClockStart() 之前调用。
void bleClockSetCallbacks(const BleClockCallbacks& cb);

// 启动 BLE 外设并开始广播。失败返回 false。
bool bleClockStart();

// 有客户端连着时返回 true（屏幕上可据此显示连接状态）。
bool bleClockConnected();
