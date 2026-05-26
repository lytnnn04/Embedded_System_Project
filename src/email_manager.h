#pragma once
// Email notifications, SMTP settings, and OTP functions
#include <Arduino.h>

void queueEmailAlert(const char *subject, const char *message);
void sendEmailAlert(const char *subject, const char *message);
void saveSMTPSettings();
void loadSMTPSettings();
bool sendOTPEmail(const char *email, const char *otpCode);

String generateOTP();
void storeOTP(const char *code);
bool verifyOTP(const char *otpCode);
void cleanupExpiredOTP();
