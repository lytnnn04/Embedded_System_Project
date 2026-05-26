#include "email_manager.h"
#include "app_globals.h"
#include "data_store.h"

// ============================================================================
// QUEUE HELPER
// ============================================================================

void queueEmailAlert(const char *subject, const char *message) {
  if (!smtpSettings.enabled) {
    safeSerialPrint("[Email] Drop - SMTP not enabled (configure via web UI)\n");
    return;
  }
  if (strlen(settings.notificationEmail) == 0) {
    safeSerialPrint("[Email] Drop - Notification email not set\n");
    return;
  }

  EmailAlert alert;
  strncpy(alert.subject, subject, sizeof(alert.subject) - 1);
  strncpy(alert.message, message, sizeof(alert.message) - 1);

  if (xQueueSend(emailQueue, &alert, 0) != pdTRUE)
    safeSerialPrint("[Email] Queue full, alert dropped\n");
  else
    safeSerialPrintf("[Email] Queued: %s\n", subject);
}

// ============================================================================
// SEND EMAIL
// ============================================================================

void sendEmailAlert(const char *subject, const char *message) {
  if (!systemStatus.wifiConnected) {
    safeSerialPrint("[Email] Failed - No WiFi\n");
    return;
  }
  if (!smtpSettings.enabled || strlen(smtpSettings.email) == 0 ||
      strlen(smtpSettings.password) == 0) {
    safeSerialPrint("[Email] Not configured\n");
    return;
  }

  if (xSemaphoreTake(xMutexEmail, pdMS_TO_TICKS(10000))) {
    safeSerialPrintf("[Email] Sending to %s...\n", settings.notificationEmail);

    String htmlBody =
        "<div style='border:2px solid #007bff; padding:20px; font-family:Arial;'>";
    htmlBody += "<h2 style='color:#007bff;'>🔐 SmartLock Alert</h2>";
    htmlBody += "<p><b>Subject:</b> " + String(subject) + "</p>";
    htmlBody += "<p>" + String(message) + "</p>";
    htmlBody += "<hr><p><b>Device Information:</b></p><ul>";
    htmlBody += "<li>Timestamp: " + getTimestamp() + "</li>";
    htmlBody += "<li>IP Address: " + WiFi.localIP().toString() + "</li>";
    htmlBody += "<li>WiFi SSID: " + WiFi.SSID() + "</li>";
    htmlBody += "<li>Door Status: " +
                String(systemStatus.isLocked ? "LOCKED" : "UNLOCKED") + "</li>";
    htmlBody += "</ul><hr>";
    htmlBody += "<p style='color:#666; font-size:12px;'>This is an automated "
                "message from your SmartLock system.</p></div>";

    ssl_client.stop();
    ssl_client.setInsecure();
    delay(100);

    SMTPClient smtp(ssl_client);

    if (!smtp.connect(smtpSettings.server, smtpSettings.port)) {
      safeSerialPrint("[Email] ✗ Connection failed\n");
      xSemaphoreGive(xMutexEmail);
      return;
    }
    if (!smtp.authenticate(smtpSettings.email, smtpSettings.password,
                           readymail_auth_password)) {
      safeSerialPrint("[Email] ✗ Authentication failed\n");
      smtp.stop();
      xSemaphoreGive(xMutexEmail);
      return;
    }

    SMTPMessage msg;
    msg.headers.add(rfc822_from, String(smtpSettings.senderName) + " <" +
                                     smtpSettings.email + ">");
    msg.headers.add(rfc822_to, settings.notificationEmail);
    msg.headers.add(rfc822_subject, subject);
    msg.html.body(htmlBody);
    msg.timestamp = time(nullptr);

    if (smtp.send(msg))
      safeSerialPrint("[Email] ✓ Sent successfully\n");
    else
      safeSerialPrintf("[Email] ✗ Failed to send\n");

    smtp.stop();
    xSemaphoreGive(xMutexEmail);
  } else {
    safeSerialPrint("[Email] Timeout - SMTP busy\n");
  }
}

// ============================================================================
// SMTP SETTINGS
// ============================================================================

void saveSMTPSettings() {
  JsonDocument doc;
  doc["server"]     = smtpSettings.server;
  doc["port"]       = smtpSettings.port;
  doc["email"]      = smtpSettings.email;
  doc["password"]   = smtpSettings.password;
  doc["senderName"] = smtpSettings.senderName;
  doc["enabled"]    = smtpSettings.enabled;

  String json;
  serializeJson(doc, json);
  preferences.putString("smtp", json);
  safeSerialPrint("[SMTP] Settings saved\n");
}

void loadSMTPSettings() {
  String json = preferences.getString("smtp", "{}");
  JsonDocument doc;
  deserializeJson(doc, json);

  strncpy(smtpSettings.server,     doc["server"]     | "smtp.gmail.com",
          sizeof(smtpSettings.server) - 1);
  smtpSettings.port = doc["port"] | 465;
  strncpy(smtpSettings.email,      doc["email"]      | "",
          sizeof(smtpSettings.email) - 1);
  strncpy(smtpSettings.password,   doc["password"]   | "",
          sizeof(smtpSettings.password) - 1);
  strncpy(smtpSettings.senderName, doc["senderName"] | "SmartLock",
          sizeof(smtpSettings.senderName) - 1);
  smtpSettings.enabled = doc["enabled"] | false;

  safeSerialPrintf("[SMTP] Loaded - %s\n",
                   smtpSettings.enabled ? "Enabled" : "Disabled");
}

// ============================================================================
// OTP
// ============================================================================

String generateOTP() {
  char otp[OTP_LENGTH + 1];
  for (int i = 0; i < OTP_LENGTH; i++)
    otp[i] = '0' + (rand() % 10);
  otp[OTP_LENGTH] = '\0';
  return String(otp);
}

void storeOTP(const char *code) {
  if (xSemaphoreTake(xMutexOTP, pdMS_TO_TICKS(1000))) {
    cleanupExpiredOTP();

    if (otpCount < MAX_ACTIVE_OTP) {
      strncpy(otpCodes[otpCount].code, code, OTP_LENGTH);
      otpCodes[otpCount].expiryTime = millis() + OTP_TIMEOUT;
      otpCodes[otpCount].used       = false;
      otpCount++;
      safeSerialPrintf("[OTP] Stored: %s (Expires in 5 minutes)\n", code);
    } else {
      otpCount = 0;
      strncpy(otpCodes[0].code, code, OTP_LENGTH);
      otpCodes[0].expiryTime = millis() + OTP_TIMEOUT;
      otpCodes[0].used       = false;
      otpCount = 1;
      safeSerialPrintf("[OTP] Buffer full, replaced oldest. New: %s\n", code);
    }

    xSemaphoreGive(xMutexOTP);
  }
}

bool verifyOTP(const char *otpCode) {
  if (xSemaphoreTake(xMutexOTP, pdMS_TO_TICKS(1000))) {
    for (int i = 0; i < otpCount; i++) {
      if (strcmp(otpCodes[i].code, otpCode) == 0 &&
          millis() < otpCodes[i].expiryTime && !otpCodes[i].used) {
        otpCodes[i].used = true;
        safeSerialPrintf("[OTP] Verified successfully: %s\n", otpCode);
        xSemaphoreGive(xMutexOTP);
        return true;
      }
    }
    safeSerialPrintf("[OTP] Verification failed: %s\n", otpCode);
    xSemaphoreGive(xMutexOTP);
    return false;
  }
  return false;
}

void cleanupExpiredOTP() {
  int writeIdx = 0;
  for (int i = 0; i < otpCount; i++) {
    if (millis() < otpCodes[i].expiryTime && !otpCodes[i].used) {
      if (writeIdx != i) {
        strncpy(otpCodes[writeIdx].code, otpCodes[i].code, OTP_LENGTH);
        otpCodes[writeIdx].expiryTime = otpCodes[i].expiryTime;
        otpCodes[writeIdx].used       = otpCodes[i].used;
      }
      writeIdx++;
    } else if (otpCodes[i].used &&
               millis() < otpCodes[i].expiryTime + 60000) {
      if (writeIdx != i) {
        strncpy(otpCodes[writeIdx].code, otpCodes[i].code, OTP_LENGTH);
        otpCodes[writeIdx].expiryTime = otpCodes[i].expiryTime;
        otpCodes[writeIdx].used       = otpCodes[i].used;
      }
      writeIdx++;
    }
  }
  otpCount = writeIdx;
}

// ============================================================================
// OTP EMAIL
// ============================================================================

bool sendOTPEmail(const char *email, const char *otpCode) {
  if (!systemStatus.wifiConnected) {
    safeSerialPrint("[OTP Email] Failed - No WiFi\n");
    return false;
  }
  if (!smtpSettings.enabled || strlen(smtpSettings.email) == 0 ||
      strlen(smtpSettings.password) == 0) {
    safeSerialPrint("[OTP Email] Not configured\n");
    return false;
  }

  if (xSemaphoreTake(xMutexEmail, pdMS_TO_TICKS(10000))) {
    safeSerialPrintf("[OTP Email] Sending OTP to %s...\n", email);

    String htmlBody =
        "<div style='border:2px solid #28a745; padding:20px; font-family:Arial;'>";
    htmlBody += "<h2 style='color:#28a745;'>🔐 SmartLock One-Time Password (OTP)</h2>";
    htmlBody += "<p style='font-size:14px;'>You requested a one-time password "
                "for your SmartLock system.</p>";
    htmlBody += "<div style='background:#f0f0f0; padding:20px; text-align:center; "
                "margin:20px 0; border-radius:5px;'>";
    htmlBody += "<p style='font-size:12px; color:#666; margin:0;'>Your OTP Code:</p>";
    htmlBody += "<p style='font-size:36px; font-weight:bold; color:#28a745; "
                "margin:10px 0;'>" + String(otpCode) + "</p>";
    htmlBody += "<p style='font-size:12px; color:#e74c3c;'>⏱️ This code expires "
                "in 5 minutes</p></div>";
    htmlBody += "<p style='font-size:14px;'><b>Instructions:</b></p><ul>";
    htmlBody += "<li>Enter the above code in the SmartLock system</li>";
    htmlBody += "<li>If you didn't request this code, ignore this email</li>";
    htmlBody += "<li>Never share this code with anyone</li></ul>";
    htmlBody += "<hr><p style='color:#666; font-size:12px;'>This is an "
                "automated message from your SmartLock system.</p></div>";

    ssl_client.stop();
    ssl_client.setInsecure();
    delay(100);

    SMTPClient smtp(ssl_client);

    if (!smtp.connect(smtpSettings.server, smtpSettings.port)) {
      safeSerialPrint("[OTP Email] ✗ Connection failed\n");
      xSemaphoreGive(xMutexEmail);
      return false;
    }
    if (!smtp.authenticate(smtpSettings.email, smtpSettings.password,
                           readymail_auth_password)) {
      safeSerialPrint("[OTP Email] ✗ Authentication failed\n");
      smtp.stop();
      xSemaphoreGive(xMutexEmail);
      return false;
    }

    SMTPMessage msg;
    msg.headers.add(rfc822_from, String(smtpSettings.senderName) + " <" +
                                     smtpSettings.email + ">");
    msg.headers.add(rfc822_to, email);
    msg.headers.add(rfc822_subject, "🔐 SmartLock One-Time Password");
    msg.html.body(htmlBody);
    msg.timestamp = time(nullptr);

    bool success = smtp.send(msg);
    if (success)
      safeSerialPrint("[OTP Email] ✓ Sent successfully\n");
    else
      safeSerialPrintf("[OTP Email] ✗ Failed to send\n");

    smtp.stop();
    xSemaphoreGive(xMutexEmail);
    return success;
  } else {
    safeSerialPrint("[OTP Email] Timeout - SMTP busy\n");
    return false;
  }
}
