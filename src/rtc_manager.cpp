#include "rtc_manager.h"
#include "app_globals.h"
#include "data_store.h"
#include <driver/gpio.h>

// RTC_DATA_ATTR variables are defined in main.cpp and extern'd via app_globals.h

time_t getRTCTime() {
  if (rtcAvailable) {
    DateTime now = rtc.now();
    return now.unixtime();  // RTC lưu GMT+7 trực tiếp — không cộng thêm offset
  } else {
    time_t now;
    time(&now);
    return now + (7 * 3600);  // fallback: time() trả UTC, cộng GMT+7
  }
}

void setRTCTime(time_t unixTime) {
  if (rtcAvailable) {
    rtc.adjust(DateTime(unixTime));
  }
}

void syncRTCWithNTP() {
  if (!rtcAvailable) return;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 1000)) {
    // getLocalTime trả GMT+7 (vì configTime đã set gmtOffset=7*3600)
    // mktime → Unix timestamp của giờ địa phương → lưu thẳng vào RTC
    rtc.adjust(DateTime((uint32_t)mktime(&timeinfo)));
    safeSerialPrint("[RTC] Synced with NTP (GMT+7 stored directly)\n");
  }
}

bool isRTCValid() {
  if (!rtcAvailable) return false;
  DateTime now = rtc.now();
  return now.year() >= 2020;
}

void restoreSecurityFromRTC() {
  security.passwordFailCount       = rtc_passwordFailCount;
  security.totalFailCount          = rtc_totalFailCount;
  security.lockoutEndTime          = rtc_lockoutEndTime;
  security.currentLockoutDuration  = rtc_lockoutDuration;
  security.systemDisabled          = rtc_systemDisabled;

  time_t now = getRTCTime();
  if (security.lockoutEndTime > 0 && now >= security.lockoutEndTime) {
    security.lockoutEndTime = 0;
    safeSerialPrint("[RTC] Lockout expired during sleep\n");
  } else if (security.lockoutEndTime > 0) {
    safeSerialPrintf("[RTC] Lockout still active: %ld seconds remaining\n",
                     (long)(security.lockoutEndTime - now));
  }

  if (security.systemDisabled) {
    Serial.println("[RTC] WARNING: System DISABLED state restored!");
  }
}

void saveSecurityToRTC() {
  rtc_passwordFailCount = security.passwordFailCount;
  rtc_totalFailCount    = security.totalFailCount;
  rtc_lockoutEndTime    = security.lockoutEndTime;
  rtc_lockoutDuration   = security.currentLockoutDuration;
  rtc_systemDisabled    = security.systemDisabled;
  rtc_lastActivityTime  = getRTCTime();
}

void updateUserActivity() {
  lastUserActivityTime = millis();
  rtc_lastActivityTime = getRTCTime();
}

void enterLightSleep(uint32_t sleepMs) {
  if (sleepMs == 0) return;
  safeSerialPrintf("[POWER] Entering light sleep for %lu ms\n", sleepMs);

  saveSecurityToRTC();
  // Fix 3: Chỉ ghi flash nếu dữ liệu chưa được lưu gần đây — tránh mòn flash khi timer tự tỉnh
  if (millis() - lastSaveTime > 30000) {
    saveData();
  }

  esp_sleep_enable_timer_wakeup(sleepMs * 1000ULL);
  // Fix 1: GPIO wakeup hỗ trợ nhiều pin đồng thời (EXT0 chỉ dùng được 1 pin — gọi 2 lần sẽ ghi đè)
  gpio_wakeup_enable((gpio_num_t)TOUCH_IRQ_PIN,     GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)FACTORY_RESET_PIN, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  esp_sleep_enable_ext1_wakeup(1ULL << DOOR_SENSOR_PIN, ESP_EXT1_WAKEUP_ANY_HIGH); // Cửa mở (MC38 → HIGH) → wake ngay

  isInSleepMode = true;
  esp_light_sleep_start();

  isInSleepMode = false;

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  // Fix 2: Chỉ reset inactivity timer khi có người dùng thực sự — không reset khi timer tự tỉnh
  if (cause != ESP_SLEEP_WAKEUP_TIMER) {
    lastUserActivityTime = millis();
  }

  // Nếu bị đánh thức bởi cảm biến cửa → bật màn hình ngay, không chờ người chạm
  if (cause == ESP_SLEEP_WAKEUP_EXT1 && backlightIsOff) {
    tft_set_backlight(255);
    backlightIsOff = false;
    safeSerialPrint("[POWER] Door wake → backlight ON immediately\n");
  }

  safeSerialPrint("[POWER] Woke up from light sleep\n");
}

void enterDeepSleep(uint32_t sleepMs) {
  if (sleepMs == 0) return;
  safeSerialPrintf("[POWER] Entering deep sleep for %lu ms\n", sleepMs);

  saveSecurityToRTC();
  saveData();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_enable_timer_wakeup(sleepMs * 1000ULL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TOUCH_IRQ_PIN, 0);

  esp_deep_sleep_start();
}

bool shouldEnterNightSleep() {
  if (!rtcAvailable) return false;
  DateTime now = rtc.now();
  int hour = now.hour();
  return (hour >= NIGHT_SLEEP_START_HOUR || hour < NIGHT_SLEEP_END_HOUR);
}
