#include "buzzer.h"
#include "../app_globals.h"

void beepSuccess() {
  if (!settings.soundEnabled)
    return;
  ledcWriteTone(BUZZER_CHANNEL, 1000);
  vTaskDelay(pdMS_TO_TICKS(100));
  ledcWrite(BUZZER_CHANNEL, 0);
  vTaskDelay(pdMS_TO_TICKS(50));
  ledcWriteTone(BUZZER_CHANNEL, 1500);
  vTaskDelay(pdMS_TO_TICKS(100));
  ledcWrite(BUZZER_CHANNEL, 0);
  vTaskDelay(pdMS_TO_TICKS(50));
  ledcWriteTone(BUZZER_CHANNEL, 2000);
  vTaskDelay(pdMS_TO_TICKS(200));
  ledcWrite(BUZZER_CHANNEL, 0);
}

void beepError() {
  if (!settings.soundEnabled)
    return;
  ledcWriteTone(BUZZER_CHANNEL, 400);
  vTaskDelay(pdMS_TO_TICKS(200));
  ledcWrite(BUZZER_CHANNEL, 0);
  vTaskDelay(pdMS_TO_TICKS(50));
  ledcWriteTone(BUZZER_CHANNEL, 400);
  vTaskDelay(pdMS_TO_TICKS(200));
  ledcWrite(BUZZER_CHANNEL, 0);
}

void beepWarning() {
  if (!settings.soundEnabled)
    return;
  ledcWriteTone(BUZZER_CHANNEL, 800);
  vTaskDelay(pdMS_TO_TICKS(500));
  ledcWrite(BUZZER_CHANNEL, 0);
}

void beepAlarm() {
  if (!settings.soundEnabled)
    return;
  for (int i = 0; i < 5; i++) {
    ledcWriteTone(BUZZER_CHANNEL, 2000);
    vTaskDelay(pdMS_TO_TICKS(200));
    ledcWrite(BUZZER_CHANNEL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
