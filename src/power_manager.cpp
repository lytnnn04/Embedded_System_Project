#include "power_manager.h"
#include "app_globals.h"
#include "rtc_manager.h"

void taskPowerManager(void *parameter) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  unsigned long lastRTCSync = 0;
  bool prevConnected = false;
  unsigned long justConnectedAt = 0;  // timestamp khi WiFi vừa kết nối
  const unsigned long RTC_SYNC_INTERVAL = 6UL * 60UL * 60UL * 1000UL; // 6 hours
  const unsigned long NTP_SETTLE_MS     = 5000UL; // đợi SNTP nhận response

  for (;;) {
    bool nowConnected = systemStatus.wifiConnected;

    // Ghi nhận thời điểm WiFi vừa kết nối
    if (nowConnected && !prevConnected) {
      justConnectedAt = millis();
      safeSerialPrint("[Power] WiFi just connected, waiting for SNTP settle...\n");
    }

    // 1. Sync RTC with NTP:
    //    - Sau khi WiFi vừa kết nối, đợi NTP_SETTLE_MS (5s) để SNTP nhận response
    //    - Hoặc định kỳ mỗi 6 tiếng
    if (rtcAvailable && nowConnected) {
      bool settleReady = (justConnectedAt > 0 &&
                          millis() - justConnectedAt >= NTP_SETTLE_MS &&
                          lastRTCSync < justConnectedAt); // chưa sync kể từ lần connect này
      bool periodicSync = (lastRTCSync > 0 &&
                           millis() - lastRTCSync > RTC_SYNC_INTERVAL);
      if (settleReady || periodicSync) {
        syncRTCWithNTP();
        lastRTCSync = millis();
      }
    }
    prevConnected = nowConnected;

    // 2. Check sleep conditions
    if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(50))) {
      bool canSleep  = (currentState == STATE_LOCKED || currentState == STATE_IDLE);
      unsigned long inactiveTime = millis() - lastUserActivityTime;
      xSemaphoreGive(xMutexState);

      if (canSleep) {
        if (inactiveTime > INACTIVITY_SLEEP_TIME) {
          enterLightSleep(30000);
        }
        // Deep sleep at night — uncomment to enable:
        // if (shouldEnterNightSleep()) {
        //   uint32_t sleepUntil = (NIGHT_SLEEP_END_HOUR - rtc.now().hour()) * 3600000;
        //   if (sleepUntil > 0) enterDeepSleep(sleepUntil);
        // }
      }
    }

    // Cycle: 1s nếu đang chờ SNTP settle, ngược lại 10s
    bool waitingSettle = (justConnectedAt > 0 &&
                          lastRTCSync < justConnectedAt &&
                          systemStatus.wifiConnected);
    vTaskDelayUntil(&xLastWakeTime,
                    pdMS_TO_TICKS(waitingSettle ? 1000 : 10000));
  }
}
