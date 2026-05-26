#pragma once
#include <Arduino.h>

// ============================================================================
// WIFI CONFIG MODULE
// Quản lý WiFi từ màn hình TFT:
//   - Quét mạng async
//   - Kết nối mạng mới
//   - Quên mạng đã lưu
//   - Cập nhật UI realtime
// ============================================================================

#define WIFI_MAX_NETWORKS   15      // Số mạng tối đa hiển thị
#define WIFI_CONNECT_TIMEOUT_MS 15000  // 15s timeout kết nối

enum class WifiScanState { IDLE, SCAN_REQUESTED, SCANNING };
enum class WifiConnectState { IDLE, CONNECTING, CONNECTED, FAILED };

// Trạng thái hiện tại (đọc từ tasks.cpp để polling)
// volatile vì được đọc/ghi từ cả Core 0 và Core 1
extern volatile WifiScanState    wifiScanState;
extern WifiConnectState wifiConnectState;
extern unsigned long    wifiConnectStartTime;

// Bridge functions declared in lvgl/ui_bridge.h with extern "C" linkage.
// Do NOT redeclare them here to avoid conflicting-declaration errors.

// Polling — gọi từ tasks.cpp
void wifi_poll_scan(void);
void wifi_poll_connect(void);
