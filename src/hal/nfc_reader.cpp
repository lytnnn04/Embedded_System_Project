#include "nfc_reader.h"
#include "app_globals.h"
#include "hal/buzzer.h"
#include "data_store.h"
#include "email_manager.h"
#include "fsm.h"
#include "security.h"
#include "tft_display.h"
#include "esp_log.h"

// Helper: hiển thị lỗi trên ui_uiwrongpin rồi tự trở về ui_uilock
// Gỡ event auto-T9 của SquareLine để tránh bị nhảy qua màn nhập mật khẩu
static void showNfcError(const char *message, uint32_t displayMs) {
  if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(100))) {
    _ui_screen_change(&ui_uiwrongpin, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0,
                      &ui_uiwrongpin_screen_init);
    // Gỡ event SCREEN_LOADED → T9 (4s) do SquareLine sinh ra
    lv_obj_remove_event_cb(ui_uiwrongpin, ui_event_uiwrongpin);
    if (ui_LabelAttemptsLeft)
      lv_label_set_text(ui_LabelAttemptsLeft, message);
    xSemaphoreGive(xMutexLVGL);
  }
  vTaskDelay(pdMS_TO_TICKS(displayMs));
  if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(100))) {
    _ui_screen_change(&ui_uilock, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0,
                      &ui_uilock_screen_init);
    xSemaphoreGive(xMutexLVGL);
  }
}

// Helper: đánh thức màn hình nếu đang tắt
static void wakeBacklight() {
  if (backlightIsOff) {
    tft_set_backlight(255);
    backlightIsOff = false;
  }
  lastUserActivityTime = millis();
}

bool initPN532() {
  // PN532_IRQ_PIN/PN532_RESET_PIN are -1 → cast to uint8_t=255 (invalid GPIO).
  // Adafruit_PN532::begin() calls pinMode(255,…) internally, triggering ESP-IDF
  // GPIO errors.  Suppress the GPIO log tag during begin() then restore it.
  esp_log_level_set("gpio", ESP_LOG_NONE);
  nfc.begin();
  esp_log_level_set("gpio", ESP_LOG_ERROR);

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) return false;

  safeSerialPrintf("[NFC] PN532 firmware: v%d.%d\n",
                   (versiondata >> 16) & 0xFF, (versiondata >> 8) & 0xFF);
  nfc.SAMConfig();
  return true;
}

bool readCard(char *uidStr, size_t maxLen) {
  uint8_t uid[7]      = {0};
  uint8_t uidLength   = 0;
  bool found = false;

  if (xSemaphoreTake(xMutexI2C, pdMS_TO_TICKS(300))) {
    found = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 150);
    xSemaphoreGive(xMutexI2C);
  } else {
    return false;
  }

  if (found && uidLength > 0) {
    String result = "";
    for (uint8_t i = 0; i < uidLength; i++) {
      if (uid[i] < 0x10) result += "0";
      result += String(uid[i], HEX);
    }
    result.toUpperCase();
    result.toCharArray(uidStr, maxLen);
    return true;
  }
  return false;
}

bool validateCard(const char *cardId, char *outUserName, size_t nameLen) {
  if (security.cardFunctionLocked || security.systemDisabled) return false;

  bool found = false;
  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
    for (int i = 0; i < userCount; i++) {
      if (users[i].active && strlen(users[i].cardId) > 0 &&
          strcmp(users[i].cardId, cardId) == 0) {
        if (strcmp(users[i].role, "Guest") == 0) {
          if (!isTimeAllowed(users[i].allowedStart, users[i].allowedEnd)) {
            safeSerialPrintf("[NFC] Guest %s outside allowed hours\n", users[i].name);
            break;
          }
        }
        if (outUserName) strncpy(outUserName, users[i].name, nameLen - 1);
        found = true;
        break;
      }
    }
    xSemaphoreGive(xMutexData);
  }
  return found;
}

void startCardScan(CardScanMode mode) {
  if (xSemaphoreTake(xMutexCard, pdMS_TO_TICKS(200))) {
    cardScanMode      = mode;
    cardScanActive    = true;
    cardScanGotResult = false;
    cardScanSuccess   = false;
    cardScanStartTime = millis();
    memset(lastScannedCardId, 0, sizeof(lastScannedCardId));
    memset(cardLoginToken, 0, sizeof(cardLoginToken));
    xSemaphoreGive(xMutexCard);
  }
}

void stopCardScan() {
  if (xSemaphoreTake(xMutexCard, pdMS_TO_TICKS(200))) {
    cardScanActive = false;
    xSemaphoreGive(xMutexCard);
  }
}

void taskCardReader(void *parameter) {
  static int  consecutiveI2cErrors = 0;
  static char lastDebounceUID[16]  = "";
  static unsigned long lastDebounceTime = 0;
  const int    MAX_I2C_ERRORS  = 5;
  const unsigned long DEBOUNCE_MS = 2000;

  for (;;) {
    // --- PN532 not available: retry every 60s ---
    if (!nfcAvailable) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      static unsigned long lastRetry = 0;
      if (millis() - lastRetry > 60000) {
        lastRetry = millis();
        if (xSemaphoreTake(xMutexI2C, pdMS_TO_TICKS(300))) {
          bool ok = initPN532();
          xSemaphoreGive(xMutexI2C);
          if (ok) {
            nfcAvailable = true;
            consecutiveI2cErrors = 0;
            safeSerialPrint("[NFC] PN532 reconnected!\n");
          }
        }
      }
      continue;
    }

    // --- Dashboard scan session timeout ---
    if (cardScanActive && (millis() - cardScanStartTime > CARD_SCAN_TIMEOUT_MS)) {
      if (xSemaphoreTake(xMutexCard, pdMS_TO_TICKS(100))) {
        cardScanActive    = false;
        cardScanGotResult = true;
        cardScanSuccess   = false;
        xSemaphoreGive(xMutexCard);
      }
      safeSerialPrint("[NFC] Scan session timed out\n");
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // --- Read card ---
    char uidStr[16] = "";
    bool cardDetected = readCard(uidStr, sizeof(uidStr));

    if (!cardDetected) {
      static unsigned long lastPing = 0;
      if (millis() - lastPing > 5000) {
        lastPing = millis();
        if (xSemaphoreTake(xMutexI2C, pdMS_TO_TICKS(200))) {
          uint32_t fw = nfc.getFirmwareVersion();
          xSemaphoreGive(xMutexI2C);
          if (!fw) {
            if (++consecutiveI2cErrors >= MAX_I2C_ERRORS) {
              nfcAvailable = false;
              consecutiveI2cErrors = 0;
              safeSerialPrint("[NFC] PN532 connection lost - disabling\n");
            }
          } else {
            consecutiveI2cErrors = 0;
          }
        }
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    consecutiveI2cErrors = 0;

    // --- Debounce ---
    if (strcmp(uidStr, lastDebounceUID) == 0 &&
        (millis() - lastDebounceTime) < DEBOUNCE_MS) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    strncpy(lastDebounceUID, uidStr, sizeof(lastDebounceUID) - 1);
    lastDebounceTime = millis();

    safeSerialPrintf("[NFC] Card detected: %s\n", uidStr);

    // Đánh thức màn hình khi có thẻ được quét
    wakeBacklight();

    // ════════
    // MODE 2: DASHBOARD SCAN
    // ════════
    if (cardScanActive) {
      if (!xSemaphoreTake(xMutexCard, pdMS_TO_TICKS(200))) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }

      strncpy(lastScannedCardId, uidStr, sizeof(lastScannedCardId) - 1);

      if (cardScanMode == CARD_SCAN_LOGIN) {
        char  userName[32] = "";
        char  userRole[16] = "";
        bool  isValid = false;

        if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
          for (int i = 0; i < userCount; i++) {
            if (users[i].active && strlen(users[i].cardId) > 0 &&
                strcmp(users[i].cardId, uidStr) == 0) {
              strncpy(userName, users[i].name, sizeof(userName) - 1);
              strncpy(userRole, users[i].role, sizeof(userRole) - 1);
              isValid = true;
              break;
            }
          }
          xSemaphoreGive(xMutexData);
        }

        if (isValid && strcmp(userRole, "Guest") == 0) {
          isValid = false;
          safeSerialPrintf("[NFC] Guest card '%s' denied dashboard login\n", userName);
        }

        if (isValid) {
          String tok = generateToken();
          if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
            if ((int)activeTokens.size() >= MAX_ACTIVE_TOKENS)
              activeTokens.erase(activeTokens.begin());
            AuthToken newToken;
            newToken.token            = tok;
            newToken.expireTime       = millis() + SESSION_TIMEOUT;
            newToken.lastActivityTime = millis();              newToken.isDefault        = false; // Card login never requires password change            activeTokens.push_back(newToken);
            xSemaphoreGive(xMutexData);
          }
          tok.toCharArray(cardLoginToken, sizeof(cardLoginToken));
          cardScanSuccess = true;
          beepSuccess();
          addLog(userName, "Card", "Entry",
                 ("Dashboard card login: " + String(uidStr)).c_str());
          safeSerialPrintf("[NFC] Dashboard login OK - %s (%s)\n", userName, userRole);
        } else {
          cardScanSuccess = false;
          beepError();
          security.cardFailCount++;
          security.totalFailCount++;
          if (security.cardFailCount >= MAX_PASSWORD_ATTEMPTS) {
            security.cardFunctionLocked = true;
            saveData();
            queueEmailAlert("[SmartLock] Cảnh báo: Chức năng thẻ bị khóa",
                            "Quét thẻ sai 5 lần. Chức năng thẻ đã bị vô hiệu hóa.");
            safeSerialPrint("[NFC] Card function LOCKED after 5 failures\n");
          }
          addLog("Unknown", "Card", "Failed Attempt",
                 ("Invalid card (dashboard login): " + String(uidStr)).c_str());
          safeSerialPrintf("[NFC] Dashboard login FAILED (card %d/%d)\n",
                           security.cardFailCount, MAX_PASSWORD_ATTEMPTS);
        }
        cardScanGotResult = true;
        cardScanActive    = false;

      } else if (cardScanMode == CARD_SCAN_ENROLL) {
        cardScanSuccess   = true;
        cardScanGotResult = true;
        cardScanActive    = false;
        safeSerialPrintf("[NFC] Card enrolled: %s\n", uidStr);

      } else if (cardScanMode == CARD_SCAN_DOOR) {
        // TFT lock-screen card scan — validate and unlock door
        char userName[32] = "";
        bool validCard = false;

        if (security.cardFunctionLocked) {
          update_scan_status("Card function locked!");
          cardScanSuccess = false;
        } else if (security.systemDisabled) {
          update_scan_status("System disabled!");
          cardScanSuccess = false;
        } else {
          validCard = validateCard(uidStr, userName, sizeof(userName));
          if (validCard) {
            strncpy(lastUnlockUserName, userName,
                    sizeof(lastUnlockUserName) - 1);
            update_scan_status("Access Granted!");
            beepSuccess();
            addLog(userName, "Card", "Entry",
                   ("TFT card access: " + String(uidStr)).c_str());
            SystemEvent evt = EVENT_AUTH_SUCCESS;
            xQueueSend(eventQueue, &evt, pdMS_TO_TICKS(100));
            cardScanSuccess = true;
          } else {
            security.cardFailCount++;
            security.totalFailCount++;
            update_scan_status("Invalid card!");
            beepError();
            addLog("Unknown", "Card", "Failed Attempt",
                   ("Invalid card (TFT door): " + String(uidStr)).c_str());
            cardScanSuccess = false;
          }
        }
        cardScanGotResult = true;
        cardScanActive    = false;
      }

      xSemaphoreGive(xMutexCard);
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    // ════════
    // MODE 1: AMBIENT — door card tap
    // ════════
    // Cho phép quét thẻ cả khi đang PASSWORD_LOCKOUT:
    // thẻ hợp lệ sẽ gửi EVENT_AUTH_SUCCESS → resetSecurityCounters() → xóa hết penalty.
    if (currentState != STATE_LOCKED &&
        currentState != STATE_IDLE &&
        currentState != STATE_PASSWORD_LOCKOUT) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    if (security.cardFunctionLocked) {
      if (settings.soundEnabled) {
        for (int i = 0; i < 3; i++) {
          ledcWriteTone(BUZZER_CHANNEL, 800);
          vTaskDelay(pdMS_TO_TICKS(100));
          ledcWrite(BUZZER_CHANNEL, 0);
          vTaskDelay(pdMS_TO_TICKS(80));
        }
      }
      safeSerialPrint("[NFC] Rejected: card function is locked\n");
      addLog("Unknown", "Card", "Failed Attempt", "Card function locked - access denied");
      showNfcError("Card function disabled!\nContact Admin to re-enable.", 3000);
      continue;
    }

    char userName[32] = "";
    unsigned long t_rfid = millis();
    bool validCard = validateCard(uidStr, userName, sizeof(userName));

    if (validCard) {
      unsigned long rfidMs = millis() - t_rfid;

      // ── RFID timing (10 mẫu) ────────────────────────────────────────────
      static unsigned long rfidSamples[10];
      static int rfidSampleCount = 0;
      static bool rfidDone = false;
      if (!rfidDone) {
        rfidSamples[rfidSampleCount++] = rfidMs;
        safeSerialPrintf("[TIMING] RFID sample %d: %lu ms\n", rfidSampleCount, rfidMs);
        if (rfidSampleCount >= 10) {
          unsigned long sum = 0;
          for (int k = 0; k < 10; k++) sum += rfidSamples[k];
          Serial.println("[TIMING] ===== RFID (10 samples) =====");
          Serial.printf("[TIMING] Average: %lu ms\n", sum / 10);
          Serial.println("[TIMING] ================================");
          rfidDone = true;
        }
      }
      // ────────────────────────────────────────────────────────────────────

      safeSerialPrintf("[NFC] Access GRANTED - %s (UID: %s)\n", userName, uidStr);
      security.cardFailCount = 0;
      strncpy(lastUnlockUserName, userName, sizeof(lastUnlockUserName) - 1);
      beepSuccess();
      SystemEvent evt = EVENT_AUTH_SUCCESS;
      xQueueSend(eventQueue, &evt, pdMS_TO_TICKS(100));
      addLog(userName, "Card", "Entry",
             ("Card access granted: " + String(uidStr)).c_str());
    } else {
      security.cardFailCount++;
      security.totalFailCount++;
      int remaining = MAX_PASSWORD_ATTEMPTS - security.cardFailCount;

      safeSerialPrintf("[NFC] Access DENIED - UID: %s (fail %d/%d)\n",
                       uidStr, security.cardFailCount, MAX_PASSWORD_ATTEMPTS);
      beepError();

      if (security.cardFailCount >= MAX_PASSWORD_ATTEMPTS) {
        security.cardFunctionLocked = true;
        saveData();
        queueEmailAlert("[SmartLock] Cảnh báo: Chức năng thẻ bị khóa",
                        "Quét thẻ sai 5 lần liên tiếp. Chức năng thẻ đã bị vô hiệu hóa.\n"
                        "Truy cập dashboard để mở lại.");
        safeSerialPrint("[NFC] Card function LOCKED after 5 failures\n");
        addLog("System", "Card", "System Alert",
               "Card function locked - too many failed attempts");
        showNfcError("Card function disabled!\nContact Admin to re-enable.", 3000);
      } else if (security.totalFailCount >= MAX_TOTAL_ATTEMPTS) {
        security.systemDisabled = true;
        fsmTransition(STATE_SYSTEM_DISABLED);
        saveData();
        queueEmailAlert("[SmartLock] Nghiêm trọng: Hệ thống bị vô hiệu hóa",
                        "Tổng số lần xác thực sai đã vượt ngưỡng.");
        safeSerialPrint("[NFC] System DISABLED (total fail limit reached)\n");
      } else {
        addLog("Unknown", "Card", "Failed Attempt",
               ("Rejected card: " + String(uidStr) +
                " (" + String(remaining) + " attempts left)").c_str());
        char attBuf[48];
        snprintf(attBuf, sizeof(attBuf), "Invalid card! %d attempt(s) left.", remaining);
        showNfcError(attBuf, 2000);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
