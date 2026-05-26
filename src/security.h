#pragma once
#include <ESPAsyncWebServer.h>
// Security, auth, token, and login functions

bool isTimeAllowed(const char *startTime, const char *endTime);
bool validatePassword(const char *password);         // Exact match (admin only)
bool containsPassword(const char *input, const char *realPassword); // Scramble check
bool validateAnyPasswordScramble(const char *input, int *matchedUserIdx); // Scramble vs all users+admin
bool isAdminRole(const char *role);
// lockDoor() / unlockDoor() declared in lvgl/ui_bridge.h (extern "C")
void recordFailedAttempt(const char *method, const char *details);
void resetSecurityCounters();
void checkLockoutStatus();
unsigned long calculateLockoutTime();

// Auth / token
String generateToken();
String generateRandomToken();
bool validateAndRefreshToken(String token);
void removeToken(String token);
String extractTokenFromRequest(AsyncWebServerRequest *request);
void cleanupExpiredTokens();
bool isAuthenticated(AsyncWebServerRequest *request);

// Login tracker
bool isLockedOut();
void recordFailedLogin();
void recordSuccessLogin();
