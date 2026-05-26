#pragma once
// RTC DS1307 helper functions
#include <Arduino.h>

time_t getRTCTime();
void setRTCTime(time_t unixTime);
void syncRTCWithNTP();
bool isRTCValid();
void restoreSecurityFromRTC();
void saveSecurityToRTC();
void updateUserActivity();
void enterLightSleep(uint32_t sleepMs);
void enterDeepSleep(uint32_t sleepMs);
bool shouldEnterNightSleep();
