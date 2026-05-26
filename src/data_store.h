#pragma once
// NVS data persistence and utility functions
#include <Arduino.h>

extern unsigned long lastSaveTime; // Timestamp của lần ghi flash gần nhất
void saveData();
void loadData();
void addLog(const char *user, const char *method, const char *type,
            const char *details);
String getTimestamp();
void factoryReset();
void resetWiFiSettings();
