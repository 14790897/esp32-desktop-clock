#pragma once

// ==================== 时钟配置 ====================
// 这里只放**不含隐私**的配置 —— 本文件会进版本库。
// WiFi 账号密码不在这里：设备用配网门户自己记（见 README），不需要写死。

// ---- 对时 ----
// 用 IP 而不是域名：Windows ICS 等网络的 DNS 代理常常不应答非 Windows 客户端
// 的查询（实测 getaddrinfo 返回 rc=202 EAI_FAIL），而 SNTP 遇到解析失败不会
// 报错，只会静默地永远同步不上。写死 IP 可绕开这个坑。
// 若 IP 日后失效，可用 `nslookup ntp.aliyun.com` 取新地址。
#define NTP_SERVER_1 "203.107.6.88"     // ntp.aliyun.com
#define NTP_SERVER_2 "162.159.200.123"  // cn.pool.ntp.org

// POSIX 时区字符串，CST-8 即东八区（北京时间）
// 其他常用值：JST-9（东京）、EST5EDT（纽约）、GMT0（伦敦）
#define TZ_INFO "CST-8"

// ---- 配网门户（首次上电或连不上时生效）----
#define WIFI_AP_PREFIX "esp32-clock"  // 热点名形如 esp32-clock-A1B2
#define WIFI_HOSTNAME "esp32-clock"   // 向路由器注册的主机名
