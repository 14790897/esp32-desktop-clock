#include "ble_clock.h"

#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>

extern "C" {
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
}

static const char* kTag = "ble";

// 128 位 UUID。注意 BLE_UUID128_INIT 的参数是**小端**（低位字节在前），
// 所以下面这串反过来读才是给人看的 UUID：
//   服务   7d9f0001-5a3e-4c1b-9e21-6b0c4c4f4b00
//   CMD    7d9f0002-…
//   STATUS 7d9f0003-…
static const ble_uuid128_t kSvcUuid =
    BLE_UUID128_INIT(0x00, 0x4b, 0x4f, 0x4c, 0x0c, 0x6b, 0x21, 0x9e,
                     0x1b, 0x4c, 0x3e, 0x5a, 0x01, 0x00, 0x9f, 0x7d);
static const ble_uuid128_t kCmdUuid =
    BLE_UUID128_INIT(0x00, 0x4b, 0x4f, 0x4c, 0x0c, 0x6b, 0x21, 0x9e,
                     0x1b, 0x4c, 0x3e, 0x5a, 0x02, 0x00, 0x9f, 0x7d);
static const ble_uuid128_t kStatusUuid =
    BLE_UUID128_INIT(0x00, 0x4b, 0x4f, 0x4c, 0x0c, 0x6b, 0x21, 0x9e,
                     0x1b, 0x4c, 0x3e, 0x5a, 0x03, 0x00, 0x9f, 0x7d);

static const char* kDeviceName = "esp32-clock";

static BleClockCallbacks gCb = {};
static uint16_t gConnHandle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t gCmdHandle = 0;
static uint16_t gStatusHandle = 0;
static uint8_t gOwnAddrType = 0;

// 单条 CMD 写入的上限。PC 侧按 MTU 分块，块大小远小于这个值。
static const int kMaxCmd = 512;

static void startAdvertising();

// CMD 的负载格式：[opcode][body...]
//   'A' append 文本    'R' replace 文本
//   'S' 状态（body[0] = 'w'/'d'/'f'）
//   'T' 时间（body 4 字节小端 Unix epoch 秒，UTC）
//   'I' 会话标识
//   'Q' 查询（状态走 STATUS 的 read，这里不用做事）
static void handleCommand(const uint8_t* data, int len) {
  if (len <= 0) return;
  const char op = static_cast<char>(data[0]);
  const char* body = reinterpret_cast<const char*>(data + 1);
  const int bodyLen = len - 1;

  switch (op) {
    case 'A':
      if (gCb.append) gCb.append(body, bodyLen);
      break;
    case 'R':
      if (gCb.replace) gCb.replace(body, bodyLen);
      break;
    case 'S':
      if (bodyLen >= 1 && gCb.state) gCb.state(body[0]);
      break;
    case 'T':
      if (bodyLen >= 4 && gCb.time) {
        uint32_t epoch = 0;
        memcpy(&epoch, body, sizeof(epoch));  // ESP32 是小端，和协议一致
        gCb.time(epoch);
      }
      break;
    case 'I':
      if (gCb.session) gCb.session(body, bodyLen);
      break;
    case 'Q':
      break;
    default:
      ESP_LOGW(kTag, "未知 opcode 0x%02x（长度 %d）", static_cast<unsigned>(data[0]), len);
      break;
  }
}

static int gattAccess(uint16_t connHandle, uint16_t attrHandle,
                      struct ble_gatt_access_ctxt* ctxt, void*) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    if (attrHandle != gCmdHandle) return BLE_ATT_ERR_UNLIKELY;
    const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0) return 0;
    if (len > kMaxCmd) {
      ESP_LOGW(kTag, "CMD 太长（%u 字节），丢弃", len);
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t buf[kMaxCmd];
    if (os_mbuf_copydata(ctxt->om, 0, len, buf) != 0) return BLE_ATT_ERR_UNLIKELY;
    handleCommand(buf, len);
    return 0;
  }

  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    if (attrHandle != gStatusHandle) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    if (gCb.status == nullptr) return BLE_ATT_ERR_UNLIKELY;
    static char buf[512];
    const int n = gCb.status(buf, sizeof(buf));
    if (n <= 0) return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, buf, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }

  return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def kSvcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &kSvcUuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &kCmdUuid.u,
                    .access_cb = gattAccess,
                    // 两种都开：PC 侧用 write-without-response，少一次往返
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                    .val_handle = &gCmdHandle,
                },
                {
                    .uuid = &kStatusUuid.u,
                    .access_cb = gattAccess,
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &gStatusHandle,
                },
                {0},
            },
    },
    {0},
};

static int gapEvent(struct ble_gap_event* event, void*) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        gConnHandle = event->connect.conn_handle;
        ESP_LOGI(kTag, "PC 已连接 (handle=%d)", gConnHandle);
      } else {
        ESP_LOGW(kTag, "连接失败 status=%d，继续广播", event->connect.status);
        startAdvertising();
      }
      return 0;

    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGI(kTag, "断开 reason=0x%02x，继续广播", event->disconnect.reason);
      gConnHandle = BLE_HS_CONN_HANDLE_NONE;
      startAdvertising();
      return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
      startAdvertising();
      return 0;

    case BLE_GAP_EVENT_MTU:
      ESP_LOGI(kTag, "MTU 协商为 %d", event->mtu.value);
      return 0;

    default:
      return 0;
  }
}

static void startAdvertising() {
  struct ble_hs_adv_fields fields = {};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

  // 只放名字。128 位服务 UUID 占 18 字节，加上名字会超过 31 字节的广播包上限；
  // PC 连上来再枚举服务就行。
  const char* name = ble_svc_gap_device_name();
  fields.name = reinterpret_cast<uint8_t*>(const_cast<char*>(name));
  fields.name_len = strlen(name);
  fields.name_is_complete = 1;

  int rc = ble_gap_adv_set_fields(&fields);
  if (rc != 0) {
    ESP_LOGE(kTag, "设置广播数据失败 rc=%d", rc);
    return;
  }

  struct ble_gap_adv_params params = {};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  rc = ble_gap_adv_start(gOwnAddrType, nullptr, BLE_HS_FOREVER, &params, gapEvent, nullptr);
  if (rc != 0 && rc != BLE_HS_EALREADY) {
    ESP_LOGE(kTag, "启动广播失败 rc=%d", rc);
  }
}

static void onSync() {
  int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_hs_util_ensure_addr rc=%d", rc);
    return;
  }
  rc = ble_hs_id_infer_auto(0, &gOwnAddrType);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_hs_id_infer_auto rc=%d", rc);
    return;
  }
  startAdvertising();
}

static void onReset(int reason) { ESP_LOGW(kTag, "NimBLE host reset, reason=%d", reason); }

static void hostTask(void*) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void bleClockSetCallbacks(const BleClockCallbacks& cb) { gCb = cb; }

bool bleClockConnected() { return gConnHandle != BLE_HS_CONN_HANDLE_NONE; }

bool bleClockStart() {
  esp_err_t err = nimble_port_init();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "nimble_port_init 失败: %s", esp_err_to_name(err));
    return false;
  }

  ble_hs_cfg.reset_cb = onReset;
  ble_hs_cfg.sync_cb = onSync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

  // 不加密、不配对：这一对设备就在桌上，加配对只会让 PC 侧每次都要处理
  // 配对流程（Windows 还会弹提示）。数据本身也不敏感。
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 0;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 0;

  ble_svc_gap_init();
  ble_svc_gatt_init();

  int rc = ble_gatts_count_cfg(kSvcs);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gatts_count_cfg rc=%d", rc);
    return false;
  }
  rc = ble_gatts_add_svcs(kSvcs);
  if (rc != 0) {
    ESP_LOGE(kTag, "ble_gatts_add_svcs rc=%d", rc);
    return false;
  }

  rc = ble_svc_gap_device_name_set(kDeviceName);
  if (rc != 0) {
    ESP_LOGE(kTag, "设置设备名失败 rc=%d", rc);
    return false;
  }

  nimble_port_freertos_init(hostTask);
  ESP_LOGI(kTag, "BLE 外设已启动，广播名 \"%s\"", kDeviceName);
  return true;
}
