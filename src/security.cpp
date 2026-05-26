#include "security.h"
#include "app_globals.h"
#include "rtc_manager.h"
#include "data_store.h"

bool isTimeAllowed(const char *startTime, const char *endTime) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    safeSerialPrint("[WARN] Cannot get time, allowing access\n");
    return true;
  }

  int currentMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;

  int startHour = 0, startMin = 0;
  sscanf(startTime, "%d:%d", &startHour, &startMin);
  int startMinutes = startHour * 60 + startMin;

  int endHour = 0, endMin = 0;
  sscanf(endTime, "%d:%d", &endHour, &endMin);
  int endMinutes = endHour * 60 + endMin;

  if (startMinutes <= endMinutes) {
    return (currentMinutes >= startMinutes && currentMinutes <= endMinutes);
  } else {
    return (currentMinutes >= startMinutes || currentMinutes <= endMinutes);
  }
}

bool validatePassword(const char *password) {
  return strcmp(password, systemPassword) == 0;
}

// ============================================================================
// SCRAMBLE PIN
// ============================================================================

bool isAdminRole(const char *role) {
  return strcmp(role, "Admin") == 0;
}

// Kiểm tra xem `input` có chứa `realPassword` như một chuỗi con liền kề không
bool containsPassword(const char *input, const char *realPassword) {
  if (!input || !realPassword) return false;
  size_t inputLen = strlen(input);
  size_t pwdLen   = strlen(realPassword);
  if (pwdLen == 0 || inputLen < pwdLen) return false;
  for (size_t i = 0; i <= inputLen - pwdLen; i++) {
    if (strncmp(input + i, realPassword, pwdLen) == 0) return true;
  }
  return false;
}

// Kiểm tra scramble PIN: input phải chứa mật khẩu admin HOẶC mật khẩu của bất kỳ user active nào
// matchedUserIdx: -1 nếu khớp admin, >= 0 nếu khớp user tại index đó
bool validateAnyPasswordScramble(const char *input, int *matchedUserIdx) {
  if (matchedUserIdx) *matchedUserIdx = -1;
  if (!input || strlen(input) == 0) return false;

  // Ưu tiên kiểm tra mật khẩu admin trước
  if (containsPassword(input, systemPassword)) {
    return true;
  }

  // Kiểm tra mật khẩu riêng của từng user đang active
  bool found = false;
  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
    for (int i = 0; i < userCount && !found; i++) {
      if (!users[i].active) continue;
      if (strlen(users[i].password) == 0) continue;
      // Chỉ kiểm tra giới hạn giờ với Guest — User/Admin/Family không bị giới hạn
      if (strcmp(users[i].role, "Guest") == 0) {
        if (!isTimeAllowed(users[i].allowedStart, users[i].allowedEnd)) continue;
      }
      if (containsPassword(input, users[i].password)) {
        if (matchedUserIdx) *matchedUserIdx = i;
        found = true;
      }
    }
    xSemaphoreGive(xMutexData);
  }
  return found;
}

void lockDoor() {
  digitalWrite(RELAY_PIN, HIGH);
  systemStatus.isLocked = true;
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED, HIGH);
  safeSerialPrint("[Door] LOCKED\n");
}

void unlockDoor() {
  digitalWrite(RELAY_PIN, LOW);
  systemStatus.isLocked = false;
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_RED, LOW);
  safeSerialPrint("[Door] UNLOCKED\n");
}

void recordFailedAttempt(const char *method, const char *details) {
  char logDetails[64];
  snprintf(logDetails, sizeof(logDetails), "%s (Attempt %d/%d)", details,
           security.totalFailCount + 1, MAX_TOTAL_ATTEMPTS);
  addLog("Unknown", method, "Failed Attempt", logDetails);

  safeSerialPrintf("[Security] Failed: Pass=%d, Total=%d\n",
                   security.passwordFailCount, security.totalFailCount);
}

void resetSecurityCounters() {
  security.passwordFailCount       = 0;
  security.cardFailCount           = 0;
  security.totalFailCount          = 0;
  security.lockoutEndTime          = 0;
  security.currentLockoutDuration  = INITIAL_LOCKOUT_TIME;
  security.cardFunctionLocked      = false;
  security.systemDisabled          = false;

  saveData();
  safeSerialPrint("[Security] Counters reset and saved to flash\n");
}

void checkLockoutStatus() {
  // Placeholder - lockout is handled in fsmHandleState()
}

unsigned long calculateLockoutTime() {
  static const unsigned long MAX_LOCKOUT_MS = 1800000UL; // 30 phút tối đa
  unsigned long lockoutTime = security.currentLockoutDuration;

  // Chuẩn bị duration cho lần lockout tiếp theo (nhân đôi, tối đa 30 phút)
  unsigned long nextDuration = lockoutTime * 2;
  if (nextDuration > MAX_LOCKOUT_MS) nextDuration = MAX_LOCKOUT_MS;
  security.currentLockoutDuration = nextDuration;

  safeSerialPrintf("[Security] Lockout: %lu ms (next: %lu ms)\n",
                   lockoutTime, nextDuration);
  return lockoutTime;
}

// ============================================================================
// AUTH / TOKEN
// ============================================================================

String generateToken() {
  String token = "";
  const char chars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  for (int i = 0; i < 32; i++) {
    token += chars[random(0, sizeof(chars) - 1)];
  }
  return token;
}

String generateRandomToken() {
  return generateToken();
}

bool validateAndRefreshToken(String token) {
  bool tokenExpired = false;
  bool tokenValid   = false;

  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
    for (auto &authToken : activeTokens) {
      if (authToken.token == token) {
        if (millis() < authToken.expireTime) {
          authToken.lastActivityTime = millis();
          authToken.expireTime       = millis() + SESSION_TIMEOUT;
          tokenValid = true;
        } else {
          tokenExpired = true;
          safeSerialPrintf("[AUTH] Token expired: %.20s..\n", token.c_str());
        }
        break;
      }
    }
    xSemaphoreGive(xMutexData);
  }

  if (tokenExpired) {
    removeToken(token);
  }

  return tokenValid;
}

void removeToken(String token) {
  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(50))) {
    for (size_t i = 0; i < activeTokens.size(); i++) {
      if (activeTokens[i].token == token) {
        activeTokens.erase(activeTokens.begin() + i);
        safeSerialPrintf("[AUTH] Token removed: %.20s...\n", token.c_str());
        break;
      }
    }
    xSemaphoreGive(xMutexData);
  }
}

String extractTokenFromRequest(AsyncWebServerRequest *request) {
  if (request->hasHeader("Authorization")) {
    String authHeader = request->header("Authorization");
    if (authHeader.startsWith("Bearer ")) {
      return authHeader.substring(7);
    }
  }
  return "";
}

void cleanupExpiredTokens() {
  if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(50))) {
    for (int i = (int)activeTokens.size() - 1; i >= 0; i--) {
      if (millis() >= activeTokens[i].expireTime) {
        safeSerialPrintf("[AUTH] Cleanup expired token: %.20s...\n",
                         activeTokens[i].token.c_str());
        activeTokens.erase(activeTokens.begin() + i);
      }
    }
    xSemaphoreGive(xMutexData);
  }
}

bool isAuthenticated(AsyncWebServerRequest *request) {
  String token = extractTokenFromRequest(request);
  if (token.length() == 0) return false;
  return validateAndRefreshToken(token);
}

// ============================================================================
// LOGIN TRACKER
// ============================================================================

bool isLockedOut() {
  time_t now = getRTCTime();
  if (now < loginTracker.lockoutEndTime) return true;

  if (loginTracker.lockoutEndTime > 0 && now >= loginTracker.lockoutEndTime) {
    loginTracker.failedCount    = 0;
    loginTracker.lockoutEndTime = 0;
    safeSerialPrint("[AUTH] Lockout expired. Reset counter.\n");
  }

  return false;
}

void recordFailedLogin() {
  loginTracker.failedCount++;
  loginTracker.lastAttemptTime = millis();

  safeSerialPrintf("[AUTH] Failed login attempt #%d\n", loginTracker.failedCount);

  if (loginTracker.failedCount >= MAX_LOGIN_ATTEMPTS) {
    loginTracker.lockoutEndTime =
        getRTCTime() + (loginTracker.lockoutDuration / 1000);
    safeSerialPrintf("[AUTH] ACCOUNT LOCKED for %lu seconds\n",
                     loginTracker.lockoutDuration / 1000);
    loginTracker.lockoutDuration *= LOCKOUT_MULTIPLIER;

    saveData();

    addLog("System", "Security", "System Alert",
           "Account locked due to too many failed login attempts");
  }
}

void recordSuccessLogin() {
  loginTracker.failedCount      = 0;
  loginTracker.lockoutEndTime   = 0;
  loginTracker.lockoutDuration  = INITIAL_LOCKOUT_TIME;
  safeSerialPrint("[AUTH] Login successful. Counter reset.\n");
}
