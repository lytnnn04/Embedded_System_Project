#include "ui_handler.h"
#include "app_globals.h"
#include "security.h"
#include "fsm.h"
#include "data_store.h"
#include "email_manager.h"
#include "hal/nfc_reader.h"
#include "hal/buzzer.h"
#include "rtc_manager.h"

// Forward declaration — defined later in USER MANAGEMENT section
static void _refresh_select_user_ui();

// ============================================================================
// SCREEN SYNC (FSM state → LVGL screen)
// ============================================================================

void updateScreenFromState() {
  SystemState state;
  if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(50))) {
    state = currentState;
    xSemaphoreGive(xMutexState);
  } else {
    return;
  }

  // --- Catch direct LVGL screen changes to reset states ---
  if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(200))) {
    static lv_obj_t* lastScreen = NULL;
    lv_obj_t* currentScreen = lv_scr_act();
    if (currentScreen != lastScreen) {
      if (currentScreen == ui_uiT9) {
        if (t9Mode == 0) {
          if (ui_Label5) lv_label_set_text(ui_Label5, "ENTER PASSCODE");
          if (ui_PasswordBox) lv_textarea_set_text(ui_PasswordBox, "");
        }
      } else if (currentScreen == ui_uilock) {
        t9Mode = 0;
        newPasswordStep = 0;
      }
      lastScreen = currentScreen;
    }
    xSemaphoreGive(xMutexLVGL);
  }
  // --------------------------------------------------------

  // ── Wrong-pin countdown block ────────────────────────────────────────────
  // While wrongPinDisplayActive is set, block screen transitions so the
  // wrong-pin screen stays visible for WRONG_PIN_DISPLAY_MS (3s), during
  // which the Arc and countdown label animate. After the timer expires,
  // fall through to normal state handling which will show ui_uilock.
  if (wrongPinDisplayActive) {
    unsigned long elapsed = millis() - wrongPinDisplayStartMs;
    if (state == STATE_LOCKED && elapsed < WRONG_PIN_DISPLAY_MS) {
      // Update countdown animation
      if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(50))) {
        int remMs     = (int)(WRONG_PIN_DISPLAY_MS - elapsed);
        int remaining = (remMs + 999) / 1000;   // ceil → 3,2,1
        int arcVal    = remMs * 100 / (int)WRONG_PIN_DISPLAY_MS;
        if (arcVal < 0) arcVal = 0;
        char buf[6];
        snprintf(buf, sizeof(buf), "%ds", remaining);
        if (ui_Label19) lv_label_set_text(ui_Label19, buf);
        if (ui_Arc2)    lv_arc_set_value(ui_Arc2, arcVal);
        xSemaphoreGive(xMutexLVGL);
      }
      return;  // Keep wrongpin screen visible
    } else {
      // Timer expired (or state changed unexpectedly).
      // Force-show the lock screen here because currentState may already be
      // STATE_LOCKED (the T9 screen is opened via a direct LVGL callback in
      // ui_uilock.c, not via a FSM state transition), so the normal
      // "if (state == lastDisplayedState) return;" guard below would
      // swallow the screen change silently.
      wrongPinDisplayActive = false;
      if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(200))) {
        _ui_screen_change(&ui_uilock, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0,
                          &ui_uilock_screen_init);
        xSemaphoreGive(xMutexLVGL);
      }
      lastDisplayedState = state; // keep in sync
      return;
    }
  }
  // ─────────────────────────────────────────────────────────────────────────

  if (state == lastDisplayedState) return;

  if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(200))) {
    switch (state) {
    case STATE_LOCKED:
    case STATE_IDLE:
      _ui_screen_change(&ui_uilock, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 500, 0,
                        &ui_uilock_screen_init);
      break;
    case STATE_PASSWORD_INPUT:
      _ui_screen_change(&ui_uiT9, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0,
                        &ui_uiT9_screen_init);
      // Cho phép nhập tối đa MAX_SCRAMBLE_LENGTH ký tự (Scramble PIN)
      lv_timer_create([](lv_timer_t* t) {
        if (ui_PasswordBox) lv_textarea_set_max_length(ui_PasswordBox, MAX_SCRAMBLE_LENGTH);
        lv_timer_del(t);
      }, 350, NULL);
      break;
    case STATE_UNLOCKED:
      _ui_screen_change(&ui_uisuccess, LV_SCR_LOAD_ANIM_FADE_ON, 500, 0,
                        &ui_uisuccess_screen_init);
      if (ui_LabelUnlockUser) {
        char greet[48];
        snprintf(greet, sizeof(greet), "Hello, %s", lastUnlockUserName);
        lv_label_set_text(ui_LabelUnlockUser, greet);
      }
      beepSuccess();
      break;
    case STATE_SYSTEM_DISABLED:
    case STATE_PASSWORD_LOCKOUT:
    case STATE_ALARM:
      _ui_screen_change(&ui_uidisabled, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0,
                        &ui_uidisabled_screen_init);
      if (ui_LabelDisabledTitle && ui_Label6) {
        if (state == STATE_ALARM) {
          lv_label_set_text(ui_LabelDisabledTitle, "DOOR TAMPERED!");
          lv_label_set_text(ui_Label6, "Unauthorized opening detected.");
          if (ui_Arc1) lv_obj_add_flag(ui_Arc1, LV_OBJ_FLAG_HIDDEN);
          if (ui_cntdown) lv_obj_add_flag(ui_cntdown, LV_OBJ_FLAG_HIDDEN);
          if (ui_LabelWaitHint) lv_obj_add_flag(ui_LabelWaitHint, LV_OBJ_FLAG_HIDDEN);
        } else if (state == STATE_PASSWORD_LOCKOUT) {
          lv_label_set_text(ui_LabelDisabledTitle, "Smartlock locked");
          lv_label_set_text(ui_Label6, "Too many attempts. Please wait.");
          if (ui_Arc1) lv_obj_clear_flag(ui_Arc1, LV_OBJ_FLAG_HIDDEN);
          if (ui_cntdown) lv_obj_clear_flag(ui_cntdown, LV_OBJ_FLAG_HIDDEN);
          if (ui_LabelWaitHint) lv_obj_clear_flag(ui_LabelWaitHint, LV_OBJ_FLAG_HIDDEN);
        } else if (state == STATE_SYSTEM_DISABLED) {
          lv_label_set_text(ui_LabelDisabledTitle, "System Disabled");
          lv_label_set_text(ui_Label6, "Please reset or unlock via Cloud.");
          if (ui_Arc1) lv_obj_add_flag(ui_Arc1, LV_OBJ_FLAG_HIDDEN);
          if (ui_cntdown) lv_obj_add_flag(ui_cntdown, LV_OBJ_FLAG_HIDDEN);
          if (ui_LabelWaitHint) lv_obj_add_flag(ui_LabelWaitHint, LV_OBJ_FLAG_HIDDEN);
        }
      }
      break;
    default:
      break;
    }
    xSemaphoreGive(xMutexLVGL);
  }

  lastDisplayedState = state;
}

void displayMessage(const char *line1, const char *line2) {
  safeSerialPrintf("[DISPLAY] %s", line1);
  if (line2) safeSerialPrintf(" - %s", line2);
  safeSerialPrint("\n");
}

void updateDisplay() {
  static SystemState lastState = STATE_IDLE;
  if (currentState == lastState) return;
  lastState = currentState;

  if (xSemaphoreTake(xMutexSerial, pdMS_TO_TICKS(200))) {
    Serial.println("\n========================================");
    switch (currentState) {
    case STATE_IDLE:
    case STATE_LOCKED:
      Serial.println("[STATE] LOCKED - Ready");
      break;
    case STATE_UNLOCKED:
      Serial.printf("[STATE] UNLOCKED - Waiting for door to close\n");
      break;
    case STATE_PASSWORD_LOCKOUT: {
      time_t remaining = security.lockoutEndTime - getRTCTime();
      Serial.printf("[STATE] LOCKED OUT - Wait %ld sec\n", (long)remaining);
    } break;
    case STATE_SYSTEM_DISABLED:
      Serial.println("[STATE] SYSTEM DISABLED");
      break;
    default:
      Serial.printf("[STATE] %s\n", getStateName(currentState));
      break;
    }
    Serial.println("========================================");
    xSemaphoreGive(xMutexSerial);
  }
}

// ============================================================================
// PASSWORD / T9 BRIDGE
// ============================================================================

void process_password_attempt(const char *password) {
  if (t9Mode == 2) {
    // Settings access gate: chấp nhận systemPassword HOẶC mật khẩu riêng của bất kỳ user Admin nào
    bool settingsGranted = validatePassword(password);
    if (!settingsGranted) {
      if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
        for (int i = 0; i < userCount; i++) {
          if (!users[i].active) continue;
          if (strcmp(users[i].role, "Admin") != 0) continue;
          if (strlen(users[i].password) == 0) continue;
          if (strcmp(password, users[i].password) == 0) {
            settingsGranted = true;
            break;
          }
        }
        xSemaphoreGive(xMutexData);
      }
    }
    if (settingsGranted) {
      t9Mode = 0;
      beepSuccess();
      // Navigate to settings and set up UI labels
      _ui_screen_change(&ui_settings, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0,
                        &ui_settings_screen_init);
      if (ui_SwitchSound) {
        if (settings.soundEnabled)
          lv_obj_add_state(ui_SwitchSound, LV_STATE_CHECKED);
        else
          lv_obj_clear_state(ui_SwitchSound, LV_STATE_CHECKED);
      }
      if (ui_Dropdown) {
        int sel = 2;
        if (settings.screenTimeout <= 5)        sel = 0;
        else if (settings.screenTimeout <= 10)  sel = 1;
        else if (settings.screenTimeout <= 30)  sel = 2;
        else if (settings.screenTimeout <= 60)  sel = 3;
        else                                    sel = 4;
        lv_dropdown_set_selected(ui_Dropdown, sel);
      }
      if (ui_uiLabelWifiCurrentIP) {
        if (systemStatus.wifiConnected)
          lv_label_set_text(ui_uiLabelWifiCurrentIP, WiFi.localIP().toString().c_str());
        else
          lv_label_set_text(ui_uiLabelWifiCurrentIP, "Not connected");
      }
    } else {
      beepError();
      if (ui_Label5) lv_label_set_text(ui_Label5, "Wrong PIN!");
      if (ui_PasswordBox) lv_textarea_set_text(ui_PasswordBox, "");
    }
    return;
  }

  if (t9Mode == 1) {
    // Change password flow
    if (newPasswordStep == -1) {
      // Step -1: Verify current password to identify who is changing
      bool matched = false;
      if (strcmp(password, systemPassword) == 0) {
        changedTargetUserIdx = -2; // System password
        matched = true;
      }
      if (!matched) {
        if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
          for (int i = 0; i < userCount; i++) {
            if (!users[i].active) continue;
            if (strlen(users[i].password) == 0) continue;
            if (strcmp(password, users[i].password) == 0) {
              changedTargetUserIdx = i;
              matched = true;
              break;
            }
          }
          xSemaphoreGive(xMutexData);
        }
      }
      if (matched) {
        newPasswordStep = 0;
        beepSuccess();
        if (ui_Label5)      lv_label_set_text(ui_Label5, "Enter new PIN");
        if (ui_PasswordBox) lv_textarea_set_text(ui_PasswordBox, "");
      } else {
        beepError();
        if (ui_Label5)      lv_label_set_text(ui_Label5, "Wrong PIN!");
        if (ui_PasswordBox) lv_textarea_set_text(ui_PasswordBox, "");
      }
    } else if (newPasswordStep == 0) {
      if (strlen(password) < PASSWORD_LENGTH) {
        if (ui_Label5) lv_label_set_text(ui_Label5, "Min 6 digits!");
        beepError();
        return;
      }
      strncpy(newPasswordTemp, password, PASSWORD_LENGTH);
      newPasswordTemp[PASSWORD_LENGTH] = '\0';
      newPasswordStep = 1;
      if (ui_Label5)      lv_label_set_text(ui_Label5, "Confirm new PIN");
      if (ui_PasswordBox) lv_textarea_set_text(ui_PasswordBox, "");
      beepSuccess();
    } else {
      if (strcmp(password, newPasswordTemp) == 0) {
        // Save to correct target
        if (changedTargetUserIdx == -2) {
          // Change system password
          strncpy(systemPassword, newPasswordTemp, PASSWORD_LENGTH + 1);
          isDefaultPassword = false;
          preferences.putString("password", systemPassword);
          saveData();
          addLog("System", "Password", "System Alert", "System password changed via TFT");
          queueEmailAlert("[SmartLock] M\u1eadt kh\u1ea9u h\u1ec7 th\u1ed1ng \u0111\u00e3 thay \u0111\u1ed5i",
                          "M\u1eadt kh\u1ea9u h\u1ec7 th\u1ed1ng \u0111\u00e3 \u0111\u01b0\u1ee3c thay \u0111\u1ed5i t\u1eeb b\u00e0n ph\u00edm TFT.");
        } else if (changedTargetUserIdx >= 0) {
          // Change user door password
          if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
            strncpy(users[changedTargetUserIdx].password, newPasswordTemp, PASSWORD_LENGTH);
            users[changedTargetUserIdx].password[PASSWORD_LENGTH] = '\0';
            char logMsg[64];
            snprintf(logMsg, sizeof(logMsg), "Door password changed via TFT");
            addLog(users[changedTargetUserIdx].name, "Password", "System Alert", logMsg);
            xSemaphoreGive(xMutexData);
          }
          saveData();
        }
        beepSuccess();
        t9Mode = 3; // temporary state to ignore input
        newPasswordStep = 0;
        changedTargetUserIdx = -1;
        memset(newPasswordTemp, 0, sizeof(newPasswordTemp));
        if (ui_Label5) lv_label_set_text(ui_Label5, "Change Success!");
        if (ui_PasswordBox) lv_textarea_set_text(ui_PasswordBox, "");

        lv_timer_create([](lv_timer_t* t) {
            t9Mode = 0;
            _ui_screen_change(&ui_settings, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, &ui_settings_screen_init);
            lv_timer_del(t);
        }, 2000, NULL);
      } else {
        beepError();
        newPasswordStep = 0;
        memset(newPasswordTemp, 0, sizeof(newPasswordTemp));
        if (ui_Label5)      lv_label_set_text(ui_Label5, "Mismatch! Enter new PIN");
        if (ui_PasswordBox) lv_textarea_set_text(ui_PasswordBox, "");
      }
    }
    return;
  }

  // Unlock mode (t9Mode == 0): dùng Scramble PIN - kiểm tra input có CHỨA mật khẩu hợp lệ không
  int matchedUserIdx = -1;
  unsigned long t_pw = millis();
  if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(100))) {
    if (validateAnyPasswordScramble(password, &matchedUserIdx)) {
      unsigned long pwMs = millis() - t_pw;

      // ── Password timing (10 mẫu) ─────────────────────────────────────────
      static unsigned long pwSamples[10];
      static int pwSampleCount = 0;
      static bool pwDone = false;
      if (!pwDone) {
        pwSamples[pwSampleCount++] = pwMs;
        safeSerialPrintf("[TIMING] Password sample %d: %lu ms\n", pwSampleCount, pwMs);
        if (pwSampleCount >= 10) {
          unsigned long sum = 0;
          for (int k = 0; k < 10; k++) sum += pwSamples[k];
          Serial.println("[TIMING] ===== Password Auth (10 samples) =====");
          Serial.printf("[TIMING] Average: %lu ms\n", sum / 10);
          Serial.println("[TIMING] ==========================================");
          pwDone = true;
        }
      }
      // ─────────────────────────────────────────────────────────────────────

      if (matchedUserIdx >= 0 && matchedUserIdx < userCount) {
        strncpy(lastUnlockUserName, users[matchedUserIdx].name,
                sizeof(lastUnlockUserName) - 1);
        addLog(users[matchedUserIdx].name, "Password", "Entry", "Correct PIN (Scramble)");
      } else {
        strncpy(lastUnlockUserName, "Admin", sizeof(lastUnlockUserName) - 1);
        addLog("Admin", "Password", "Entry", "Correct PIN (Scramble)");
      }
      SystemEvent evt = EVENT_AUTH_SUCCESS;
      xQueueSend(eventQueue, &evt, pdMS_TO_TICKS(10));
    } else {
      security.passwordFailCount++;
      recordFailedAttempt("Password", "Wrong PIN");
      SystemEvent evt = EVENT_AUTH_FAILED;
      xQueueSend(eventQueue, &evt, pdMS_TO_TICKS(10));
      safeSerialPrintf("[Auth] PIN wrong (attempt %d/%d)\n",
                       security.passwordFailCount, MAX_PASSWORD_ATTEMPTS);
    }
    xSemaphoreGive(xMutexState);
  }
}

// ============================================================================
// SETTINGS BRIDGE FUNCTIONS
// ============================================================================

void save_sound_settings(bool enable) {
  settings.soundEnabled = enable;
  preferences.putBool("soundEnabled", enable);
  safeSerialPrintf("[Settings] Sound %s\n", enable ? "ON" : "OFF");
}

void enter_settings_mode(void) {
  // Gate với admin password (exact match, KHÔNG scramble)
  t9Mode = 2;
  newPasswordStep = 0;
  // Override the _ui_screen_change to settings that was triggered in uilock.c
  _ui_screen_change(&ui_uiT9, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0,
                    &ui_uiT9_screen_init);
  if (ui_Label5)      lv_label_set_text(ui_Label5, "Enter admin PIN");
  if (ui_PasswordBox) lv_textarea_set_text(ui_PasswordBox, "");
  // KHÔNG giới hạn 6 ký tự ở đây — giữ MAX_SCRAMBLE_LENGTH để không phá vỡ
  // Scramble PIN khi user quay lại unlock mode mà không qua state transition
  lv_timer_create([](lv_timer_t* t) {
    if (ui_PasswordBox) lv_textarea_set_max_length(ui_PasswordBox, MAX_SCRAMBLE_LENGTH);
    lv_timer_del(t);
  }, 400, NULL);
}

void change_screen_timeout(int seconds) {
  settings.screenTimeout = seconds;
  preferences.putInt("screenTimeout", seconds);
  safeSerialPrintf("[Settings] Screen timeout: %ds\n", seconds);
}

void enter_change_password(void) {
  t9Mode = 1;
  newPasswordStep = -1; // Start with: verify current password
  changedTargetUserIdx = -1;
  memset(newPasswordTemp, 0, sizeof(newPasswordTemp));
  _ui_screen_change(&ui_uiT9, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0,
                    &ui_uiT9_screen_init);
  if (ui_Label5)      lv_label_set_text(ui_Label5, "Enter current PIN");
  if (ui_PasswordBox) lv_textarea_set_text(ui_PasswordBox, "");
  // Passwords are exactly PASSWORD_LENGTH digits, use that as the limit
  lv_timer_create([](lv_timer_t* t) {
    if (ui_PasswordBox) lv_textarea_set_max_length(ui_PasswordBox, PASSWORD_LENGTH);
    lv_timer_del(t);
  }, 400, NULL);
}

void enter_add_card(void) {
  selectedUserIndex = 0;
  _ui_screen_change(&ui_uiselectuser, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0,
                    &ui_uiselectuser_screen_init);
  // Screen init just ran; it will be fully built after this call.
  // _refresh_select_user_ui reads users[] and updates LVGL labels.
  // Grab data mutex briefly just to take a consistent snapshot (no LVGL ops inside).
  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(50))) {
    xSemaphoreGive(xMutexData);
  }
  _refresh_select_user_ui();
}

void factory_reset_confirm(void) {
  safeSerialPrint("[Settings] Factory reset triggered from TFT!\n");
  beepAlarm();
  factoryReset();
}

void user_prev(void) {
  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(50))) {
    if (userCount > 0) {
      selectedUserIndex--;
      if (selectedUserIndex < 0) selectedUserIndex = userCount - 1;
    }
    xSemaphoreGive(xMutexData);
  }
  _refresh_select_user_ui();
}

void user_next(void) {
  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(50))) {
    if (userCount > 0) {
      selectedUserIndex++;
      if (selectedUserIndex >= userCount) selectedUserIndex = 0;
    }
    xSemaphoreGive(xMutexData);
  }
  _refresh_select_user_ui();
}

void scan_for_user(void) {
  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(50))) {
    if (selectedUserIndex < 0 || selectedUserIndex >= userCount) {
      xSemaphoreGive(xMutexData);
      beepError();
      return;
    }
    xSemaphoreGive(xMutexData);
  }
  startCardScan(CARD_SCAN_ENROLL);
  tftEnrollActive = true;
  _ui_screen_change(&ui_uiscancard, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0,
                    &ui_uiscancard_screen_init);
  if (ui_LabelScanStatus)
    lv_label_set_text(ui_LabelScanStatus, "Waiting for card...");
}

// ============================================================================
// UI LABEL UPDATE HELPERS
// ============================================================================

void update_wifi_ip(const char *ip) {
  if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(50))) {
    if (ui_uiLabelWifiCurrentIP) lv_label_set_text(ui_uiLabelWifiCurrentIP, ip);
    xSemaphoreGive(xMutexLVGL);
  }
}

void update_unlock_username(const char *name) {
  strncpy(lastUnlockUserName, name, sizeof(lastUnlockUserName) - 1);
}

void update_scan_status(const char *status) {
  if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(50))) {
    if (ui_LabelScanStatus) lv_label_set_text(ui_LabelScanStatus, status);
    xSemaphoreGive(xMutexLVGL);
  }
}

void update_attempts_left(int remaining) {
  if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(50))) {
    if (ui_LabelAttemptsLeft) {
      char buf[40];
      snprintf(buf, sizeof(buf), "%d more attempts remaining.", remaining);
      lv_label_set_text(ui_LabelAttemptsLeft, buf);
    }
    xSemaphoreGive(xMutexLVGL);
  }
}

void update_user_display(const char *name, const char *role,
                         const char *cardStatus) {
  // Always called from LVGL-context helpers or external tasks via update_scan_status path.
  // Use mutex only when called from non-LVGL context.
  if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(50))) {
    if (ui_LabelSelectedUser) lv_label_set_text(ui_LabelSelectedUser, name);
    if (ui_LabelSelectedRole) lv_label_set_text(ui_LabelSelectedRole, role);
    if (ui_LabelCardStatus)   lv_label_set_text(ui_LabelCardStatus, cardStatus);
    xSemaphoreGive(xMutexLVGL);
  }
}

void update_user_index(int current, int total) {
  if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(50))) {
    if (ui_LabelUserIndex) {
      char buf[16];
      snprintf(buf, sizeof(buf), "%d / %d", current, total);
      lv_label_set_text(ui_LabelUserIndex, buf);
    }
    xSemaphoreGive(xMutexLVGL);
  }
}

// ============================================================================
// CARD SCAN — door unlock from TFT lock screen
// ============================================================================

void enter_card_scan(void) {
  // Called from LVGL event (ui_event_BtnCardLogin), already in LVGL context.
  // No additional mutex needed for LVGL operations here.
  startCardScan(CARD_SCAN_DOOR);
  if (ui_LabelScanStatus)
    lv_label_set_text(ui_LabelScanStatus, "Hold card near reader...");
}

// ============================================================================
// CARD MANAGEMENT — remove card, cancel scan
// ============================================================================

void logic_remove_card_for_user(lv_event_t* /*e*/) {
  // Called from LVGL event context — no xMutexLVGL needed for UI ops.
  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
    if (selectedUserIndex >= 0 && selectedUserIndex < userCount) {
      char logMsg[64];
      snprintf(logMsg, sizeof(logMsg), "Card removed via TFT");
      addLog(users[selectedUserIndex].name, "Card", "System Alert", logMsg);
      memset(users[selectedUserIndex].cardId, 0, sizeof(users[selectedUserIndex].cardId));
    }
    xSemaphoreGive(xMutexData);
  }
  saveData();
  beepSuccess();
  // Refresh UI directly (already in LVGL context)
  if (ui_LabelCardStatus) lv_label_set_text(ui_LabelCardStatus, "No card");
  if (ui_BtnRemoveCard)   lv_obj_add_flag(ui_BtnRemoveCard, LV_OBJ_FLAG_HIDDEN);
  safeSerialPrint("[TFT] Card removed for user\n");
}

void logic_cancel_card_scan(lv_event_t* /*e*/) {
  // Called from LVGL event context (cancel button on uiscancard).
  stopCardScan();
  tftEnrollActive = false;
  // Return to user selection screen
  enter_add_card();
}

// Helper: refresh toàn bộ UI trên ui_uiselectuser sau khi userCount thay đổi.
// Gọi từ trong LVGL context (event callback).
static void _refresh_select_user_ui() {
  if (userCount == 0) {
    if (ui_LabelSelectedUser) lv_label_set_text(ui_LabelSelectedUser, "No users");
    if (ui_LabelSelectedRole) lv_label_set_text(ui_LabelSelectedRole, "");
    if (ui_LabelCardStatus)   lv_label_set_text(ui_LabelCardStatus, "");
    if (ui_LabelUserIndex)    lv_label_set_text(ui_LabelUserIndex, "0 / 0");
    if (ui_BtnRemoveCard)     lv_obj_add_flag(ui_BtnRemoveCard, LV_OBJ_FLAG_HIDDEN);
    if (ui_BtnDeleteUser)     lv_obj_add_flag(ui_BtnDeleteUser, LV_OBJ_FLAG_HIDDEN);
    if (ui_BtnScanForUser)    lv_obj_add_state(ui_BtnScanForUser, LV_STATE_DISABLED);
  } else {
    // Clamp selectedUserIndex
    if (selectedUserIndex >= userCount) selectedUserIndex = userCount - 1;
    if (selectedUserIndex < 0)          selectedUserIndex = 0;
    bool hasCard = strlen(users[selectedUserIndex].cardId) > 0;
    if (ui_LabelSelectedUser) lv_label_set_text(ui_LabelSelectedUser, users[selectedUserIndex].name);
    if (ui_LabelSelectedRole) lv_label_set_text(ui_LabelSelectedRole, users[selectedUserIndex].role);
    if (ui_LabelCardStatus)   lv_label_set_text(ui_LabelCardStatus, hasCard ? "Card assigned" : "No card");
    char buf[16];
    snprintf(buf, sizeof(buf), "%d / %d", selectedUserIndex + 1, userCount);
    if (ui_LabelUserIndex) lv_label_set_text(ui_LabelUserIndex, buf);
    if (ui_BtnRemoveCard) {
      if (hasCard) lv_obj_clear_flag(ui_BtnRemoveCard, LV_OBJ_FLAG_HIDDEN);
      else         lv_obj_add_flag(ui_BtnRemoveCard, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_BtnDeleteUser)  lv_obj_clear_flag(ui_BtnDeleteUser, LV_OBJ_FLAG_HIDDEN);
    if (ui_BtnScanForUser) lv_obj_clear_state(ui_BtnScanForUser, LV_STATE_DISABLED);
  }
}

// ============================================================================
// USER MANAGEMENT — add / delete user from TFT
// ============================================================================

void logic_add_new_user(lv_event_t* /*e*/) {
  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
    if (userCount >= MAX_USERS) {
      xSemaphoreGive(xMutexData);
      beepError();
      if (ui_LabelCardStatus) lv_label_set_text(ui_LabelCardStatus, "Max users reached!");
      return;
    }
    // Build new User
    User* u = &users[userCount];
    memset(u, 0, sizeof(User));
    // Auto-generate ID from millis
    snprintf(u->id, sizeof(u->id), "%lu", millis());
    // Auto-generate name: find first free "User N"
    char autoName[32];
    snprintf(autoName, sizeof(autoName), "User %d", userCount + 1);
    strncpy(u->name, autoName, sizeof(u->name) - 1);
    strncpy(u->role, "User", sizeof(u->role) - 1);
    u->active = true;
    strncpy(u->allowedStart, "07:00", sizeof(u->allowedStart) - 1);
    strncpy(u->allowedEnd,   "18:00", sizeof(u->allowedEnd) - 1);
    selectedUserIndex = userCount;
    userCount++;
    xSemaphoreGive(xMutexData);
  }
  saveData();
  addLog("System", "User", "System Alert", "New user added via TFT");
  beepSuccess();
  _refresh_select_user_ui();
  safeSerialPrintf("[TFT] New user added. Total: %d\n", userCount);
}

void logic_delete_current_user(lv_event_t* /*e*/) {
  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
    if (userCount == 0 || selectedUserIndex < 0 || selectedUserIndex >= userCount) {
      xSemaphoreGive(xMutexData);
      beepError();
      return;
    }
    char delName[32];
    strncpy(delName, users[selectedUserIndex].name, sizeof(delName) - 1);
    // Shift array left to fill the gap
    for (int i = selectedUserIndex; i < userCount - 1; i++) {
      users[i] = users[i + 1];
    }
    memset(&users[userCount - 1], 0, sizeof(User));
    userCount--;
    if (selectedUserIndex >= userCount) selectedUserIndex = userCount - 1;
    xSemaphoreGive(xMutexData);
    char logMsg[64];
    snprintf(logMsg, sizeof(logMsg), "User '%s' deleted via TFT", delName);
    addLog("System", "User", "System Alert", logMsg);
  }
  saveData();
  beepSuccess();
  _refresh_select_user_ui();
  safeSerialPrintf("[TFT] User deleted. Total: %d\n", userCount);
}
