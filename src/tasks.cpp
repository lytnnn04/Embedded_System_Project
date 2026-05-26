#include "tasks.h"
#include "app_globals.h"
#include "fsm.h"
#include "security.h"
#include "email_manager.h"
#include "data_store.h"
#include "rtc_manager.h"
#include "ui_handler.h"
#include "wifi_config.h"
#include "hal/buzzer.h"
#include "hal/door_sensor.h"
#include "hal/nfc_reader.h"
#include "tft_display.h"

// ============================================================================
// TASK 2: FSM CONTROLLER (Core 1, Priority 3)
// ============================================================================

void taskFSM(void *parameter) {
  SystemEvent event;

  for (;;) {
    // Receive event from queue (block up to 100ms)
    if (xQueueReceive(eventQueue, &event, pdMS_TO_TICKS(100))) {
      if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(50))) {
        fsmProcessEvent(event);
        xSemaphoreGive(xMutexState);
      }
    }

    // Check state timeout conditions (e.g., lockout expiry)
    if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(50))) {
      fsmHandleState();
      xSemaphoreGive(xMutexState);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ============================================================================
// TASK 3: WiFi MONITOR (Core 0, Priority 1)
// ============================================================================

void taskWiFiMonitor(void *parameter) {
  bool prevWifiConnected = false;
  for (;;) {
    // Chỉ cho phép Auto-Manager chạy nền nếu màn hình KHÔNG quét Wi-Fi
    if (wifiScanState == WifiScanState::IDLE) {
        wifiManagerAsync.loop();
    }
    bool nowConnected = wifiManagerAsync.isConnected();
    systemStatus.wifiConnected = nowConnected;

    // Sync NTP ngay khi WiFi vừa kết nối
    if (nowConnected && !prevWifiConnected) {
      safeSerialPrint("[WiFi] Connected! Triggering immediate NTP sync...\n");
      configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");
      // taskPowerManager will detect justConnected and call syncRTCWithNTP()
      // No blocking delay here to avoid stalling wifi_poll_scan/connect
    }
    prevWifiConnected = nowConnected;

    // WiFi scan & connect polling chạy trên Core 0 — cùng core với WiFi stack
    wifi_poll_scan();
    wifi_poll_connect();

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============================================================================
// TASK 4: DISPLAY (Core 1, Priority 2) — LVGL + screen sync
// ============================================================================

void taskDisplay(void *parameter) {
  TickType_t xLastWakeTime = xTaskGetTickCount();

  // Wait 500ms for LVGL + all objects to finish init
  vTaskDelay(pdMS_TO_TICKS(500));

  // Show boot screen
  if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(500))) {
    lv_disp_load_scr(ui_uiboot);
    xSemaphoreGive(xMutexLVGL);
    Serial.println("[Display] Boot screen loaded");
  } else {
    Serial.println("[Display] WARN: Failed to take LVGL mutex for boot screen!");
  }

  // Wait for setup() to signal systemReady (max 30s)
  for (int i = 0; i < 300 && !systemReady; i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // Extra render time before transitioning
  vTaskDelay(pdMS_TO_TICKS(500));

  // Transition away from boot screen
  if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(200))) {
    if (lv_scr_act() == ui_uiboot) {
      SystemState bootState = currentState;
      if (bootState == STATE_PASSWORD_LOCKOUT ||
          bootState == STATE_SYSTEM_DISABLED  ||
          bootState == STATE_ALARM) {
        _ui_screen_change(&ui_uidisabled, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0,
                          &ui_uidisabled_screen_init);
        Serial.println("[Display] Switched to disabled screen (lockout restored)");
      } else {
        _ui_screen_change(&ui_uilock, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0,
                          &ui_uilock_screen_init);
        Serial.println("[Display] Switched to lock screen");
      }
    }
    xSemaphoreGive(xMutexLVGL);
  }

  for (;;) {
    // 1. LVGL timer handler
    if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(10))) {
      lv_timer_handler();
      xSemaphoreGive(xMutexLVGL);
    }

    // 2. Sync FSM state → LVGL screen every 100ms
    static unsigned long lastScreenSync = 0;
    if (millis() - lastScreenSync > 100) {
      updateScreenFromState();
      lastScreenSync = millis();
    }

    // 3. Update isSystemLockedOut for UI events
    if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(5))) {
      time_t now = getRTCTime();
      isSystemLockedOut =
          security.systemDisabled ||
          (security.lockoutEndTime > 0 && now < security.lockoutEndTime);
      xSemaphoreGive(xMutexState);
    }

    // 4. Periodic checks every 1000ms
    static unsigned long lastPeriodicCheck = 0;
    if (millis() - lastPeriodicCheck >= 1000) {
      lastPeriodicCheck = millis();

      // 4a. Clock display update
      char timeBuf[16];
      bool gotTime = false;
      if (rtcAvailable) {
        time_t nowT = getRTCTime();
        struct tm *t = localtime(&nowT);
        if (t) { strftime(timeBuf, sizeof(timeBuf), "%H:%M", t); gotTime = true; }
      }
      if (!gotTime) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 0)) {
          strftime(timeBuf, sizeof(timeBuf), "%H:%M", &timeinfo);
          gotTime = true;
        }
      }
      if (!gotTime) {
        strcpy(timeBuf, "--:--"); // Fallback if no RTC and no SNTP
      }

      if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(20))) {
        if (ui_Time) lv_label_set_text(ui_Time, timeBuf);
        
        // Date
        char dateBuf[32];
        bool gotDate = false;
        if (rtcAvailable) {
          time_t nowD = getRTCTime();
          struct tm *td = localtime(&nowD);
          if (td) { strftime(dateBuf, sizeof(dateBuf), "%A, %d/%m/%Y", td); gotDate = true; }
        }
        if (!gotDate) {
          struct tm tdi;
          if (getLocalTime(&tdi, 0)) {
            strftime(dateBuf, sizeof(dateBuf), "%A, %d/%m/%Y", &tdi);
            gotDate = true;
          }
        }
        if (!gotDate) {
          strcpy(dateBuf, "No Time Sync");
        }
        if (ui_Date) lv_label_set_text(ui_Date, dateBuf);
        
        xSemaphoreGive(xMutexLVGL);
      }

      // 4b. Backlight timeout (configurable via Screen Timeout setting)
      if (!backlightIsOff &&
          (millis() - lastUserActivityTime >= (unsigned long)settings.screenTimeout * 1000UL)) {
        tft_set_backlight(0);
        backlightIsOff = true;
        Serial.println("[BL] Backlight OFF (inactivity)");
      }

      // 4c. Lockout countdown on uidisabled screen
      {
        SystemState curState = STATE_IDLE;
        time_t lockEnd = 0;
        if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(5))) {
          curState = currentState;
          lockEnd  = security.lockoutEndTime;
          xSemaphoreGive(xMutexState);
        }
        if (curState == STATE_PASSWORD_LOCKOUT) {
          time_t now = getRTCTime();
          long remaining = (long)(lockEnd - now);
          if (remaining < 0) remaining = 0;
          int mm = (int)(remaining / 60);
          int ss = (int)(remaining % 60);
          char cntBuf[8];
          snprintf(cntBuf, sizeof(cntBuf), "%02d:%02d", mm, ss);
          int arcVal = (lockoutDurationSec > 0)
                           ? (int)(remaining * 100 / lockoutDurationSec)
                           : 0;
          arcVal = (arcVal < 0) ? 0 : (arcVal > 100 ? 100 : arcVal);
          if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(10))) {
            if (ui_cntdown) lv_label_set_text(ui_cntdown, cntBuf);
            if (ui_Arc1)    lv_arc_set_value(ui_Arc1, arcVal);
            xSemaphoreGive(xMutexLVGL);
          }
        }
      }

      // 4d. (Removed: auto-relock timer countdown — relock is now triggered by door close)

      // 4e. TFT NFC enrollment result poll
      if (tftEnrollActive && cardScanGotResult) {
        tftEnrollActive = false;
        cardScanGotResult = false; // prevent 4f block double-firing
        if (cardScanSuccess && strlen(lastScannedCardId) > 0) {
          if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
            if (selectedUserIndex >= 0 && selectedUserIndex < userCount) {
              strncpy(users[selectedUserIndex].cardId, lastScannedCardId,
                      sizeof(users[selectedUserIndex].cardId) - 1);
              safeSerialPrintf("[NFC] Card %s assigned to %s\n",
                               lastScannedCardId,
                               users[selectedUserIndex].name);
              addLog(users[selectedUserIndex].name, "Card", "System Alert",
                     "NFC card enrolled via TFT");
            }
            xSemaphoreGive(xMutexData);
          }
          saveData();
          if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(50))) {
            if (ui_LabelScanStatus)
              lv_label_set_text(ui_LabelScanStatus, "Card assigned!");
            xSemaphoreGive(xMutexLVGL);
          }
          beepSuccess();
          vTaskDelay(pdMS_TO_TICKS(1500));
          if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(50))) {
            enter_add_card();
            xSemaphoreGive(xMutexLVGL);
          }
        } else {
          if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(50))) {
            if (ui_LabelScanStatus)
              lv_label_set_text(ui_LabelScanStatus, "Scan failed/timeout");
            xSemaphoreGive(xMutexLVGL);
          }
          beepError();
          vTaskDelay(pdMS_TO_TICKS(1500));
          if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(50))) {
            enter_add_card();
            xSemaphoreGive(xMutexLVGL);
          }
        }
      }

      // 4f. Auto-return to lock screen after failed door card scan
      {
        static unsigned long doorScanFailShowTime = 0;
        if (!tftEnrollActive && cardScanGotResult && !cardScanSuccess &&
            lv_scr_act() == ui_uiscancard && currentState == STATE_LOCKED) {
          if (doorScanFailShowTime == 0)
            doorScanFailShowTime = millis();
          if (millis() - doorScanFailShowTime > 2000) {
            doorScanFailShowTime = 0;
            cardScanGotResult = false;
            if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(50))) {
              _ui_screen_change(&ui_uilock, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0,
                                &ui_uilock_screen_init);
              xSemaphoreGive(xMutexLVGL);
            }
          }
        } else if (cardScanActive) {
          doorScanFailShowTime = 0;
        }
      }
      // 4g. If user navigated away while enrollment was active, cancel it
      if (tftEnrollActive && lv_scr_act() != ui_uiscancard) {
        tftEnrollActive = false;
        stopCardScan();
      }
    } // end periodic 1s block

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(5));
  }
}

// ============================================================================
// TASK 6: EMAIL SENDER (Core 0, Priority 1)
// ============================================================================

void taskEmail(void *parameter) {
  EmailAlert alert;

  for (;;) {
    if (xQueueReceive(emailQueue, &alert, pdMS_TO_TICKS(1000))) {
      if (smtpSettings.enabled && strlen(settings.notificationEmail) > 0) {
        sendEmailAlert(alert.subject, alert.message);
      } else {
        safeSerialPrint("[Email] Skipped - Not configured\n");
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ============================================================================
// TASK 7: FACTORY RESET HANDLER (Core 0, Priority 4)
// ============================================================================

void taskFactoryReset(void *parameter) {
  for (;;) {
    if (xSemaphoreTake(xSemaphoreFactoryReset, portMAX_DELAY) == pdTRUE) {
      unsigned long pressTime = factoryResetPressTime;

      safeSerialPrint("[FACTORY] Button pressed, hold 5 seconds to reset...\n");
      beepWarning();

      vTaskDelay(pdMS_TO_TICKS(FACTORY_RESET_HOLD_TIME));

      if (digitalRead(FACTORY_RESET_PIN) == LOW) {
        unsigned long actualHoldTime = millis() - pressTime;
        if (actualHoldTime >= FACTORY_RESET_HOLD_TIME) {
          safeSerialPrint("[FACTORY] RESET TRIGGERED!\n");
          beepAlarm();
          factoryReset();
        }
      } else {
        safeSerialPrint("[FACTORY] Button released too early, reset cancelled\n");
      }
    }
  }
}
