#include "web_server.h"
#include "app_globals.h"
#include "security.h"
#include "data_store.h"
#include "email_manager.h"
#include "fsm.h"
#include "hal/door_sensor.h"
#include "hal/nfc_reader.h"
#include "rtc_manager.h"

void setupWebServer() {
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods",
                                       "GET, POST, PUT, DELETE, OPTIONS");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers",
                                       "Content-Type, Authorization");

  server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->method() == HTTP_OPTIONS) {
      request->send(200);
      return;
    }
    // Chỉ redirect captive portal khi CHƯA kết nối WiFi thật
    if (!wifiManagerAsync.isConnected() && wifiManagerAsync.isAPMode()) {
      request->redirect("http://192.168.4.1/");
      return;
    }
    if (!request->url().startsWith("/api")) {
      request->send(SPIFFS, "/index.html", "text/html; charset=utf-8");
      return;
    }
    request->send(404, "application/json", "{\"error\":\"Not Found\"}");
  });

  // ── POST /api/login ──────────────────────────────────────────────────────
  AsyncCallbackJsonWebHandler *loginHandler = new AsyncCallbackJsonWebHandler(
      "/api/login", [](AsyncWebServerRequest *request, JsonVariant &json) {
        if (millis() - lastLoginRequestTime < MIN_REQUEST_INTERVAL) {
          request->send(429, "application/json",
                        "{\"success\":false,\"error\":\"Too many requests. Please wait.\"}");
          return;
        }
        lastLoginRequestTime = millis();

        if (isLockedOut()) {
          long rem = (long)(loginTracker.lockoutEndTime - getRTCTime());
          String resp = "{\"success\":false,\"locked\":true,\"remaining\":" +
                        String(rem) + ",\"failedAttempts\":" +
                        String(loginTracker.failedCount) +
                        ",\"message\":\"Account locked due to too many failed attempts\"}";
          request->send(403, "application/json", resp);
          return;
        }

        const char *password = json.as<JsonObject>()["password"] | "";

        // Kiểm tra systemPassword trước, sau đó kiểm tra user có role Admin
        bool loginGranted = validatePassword(password);
        char loginUserName[32] = "Admin";
        bool isDefault = loginGranted && (strcmp(password, "123456") == 0);

        if (!loginGranted) {
          if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
            for (int i = 0; i < userCount && !loginGranted; i++) {
              if (!users[i].active) continue;
              if (strcmp(users[i].role, "Admin") != 0) continue;
              if (strlen(users[i].password) == 0) continue;
              if (strcmp(password, users[i].password) == 0) {
                loginGranted = true;
                isDefault    = false;
                strncpy(loginUserName, users[i].name, sizeof(loginUserName) - 1);
              }
            }
            xSemaphoreGive(xMutexData);
          }
        }

        if (loginGranted) {
          String newToken = generateRandomToken();
          if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
            if (activeTokens.size() >= MAX_ACTIVE_TOKENS) {
              int oldestIdx = 0;
              unsigned long oldestTime = activeTokens[0].lastActivityTime;
              for (size_t i = 1; i < activeTokens.size(); i++) {
                if (activeTokens[i].lastActivityTime < oldestTime) {
                  oldestTime = activeTokens[i].lastActivityTime;
                  oldestIdx  = i;
                }
              }
              activeTokens.erase(activeTokens.begin() + oldestIdx);
            }
            AuthToken authToken;
            authToken.token            = newToken;
            authToken.expireTime       = millis() + SESSION_TIMEOUT;
            authToken.lastActivityTime = millis();
            authToken.isDefault        = isDefault;
            activeTokens.push_back(authToken);
            recordSuccessLogin();
            xSemaphoreGive(xMutexData);

            String resp = "{\"success\":true,\"token\":\"" + newToken +
                          "\",\"isDefault\":" + (isDefault ? "true" : "false") +
                          ",\"failedAttempts\":0,\"locked\":false}";
            char logMsg[64];
            snprintf(logMsg, sizeof(logMsg), "Login successful (%s)", loginUserName);
            addLog(loginUserName, "Remote App", "Entry", logMsg);
            request->send(200, "application/json", resp);
          } else {
            request->send(503, "application/json",
                          "{\"success\":false,\"error\":\"Service unavailable\"}");
          }
        } else {
          recordFailedLogin();
          bool nowLocked = isLockedOut();
          long rem       = nowLocked ? (long)(loginTracker.lockoutEndTime - getRTCTime()) : 0;
          String resp = "{\"success\":false,\"locked\":" +
                        String(nowLocked ? "true" : "false") +
                        ",\"remaining\":" + String(rem) +
                        ",\"failedAttempts\":" + String(loginTracker.failedCount) +
                        ",\"message\":\"Invalid password\"}";
          addLog("Unknown", "Remote App", "Failed Attempt", "Invalid password");
          request->send(401, "application/json", resp);
        }
      });
  server.addHandler(loginHandler);

  // ── POST /logout ─────────────────────────────────────────────────────────
  server.on("/logout", HTTP_POST, [](AsyncWebServerRequest *request) {
    String token = extractTokenFromRequest(request);
    if (token.length() == 0) {
      request->send(401, "application/json", "{\"error\":\"No token provided\"}");
      return;
    }
    removeToken(token);
    addLog("Admin", "Remote App", "Exit", "Logout");
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // ── GET /auth-check ───────────────────────────────────────────────────────
  server.on("/auth-check", HTTP_GET, [](AsyncWebServerRequest *request) {
    String token  = extractTokenFromRequest(request);
    bool locked   = isLockedOut();
    long rem      = locked ? (long)(loginTracker.lockoutEndTime - getRTCTime()) : 0;
    bool isAuth   = (!locked && token.length() > 0) ? validateAndRefreshToken(token) : false;

    // Return per-token isDefault so Admin users with own passwords
    // aren't force-redirected to password change screen
    bool tokenIsDefault = false;
    if (isAuth && xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
      for (auto &t : activeTokens) {
        if (t.token == token) { tokenIsDefault = t.isDefault; break; }
      }
      xSemaphoreGive(xMutexData);
    }

    String json = "{\"locked\":" + String(locked ? "true" : "false") +
                  ",\"remaining\":" + String(rem) +
                  ",\"authenticated\":" + String(isAuth ? "true" : "false") +
                  ",\"isDefaultPassword\":" + String(tokenIsDefault ? "true" : "false") + "}";
    request->send(200, "application/json", json);
  });

  // ── GET /api/status ───────────────────────────────────────────────────────
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool isConnected = (WiFi.status() == WL_CONNECTED);
    String json = "{";
    json += "\"isLocked\":"         + String(systemStatus.isLocked        ? "true" : "false") + ",";
    json += "\"batteryLevel\":"     + String(systemStatus.batteryLevel)                        + ",";
    json += "\"wifiConnected\":"    + String(isConnected                   ? "true" : "false") + ",";
    json += "\"wifiSSID\":\""       + (isConnected ? WiFi.SSID() : "")                         + "\",";
    json += "\"lastSync\":\""       + getTimestamp()                                           + "\",";
    json += "\"stateName\":\""      + String(getStateName(currentState))                       + "\",";
    json += "\"systemDisabled\":"   + String(security.systemDisabled       ? "true" : "false") + ",";
    json += "\"failedAttempts\":"   + String(security.totalFailCount)                          + ",";
    json += "\"doorOpen\":"         + String(doorIsOpen                    ? "true" : "false") + ",";
    json += "\"doorAlarm\":"        + String(doorAlarmActive               ? "true" : "false") + ",";
    json += "\"doorSensorEnabled\":" + String(doorSensorEnabled            ? "true" : "false") + ",";
    json += "\"cardFunctionLocked\":" + String(security.cardFunctionLocked ? "true" : "false") + ",";
    json += "\"isDefaultPassword\":" + String(isDefaultPassword            ? "true" : "false") + "}";
    request->send(200, "application/json", json);
  });

  // ── POST /api/toggle-lock ─────────────────────────────────────────────────
  server.on("/api/toggle-lock", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"success\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    if (isDefaultPassword) {
      request->send(403, "application/json",
                    "{\"success\":false,\"error\":\"CHANGE_PASSWORD_REQUIRED\"}");
      return;
    }
    SystemEvent evt = systemStatus.isLocked ? EVENT_REMOTE_UNLOCK : EVENT_REMOTE_LOCK;
    xQueueSend(eventQueue, &evt, pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(200));
    String json = "{\"success\":true,\"data\":{\"isLocked\":" +
                  String(systemStatus.isLocked ? "true" : "false") + "}}";
    request->send(200, "application/json", json);
  });

  // ── POST /api/reset-system ────────────────────────────────────────────────
  server.on("/api/reset-system", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"success\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    if (isDefaultPassword) {
      request->send(403, "application/json",
                    "{\"success\":false,\"error\":\"CHANGE_PASSWORD_REQUIRED\"}");
      return;
    }
    SystemEvent evt = EVENT_REMOTE_RESET;
    xQueueSend(eventQueue, &evt, pdMS_TO_TICKS(100));
    request->send(200, "application/json", "{\"success\":true}");
  });

  // ── GET /api/logs ─────────────────────────────────────────────────────────
  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"success\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    if (isDefaultPassword) {
      request->send(403, "application/json",
                    "{\"success\":false,\"error\":\"CHANGE_PASSWORD_REQUIRED\"}");
      return;
    }
    String json = "[";
    if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
      for (int i = 0; i < logCount; i++) {
        if (i > 0) json += ",";
        json += "{\"id\":\""        + String(logs[i].id)        + "\",";
        json += "\"timestamp\":\""  + String(logs[i].timestamp) + "\",";
        json += "\"user\":\""       + String(logs[i].user)      + "\",";
        json += "\"method\":\""     + String(logs[i].method)    + "\",";
        json += "\"type\":\""       + String(logs[i].type)      + "\",";
        json += "\"details\":\""    + String(logs[i].details)   + "\"}";
      }
      xSemaphoreGive(xMutexData);
    }
    json += "]";
    request->send(200, "application/json", json);
  });

  // ── GET /api/users ────────────────────────────────────────────────────────
  server.on("/api/users", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"success\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    if (isDefaultPassword) {
      request->send(403, "application/json",
                    "{\"success\":false,\"error\":\"CHANGE_PASSWORD_REQUIRED\"}");
      return;
    }
    String json = "[";
    if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
      for (int i = 0; i < userCount; i++) {
        if (i > 0) json += ",";
        json += "{\"id\":\""           + String(users[i].id)           + "\",";
        json += "\"name\":\""          + String(users[i].name)         + "\",";
        json += "\"role\":\""          + String(users[i].role)         + "\",";
        json += "\"cardId\":\""        + String(users[i].cardId)       + "\",";
        json += "\"active\":"          + String(users[i].active ? "true" : "false") + ",";
        json += "\"hasPassword\":"      + String(strlen(users[i].password) > 0 ? "true" : "false") + ",";
        json += "\"allowedStart\":\""  + String(users[i].allowedStart) + "\",";
        json += "\"allowedEnd\":\""    + String(users[i].allowedEnd)   + "\"}";
      }
      xSemaphoreGive(xMutexData);
    }
    json += "]";
    request->send(200, "application/json", json);
  });

  // ── GET /api/settings ─────────────────────────────────────────────────────
  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"success\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    if (isDefaultPassword) {
      request->send(403, "application/json",
                    "{\"success\":false,\"error\":\"CHANGE_PASSWORD_REQUIRED\"}");
      return;
    }
    String json = "{\"notificationEmail\":\"" + String(settings.notificationEmail) +
                  "\",\"screenTimeout\":"       + String(settings.screenTimeout) +
                  ",\"soundEnabled\":"           + String(settings.soundEnabled ? "true" : "false") + "}";
    request->send(200, "application/json", json);
  });

  // ── GET /api/security ─────────────────────────────────────────────────────
  server.on("/api/security", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"error\":\"Unauthorized\"}");
      return;
    }
    String json = "{\"passwordFailCount\":"  + String(security.passwordFailCount) +
                  ",\"totalFailCount\":"      + String(security.totalFailCount) +
                  ",\"systemDisabled\":"      + String(security.systemDisabled ? "true" : "false") + "}";
    request->send(200, "application/json", json);
  });

  // ── OTP: POST /api/otp/request ────────────────────────────────────────────
  AsyncCallbackJsonWebHandler *requestOTPHandler = new AsyncCallbackJsonWebHandler(
      "/api/otp/request",
      [](AsyncWebServerRequest *request, JsonVariant &json) {
        static unsigned long lastOTPRequest = 0;
        unsigned long now = millis();
        if (now - lastOTPRequest < 10000) {
          request->send(429, "application/json",
                        "{\"success\":false,\"error\":\"Too many requests. Please wait 10 seconds.\"}");
          return;
        }
        if (strlen(settings.notificationEmail) == 0) {
          request->send(400, "application/json",
                        "{\"success\":false,\"error\":\"Admin email not configured\"}");
          return;
        }
        String otpCode = generateOTP();
        storeOTP(otpCode.c_str());
        bool sent = sendOTPEmail(settings.notificationEmail, otpCode.c_str());
        if (sent) {
          lastOTPRequest = now;
          addLog("System", "OTP", "Security", "OTP requested and sent");
          String resp = "{\"success\":true,\"message\":\"OTP sent to admin email\","
                        "\"expirySeconds\":" + String(OTP_TIMEOUT / 1000) + "}";
          request->send(200, "application/json", resp);
        } else {
          request->send(500, "application/json",
                        "{\"success\":false,\"error\":\"Failed to send OTP email\"}");
        }
      });
  server.addHandler(requestOTPHandler);

  // ── OTP: POST /api/otp/verify ─────────────────────────────────────────────
  AsyncCallbackJsonWebHandler *verifyOTPHandler = new AsyncCallbackJsonWebHandler(
      "/api/otp/verify",
      [](AsyncWebServerRequest *request, JsonVariant &json) {
        const char *otpCode = json.as<JsonObject>()["code"] | "";
        if (strlen(otpCode) != OTP_LENGTH) {
          request->send(400, "application/json",
                        "{\"success\":false,\"error\":\"Invalid OTP format\"}");
          return;
        }
        if (verifyOTP(otpCode)) {
          addLog("System", "OTP", "Security", "OTP verified successfully");
          String newToken = generateRandomToken();
          if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
            if (activeTokens.size() >= MAX_ACTIVE_TOKENS) {
              int oldestIdx = 0;
              unsigned long oldestTime = activeTokens[0].lastActivityTime;
              for (size_t i = 1; i < activeTokens.size(); i++) {
                if (activeTokens[i].lastActivityTime < oldestTime) {
                  oldestTime = activeTokens[i].lastActivityTime;
                  oldestIdx  = i;
                }
              }
              activeTokens.erase(activeTokens.begin() + oldestIdx);
            }
            AuthToken authToken;
            authToken.token            = newToken;
            authToken.expireTime       = millis() + SESSION_TIMEOUT;
            authToken.lastActivityTime = millis();
            authToken.isDefault        = false; // OTP login never requires password change
            activeTokens.push_back(authToken);
            xSemaphoreGive(xMutexData);
          }
          String resp = "{\"success\":true,\"message\":\"OTP verified\",\"token\":\"" +
                        newToken + "\"}";
          request->send(200, "application/json", resp);
        } else {
          addLog("System", "OTP", "Security", "OTP verification failed");
          request->send(401, "application/json",
                        "{\"success\":false,\"error\":\"Invalid or expired OTP\"}");
        }
      });
  server.addHandler(verifyOTPHandler);

  // ── GET /api/otp/status ───────────────────────────────────────────────────
  server.on("/api/otp/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    int activeOTPCount = 0;
    if (xSemaphoreTake(xMutexOTP, pdMS_TO_TICKS(1000))) {
      cleanupExpiredOTP();
      for (int i = 0; i < otpCount; i++) {
        if (!otpCodes[i].used && millis() < otpCodes[i].expiryTime)
          activeOTPCount++;
      }
      xSemaphoreGive(xMutexOTP);
    }
    String json = "{\"activeOTP\":" + String(activeOTPCount) +
                  ",\"maxOTP\":"     + String(MAX_ACTIVE_OTP) +
                  ",\"otpTimeoutSeconds\":" + String(OTP_TIMEOUT / 1000) + "}";
    request->send(200, "application/json", json);
  });

  // ── DOOR SENSOR APIs ──────────────────────────────────────────────────────
  server.on("/api/door-sensor", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{";
    json += "\"doorOpen\":"      + String(doorIsOpen     ? "true" : "false") + ",";
    json += "\"alarmActive\":"   + String(doorAlarmActive ? "true" : "false") + ",";
    json += "\"sensorEnabled\":" + String(doorSensorEnabled ? "true" : "false") + ",";
    if (doorOpenTime > 0 && doorIsOpen)
      json += "\"openDuration\":" + String((millis() - doorOpenTime) / 1000) + ",";
    else
      json += "\"openDuration\":0,";
    if (doorAlarmActive && doorAlarmStart > 0) {
      unsigned long elapsed   = millis() - doorAlarmStart;
      unsigned long remaining = (elapsed < DOOR_ALARM_DURATION) ?
                                (DOOR_ALARM_DURATION - elapsed) / 1000 : 0;
      json += "\"alarmRemaining\":" + String(remaining);
    } else {
      json += "\"alarmRemaining\":0";
    }
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/api/door-sensor/acknowledge", HTTP_POST,
            [](AsyncWebServerRequest *request) {
              if (!isAuthenticated(request)) {
                request->send(401, "application/json",
                              "{\"success\":false,\"error\":\"Unauthorized\"}");
                return;
              }
              if (doorAlarmActive) {
                stopDoorAlarm();
                SystemEvent evt = EVENT_ALARM_ACKNOWLEDGE;
                xQueueSend(eventQueue, &evt, pdMS_TO_TICKS(100));
                addLog("Admin", "Remote App", "Security Alert",
                       "Alarm acknowledged via dashboard");
                request->send(200, "application/json",
                              "{\"success\":true,\"message\":\"Alarm stopped\"}");
              } else {
                request->send(200, "application/json",
                              "{\"success\":true,\"message\":\"No active alarm\"}");
              }
            });

  server.on("/api/door-sensor/toggle", HTTP_POST,
            [](AsyncWebServerRequest *request) {
              if (!isAuthenticated(request)) {
                request->send(401, "application/json",
                              "{\"success\":false,\"error\":\"Unauthorized\"}");
                return;
              }
              doorSensorEnabled = !doorSensorEnabled;
              if (!doorSensorEnabled && doorAlarmActive) {
                stopDoorAlarm();
                SystemEvent evt = EVENT_ALARM_ACKNOWLEDGE;
                xQueueSend(eventQueue, &evt, pdMS_TO_TICKS(100));
              }
              addLog("Admin", "Remote App", "System Alert",
                     doorSensorEnabled ? "Door sensor enabled" : "Door sensor disabled");
              String json = "{\"success\":true,\"sensorEnabled\":" +
                            String(doorSensorEnabled ? "true" : "false") + "}";
              request->send(200, "application/json", json);
            });

  // ── POST /api/change-password ─────────────────────────────────────────────
  AsyncCallbackJsonWebHandler *changePasswordHandler = new AsyncCallbackJsonWebHandler(
      "/api/change-password",
      [](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!isAuthenticated(request)) {
          request->send(401, "application/json",
                        "{\"success\":false,\"error\":\"Unauthorized\"}");
          return;
        }
        const char *oldPassword = json.as<JsonObject>()["oldPassword"] | "";
        const char *newPassword = json.as<JsonObject>()["newPassword"] | "";

        if (!validatePassword(oldPassword)) {
          request->send(400, "application/json",
                        "{\"success\":false,\"error\":\"Old password incorrect\"}");
          return;
        }
        if (strlen(newPassword) != PASSWORD_LENGTH) {
          request->send(400, "application/json",
                        "{\"success\":false,\"error\":\"Password must be 6 digits\"}");
          return;
        }
        strncpy(systemPassword, newPassword, PASSWORD_LENGTH);
        systemPassword[PASSWORD_LENGTH] = '\0';
        isDefaultPassword = false;
        saveData();
        addLog("Admin", "Remote App", "System Alert", "Password changed");
        queueEmailAlert("🔑 Password Changed",
                        "System password has been changed successfully.");
        request->send(200, "application/json", "{\"success\":true}");
      });
  server.addHandler(changePasswordHandler);

  // ── POST /api/users (add) ─────────────────────────────────────────────────
  AsyncCallbackJsonWebHandler *addUserHandler = new AsyncCallbackJsonWebHandler(
      "/api/users", [](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!isAuthenticated(request)) {
          request->send(401, "application/json",
                        "{\"success\":false,\"error\":\"Unauthorized\"}");
          return;
        }
        if (userCount >= MAX_USERS) {
          request->send(400, "application/json",
                        "{\"success\":false,\"error\":\"Max users reached\"}");
          return;
        }
        JsonObject jsonObj = json.as<JsonObject>();
        if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
          User *newUser = &users[userCount];
          const char *id = jsonObj["id"] | "";
          if (strlen(id) == 0)
            snprintf(newUser->id, sizeof(newUser->id), "U%03d", userCount + 1);
          else
            strncpy(newUser->id, id, sizeof(newUser->id) - 1);

          strncpy(newUser->name,         jsonObj["name"]         | "Unknown", sizeof(newUser->name) - 1);
          strncpy(newUser->role,         jsonObj["role"]         | "User",    sizeof(newUser->role) - 1);
          strncpy(newUser->cardId,       jsonObj["cardId"]       | "",        sizeof(newUser->cardId) - 1);
          newUser->active = jsonObj["active"] | true;
          strncpy(newUser->allowedStart, jsonObj["allowedStart"] | "07:00",   sizeof(newUser->allowedStart) - 1);
          strncpy(newUser->allowedEnd,   jsonObj["allowedEnd"]   | "18:00",   sizeof(newUser->allowedEnd) - 1);
          // Mật khẩu riêng của user (tùy chọn, dùng để mở cửa)
          const char *userPwd = jsonObj["password"] | "";
          if (strlen(userPwd) > 0 && strlen(userPwd) <= (size_t)PASSWORD_LENGTH) {
            strncpy(newUser->password, userPwd, PASSWORD_LENGTH);
            newUser->password[PASSWORD_LENGTH] = '\0';
          } else {
            memset(newUser->password, 0, sizeof(newUser->password));
          }
          userCount++;
          xSemaphoreGive(xMutexData);

          saveData();
          addLog("Admin", "Remote App", "System Alert", "User added");

          char emailMsg[256];
          snprintf(emailMsg, sizeof(emailMsg),
                   "New user added to SmartLock system:\n\nName: %s\nRole: %s\nCard ID: %s",
                   newUser->name, newUser->role, newUser->cardId);
          queueEmailAlert("🔐 New User Added", emailMsg);

          request->send(200, "application/json", "{\"success\":true}");
        } else {
          request->send(500, "application/json",
                        "{\"success\":false,\"error\":\"Server busy\"}");
        }
      });
  server.addHandler(addUserHandler);

  // ── POST /api/users/update ────────────────────────────────────────────────
  AsyncCallbackJsonWebHandler *updateUserHandler = new AsyncCallbackJsonWebHandler(
      "/api/users/update",
      [](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!isAuthenticated(request)) {
          request->send(401, "application/json",
                        "{\"success\":false,\"error\":\"Unauthorized\"}");
          return;
        }
        JsonObject jsonObj = json.as<JsonObject>();
        const char *id = jsonObj["id"] | "";
        if (strlen(id) == 0) {
          request->send(400, "application/json",
                        "{\"success\":false,\"error\":\"Missing id\"}");
          return;
        }
        bool found = false;
        if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
          for (int i = 0; i < userCount; i++) {
            if (strcmp(users[i].id, id) == 0) {
              if (jsonObj["name"].is<const char *>())
                strncpy(users[i].name, jsonObj["name"] | users[i].name,
                        sizeof(users[i].name) - 1);
              if (jsonObj["role"].is<const char *>())
                strncpy(users[i].role, jsonObj["role"] | users[i].role,
                        sizeof(users[i].role) - 1);
              if (jsonObj["cardId"].is<const char *>())
                strncpy(users[i].cardId, jsonObj["cardId"] | "",
                        sizeof(users[i].cardId) - 1);
              if (jsonObj["active"].is<bool>())
                users[i].active = jsonObj["active"] | users[i].active;
              if (jsonObj["allowedStart"].is<const char *>())
                strncpy(users[i].allowedStart,
                        jsonObj["allowedStart"] | users[i].allowedStart,
                        sizeof(users[i].allowedStart) - 1);
              if (jsonObj["allowedEnd"].is<const char *>())
                strncpy(users[i].allowedEnd,
                        jsonObj["allowedEnd"] | users[i].allowedEnd,
                        sizeof(users[i].allowedEnd) - 1);
              found = true;
              break;
            }
          }
          xSemaphoreGive(xMutexData);
        }
        if (found) {
          saveData();
          addLog("Admin", "Remote App", "System Alert", "User updated");
          request->send(200, "application/json", "{\"success\":true}");
        } else {
          request->send(404, "application/json",
                        "{\"success\":false,\"error\":\"User not found\"}");
        }
      });
  server.addHandler(updateUserHandler);

  // ── DELETE /api/users ─────────────────────────────────────────────────────
  server.on("/api/users", HTTP_DELETE, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"success\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    if (!request->hasParam("id")) {
      request->send(400, "application/json",
                    "{\"success\":false,\"error\":\"Missing id parameter\"}");
      return;
    }
    String idToDelete = request->getParam("id")->value();
    bool found = false;

    if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
      for (int i = 0; i < userCount; i++) {
        if (String(users[i].id) == idToDelete) {
          for (int j = i; j < userCount - 1; j++) users[j] = users[j + 1];
          userCount--;
          found = true;
          break;
        }
      }
      xSemaphoreGive(xMutexData);
      if (found) {
        saveData();
        addLog("Admin", "Remote App", "System Alert", "User deleted");
        queueEmailAlert("🗑️ User Deleted",
                        "A user has been removed from SmartLock system.");
        request->send(200, "application/json", "{\"success\":true}");
      } else {
        request->send(404, "application/json",
                      "{\"success\":false,\"error\":\"User not found\"}");
      }
    } else {
      request->send(500, "application/json",
                    "{\"success\":false,\"error\":\"Server busy\"}");
    }
  });

  // ── POST /api/update-settings ─────────────────────────────────────────────
  server.on(
      "/api/update-settings", HTTP_POST, [](AsyncWebServerRequest *request) {},
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len,
         size_t index, size_t total) {
        if (!isAuthenticated(request)) {
          request->send(401, "application/json",
                        "{\"success\":false,\"error\":\"Unauthorized\"}");
          return;
        }
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, (char *)data, len);
        if (error) {
          request->send(400, "application/json",
                        "{\"success\":false,\"error\":\"Invalid JSON\"}");
          return;
        }
        if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
          if (doc["notificationEmail"].is<const char *>())
            strncpy(settings.notificationEmail, doc["notificationEmail"] | "",
                    sizeof(settings.notificationEmail) - 1);
          if (doc["screenTimeout"].is<int>())
            settings.screenTimeout = doc["screenTimeout"] | 30;
          if (doc["soundEnabled"].is<bool>())
            settings.soundEnabled = doc["soundEnabled"] | true;
          xSemaphoreGive(xMutexData);
          saveData();
          addLog("Admin", "Remote App", "System Alert", "Settings updated");
          request->send(200, "application/json", "{\"success\":true}");
        } else {
          request->send(500, "application/json",
                        "{\"success\":false,\"error\":\"Server busy\"}");
        }
      });

  // ── GET /api/wifi ─────────────────────────────────────────────────────────
  server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "{\"connected\":"  + String(WiFi.status() == WL_CONNECTED ? "true" : "false") +
                  ",\"ssid\":\""     + String(WiFi.SSID())               +
                  "\",\"ip\":\""     + WiFi.localIP().toString()          +
                  "\",\"rssi\":"     + String(WiFi.RSSI())                +
                  ",\"mac\":\""      + WiFi.macAddress()                  + "\"}";
    request->send(200, "application/json", json);
  });

  // ── POST /api/reset-wifi ──────────────────────────────────────────────────
  server.on("/api/reset-wifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"success\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    addLog("Admin", "Remote App", "System Alert", "WiFi reset requested");
    request->send(200, "application/json",
                  "{\"success\":true,\"message\":\"WiFi will reset - entering AP config mode\"}");
    resetWiFiSettings();
  });

  // ── GET /api/smtp ─────────────────────────────────────────────────────────
  server.on("/api/smtp", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"success\":false}");
      return;
    }
    String json = "{\"server\":\""     + String(smtpSettings.server)     +
                  "\",\"port\":"        + String(smtpSettings.port)       +
                  ",\"email\":\""       + String(smtpSettings.email)      +
                  "\",\"senderName\":\"" + String(smtpSettings.senderName) +
                  "\",\"enabled\":"     + String(smtpSettings.enabled ? "true" : "false") + "}";
    request->send(200, "application/json", json);
  });

  // ── POST /api/smtp ────────────────────────────────────────────────────────
  AsyncCallbackJsonWebHandler *smtpHandler = new AsyncCallbackJsonWebHandler(
      "/api/smtp", [](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!isAuthenticated(request)) {
          request->send(401, "application/json", "{\"success\":false}");
          return;
        }
        JsonObject jsonObj = json.as<JsonObject>();
        if (xSemaphoreTake(xMutexEmail, pdMS_TO_TICKS(1000))) {
          if (jsonObj["server"].is<const char *>())
            strncpy(smtpSettings.server, jsonObj["server"] | "smtp.gmail.com",
                    sizeof(smtpSettings.server) - 1);
          if (jsonObj["port"].is<int>())
            smtpSettings.port = jsonObj["port"] | 587;
          if (jsonObj["email"].is<const char *>())
            strncpy(smtpSettings.email, jsonObj["email"] | "",
                    sizeof(smtpSettings.email) - 1);
          if (jsonObj["password"].is<const char *>())
            strncpy(smtpSettings.password, jsonObj["password"] | "",
                    sizeof(smtpSettings.password) - 1);
          if (jsonObj["senderName"].is<const char *>())
            strncpy(smtpSettings.senderName, jsonObj["senderName"] | "SmartLock",
                    sizeof(smtpSettings.senderName) - 1);
          if (jsonObj["enabled"].is<bool>())
            smtpSettings.enabled = jsonObj["enabled"] | false;
          xSemaphoreGive(xMutexEmail);
          saveSMTPSettings();
          addLog("Admin", "Remote App", "System Alert", "SMTP settings updated");
          request->send(200, "application/json", "{\"success\":true}");
        } else {
          request->send(500, "application/json",
                        "{\"success\":false,\"error\":\"Server busy\"}");
        }
      });
  server.addHandler(smtpHandler);

  // ── POST /api/smtp/test ───────────────────────────────────────────────────
  server.on("/api/smtp/test", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"success\":false}");
      return;
    }
    queueEmailAlert("✅ Test Email",
                    "This is a test email from your SmartLock system.");
    request->send(200, "application/json",
                  "{\"success\":true,\"message\":\"Test email queued\"}");
  });

  // ── NFC CARD APIs ─────────────────────────────────────────────────────────

  server.on("/api/login/start-scan", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (security.cardFunctionLocked || security.systemDisabled) {
      request->send(403, "application/json",
                    "{\"success\":false,\"error\":\"Chức năng thẻ đang bị khóa\"}");
      return;
    }
    if (!nfcAvailable) {
      request->send(503, "application/json",
                    "{\"success\":false,\"error\":\"Đầu đọc thẻ không khả dụng\"}");
      return;
    }
    startCardScan(CARD_SCAN_LOGIN);
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/login/poll-scan", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (cardScanGotResult) {
      String json;
      if (cardScanSuccess)
        json = "{\"status\":\"scanned\",\"token\":\"" + String(cardLoginToken) + "\"}";
      else
        json = "{\"status\":\"failed\",\"message\":\"Thẻ không có quyền truy cập\"}";
      cardScanGotResult = false;
      request->send(200, "application/json", json);
      return;
    }
    if (!cardScanActive) {
      request->send(200, "application/json", "{\"status\":\"timeout\"}");
      return;
    }
    request->send(200, "application/json", "{\"status\":\"waiting\"}");
  });

  server.on("/api/cards/start-scan", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"success\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    if (!nfcAvailable) {
      request->send(503, "application/json",
                    "{\"success\":false,\"error\":\"Đầu đọc thẻ không khả dụng\"}");
      return;
    }
    startCardScan(CARD_SCAN_ENROLL);
    request->send(200, "application/json", "{\"success\":true}");
  });

  server.on("/api/cards/poll-scan", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"success\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    if (cardScanGotResult && cardScanMode == CARD_SCAN_ENROLL) {
      String json;
      if (cardScanSuccess)
        json = "{\"status\":\"scanned\",\"cardId\":\"" + String(lastScannedCardId) + "\"}";
      else
        json = "{\"status\":\"timeout\"}";
      cardScanGotResult = false;
      request->send(200, "application/json", json);
      return;
    }
    if (!cardScanActive) {
      request->send(200, "application/json", "{\"status\":\"timeout\"}");
      return;
    }
    request->send(200, "application/json", "{\"status\":\"waiting\"}");
  });

  server.on("/api/reset-card-lock", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!isAuthenticated(request)) {
      request->send(401, "application/json", "{\"success\":false,\"error\":\"Unauthorized\"}");
      return;
    }
    security.cardFailCount      = 0;
    security.cardFunctionLocked = false;
    saveData();
    addLog("Admin", "Remote App", "System Alert", "Card lock reset via dashboard");
    request->send(200, "application/json",
                  "{\"success\":true,\"message\":\"Card function unlocked\"}");
  });

  // ── POST /api/user/change-password ───────────────────────────────────────
  // Admin đổi mật khẩu mở cửa của một user cụ thể (khác với admin password)
  AsyncCallbackJsonWebHandler *changeUserPwdHandler = new AsyncCallbackJsonWebHandler(
      "/api/user/change-password",
      [](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!isAuthenticated(request)) {
          request->send(401, "application/json",
                        "{\"success\":false,\"error\":\"Unauthorized\"}");
          return;
        }
        JsonObject jsonObj = json.as<JsonObject>();
        const char *userId   = jsonObj["id"]       | "";
        const char *password = jsonObj["password"] | "";

        if (strlen(password) != PASSWORD_LENGTH) {
          request->send(400, "application/json",
                        "{\"success\":false,\"error\":\"Password must be exactly 6 digits\"}");
          return;
        }

        bool found = false;
        if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
          for (int i = 0; i < userCount; i++) {
            if (strcmp(users[i].id, userId) == 0) {
              strncpy(users[i].password, password, PASSWORD_LENGTH);
              users[i].password[PASSWORD_LENGTH] = '\0';
              found = true;
              break;
            }
          }
          xSemaphoreGive(xMutexData);
        }
        if (found) {
          saveData();
          addLog("Admin", "Remote App", "System Alert", "User door password changed");
          request->send(200, "application/json", "{\"success\":true}");
        } else {
          request->send(404, "application/json",
                        "{\"success\":false,\"error\":\"User not found\"}");
        }
      });
  server.addHandler(changeUserPwdHandler);

  // ── POST /api/user/set-role ───────────────────────────────────────────────
  // Nâng quyền / giáng quyền user: Admin ↔ User ↔ Guest
  AsyncCallbackJsonWebHandler *setRoleHandler = new AsyncCallbackJsonWebHandler(
      "/api/user/set-role",
      [](AsyncWebServerRequest *request, JsonVariant &json) {
        if (!isAuthenticated(request)) {
          request->send(401, "application/json",
                        "{\"success\":false,\"error\":\"Unauthorized\"}");
          return;
        }
        JsonObject jsonObj = json.as<JsonObject>();
        const char *userId = jsonObj["id"]   | "";
        const char *role   = jsonObj["role"] | "";

        if (strlen(userId) == 0) {
          request->send(400, "application/json",
                        "{\"success\":false,\"error\":\"id is required\"}");
          return;
        }
        // Validate role value
        if (strcmp(role, "Admin") != 0 && strcmp(role, "User") != 0 &&
            strcmp(role, "Family") != 0 && strcmp(role, "Guest") != 0) {
          request->send(400, "application/json",
                        "{\"success\":false,\"error\":\"Invalid role. Must be Admin, User, Family, or Guest\"}");
          return;
        }

        bool found = false;
        if (xSemaphoreTake(xMutexData, pdMS_TO_TICKS(100))) {
          for (int i = 0; i < userCount; i++) {
            if (strcmp(users[i].id, userId) == 0) {
              strncpy(users[i].role, role, sizeof(users[i].role) - 1);
              users[i].role[sizeof(users[i].role) - 1] = '\0';
              found = true;
              break;
            }
          }
          xSemaphoreGive(xMutexData);
        }
        if (found) {
          saveData();
          char logMsg[64];
          snprintf(logMsg, sizeof(logMsg), "User role changed to: %s", role);
          addLog("Admin", "Remote App", "System Alert", logMsg);
          request->send(200, "application/json", "{\"success\":true}");
        } else {
          request->send(404, "application/json",
                        "{\"success\":false,\"error\":\"User not found\"}");
        }
      });
  server.addHandler(setRoleHandler);

  // ── Serve static assets ───────────────────────────────────────────────────
  server.serveStatic("/assets/", SPIFFS, "/assets/");
}
