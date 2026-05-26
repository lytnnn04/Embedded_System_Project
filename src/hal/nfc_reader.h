#pragma once
#include "../app_globals.h"
// NFC / PN532 card reader functions and task

bool initPN532();
bool readCard(char *uidStr, size_t maxLen);
bool validateCard(const char *cardId, char *outUserName, size_t nameLen);
void startCardScan(CardScanMode mode);
void stopCardScan();
void taskCardReader(void *parameter);
