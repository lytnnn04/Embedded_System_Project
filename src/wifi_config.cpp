// ============================================================================
// wifi_config.cpp — Quản lý WiFi từ màn hình TFT
// ============================================================================

#include "app_globals.h"
#include "wifi_config.h"
#include "ui_handler.h"
#include <esp_wifi.h>

// -- Public state ------------------------------------------------------------
volatile WifiScanState wifiScanState = WifiScanState::IDLE;
WifiConnectState wifiConnectState    = WifiConnectState::IDLE;
unsigned long    wifiConnectStartTime = 0;

// -- Scan result storage -----------------------------------------------------
static char  _ssids[WIFI_MAX_NETWORKS][33];
static int   _rssi[WIFI_MAX_NETWORKS];
static bool  _secured[WIFI_MAX_NETWORKS];
static int   _netCount = 0;

// SSID đang chờ kết nối
static char  _pendingSSID[33];

// Scan retry
static uint8_t _scanRetries = 0;
static unsigned long _scanStartTime = 0;
#define WIFI_SCAN_ASYNC_TIMEOUT_MS 8000  // 8s timeout toàn bộ 1 lần scan
#define WIFI_SCAN_MAX_RETRIES       2    // Retry tối đa khi scan failed (-2), không retry khi 0 mạng

// ============================================================================
// HELPERS
// ============================================================================

// Chuyển RSSI → chuỗi bar: ████░░░░
static void rssi_bar(int rssi, char *out) {
    int bars;
    if      (rssi >= -50) bars = 4;
    else if (rssi >= -65) bars = 3;
    else if (rssi >= -75) bars = 2;
    else                  bars = 1;
    int i = 0;
    for (; i < bars;     i++) { out[i*3]=(char)0xe2; out[i*3+1]=(char)0x96; out[i*3+2]=(char)0x88; }
    for (; i < 4;        i++) { out[i*3]=(char)0xe2; out[i*3+1]=(char)0x96; out[i*3+2]=(char)0x91; }
    out[12] = '\0';
}

// Cập nhật label trạng thái WiFi hiện tại (SSID + IP) trên wifi screen
static void _refresh_current_labels(void) {
    if (!ui_uiLabelWifiCurrentSSID || !ui_uiLabelWifiCurrentIP) return;
    if (WiFi.status() == WL_CONNECTED) {
        lv_label_set_text(ui_uiLabelWifiCurrentSSID, WiFi.SSID().c_str());
        lv_label_set_text(ui_uiLabelWifiCurrentIP, WiFi.localIP().toString().c_str());
    } else {
        lv_label_set_text(ui_uiLabelWifiCurrentSSID, "Not Connected");
        lv_label_set_text(ui_uiLabelWifiCurrentIP, "");
    }
}

// ============================================================================
// DYNAMIC LIST — callback khi tap vào 1 mạng
// ============================================================================

static void _wifi_item_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= _netCount) return;
    wifi_show_connect_popup(_ssids[idx], _secured[idx]);
}

// Xoá hết item cũ và tạo lại danh sách mạng
static void _populate_list(void) {
    if (!ui_uiPanelWifiList) return;

    lv_obj_clean(ui_uiPanelWifiList);

    // Thiết lập flex layout: các item xếp dọc, cách nhau 4px
    lv_obj_set_flex_flow(ui_uiPanelWifiList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_uiPanelWifiList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ui_uiPanelWifiList, 4, 0);
    lv_obj_set_style_pad_all(ui_uiPanelWifiList, 4, 0);

    if (_netCount == 0) {
        lv_obj_t *lbl = lv_label_create(ui_uiPanelWifiList);
        lv_label_set_text(lbl, "No networks found");
        lv_obj_set_width(lbl, LV_PCT(100));
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
        lv_obj_center(lbl);
        return;
    }

    for (int i = 0; i < _netCount; i++) {
        // Row container
        lv_obj_t *btn = lv_btn_create(ui_uiPanelWifiList);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_height(btn, 42);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1e2a3e), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a3a5e), LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        lv_obj_add_event_cb(btn, _wifi_item_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        // SSID label
        lv_obj_t *ssid_lbl = lv_label_create(btn);
        lv_label_set_text(ssid_lbl, _ssids[i]);
        lv_obj_set_width(ssid_lbl, 180);
        lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(ssid_lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_12, 0);
        lv_obj_align(ssid_lbl, LV_ALIGN_LEFT_MID, 4, 0);

        // Lock icon nếu có bảo mật
        if (_secured[i]) {
            lv_obj_t *lock = lv_label_create(btn);
            lv_label_set_text(lock, LV_SYMBOL_EYE_CLOSE);
            lv_obj_set_style_text_color(lock, lv_color_hex(0xaaaacc), 0);
            lv_obj_set_style_text_font(lock, &lv_font_montserrat_12, 0);
            lv_obj_align(lock, LV_ALIGN_RIGHT_MID, -32, 0);
        }

        // Signal bar
        char bar[16];
        rssi_bar(_rssi[i], bar);
        lv_obj_t *sig = lv_label_create(btn);
        lv_label_set_text(sig, bar);
        lv_obj_set_style_text_color(sig, lv_color_hex(0x4ade80), 0);
        lv_obj_set_style_text_font(sig, &lv_font_montserrat_12, 0);
        lv_obj_align(sig, LV_ALIGN_RIGHT_MID, -2, 0);
    }
}

// ============================================================================
// BRIDGE FUNCTIONS (gọi từ LVGL event — không cần mutex)
// ============================================================================

void enter_wifi_settings(void) {
    // Cập nhật labels trước khi chuyển màn
    _refresh_current_labels();
    // Reset scan status
    if (ui_uiLabelWifiScanStatus)
        lv_label_set_text(ui_uiLabelWifiScanStatus, "Press Scan To Find Networks");
    // Xoá list cũ
    if (ui_uiPanelWifiList) lv_obj_clean(ui_uiPanelWifiList);
    _ui_screen_change(&ui_uiWifi, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, &ui_uiWifi_screen_init);
}

void wifi_on_screen_loaded(void) {
    // Gọi khi WiFi screen loaded — cập nhật labels SSID + IP
    _refresh_current_labels();
    if (ui_uiLabelWifiScanStatus)
        lv_label_set_text(ui_uiLabelWifiScanStatus, "Press Scan To Find Networks");
    safeSerialPrint("[WiFi-UI] Screen loaded, labels refreshed\n");
}

void wifi_scan_start(void) {
    if (wifiScanState == WifiScanState::SCANNING ||
        wifiScanState == WifiScanState::SCAN_REQUESTED) return;

    if (ui_uiPanelWifiList) lv_obj_clean(ui_uiPanelWifiList);

    _scanRetries  = 0;
    if (ui_uiLabelWifiScanStatus)
        lv_label_set_text(ui_uiLabelWifiScanStatus, "Scanning...");
    if (ui_uiBtnWifiScan) lv_obj_add_state(ui_uiBtnWifiScan, LV_STATE_DISABLED);

    // CHỈ set flag — KHÔNG gọi WiFi.scanNetworks() ở đây!
    // Hàm này chạy trên Core 1 (LVGL event), nhưng WiFi stack chạy trên Core 0.
    // Core 0 sẽ nhận flag SCAN_REQUESTED và gọi WiFi.scanNetworks() trong wifi_poll_scan().
    wifiScanState = WifiScanState::SCAN_REQUESTED;
    safeSerialPrint("[WiFi-UI] Scan requested from TFT (Core 1 → Core 0)\n");
}

void wifi_scan_cancel(void) {
    if (wifiScanState != WifiScanState::IDLE) {
        WiFi.scanDelete();
        wifiScanState = WifiScanState::IDLE;
    }
    wifiConnectState = WifiConnectState::IDLE;
}

void wifi_forget(void) {
    safeSerialPrint("[WiFi-UI] Forgetting WiFi credentials\n");
    wifiManagerAsync.resetSettings();
    WiFi.disconnect(true);
    systemStatus.wifiConnected = false;
    if (ui_uiLabelWifiCurrentSSID)
        lv_label_set_text(ui_uiLabelWifiCurrentSSID, "Not Connected");
    if (ui_uiLabelWifiCurrentIP)
        lv_label_set_text(ui_uiLabelWifiCurrentIP, "");
    if (ui_uiLabelWifiScanStatus)
        lv_label_set_text(ui_uiLabelWifiScanStatus, "WiFi credentials cleared");
}

void wifi_connect_to(const char *ssid, const char *pass) {
    if (!ssid || strlen(ssid) == 0) return;

    safeSerialPrintf("[WiFi-UI] Connecting to: %s\n", ssid);

    // Lưu credentials vào Preferences (cùng namespace với WiFiManagerAsync)
    Preferences prefs;
    prefs.begin("wifi_config", false);  // Namespace mặc định của WiFiManagerAsync là "wifi_config"
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass ? pass : "");  // KEY PHẢI LÀ "pass" — khớp với WiFiManagerAsync
    prefs.end();

    // Cập nhật UI connect popup
    if (ui_uiLabelConnectMsg)
        lv_label_set_text(ui_uiLabelConnectMsg, "Connecting...");
    if (ui_uiBtnWifiConnectOK)
        lv_obj_add_state(ui_uiBtnWifiConnectOK, LV_STATE_DISABLED);

    // Kết nối WiFi
    WiFi.disconnect(false);
    WiFi.begin(ssid, (pass && strlen(pass) > 0) ? pass : nullptr);

    wifiConnectState     = WifiConnectState::CONNECTING;
    wifiConnectStartTime = millis();
}

void wifi_show_connect_popup(const char *ssid, bool secured) {
    if (!ssid) return;
    strncpy(_pendingSSID, ssid, sizeof(_pendingSSID) - 1);
    _pendingSSID[sizeof(_pendingSSID) - 1] = '\0';

    // Nếu không có mật khẩu → kết nối luôn
    if (!secured) {
        wifi_connect_to(ssid, "");
        // Vẫn hiện popup để user thấy trạng thái
    }

    if (ui_uiLabelConnectSSID) lv_label_set_text(ui_uiLabelConnectSSID, ssid);
    if (ui_uiTextAreaWifiPass) lv_textarea_set_text(ui_uiTextAreaWifiPass, "");
    if (ui_uiLabelConnectMsg)  lv_label_set_text(ui_uiLabelConnectMsg, "");
    if (ui_Passhint) {
        if (secured) lv_obj_clear_flag(ui_Passhint, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(ui_Passhint, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_uiTextAreaWifiPass) {
        if (secured) lv_obj_clear_flag(ui_uiTextAreaWifiPass, LV_OBJ_FLAG_HIDDEN);
        else         lv_obj_add_flag(ui_uiTextAreaWifiPass, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_uiBtnWifiConnectOK) {
        lv_obj_clear_state(ui_uiBtnWifiConnectOK, LV_STATE_DISABLED);
        if (!secured) lv_obj_add_flag(ui_uiBtnWifiConnectOK, LV_OBJ_FLAG_HIDDEN);
        else          lv_obj_clear_flag(ui_uiBtnWifiConnectOK, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_uiPanelWifiOverlay) lv_obj_clear_flag(ui_uiPanelWifiOverlay, LV_OBJ_FLAG_HIDDEN);
    if (ui_uiPanelWifiConnect) lv_obj_clear_flag(ui_uiPanelWifiConnect, LV_OBJ_FLAG_HIDDEN);
    // Mở keyboard — di chuyển lên vùng nhìn thấy
    if (ui_uiKeyboardWifi && secured) {
        lv_obj_set_y(ui_uiKeyboardWifi, 120);  // Keyboard 240px cao, bottom sát đáy 480px
        lv_obj_clear_flag(ui_uiKeyboardWifi, LV_OBJ_FLAG_HIDDEN);
    }
    // Dịch popup connect lên cao hơn để không bị keyboard che
    if (ui_uiPanelWifiConnect) {
        lv_obj_set_y(ui_uiPanelWifiConnect, secured ? -150 : -62);
    }
}

void wifi_hide_connect_popup(void) {
    if (ui_uiPanelWifiOverlay) lv_obj_add_flag(ui_uiPanelWifiOverlay, LV_OBJ_FLAG_HIDDEN);
    if (ui_uiPanelWifiConnect) {
        lv_obj_add_flag(ui_uiPanelWifiConnect, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(ui_uiPanelWifiConnect, -62);  // reset vị trí gốc
    }
    if (ui_uiKeyboardWifi) {
        lv_obj_add_flag(ui_uiKeyboardWifi, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(ui_uiKeyboardWifi, 500);  // đẩy về off-screen
    }
    if (ui_uiTextAreaWifiPass) lv_textarea_set_text(ui_uiTextAreaWifiPass, "");
    if (ui_uiLabelConnectMsg)  lv_label_set_text(ui_uiLabelConnectMsg, "");
    wifiConnectState = WifiConnectState::IDLE;
}

// ============================================================================
// POLLING (gọi từ tasks.cpp — cần xMutexLVGL)
// ============================================================================

void wifi_poll_scan(void) {
    // ── Bước 1: SCAN_REQUESTED → khởi động async scan, trả về ngay ──────────
    if (wifiScanState == WifiScanState::SCAN_REQUESTED) {
        wifiScanState = WifiScanState::SCANNING;
        _scanRetries  = 0;
        _scanStartTime = millis();
        safeSerialPrintf("[WiFi-UI] Async scan started on Core %d\n", xPortGetCoreID());

        // Đảm bảo STA mode đang bật
        if ((WiFi.getMode() & WIFI_MODE_STA) == 0) {
            WiFi.mode(WIFI_AP_STA);
        }

        WiFi.scanDelete();
        // async=true: hàm trả về ngay lập tức, không block
        // show_hidden=false, passive=false → active scan nhanh hơn
        int ret = WiFi.scanNetworks(/*async*/true, /*show_hidden*/false);
        if (ret == WIFI_SCAN_FAILED) {
            // Driver từ chối bắt đầu scan (hiếm) → báo lỗi ngay
            safeSerialPrint("[WiFi-UI] Failed to start async scan\n");
            wifiScanState = WifiScanState::IDLE;
            if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(100))) {
                if (ui_uiLabelWifiScanStatus)
                    lv_label_set_text(ui_uiLabelWifiScanStatus, "Scan Error! Try again.");
                if (ui_uiBtnWifiScan)
                    lv_obj_clear_state(ui_uiBtnWifiScan, LV_STATE_DISABLED);
                xSemaphoreGive(xMutexLVGL);
            }
        }
        return; // Trả về ngay, không block task
    }

    // ── Bước 2: SCANNING → kiểm tra kết quả mỗi 100ms ──────────────────────
    if (wifiScanState == WifiScanState::SCANNING) {
        int n = WiFi.scanComplete();

        if (n == WIFI_SCAN_RUNNING) {
            // Scan vẫn đang chạy — kiểm tra timeout
            if (millis() - _scanStartTime > WIFI_SCAN_ASYNC_TIMEOUT_MS) {
                safeSerialPrint("[WiFi-UI] Async scan timeout\n");
                WiFi.scanDelete();
                wifiScanState = WifiScanState::IDLE;
                if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(100))) {
                    if (ui_uiLabelWifiScanStatus)
                        lv_label_set_text(ui_uiLabelWifiScanStatus, "Scan timeout! Try again.");
                    if (ui_uiBtnWifiScan)
                        lv_obj_clear_state(ui_uiBtnWifiScan, LV_STATE_DISABLED);
                    xSemaphoreGive(xMutexLVGL);
                }
            }
            return; // Chưa xong, thử lại vòng tiếp theo
        }

        // Scan đã hoàn thành (n >= 0) hoặc lỗi (n == WIFI_SCAN_FAILED)
        if (n == WIFI_SCAN_FAILED && _scanRetries < WIFI_SCAN_MAX_RETRIES) {
            // Chỉ retry khi lỗi thực sự, không retry khi 0 mạng
            _scanRetries++;
            _scanStartTime = millis();
            safeSerialPrintf("[WiFi-UI] Scan failed, retry %d/%d\n",
                             _scanRetries, WIFI_SCAN_MAX_RETRIES);
            WiFi.scanDelete();
            WiFi.scanNetworks(/*async*/true, /*show_hidden*/false);
            return;
        }

        // Cập nhật UI
        if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(500))) {
            if (ui_uiBtnWifiScan)
                lv_obj_clear_state(ui_uiBtnWifiScan, LV_STATE_DISABLED);

            if (n == WIFI_SCAN_FAILED) {
                if (ui_uiLabelWifiScanStatus)
                    lv_label_set_text(ui_uiLabelWifiScanStatus, "Scan Error! Try again.");
                _netCount = 0;
            } else if (n == 0) {
                if (ui_uiLabelWifiScanStatus)
                    lv_label_set_text(ui_uiLabelWifiScanStatus, "No networks found");
                _netCount = 0;
            } else {
                _netCount = (n > WIFI_MAX_NETWORKS) ? WIFI_MAX_NETWORKS : n;
                for (int i = 0; i < _netCount; i++) {
                    strncpy(_ssids[i], WiFi.SSID(i).c_str(), 32);
                    _ssids[i][32] = '\0';
                    _rssi[i]     = WiFi.RSSI(i);
                    _secured[i]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
                }
                char buf[32];
                snprintf(buf, sizeof(buf), "Found %d networks", _netCount);
                if (ui_uiLabelWifiScanStatus)
                    lv_label_set_text(ui_uiLabelWifiScanStatus, buf);
            }

            WiFi.scanDelete();
            _populate_list();
            xSemaphoreGive(xMutexLVGL);
        } else {
            safeSerialPrint("[WiFi-UI] Timeout waiting for LVGL mutex\n");
            WiFi.scanDelete();
        }

        wifiScanState = WifiScanState::IDLE;
        safeSerialPrintf("[WiFi-UI] Scan finished: %d networks\n", n);
    }
}

void wifi_poll_connect(void) {
    if (wifiConnectState != WifiConnectState::CONNECTING) return;

    static unsigned long connectedAt = 0;

    wl_status_t status = WiFi.status();

    if (status == WL_CONNECTED) {
        if (connectedAt == 0) {
            // Vừa kết nối xong — hiển thị "Connected!" ngay
            connectedAt = millis();
            systemStatus.wifiConnected = true;
            safeSerialPrintf("[WiFi-UI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
            if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(20))) {
                if (ui_uiLabelConnectMsg)
                    lv_label_set_text(ui_uiLabelConnectMsg, "Connected!");
                xSemaphoreGive(xMutexLVGL);
            }
        } else if (millis() - connectedAt > 1500) {
            // Sau 1.5s → đóng popup
            connectedAt = 0;
            wifiConnectState = WifiConnectState::CONNECTED;
            if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(20))) {
                wifi_hide_connect_popup();
                _refresh_current_labels();
                xSemaphoreGive(xMutexLVGL);
            }
        }
        return;
    }

    // Timeout
    if (millis() - wifiConnectStartTime > WIFI_CONNECT_TIMEOUT_MS) {
        connectedAt = 0;
        wifiConnectState = WifiConnectState::FAILED;
        WiFi.disconnect();
        safeSerialPrint("[WiFi-UI] Connect timeout\n");
        if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(20))) {
            if (ui_uiLabelConnectMsg)
                lv_label_set_text(ui_uiLabelConnectMsg, "Failed! Check password");
            if (ui_uiBtnWifiConnectOK)
                lv_obj_clear_state(ui_uiBtnWifiConnectOK, LV_STATE_DISABLED);
            xSemaphoreGive(xMutexLVGL);
        }
        return;
    }

    // Hiển thị trạng thái đang kết nối với dấu chấm động
    static uint8_t dots = 0;
    dots = (dots + 1) % 4;
    char msg[20];
    snprintf(msg, sizeof(msg), "Connecting%.*s", dots, "...");
    if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(10))) {
        if (ui_uiLabelConnectMsg) lv_label_set_text(ui_uiLabelConnectMsg, msg);
        xSemaphoreGive(xMutexLVGL);
    }
}
