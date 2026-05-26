#include "app_globals.h"
#include "hal/buzzer.h"
#include "hal/door_sensor.h"
#include "hal/nfc_reader.h"
#include "rtc_manager.h"
#include "power_manager.h"
#include "fsm.h"
#include "security.h"
#include "email_manager.h"
#include "data_store.h"
#include "web_server.h"
#include "ui_handler.h"
#include "tasks.h"

// ============================================================================
// GLOBAL VARIABLE DEFINITIONS
// ============================================================================

AsyncWebServer server(80);
Preferences preferences;
WiFiManagerAsync wifiManagerAsync(&server);

volatile SystemState currentState  = STATE_IDLE;
volatile SystemState previousState = STATE_IDLE;

SecurityCounters security = {0, 0, 0, 0, INITIAL_LOCKOUT_TIME, false, false};
char systemPassword[PASSWORD_LENGTH + 1] = "123456";
bool isDefaultPassword = true;

User users[MAX_USERS];
int userCount = 0;
AccessLog logs[MAX_LOGS];
int logCount = 0;
int logIndex = 0;
SystemStatus systemStatus = {true, 100, false, ""};
Settings settings = {"", 10, true};

unsigned long lastActivityTime = 0;
unsigned long stateEntryTime   = 0;
unsigned long unlockTime       = 0;

std::vector<AuthToken> activeTokens;

LoginAttemptTracker loginTracker = {0, 0, INITIAL_LOCKOUT_TIME, 0};
unsigned long lastLoginRequestTime = 0;

SMTPSettings smtpSettings = {"smtp.gmail.com", 465, "", "", "SmartLock", false};
WiFiClientSecure ssl_client;
SemaphoreHandle_t xMutexEmail = NULL;
QueueHandle_t emailQueue = NULL;

OTPCode otpCodes[MAX_ACTIVE_OTP];
int otpCount = 0;
SemaphoreHandle_t xMutexOTP  = NULL;
SemaphoreHandle_t xMutexCard = NULL;
SemaphoreHandle_t xMutexI2C  = NULL;

volatile bool         doorIsOpen              = false;
volatile bool         doorAlarmActive         = false;
volatile unsigned long doorOpenTime           = 0;
volatile unsigned long doorAlarmStart         = 0;
volatile bool         doorSensorEnabled       = true;
volatile unsigned long doorAuthorizedOpenTime = 0;
volatile bool         doorLeftOpenWarning     = false;
volatile unsigned long doorLeftOpenLastBeep   = 0;
volatile bool         doorLeftOpenAlarmActive = false;
TaskHandle_t taskDoorSensorHandle = NULL;

volatile unsigned long factoryResetPressTime = 0;
volatile bool          factoryResetTriggered = false;
SemaphoreHandle_t xSemaphoreFactoryReset = NULL;

TaskHandle_t taskFSMHandle          = NULL;
TaskHandle_t taskWebServerHandle    = NULL;
TaskHandle_t taskDisplayHandle      = NULL;
TaskHandle_t taskPowerManagerHandle = NULL;

SemaphoreHandle_t xMutexState  = NULL;
SemaphoreHandle_t xMutexData   = NULL;
SemaphoreHandle_t xMutexSerial = NULL;
SemaphoreHandle_t xMutexLVGL   = NULL;
QueueHandle_t eventQueue       = NULL;

RTC_DS1307 rtc;
bool rtcAvailable = false;

unsigned long lastUserActivityTime = 0;
bool          isInSleepMode        = false;
volatile bool backlightIsOff       = false;
volatile long lockoutDurationSec   = 0;

Adafruit_PN532 nfc(PN532_IRQ_PIN, PN532_RESET_PIN);
bool nfcAvailable = false;

volatile CardScanMode cardScanMode   = CARD_SCAN_IDLE;
volatile bool         cardScanActive = false;
unsigned long cardScanStartTime      = 0;
char  lastScannedCardId[16] = "";
volatile bool cardScanGotResult = false;
volatile bool cardScanSuccess   = false;
char  cardLoginToken[48] = "";

bool isSystemLockedOut = false;
volatile SystemState lastDisplayedState = STATE_IDLE;
volatile bool systemReady = false;

int  t9Mode               = 0;
int  newPasswordStep      = 0;
char newPasswordTemp[PASSWORD_LENGTH + 1] = "";
int  changedTargetUserIdx = -1; // -2=system password, >=0=user index

int  selectedUserIndex    = 0;
char lastUnlockUserName[32] = "User";
volatile bool tftEnrollActive = false;
volatile bool wrongPinDisplayActive = false;
volatile unsigned long wrongPinDisplayStartMs = 0;

// ============================================================================
// RTC_DATA_ATTR — persistent across deep sleep
// ============================================================================
RTC_DATA_ATTR int          rtc_passwordFailCount = 0;
RTC_DATA_ATTR int          rtc_totalFailCount    = 0;
RTC_DATA_ATTR time_t       rtc_lockoutEndTime    = 0;
RTC_DATA_ATTR unsigned long rtc_lockoutDuration  = INITIAL_LOCKOUT_TIME;
RTC_DATA_ATTR bool         rtc_systemDisabled    = false;
RTC_DATA_ATTR int          rtc_bootCount         = 0;
RTC_DATA_ATTR time_t       rtc_lastActivityTime  = 0;

// ============================================================================
// SERIAL HELPERS
// ============================================================================

void safeSerialPrint(const String &msg) {
  if (xSemaphoreTake(xMutexSerial, pdMS_TO_TICKS(100))) {
    Serial.print(msg);
    xSemaphoreGive(xMutexSerial);
  }
}

void safeSerialPrintf(const char *format, ...) {
  if (xSemaphoreTake(xMutexSerial, pdMS_TO_TICKS(100))) {
    va_list args;
    va_start(args, format);
    char buffer[256];
    vsnprintf(buffer, sizeof(buffer), format, args);
    Serial.print(buffer);
    va_end(args);
    xSemaphoreGive(xMutexSerial);
  }
}

// ============================================================================
// FACTORY RESET ISR
// ============================================================================

void IRAM_ATTR factoryResetISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (digitalRead(FACTORY_RESET_PIN) == LOW) {
    factoryResetPressTime = millis();
    xSemaphoreGiveFromISR(xSemaphoreFactoryReset, &xHigherPriorityTaskWoken);
  }
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// ============================================================================
// SETUP
// ============================================================================

void setup() {
  Serial.begin(115200);
  unsigned long t_boot = millis();
  delay(500);
  Serial.println("\n=== HE THONG KHOA CUA THONG MINH IoT ===");
  Serial.flush();

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_RED,    OUTPUT);
  pinMode(RELAY_PIN,  OUTPUT);
  pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_GREEN,  LOW);
  digitalWrite(LED_RED,    LOW);
  digitalWrite(RELAY_PIN,  HIGH);

  Serial.println("[TEST] Blinking LED_GREEN (GPIO5) and LED_RED (GPIO6)...");
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_GREEN, HIGH); delay(200); digitalWrite(LED_GREEN, LOW);
    digitalWrite(LED_RED,   HIGH); delay(200); digitalWrite(LED_RED,   LOW);
  }
  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_GREEN, LOW);

  ledcSetup(BUZZER_CHANNEL, 2000, BUZZER_RESOLUTION);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcWrite(BUZZER_CHANNEL, 0);
  Serial.println("[OK] LEDC (Buzzer) initialized");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Serial.printf("[I2C] SDA=%d, SCL=%d\n", I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setTimeOut(1000);
  if (rtc.begin()) {
    rtcAvailable = true;
    rtc_bootCount++;
    Serial.printf("[OK] RTC DS1307 initialized (Boot count: %d)\n", rtc_bootCount);
    if (!rtc.isrunning()) {
      Serial.println("[WARN] RTC not running! Setting to compile time...");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    DateTime now = rtc.now();
    Serial.printf("[RTC] Current time: %04d-%02d-%02d %02d:%02d:%02d\n",
                  now.year(), now.month(), now.day(),
                  now.hour(), now.minute(), now.second());
    if (rtc_bootCount > 1) {
      Serial.println("[RTC] Restoring security state from RTC memory...");
      restoreSecurityFromRTC();
    }
  } else {
    rtcAvailable = false;
    Serial.println("[WARN] RTC DS1307 not found! Using NTP only.");
  }

  Serial.println("[...] Initializing LVGL...");
  lv_init();
  Serial.println("[OK] LVGL initialized");

  Serial.println("[...] Initializing TFT Display...");
  tft_display_init();
  Serial.println("[OK] TFT Display initialized");

  if (!SPIFFS.begin(true)) {
    Serial.println("[ERROR] SPIFFS Mount Failed!");
  } else {
    Serial.println("[OK] SPIFFS initialized");
  }
  preferences.begin("smartlock", false);
  loadData();

  Serial.println("[...] Starting WiFi Manager (Async)...");
  wifiManagerAsync.setAPCredentials(WIFI_AP_NAME, WIFI_AP_PASSWORD);
  wifiManagerAsync.setConnectTimeout(15000);
  wifiManagerAsync.setDebug(true);

  wifiManagerAsync.onConnected([]() {
    systemStatus.wifiConnected = true;
    WiFi.setSleep(false);
    if (MDNS.begin(MDNS_HOSTNAME)) {
      Serial.printf("[OK] mDNS started: http://%s.local\n", MDNS_HOSTNAME);
      MDNS.addService("http", "tcp", 80);
      MDNS.addServiceTxt("http", "tcp", "board", "ESP32-S3");
      MDNS.addServiceTxt("http", "tcp", "app", "SmartLock Dashboard");
    } else {
      Serial.println("[WARN] mDNS failed to start");
    }
    configTime(7 * 3600, 0, "pool.ntp.org");
    if (rtcAvailable) {
      struct tm timeinfo;
      int retries = 0;
      while (!getLocalTime(&timeinfo, 1000) && retries < 10) retries++;
      if (retries < 10) {
        time_t now; time(&now);
        rtc.adjust(DateTime(now));
        Serial.printf("[RTC] Synced with NTP: %04d-%02d-%02d %02d:%02d:%02d\n",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1,
                      timeinfo.tm_mday, timeinfo.tm_hour,
                      timeinfo.tm_min, timeinfo.tm_sec);
      }
    }
      update_wifi_ip(WiFi.localIP().toString().c_str());
    });

    wifiManagerAsync.onAPMode([]() {
      Serial.printf("[INFO] AP Mode: %s (Pass: %s)\n", WIFI_AP_NAME, WIFI_AP_PASSWORD);
      Serial.printf("[INFO] AP IP: %s\n", WiFi.softAPIP().toString().c_str());  
    });

    wifiManagerAsync.onDisconnected([]() {
      systemStatus.wifiConnected = false;
      Serial.println("[WARN] WiFi disconnected!");
    });

  xMutexState            = xSemaphoreCreateMutex();
  xMutexData             = xSemaphoreCreateMutex();
  xMutexSerial           = xSemaphoreCreateMutex();
  xMutexEmail            = xSemaphoreCreateMutex();
  xMutexOTP              = xSemaphoreCreateMutex();
  xMutexLVGL             = xSemaphoreCreateMutex();
  xMutexCard             = xSemaphoreCreateMutex();
  xMutexI2C              = xSemaphoreCreateMutex();
  xSemaphoreFactoryReset = xSemaphoreCreateBinary();
  eventQueue             = xQueueCreate(10, sizeof(SystemEvent));
  emailQueue             = xQueueCreate(5,  sizeof(EmailAlert));

  Serial.println("[...] Initializing PN532 NFC (I2C)...");
  if (initPN532()) {
    nfcAvailable = true;
    Serial.println("[OK] PN532 NFC ready (addr 0x24)");
  } else {
    nfcAvailable = false;
    Serial.println("[WARN] PN532 not found - card features disabled");
  }

  touch_driver_init(NULL);
  Serial.println("[OK] Touch driver initialized");
  ui_init();
  Serial.println("[OK] LVGL UI initialized");

  loadSMTPSettings();
  setupWebServer();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  delay(100);
  wifiManagerAsync.begin();

  attachInterrupt(digitalPinToInterrupt(FACTORY_RESET_PIN), factoryResetISR, FALLING);
  Serial.println("[OK] Factory reset interrupt attached");

  doorSensorInit();
  Serial.println("[OK] Door sensor MC38 initialized");

  xTaskCreatePinnedToCore(taskFSM,          "FSM",          6144, NULL, 3, &taskFSMHandle,          0);
  xTaskCreatePinnedToCore(taskWiFiMonitor,  "WiFi",         4096, NULL, 1, &taskWebServerHandle,    0);
  xTaskCreatePinnedToCore(taskDisplay,      "Display",      8192, NULL, 2, &taskDisplayHandle,      1);
  xTaskCreatePinnedToCore(taskEmail,        "Email",        12288, NULL, 1, NULL,                    1);
  xTaskCreatePinnedToCore(taskFactoryReset, "FactoryReset", 4096, NULL, 4, NULL,                    0);
  xTaskCreatePinnedToCore(taskPowerManager, "PowerMgr",     3072, NULL, 1, &taskPowerManagerHandle, 0);
  xTaskCreatePinnedToCore(taskDoorSensor,   "DoorSensor",   4096, NULL, 3, &taskDoorSensorHandle,   0);
  xTaskCreatePinnedToCore(taskCardReader,   "CardRdr",      4096, NULL, 3, NULL,                    0);
  Serial.println("[OK] FreeRTOS tasks created");

  lastUserActivityTime = millis();
  if (security.systemDisabled) {
    currentState = STATE_SYSTEM_DISABLED;
    Serial.println("[Security] Restored: SYSTEM_DISABLED");
  } else if (security.lockoutEndTime > 0 && security.lockoutEndTime > getRTCTime()) {
    currentState = STATE_PASSWORD_LOCKOUT;
    lockoutDurationSec = (long)(security.currentLockoutDuration / 1000);
    Serial.printf("[Security] Restored: PASSWORD_LOCKOUT, %ld sec remaining\n",
                  (long)(security.lockoutEndTime - getRTCTime()));
  } else {
    currentState = STATE_LOCKED;
  }
  stateEntryTime = millis();

  systemReady = true;
  Serial.printf("[TIMING] Boot time: %lu ms\n", millis() - t_boot);
  Serial.println("[OK] System ready - boot screen will now transition");
}

void loop() {
  static unsigned long lastCleanup = 0;
  if (millis() - lastCleanup > 10000) {
    cleanupExpiredTokens();
    lastCleanup = millis();
  }

  vTaskDelay(pdMS_TO_TICKS(100));
}