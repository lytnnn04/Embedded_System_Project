#pragma once
// ============================================================================
// app_globals.h - Shared types, constants, and extern declarations
// Included by all modules. Definitions live in main.cpp.
// ============================================================================

#include <Arduino.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WiFiManagerAsync.h>
#include <time.h>
#include <Wire.h>
#include <RTClib.h>
#include <esp_sleep.h>

#define ENABLE_SMTP
#define ENABLE_DEBUG
#include <ReadyMail.h>
#include <WiFiClientSecure.h>
#include <vector>

#include "tft_display.h"
#include "touch_driver.h"
#include "ui.h"
#include "wifi_config.h"
#include "ui_bridge.h"
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <Adafruit_PN532.h>

// ============================================================================
// CONFIGURATION
// ============================================================================
#define MDNS_HOSTNAME        "smartlock"
#define WIFI_AP_NAME         "SmartLock_Setup"
#define WIFI_AP_PASSWORD     "12345678"
#define WIFI_CONFIG_TIMEOUT  180

#define TOUCH_SPI_SCK   40

#define TOUCH_SPI_MISO  41

#define TOUCH_SPI_MOSI  42

#define TOUCH_CS_PIN    39

#define TOUCH_IRQ_PIN   2

#define RELAY_PIN    8
#define BUZZER_PIN   17
#define LED_GREEN    5
#define LED_RED      6
#define FACTORY_RESET_PIN 4

#define DOOR_SENSOR_PIN    15
#define DOOR_OPEN_STATE    HIGH
#define DOOR_CLOSED_STATE  LOW

#define I2C_SDA_PIN 47
#define I2C_SCL_PIN 48

#define INACTIVITY_SLEEP_TIME  300000
#define BACKLIGHT_TIMEOUT_MS    30000
#define NIGHT_SLEEP_START_HOUR     23
#define NIGHT_SLEEP_END_HOUR        6

#define BUZZER_CHANNEL    0
#define BUZZER_RESOLUTION 8

// ============================================================================
// SECURITY CONSTANTS
// ============================================================================
#define MAX_PASSWORD_ATTEMPTS  5
#define MAX_LOGIN_ATTEMPTS     5
#define MAX_TOTAL_ATTEMPTS    10
#define INITIAL_LOCKOUT_TIME  30000UL
#define AUTO_LOCK_TIME        10000
#define DOOR_ALARM_DELAY       5000
#define DOOR_DEBOUNCE_MS        100
#define DOOR_ALARM_DURATION    60000
#define DOOR_LEFT_OPEN_WARN_MS   10000   // 10s sau khi mở cửa hợp lệ mà chưa đóng
#define DOOR_LEFT_OPEN_REPEAT_MS 30000   // Nhắc lại mỗi 30s

#define PN532_IRQ_PIN   (-1)
#define PN532_RESET_PIN (-1)

#define PASSWORD_LENGTH    6
#define MAX_SCRAMBLE_LENGTH 20
#define MAX_USERS         20
#define MAX_LOGS          50

#define SESSION_TIMEOUT    300000UL
#define MAX_ACTIVE_TOKENS  10
#define MIN_REQUEST_INTERVAL 1000
#define LOCKOUT_MULTIPLIER   3

#define OTP_TIMEOUT       300000UL
#define OTP_LENGTH        6
#define MAX_ACTIVE_OTP    10

#define CARD_SCAN_TIMEOUT_MS  30000
#define FACTORY_RESET_HOLD_TIME 5000

// ============================================================================
// FSM STATES & EVENTS
// ============================================================================
typedef enum {
  STATE_IDLE,
  STATE_PASSWORD_INPUT,
  STATE_VALIDATING,
  STATE_PASSWORD_LOCKOUT,
  STATE_SYSTEM_DISABLED,
  STATE_UNLOCKED,
  STATE_LOCKED,
  STATE_ALARM
} SystemState;

typedef enum {
  EVENT_NONE,
  EVENT_KEY_PRESSED,
  EVENT_PASSWORD_COMPLETE,
  EVENT_AUTH_SUCCESS,
  EVENT_AUTH_FAILED,
  EVENT_TIMEOUT,
  EVENT_LOCKOUT_EXPIRED,
  EVENT_REMOTE_UNLOCK,
  EVENT_REMOTE_LOCK,
  EVENT_REMOTE_RESET,
  EVENT_AUTO_LOCK,
  EVENT_DOOR_OPENED,
  EVENT_DOOR_CLOSED,
  EVENT_DOOR_TAMPER,
  EVENT_ALARM_ACKNOWLEDGE
} SystemEvent;

typedef enum {
  CARD_SCAN_IDLE,
  CARD_SCAN_LOGIN,
  CARD_SCAN_ENROLL,
  CARD_SCAN_DOOR,    // TFT card button: scan to unlock door
} CardScanMode;

// ============================================================================
// DATA STRUCTURES
// ============================================================================
struct User {
  char id[16];
  char name[32];
  char role[16];          // "Admin" | "User" | "Guest"
  char cardId[16];
  bool active;
  char allowedStart[8];
  char allowedEnd[8];
  char password[PASSWORD_LENGTH + 1]; // Mật khẩu riêng mở cửa (khác admin password)
};

struct AccessLog {
  char id[16];
  char timestamp[32];
  char user[32];
  char method[16];
  char type[20];
  char details[64];
};

struct SystemStatus {
  bool isLocked;
  int batteryLevel;
  bool wifiConnected;
  char lastSync[32];
};

struct Settings {
  char notificationEmail[64];
  int screenTimeout; // backlight screen timeout in seconds
  bool soundEnabled;
};

struct SMTPSettings {
  char server[64];
  int port;
  char email[64];
  char password[64];
  char senderName[32];
  bool enabled;
};

struct SecurityCounters {
  int cardFailCount;
  int passwordFailCount;
  int totalFailCount;
  time_t lockoutEndTime;
  unsigned long currentLockoutDuration;
  bool cardFunctionLocked;
  bool systemDisabled;
};

struct AuthToken {
  String token;
  unsigned long expireTime;
  unsigned long lastActivityTime;
  bool isDefault; // Whether this session requires password change
};

struct LoginAttemptTracker {
  int failedCount;
  time_t lockoutEndTime;
  unsigned long lockoutDuration;
  unsigned long lastAttemptTime;
};

struct OTPCode {
  char code[7];
  unsigned long expiryTime;
  bool used;
};

struct EmailAlert {
  char subject[64];
  char message[256];
};

// ============================================================================
// EXTERN GLOBAL DECLARATIONS
// ============================================================================

// Web server & WiFi
extern AsyncWebServer server;
extern Preferences preferences;
extern WiFiManagerAsync wifiManagerAsync;

// FSM
extern volatile SystemState currentState;
extern volatile SystemState previousState;

// Security
extern SecurityCounters security;
extern char systemPassword[PASSWORD_LENGTH + 1];
extern bool isDefaultPassword;

// System data
extern User users[MAX_USERS];
extern int userCount;
extern AccessLog logs[MAX_LOGS];
extern int logCount;
extern int logIndex;
extern SystemStatus systemStatus;
extern Settings settings;

// Timing
extern unsigned long lastActivityTime;
extern unsigned long stateEntryTime;
extern unsigned long unlockTime;

// Auth & tokens
extern std::vector<AuthToken> activeTokens;
extern LoginAttemptTracker loginTracker;
extern unsigned long lastLoginRequestTime;

// Email / SMTP
extern SMTPSettings smtpSettings;
extern WiFiClientSecure ssl_client;
extern SemaphoreHandle_t xMutexEmail;
extern QueueHandle_t emailQueue;

// OTP
extern OTPCode otpCodes[MAX_ACTIVE_OTP];
extern int otpCount;
extern SemaphoreHandle_t xMutexOTP;

// NFC / card scan
extern Adafruit_PN532 nfc;
extern bool nfcAvailable;
extern volatile CardScanMode cardScanMode;
extern volatile bool cardScanActive;
extern unsigned long cardScanStartTime;
extern char lastScannedCardId[16];
extern volatile bool cardScanGotResult;
extern volatile bool cardScanSuccess;
extern char cardLoginToken[48];
extern SemaphoreHandle_t xMutexCard;
extern SemaphoreHandle_t xMutexI2C;

// Door sensor
extern volatile bool doorIsOpen;
extern volatile bool doorAlarmActive;
extern volatile unsigned long doorOpenTime;
extern volatile unsigned long doorAlarmStart;
extern volatile bool doorSensorEnabled;
extern volatile unsigned long doorAuthorizedOpenTime;
extern volatile bool doorLeftOpenWarning;
extern volatile unsigned long doorLeftOpenLastBeep;
extern volatile bool doorLeftOpenAlarmActive;
extern TaskHandle_t taskDoorSensorHandle;

// Factory reset
extern volatile unsigned long factoryResetPressTime;
extern volatile bool factoryResetTriggered;
extern SemaphoreHandle_t xSemaphoreFactoryReset;

// FreeRTOS handles & synchronisation primitives
extern TaskHandle_t taskFSMHandle;
extern TaskHandle_t taskWebServerHandle;
extern TaskHandle_t taskDisplayHandle;
extern TaskHandle_t taskPowerManagerHandle;
extern SemaphoreHandle_t xMutexState;
extern SemaphoreHandle_t xMutexData;
extern SemaphoreHandle_t xMutexSerial;
extern SemaphoreHandle_t xMutexLVGL;
extern QueueHandle_t eventQueue;

// RTC
extern RTC_DS1307 rtc;
extern bool rtcAvailable;

// RTC_DATA_ATTR variables (persistent across deep sleep, defined in main.cpp)
extern RTC_DATA_ATTR int          rtc_passwordFailCount;
extern RTC_DATA_ATTR int          rtc_totalFailCount;
extern RTC_DATA_ATTR time_t       rtc_lockoutEndTime;
extern RTC_DATA_ATTR unsigned long rtc_lockoutDuration;
extern RTC_DATA_ATTR bool         rtc_systemDisabled;
extern RTC_DATA_ATTR int          rtc_bootCount;
extern RTC_DATA_ATTR time_t       rtc_lastActivityTime;

// Power management
extern unsigned long lastUserActivityTime;
extern bool isInSleepMode;
extern volatile bool backlightIsOff;
extern volatile long lockoutDurationSec;

// UI state
extern volatile SystemState lastDisplayedState;
extern volatile bool systemReady;
extern int t9Mode;
extern int newPasswordStep;
extern char newPasswordTemp[PASSWORD_LENGTH + 1];
extern int changedTargetUserIdx; // -2=system password, >=0=user index
extern int selectedUserIndex;
extern char lastUnlockUserName[32];
extern volatile bool tftEnrollActive;
extern bool isSystemLockedOut;
extern volatile bool wrongPinDisplayActive;
extern volatile unsigned long wrongPinDisplayStartMs;
#define WRONG_PIN_DISPLAY_MS 3000U

// Serial helpers (defined in main.cpp)
void safeSerialPrint(const String &msg);
void safeSerialPrintf(const char *fmt, ...);
