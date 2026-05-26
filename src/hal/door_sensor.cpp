#include "door_sensor.h"
#include "app_globals.h"
#include "hal/buzzer.h"
#include "data_store.h"
#include "email_manager.h"
#include "tft_display.h"

// ============================================================================
// INTERRUPT — chạy ngay khi GPIO thay đổi mức (CHANGE)
// ============================================================================
static SemaphoreHandle_t doorISRSema = NULL;

static void IRAM_ATTR doorSensorISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(doorISRSema, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void doorSensorInit() {
  pinMode(DOOR_SENSOR_PIN, INPUT_PULLUP);
  doorIsOpen = (digitalRead(DOOR_SENSOR_PIN) == DOOR_OPEN_STATE);
  safeSerialPrintf("[DOOR] Sensor initialized. Door is %s\n",
                   doorIsOpen ? "OPEN" : "CLOSED");

  // Tạo binary semaphore để ISR wake task
  doorISRSema = xSemaphoreCreateBinary();

  // Gắn ngắt: kích hoạt cả cạnh lên lẫn cạnh xuống
  attachInterrupt(digitalPinToInterrupt(DOOR_SENSOR_PIN), doorSensorISR, CHANGE);
  safeSerialPrint("[DOOR] Interrupt attached (CHANGE mode)\n");
}

bool isDoorOpen() {
  return (digitalRead(DOOR_SENSOR_PIN) == DOOR_OPEN_STATE);
}

void startDoorAlarm() {
  if (doorAlarmActive) return;

  doorAlarmActive = true;
  doorAlarmStart  = millis();

  // Bật màn hình ngay lập tức dù đang tắt hay đang sleep
  if (backlightIsOff) {
    tft_set_backlight(255);
    backlightIsOff = false;
    safeSerialPrint("[ALARM] Backlight forced ON — door tamper!\n");
  }
  lastUserActivityTime = millis(); // Reset timeout để màn không tắt lại ngay

  safeSerialPrint("[ALARM] *** DOOR TAMPER DETECTED! ALARM ACTIVATED ***\n");

  addLog("System", "Door Sensor", "Security Alert",
         "Unauthorized door opening detected!");

  queueEmailAlert(
      "🚨 INTRUSION ALERT - Door Opened!",
      "WARNING: The door was opened while the system was LOCKED!\n\n"
      "This may indicate an unauthorized entry attempt.\n\n"
      "Please check your property immediately.\n\n"
      "To dismiss this alarm, open the SmartLock Dashboard and acknowledge.");

  SystemEvent evt = EVENT_DOOR_TAMPER;
  xQueueSend(eventQueue, &evt, pdMS_TO_TICKS(10));
}

void stopDoorAlarm() {
  if (!doorAlarmActive) return;

  doorAlarmActive = false;
  doorAlarmStart  = 0;
  ledcWrite(BUZZER_CHANNEL, 0);
  digitalWrite(LED_RED, HIGH); // Khôi phục trạng thái khóa ngay — không để LED ở LOW ngẫu nhiên

  safeSerialPrint("[ALARM] Alarm acknowledged and stopped\n");
  addLog("System", "Door Sensor", "Security Alert", "Alarm acknowledged");
}

void beepAlarmContinuous() {
  if (!settings.soundEnabled) return;

  static unsigned long lastToggle = 0;
  static bool highTone = false;

  if (millis() - lastToggle > 300) {
    highTone = !highTone;
    ledcWriteTone(BUZZER_CHANNEL, highTone ? 2500 : 1000);
    lastToggle = millis();
  }
}

void taskDoorSensor(void *parameter) {
  bool lastDoorState = isDoorOpen();

  safeSerialPrint("[DOOR] Door sensor task started (interrupt-driven)\n");

  // Boot-time tamper check: nếu cửa đang hở ngay khi khởi động → hú ngay
  while (!systemReady) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (doorSensorEnabled && isDoorOpen()) {
    safeSerialPrint("[DOOR] BOOT CHECK: Door is OPEN at startup — triggering alarm!\n");
    doorIsOpen   = true;
    doorOpenTime = millis();
    startDoorAlarm();
    SystemEvent evt = EVENT_DOOR_OPENED;
    xQueueSend(eventQueue, &evt, 0);
  }

  for (;;) {
    // Chờ ISR báo có thay đổi GPIO, timeout 50ms để vẫn xử lý alarm beeping
    bool interrupted = (xSemaphoreTake(doorISRSema, pdMS_TO_TICKS(50)) == pdTRUE);

    if (interrupted && doorSensorEnabled) {
      // Debounce: chờ tín hiệu ổn định rồi đọc lại
      vTaskDelay(pdMS_TO_TICKS(DOOR_DEBOUNCE_MS));
      bool currentDoorState = isDoorOpen();

      if (currentDoorState != lastDoorState) {
        lastDoorState = currentDoorState;
        doorIsOpen    = currentDoorState;

        if (currentDoorState) {
          // === DOOR OPENED ===
          doorOpenTime = millis();
          safeSerialPrint("[DOOR] Door OPENED\n");

          SystemState state = STATE_IDLE;
          if (xSemaphoreTake(xMutexState, pdMS_TO_TICKS(10))) {
            state = currentState;
            xSemaphoreGive(xMutexState);
          }

          if (state == STATE_UNLOCKED) {
            safeSerialPrint("[DOOR] Door opened (authorized - system unlocked)\n");
            addLog("System", "Door Sensor", "Entry", "Door opened (authorized)");
            doorAuthorizedOpenTime  = millis();
            doorLeftOpenWarning     = false;
            doorLeftOpenLastBeep    = 0;
            doorLeftOpenAlarmActive = false;
          } else {
            // Mọi trạng thái khác (LOCKED, IDLE, PASSWORD_LOCKOUT,
            // SYSTEM_DISABLED, PASSWORD_INPUT, VALIDATING, ALARM...)
            // đều là cạy cửa trái phép → alarm ngay lập tức
            safeSerialPrintf("[DOOR] WARNING: Door forced open in state %d! Triggering alarm!\n",
                             (int)state);
            startDoorAlarm();
          }

          SystemEvent evt = EVENT_DOOR_OPENED;
          xQueueSend(eventQueue, &evt, 0);

        } else {
          // === DOOR CLOSED ===
          safeSerialPrint("[DOOR] Door CLOSED\n");
          doorAuthorizedOpenTime  = 0;
          doorLeftOpenWarning     = false;
          doorLeftOpenLastBeep    = 0;

          // Dừng còi cảnh báo quên đóng cửa ngay lập tức
          if (doorLeftOpenAlarmActive) {
            doorLeftOpenAlarmActive = false;
            ledcWrite(BUZZER_CHANNEL, 0);
            safeSerialPrint("[DOOR] Left-open alarm cleared — door closed\n");
          }

          SystemEvent evt = EVENT_DOOR_CLOSED;
          xQueueSend(eventQueue, &evt, 0);
        }
      }
    }

    // 2. Active alarm handling (chạy mỗi lần timeout 50ms hoặc sau sự kiện)
    if (doorAlarmActive) {
      beepAlarmContinuous();

      // Dùng biến state riêng thay vì !digitalRead để tránh read-modify-write race
      // (taskFSM ở cùng core Prio 3 có thể chen giữa read và write GPIO)
      static unsigned long lastLedBlink = 0;
      static bool ledBlinkState = false;
      if (millis() - lastLedBlink > 200) {
        ledBlinkState = !ledBlinkState;
        digitalWrite(LED_RED, ledBlinkState ? HIGH : LOW);
        lastLedBlink = millis();
      }

      if (millis() - doorAlarmStart > DOOR_ALARM_DURATION) {
        safeSerialPrint("[ALARM] Auto-stop after timeout\n");
        stopDoorAlarm();
        if (!doorIsOpen) {
          SystemEvent evt = EVENT_ALARM_ACKNOWLEDGE;
          xQueueSend(eventQueue, &evt, 0);
        }
      }
    }

    // 4. "Door left open" — cảnh báo liên tục cho đến khi đóng cửa
    if (doorAuthorizedOpenTime > 0 && doorIsOpen && !doorAlarmActive) {
      unsigned long openDuration = millis() - doorAuthorizedOpenTime;

      if (openDuration >= DOOR_LEFT_OPEN_WARN_MS) {
        // Lần đầu vượt ngưỡng: chuyển trạng thái + gửi mail
        if (!doorLeftOpenAlarmActive) {
          doorLeftOpenAlarmActive = true;
          doorLeftOpenWarning     = true;

          addLog("System", "Door Sensor", "Security Alert",
                 "Canh bao: Quen dong cua! Coi hu lien tuc.");
          safeSerialPrint("[DOOR] Left-open alarm: buzzer ON until door closed!\n");

          queueEmailAlert(
              "[SmartLock] Canh bao: Quen dong cua!",
              "Cua da mo qua 10 giay ma chua duoc dong lai.\n\n"
              "He thong dang phat canh bao lien tuc bang coi hieu.\n"
              "Vui long kiem tra va dong cua lai ngay.");

          // Chuyển FSM sang trạng thái cảnh báo
          SystemEvent evt = EVENT_DOOR_TAMPER;
          xQueueSend(eventQueue, &evt, 0);
        }

        // Còi hú liên tục cho đến khi cửa đóng
        if (settings.soundEnabled) {
          beepAlarmContinuous();
        }
      }
    }

    // Không cần vTaskDelayUntil — semaphore timeout 50ms đảm nhận vai trò này
  }
}
