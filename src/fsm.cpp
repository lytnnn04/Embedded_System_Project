#include "fsm.h"
#include "app_globals.h"
#include "hal/buzzer.h"
#include "hal/door_sensor.h"
#include "security.h"
#include "data_store.h"
#include "email_manager.h"
#include "rtc_manager.h"

const char *getStateName(SystemState state) {
  switch (state) {
  case STATE_IDLE:              return "IDLE";
  case STATE_PASSWORD_INPUT:    return "PASSWORD_INPUT";
  case STATE_VALIDATING:        return "VALIDATING";
  case STATE_PASSWORD_LOCKOUT:  return "PASSWORD_LOCKOUT";
  case STATE_SYSTEM_DISABLED:   return "SYSTEM_DISABLED";
  case STATE_UNLOCKED:          return "UNLOCKED";
  case STATE_LOCKED:            return "LOCKED";
  case STATE_ALARM:             return "ALARM";
  default:                      return "UNKNOWN";
  }
}

void fsmTransition(SystemState newState) {
  if (currentState != newState) {
    previousState  = currentState;
    currentState   = newState;
    stateEntryTime = millis();

    safeSerialPrintf("[FSM] %s -> %s\n",
                     getStateName(previousState), getStateName(newState));

    switch (newState) {
    case STATE_UNLOCKED:
      unlockDoor();
      unlockTime = millis();
      break;

    case STATE_LOCKED:
      lockDoor();
      t9Mode = 0;
      break;

    case STATE_PASSWORD_LOCKOUT:
      beepWarning();
      break;

    case STATE_SYSTEM_DISABLED:
      lockDoor();
      beepAlarm();
      addLog("System", "System", "System Alert", "System disabled");
      break;

    case STATE_ALARM:
      if (!doorIsOpen) {
        lockDoor();
      }
      break;

    case STATE_PASSWORD_INPUT:
      break;

    case STATE_IDLE:
      break;

    default:
      break;
    }
  }
}

void fsmProcessEvent(SystemEvent event) {
  switch (event) {
  case EVENT_AUTH_SUCCESS:
    resetSecurityCounters();
    fsmTransition(STATE_UNLOCKED);
    break;

  case EVENT_AUTH_FAILED:
    security.totalFailCount++;

    if (security.totalFailCount >= MAX_TOTAL_ATTEMPTS) {
      security.systemDisabled = true;
      fsmTransition(STATE_SYSTEM_DISABLED);
      saveData();
      queueEmailAlert("Cảnh báo: hệ thống bị vô hiệu hóa.",
                      "Quá nhiều lần xác thực thất bại. Hệ thống đã bị khóa.");
    } else if (security.passwordFailCount >= MAX_PASSWORD_ATTEMPTS) {
      long dur = (long)(calculateLockoutTime() / 1000);
      lockoutDurationSec      = dur;
      security.lockoutEndTime = getRTCTime() + dur;
      fsmTransition(STATE_PASSWORD_LOCKOUT);
      saveData();
    } else {
      beepError();
      int attemptsLeft = MAX_PASSWORD_ATTEMPTS - security.passwordFailCount;
      wrongPinDisplayActive  = true;
      wrongPinDisplayStartMs = millis();
      if (xSemaphoreTake(xMutexLVGL, pdMS_TO_TICKS(200))) {
        _ui_screen_change(&ui_uiwrongpin, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0,
                          &ui_uiwrongpin_screen_init);
        if (ui_LabelAttemptsLeft) {
          char attBuf[40];
          snprintf(attBuf, sizeof(attBuf), "%d more attempts remaining.", attemptsLeft);
          lv_label_set_text(ui_LabelAttemptsLeft, attBuf);
        }
        if (ui_Label19) lv_label_set_text(ui_Label19, "3s");
        if (ui_Arc2)    lv_arc_set_value(ui_Arc2, 100);
        xSemaphoreGive(xMutexLVGL);
      }
      fsmTransition(STATE_LOCKED);
    }
    break;

  case EVENT_LOCKOUT_EXPIRED:
    if (currentState == STATE_PASSWORD_LOCKOUT) {
      security.passwordFailCount = MAX_PASSWORD_ATTEMPTS - 1;
      fsmTransition(STATE_LOCKED);
      saveData();
    }
    break;

  case EVENT_REMOTE_UNLOCK:
    resetSecurityCounters();
    security.systemDisabled = false;
    fsmTransition(STATE_UNLOCKED);
    addLog("Remote", "Remote App", "Entry", "Unlocked via dashboard");
    break;

  case EVENT_REMOTE_LOCK:
    fsmTransition(STATE_LOCKED);
    addLog("Remote", "Remote App", "Exit", "Locked via dashboard");
    break;

  case EVENT_REMOTE_RESET:
    resetSecurityCounters();
    security.systemDisabled = false;
    fsmTransition(STATE_LOCKED);
    addLog("System", "Remote App", "System Alert", "System reset");
    break;

  case EVENT_AUTO_LOCK:
    fsmTransition(STATE_LOCKED);
    addLog("System", "System", "Exit", "Auto-locked");
    break;

  case EVENT_DOOR_TAMPER:
    fsmTransition(STATE_ALARM);
    break;

  case EVENT_DOOR_OPENED:
    break;

  case EVENT_DOOR_CLOSED:
    if (currentState == STATE_ALARM) {
      stopDoorAlarm();
      fsmTransition(STATE_LOCKED);
      addLog("System", "Door Sensor", "System Alert", "Door closed, alarm cleared");
    } else if (currentState == STATE_UNLOCKED) {
      fsmTransition(STATE_LOCKED);
      addLog("System", "Door Sensor", "Exit", "Door closed - auto locked");
    }
    break;

  case EVENT_ALARM_ACKNOWLEDGE:
    stopDoorAlarm();
    fsmTransition(STATE_LOCKED);
    addLog("System", "Door Sensor", "System Alert", "Alarm dismissed");
    break;

  default:
    break;
  }
}

void fsmHandleState() {
  switch (currentState) {
  case STATE_PASSWORD_LOCKOUT:
    if (getRTCTime() >= security.lockoutEndTime) {
      SystemEvent evt = EVENT_LOCKOUT_EXPIRED;
      xQueueSend(eventQueue, &evt, 0);
    }
    break;
  case STATE_UNLOCKED:
    if (millis() - unlockTime > AUTO_LOCK_TIME * 3) {
      SystemEvent evt = EVENT_AUTO_LOCK;
      xQueueSend(eventQueue, &evt, 0);
      safeSerialPrint("[FSM] Fallback auto-lock (timeout)\n");
    }
    break;
  default:
    break;
  }
}
