/**
 * @file touch_driver.cpp
 * XPT2046 Touch driver implementation for LVGL
 *
 * Sử dụng SPI2 (HSPI) - bus riêng cho touch.
 * Các chân an toàn cho ESP32-S3 với OPI PSRAM.
 *
 * Chân:
 *   T_CLK  = GPIO 40 (SPI2_SCK)
 *   T_MISO = GPIO 41 (SPI2_MISO)
 *   T_MOSI = GPIO 42 (SPI2_MOSI)
 *   T_CS   = GPIO 16
 *   T_IRQ  = GPIO 18
 */

#include "touch_driver.h"
#include "tft_display.h"
#include <SPI.h>
#include <lvgl.h>

// Biến từ main.cpp – dùng để wake backlight khi có cảm ứng
extern unsigned long lastUserActivityTime;
extern volatile bool backlightIsOff;

// ============================================================================
// PIN DEFINITIONS
// ============================================================================
#define TOUCH_SPI_SCK   40
#define TOUCH_SPI_MISO  41
#define TOUCH_SPI_MOSI  42
#define TOUCH_CS_PIN    39
#define TOUCH_IRQ_PIN   2

// XPT2046 Commands
#define XPT2046_CMD_X 0xD0  // Channel X (12-bit, diff mode)
#define XPT2046_CMD_Y 0x90  // Channel Y (12-bit, diff mode)
#define XPT2046_CMD_Z1 0xB0 // Z1 pressure
#define XPT2046_CMD_Z2 0xC0 // Z2 pressure

// ============================================================================
// CALIBRATION
// ============================================================================
// Để tìm giá trị đúng: đặt TOUCH_CALIBRATE_MODE 1, build & upload,
// mở Serial Monitor (115200), chạm vào 4 góc màn hình và ghi lại
// raw_x/raw_y tại mỗi góc:
//   Góc trên-trái  → ghi raw_x (= X_MIN hoặc X_MAX) và raw_y (= Y_MIN)
//   Góc dưới-phải → ghi raw_x ngược lại và raw_y (= Y_MAX)
// Sau đó cập nhật 4 define bên dưới và đặt TOUCH_CALIBRATE_MODE 0.
#define TOUCH_CALIBRATE_MODE 0

// Raw ADC limits của XPT2046 cho màn hình này
// Calibrated: Góc trên-trái raw=(3710,219), Góc dưới-phải raw=(215,3878)
// X: giá trị lớn = bên trái (X_MIN > X_MAX vì trục đảo ngược)
// Y: giá trị nhỏ = phía trên
#define TOUCH_X_MIN 3710
#define TOUCH_X_MAX 215
#define TOUCH_Y_MIN 219
#define TOUCH_Y_MAX 3878

// Screen dimensions (portrait)
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 480

// Touch pressure threshold (giảm = nhạy hơn, tăng = chắc chắn hơn)
#define TOUCH_Z_THRESHOLD 200

// SPI speed cho touch
#define TOUCH_SPI_FREQ 2000000 // 2 MHz

// Số mẫu lấy mỗi lần đọc (phải chẵn, >= 4)
// Thuật toán bỏ (N_SAMPLES/4) cao nhất và (N_SAMPLES/4) thấp nhất,
// lấy trung bình của phần giữa → kháng nhiễu mạnh ở mép màn.
#define TOUCH_N_SAMPLES 8

// ============================================================================
// STATIC VARIABLES
// ============================================================================
static SPIClass *touchSPI = NULL;
static lv_indev_drv_t indev_drv;

// ============================================================================
// XPT2046 FILTERED READ
// ============================================================================

/**
 * Sắp xếp mảng uint16_t tại chỗ (insertion sort, tối ưu cho mảng nhỏ).
 */
static void sort_u16(uint16_t *arr, int n) {
  for (int i = 1; i < n; i++) {
    uint16_t key = arr[i];
    int j = i - 1;
    while (j >= 0 && arr[j] > key) {
      arr[j + 1] = arr[j];
      j--;
    }
    arr[j + 1] = key;
  }
}

/**
 * Đọc tọa độ touch với median filter mạnh.
 *
 * Thuật toán:
 *   1. Kiểm tra IRQ và pressure — bỏ qua nếu không có chạm.
 *   2. Giữ CS LOW liên tục cho toàn bộ phiên đọc (ổn định nhất).
 *   3. Gửi 1 mẫu "settle" (bỏ đi) cho mỗi trục để ADC ổn định.
 *   4. Lấy TOUCH_N_SAMPLES mẫu cho X, rồi TOUCH_N_SAMPLES mẫu cho Y.
 *   5. Sắp xếp từng trục, bỏ (N/4) mẫu thấp nhất và (N/4) cao nhất,
 *      lấy trung bình phần giữa → kháng nhiễu tốt ở mép màn.
 */
static bool xpt2046_read(int16_t *x, int16_t *y) {
  // Kiểm tra IRQ pin (LOW = đang chạm)
  if (digitalRead(TOUCH_IRQ_PIN) == HIGH) {
    return false;
  }

  touchSPI->beginTransaction(SPISettings(TOUCH_SPI_FREQ, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS_PIN, LOW);

  // --- Pressure check ---
  touchSPI->transfer(XPT2046_CMD_Z1);
  uint16_t z1 = touchSPI->transfer16(0x0000) >> 3;
  touchSPI->transfer(XPT2046_CMD_Z2);
  uint16_t z2 = touchSPI->transfer16(0x0000) >> 3;
  int16_t z = (int16_t)(z1 - z2 + 4095);

  if (z < TOUCH_Z_THRESHOLD) {
    digitalWrite(TOUCH_CS_PIN, HIGH);
    touchSPI->endTransaction();
    return false;
  }

  // --- Lấy mẫu X (settle + N_SAMPLES) ---
  const int N = TOUCH_N_SAMPLES;
  const int trim = N / 4; // số mẫu bỏ ở mỗi đầu
  uint16_t xs[N], ys[N];

  // Settle X
  touchSPI->transfer(XPT2046_CMD_X);
  touchSPI->transfer16(0x0000); // bỏ mẫu đầu tiên
  for (int i = 0; i < N; i++) {
    touchSPI->transfer(XPT2046_CMD_X);
    xs[i] = touchSPI->transfer16(0x0000) >> 3;
  }

  // Settle Y
  touchSPI->transfer(XPT2046_CMD_Y);
  touchSPI->transfer16(0x0000); // bỏ mẫu đầu tiên
  for (int i = 0; i < N; i++) {
    touchSPI->transfer(XPT2046_CMD_Y);
    ys[i] = touchSPI->transfer16(0x0000) >> 3;
  }

  digitalWrite(TOUCH_CS_PIN, HIGH);
  touchSPI->endTransaction();

  // --- Median filter ---
  sort_u16(xs, N);
  sort_u16(ys, N);

  int32_t sum_x = 0, sum_y = 0;
  for (int i = trim; i < N - trim; i++) {
    sum_x += xs[i];
    sum_y += ys[i];
  }
  int kept = N - 2 * trim;
  uint16_t raw_x = (uint16_t)(sum_x / kept);
  uint16_t raw_y = (uint16_t)(sum_y / kept);

#if TOUCH_CALIBRATE_MODE
  Serial.printf("[TOUCH-CAL] raw_x=%4d  raw_y=%4d  z=%d\n", raw_x, raw_y, z);
#endif

  // --- Ánh xạ tọa độ ---
  *x = (int16_t)map(raw_x, TOUCH_X_MIN, TOUCH_X_MAX, 0, SCREEN_WIDTH - 1);
  *y = (int16_t)map(raw_y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SCREEN_HEIGHT - 1);

  // Clamp
  if (*x < 0)              *x = 0;
  if (*x >= SCREEN_WIDTH)  *x = SCREEN_WIDTH  - 1;
  if (*y < 0)              *y = 0;
  if (*y >= SCREEN_HEIGHT) *y = SCREEN_HEIGHT - 1;

  return true;
}

// ============================================================================
// LVGL INPUT DEVICE CALLBACK
// ============================================================================

/**
 * LVGL read callback cho touch input device.
 * Được gọi mỗi LV_INDEV_DEF_READ_PERIOD ms.
 */
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  int16_t x, y;
  bool touched = false;

  touched = xpt2046_read(&x, &y);

  if (touched) {
    // Nếu backlight đang tắt → bật lại và bỏ qua lần chạm này (không truyền vào UI)
    if (backlightIsOff) {
      tft_set_backlight(255);
      backlightIsOff = false;
      lastUserActivityTime = millis();
      Serial.println("[BL] Backlight ON (touch wake)");
      data->state = LV_INDEV_STATE_RELEASED; // bỏ qua chạm đầu tiên
      return;
    }
    lastUserActivityTime = millis(); // cập nhật activity mỗi lần chạm
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================

void touch_driver_init(SemaphoreHandle_t /* unused */) {
  Serial.println("[TOUCH] Initializing XPT2046 touch on SPI3 (HSPI)...");

  // 1. Cấu hình chân CS và IRQ (Bật PULLUP cho IRQ để chống nhiễu loạn cảm ứng)
  pinMode(TOUCH_CS_PIN, OUTPUT);
  digitalWrite(TOUCH_CS_PIN, HIGH); // Deselect
  pinMode(TOUCH_IRQ_PIN, INPUT_PULLUP);    

  // 2. Khởi tạo HSPI (SPI3) - BẮT BUỘC ĐỂ KHÔNG XUNG ĐỘT VỚI MÀN HÌNH
  touchSPI = new SPIClass(HSPI);
  touchSPI->begin(TOUCH_SPI_SCK, TOUCH_SPI_MISO, TOUCH_SPI_MOSI);

  Serial.printf("[TOUCH] SPI: SCK=%d, MISO=%d, MOSI=%d\n", TOUCH_SPI_SCK,
                TOUCH_SPI_MISO, TOUCH_SPI_MOSI);
  Serial.printf("[TOUCH] CS=%d, IRQ=%d\n", TOUCH_CS_PIN, TOUCH_IRQ_PIN);

  // 3. Đăng ký LVGL input device driver
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read_cb;
  lv_indev_drv_register(&indev_drv);

  Serial.println("[TOUCH] LVGL input device registered OK");
}
