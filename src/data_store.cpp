#include "data_store.h"
#include "app_globals.h"
#include "rtc_manager.h"
#include "hal/buzzer.h"

// ============================================================================
// LOGGING
// ============================================================================

void addLog(const char *user, const char *method, const char *type,
            const char *details) {
  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
    AccessLog *log = &logs[logIndex];

    snprintf(log->id, sizeof(log->id), "%lu", millis());
    strncpy(log->timestamp, getTimestamp().c_str(), sizeof(log->timestamp) - 1);
    strncpy(log->user,      user,    sizeof(log->user) - 1);
    strncpy(log->method,    method,  sizeof(log->method) - 1);
    strncpy(log->type,      type,    sizeof(log->type) - 1);
    strncpy(log->details,   details, sizeof(log->details) - 1);

    logIndex = (logIndex + 1) % MAX_LOGS;
    if (logCount < MAX_LOGS) logCount++;

    strncpy(systemStatus.lastSync, log->timestamp,
            sizeof(systemStatus.lastSync) - 1);

    xSemaphoreGive(xMutexData);

    safeSerialPrintf("[LOG] %s | %s | %s\n", user, type, details);
  }
}

String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    return String(millis());
  }
  char buffer[32];
  strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buffer);
}

// ============================================================================
// DATA PERSISTENCE
// ============================================================================

unsigned long lastSaveTime = 0;

void saveData() {
  lastSaveTime = millis(); // Cập nhật timestamp để enterLightSleep bỏ qua save trùng lặp
  preferences.putString("password", systemPassword);
  preferences.putBool("isDefaultPwd", isDefaultPassword);
  preferences.putInt("userCount", userCount);

  for (int i = 0; i < userCount && i < MAX_USERS; i++) {
    char key[16];
    snprintf(key, sizeof(key), "user%d", i);

    JsonDocument doc;
    doc["id"]           = users[i].id;
    doc["name"]         = users[i].name;
    doc["role"]         = users[i].role;
    doc["cardId"]       = users[i].cardId;
    doc["active"]       = users[i].active;
    doc["allowedStart"] = users[i].allowedStart;
    doc["allowedEnd"]   = users[i].allowedEnd;
    doc["password"]     = users[i].password;

    String json;
    serializeJson(doc, json);
    preferences.putString(key, json);
  }

  JsonDocument settingsDoc;
  settingsDoc["email"]     = settings.notificationEmail;
  settingsDoc["screenTimeout"] = settings.screenTimeout;
  settingsDoc["sound"]     = settings.soundEnabled;

  String settingsJson;
  serializeJson(settingsDoc, settingsJson);
  preferences.putString("settings", settingsJson);

  // Security state (survives reboot)
  preferences.putInt("secPassFails",    security.passwordFailCount);
  preferences.putInt("secTotalFails",   security.totalFailCount);
  preferences.putBool("secSysDisabled", security.systemDisabled);
  preferences.putULong("secLockoutDur", security.currentLockoutDuration);
  preferences.putULong("secLockEnd",    (unsigned long)security.lockoutEndTime);

  // Login tracker
  preferences.putInt("loginFails",      loginTracker.failedCount);
  preferences.putULong("loginLockDur",  loginTracker.lockoutDuration);
  preferences.putULong("loginLockEnd",  (unsigned long)loginTracker.lockoutEndTime);

  // Door sensor
  preferences.putBool("doorSensorEn",   doorSensorEnabled);

  // NFC card lock
  preferences.putInt("cardFails",        security.cardFailCount);
  preferences.putBool("cardFuncLock",    security.cardFunctionLocked);

  safeSerialPrint("[Data] Security state saved to flash\n");
}

void loadData() {
  String pwd = preferences.getString("password", "123456");
  strncpy(systemPassword, pwd.c_str(), PASSWORD_LENGTH);
  isDefaultPassword = preferences.getBool("isDefaultPwd", true);

  if (strcmp(systemPassword, "123456") != 0) {
    isDefaultPassword = false;
  }

  userCount = preferences.getInt("userCount", 0);
  if (userCount < 0 || userCount > MAX_USERS) userCount = 0;

  for (int i = 0; i < userCount && i < MAX_USERS; i++) {
    char key[16];
    snprintf(key, sizeof(key), "user%d", i);

    String json = preferences.getString(key, "{}");
    JsonDocument doc;
    deserializeJson(doc, json);

    strncpy(users[i].id,           doc["id"]           | "", sizeof(users[i].id) - 1);
    strncpy(users[i].name,         doc["name"]         | "", sizeof(users[i].name) - 1);
    strncpy(users[i].role,         doc["role"]         | "User", sizeof(users[i].role) - 1);
    strncpy(users[i].cardId,       doc["cardId"]       | "", sizeof(users[i].cardId) - 1);
    users[i].active = doc["active"] | true;
    strncpy(users[i].allowedStart, doc["allowedStart"] | "07:00", sizeof(users[i].allowedStart) - 1);
    strncpy(users[i].allowedEnd,   doc["allowedEnd"]   | "18:00", sizeof(users[i].allowedEnd) - 1);
    strncpy(users[i].password,     doc["password"]     | "", sizeof(users[i].password) - 1);
    users[i].password[PASSWORD_LENGTH] = '\0';
  }

  String settingsJson = preferences.getString("settings", "{}");
  JsonDocument settingsDoc;
  deserializeJson(settingsDoc, settingsJson);

  strncpy(settings.notificationEmail, settingsDoc["email"] | "",
          sizeof(settings.notificationEmail) - 1);
  settings.screenTimeout = settingsDoc["screenTimeout"] | 30;
  settings.soundEnabled = settingsDoc["sound"]    | true;

  // Restore security counters
  security.passwordFailCount      = preferences.getInt("secPassFails", 0);
  security.totalFailCount         = preferences.getInt("secTotalFails", 0);
  security.systemDisabled         = preferences.getBool("secSysDisabled", false);
  security.currentLockoutDuration = preferences.getULong("secLockoutDur", INITIAL_LOCKOUT_TIME);

  time_t secLockEnd = (time_t)preferences.getULong("secLockEnd", 0);
  time_t now        = getRTCTime();

  if (secLockEnd > 0 && secLockEnd > now) {
    security.lockoutEndTime = secLockEnd;
    Serial.printf("[Security] Restored lockout: %ld seconds remaining (RTC-based)\n",
                  (long)(secLockEnd - now));
  } else {
    security.lockoutEndTime = 0;
    if (secLockEnd > 0)
      Serial.println("[Security] Lockout expired during power off");
  }

  // Restore login tracker
  loginTracker.failedCount      = preferences.getInt("loginFails", 0);
  loginTracker.lockoutDuration  = preferences.getULong("loginLockDur", INITIAL_LOCKOUT_TIME);

  time_t loginLockEnd = (time_t)preferences.getULong("loginLockEnd", 0);
  if (loginLockEnd > 0 && loginLockEnd > now) {
    loginTracker.lockoutEndTime = loginLockEnd;
    Serial.printf("[Login] Restored lockout: %ld seconds remaining (RTC-based)\n",
                  (long)(loginLockEnd - now));
  } else {
    loginTracker.lockoutEndTime = 0;
    if (loginLockEnd > 0)
      Serial.println("[Login] Lockout expired during power off");
  }

  doorSensorEnabled           = preferences.getBool("doorSensorEn", true);
  security.cardFailCount      = preferences.getInt("cardFails", 0);
  security.cardFunctionLocked = preferences.getBool("cardFuncLock", false);

  if (security.systemDisabled)
    Serial.println("[Security] WARNING: System was DISABLED before power off!");
  if (security.totalFailCount > 0)
    Serial.printf("[Security] Restored fail counts - Pass:%d Total:%d\n",
                  security.passwordFailCount, security.totalFailCount);
}

// ============================================================================
// FACTORY RESET & WIFI RESET
// ============================================================================

void resetWiFiSettings() {
  wifiManagerAsync.resetAndStartAP();
  Serial.println("[WiFi] Credentials cleared - Now in AP config mode");
}

void factoryReset() {
  safeSerialPrint("\n========================================\n");
  safeSerialPrint("[FACTORY RESET] WIPING ALL DATA...\n");
  safeSerialPrint("========================================\n");

  for (int i = 0; i < 10; i++) {
    digitalWrite(LED_GREEN, !digitalRead(LED_GREEN));
    digitalWrite(LED_RED,   !digitalRead(LED_RED));
    delay(100);
  }

  safeSerialPrint("[FACTORY] Clearing Preferences...\n");
  preferences.clear();

  safeSerialPrint("[FACTORY] Clearing WiFi credentials...\n");
  wifiManagerAsync.resetSettings();

  safeSerialPrint("[FACTORY] Factory reset complete! Rebooting in 2 seconds...\n");

  for (int i = 0; i < 3; i++) {
    ledcWriteTone(BUZZER_CHANNEL, 2000);
    delay(200);
    ledcWrite(BUZZER_CHANNEL, 0);
    delay(200);
  }

  delay(2000);
  ESP.restart();
}
