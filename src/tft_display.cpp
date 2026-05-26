/**
 * @file tft_display.cpp
 * TFT Display driver — TRUE DMA-speed SPI + Async Double Buffering
 * ILI9488 (18-bit RGB666) on SPI (HSPI/SPI3) + LVGL display driver
 *
 * ═══════════════════════════════════════════════════════════════════
 * WHY CUSTOM DMA: TFT_eSPI disables its DMA engine for ILI9488 because
 * the display needs 18-bit color (3 bytes/pixel) — the library's
 * pushPixelsDMA() only handles 16-bit (2 bytes/pixel).
 *
 * The default pushPixels() for 18-bit calls spi.transfer() THREE TIMES
 * per pixel — each one is a full SPI transaction with setup/teardown.
 * For a 320×80 buffer that's 76,800 individual SPI transactions!
 *
 * SOLUTION: Direct SPI FIFO register manipulation.
 * We pack 20 RGB666 pixels (60 bytes) into 15 SPI W-registers per burst,
 * then the SPI hardware clocks all 60 bytes out in one shot.
 * Same technique as TFT_eSPI's pushBlock() for solid 18-bit colors,
 * but extended to handle non-uniform pixel data.
 *
 * PERFORMANCE:
 *   Old: 25600 px × 3 × spi.transfer() ≈ 38 ms per flush
 *   New: 25600/20 × SPI burst(60B @ 40MHz) ≈ 15 ms per flush  (~2.5× faster)
 *   + Async: Core 0 sends SPI while Core 1 renders next frame
 * ═══════════════════════════════════════════════════════════════════
 *
 * Architecture:
 *   taskDisplay (Core 1)          flushTask (Core 0)
 *   ─────────────────────         ──────────────────
 *   lv_timer_handler()
 *     → render into buf1
 *     → flush_cb(buf1)  ──notify──→  pushPixels_rgb666(buf1)  ← SPI FIFO burst
 *     → render into buf2              ↓
 *     → [wait if buf1 busy]          flush_ready(buf1)
 *     → flush_cb(buf2)  ──notify──→  pushPixels_rgb666(buf2)  ← SPI FIFO burst
 *     → render into buf1              ↓
 *       ...                          flush_ready(buf2)
 */

#include "tft_display.h"
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <esp_heap_caps.h>

// SPI register access (also included via TFT_eSPI_ESP32_S3.h, explicit for clarity)
#include "soc/spi_reg.h"
#include "soc/gpio_struct.h"

// ============================================================================
// CONSTANTS
// ============================================================================

// ILI9488 3.5": 320×480 portrait
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 480

// Draw buffer: 80 lines per buffer (was 20)
// 320×80 = 25,600 pixels = 50 KB per buffer (×2 = 100 KB in PSRAM)
// Larger buffers → fewer flush calls → less SPI command overhead
#define DRAW_BUF_LINES 80

// Fallback buffer size if PSRAM allocation fails
#define DRAW_BUF_LINES_FALLBACK 20

// SPI FIFO capacity: 16 × 32-bit registers = 64 bytes
// 20 pixels × 3 bytes(RGB666) = 60 bytes = 15 registers per burst
#define PIXELS_PER_BURST 20

// Async flush task configuration
#define FLUSH_TASK_STACK    4096
#define FLUSH_TASK_PRIORITY 5     // Higher than display task (priority 2)
#define FLUSH_TASK_CORE     0     // Core 0 (LVGL rendering is on Core 1)

// ============================================================================
// STATIC VARIABLES
// ============================================================================

static TFT_eSPI tft = TFT_eSPI();
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;

// ── Async flush task ──
static TaskHandle_t flushTaskHandle = NULL;

// Flush request parameters (written by flush_cb, read by flush task)
// FreeRTOS notification provides memory barrier for cross-core visibility
static struct {
  lv_disp_drv_t *drv;
  lv_area_t      area;
  lv_color_t    *color_p;
} flush_req;

// Flag: true = async mode active, false = fallback to sync
static bool asyncFlushEnabled = false;

// ============================================================================
// RGB666 SPI FIFO BURST — THE CORE OPTIMIZATION
// ============================================================================
//
// SPI_PORT is defined by TFT_eSPI_ESP32_S3.h based on USE_HSPI_PORT:
//   HSPI → SPI_PORT = 3  (SPI3 peripheral on ESP32-S3)
//
// SPI W-registers (W0-W15): The SPI hardware sends bytes in little-endian
// order from each register — W0[7:0] first, then W0[15:8], ..., W0[31:24],
// then W1[7:0], etc.
//
// RGB666 packing for 4 pixels (c0,c1,c2,c3) into 3 words (12 bytes):
//   word0 = R0 | (G0<<8) | (B0<<16) | (R1<<24)
//   word1 = G1 | (B1<<8) | (R2<<16) | (G2<<24)
//   word2 = B2 | (R3<<8) | (G3<<16) | (B3<<24)
//
// This sends: R0,G0,B0, R1,G1,B1, R2,G2,B2, R3,G3,B3  ✓ ILI9488 expects
//
// 5 groups × 4 pixels = 20 pixels → 15 registers → 480 bits per SPI burst

/**
 * Push RGB565 pixels to ILI9488 as RGB666 via direct SPI FIFO register writes.
 *
 * MUST be called after tft.startWrite() + tft.setAddrWindow() and before
 * tft.endWrite(). The SPI bus is in data mode (DC high, CS low).
 *
 * @param data  Pointer to RGB565 pixel array (native uint16_t, LV_COLOR_16_SWAP=0)
 * @param len   Number of pixels to send
 */
static void IRAM_ATTR pushPixels_rgb666(const uint16_t *data, uint32_t len) {

  // ── Main loop: 20 pixels (60 bytes) per SPI FIFO burst ──
  while (len >= PIXELS_PER_BURST) {
    // Wait for previous SPI transfer to complete
    while (READ_PERI_REG(SPI_CMD_REG(SPI_PORT)) & SPI_USR);

    // Set transfer length: 20 pixels × 3 bytes × 8 bits = 480 bits
    WRITE_PERI_REG(SPI_MOSI_DLEN_REG(SPI_PORT), 479);

    // Pack 5 groups of 4 pixels into 15 SPI W-registers
    uint32_t regAddr = SPI_W0_REG(SPI_PORT);

    for (int g = 0; g < 5; g++) {
      uint16_t c0 = data[0], c1 = data[1], c2 = data[2], c3 = data[3];
      data += 4;

      // RGB565 → RGB666: extract and shift to 8-bit positions
      // R: bits[15:11] → bits[7:3]  (top 5 bits of byte)
      // G: bits[10:5]  → bits[7:2]  (top 6 bits of byte)
      // B: bits[4:0]   → bits[7:3]  (top 5 bits of byte)
      uint8_t r0 = (c0 >> 8) & 0xF8, g0 = (c0 >> 3) & 0xFC, b0 = (c0 << 3) & 0xF8;
      uint8_t r1 = (c1 >> 8) & 0xF8, g1 = (c1 >> 3) & 0xFC, b1 = (c1 << 3) & 0xF8;
      uint8_t r2 = (c2 >> 8) & 0xF8, g2 = (c2 >> 3) & 0xFC, b2 = (c2 << 3) & 0xF8;
      uint8_t r3 = (c3 >> 8) & 0xF8, g3 = (c3 >> 3) & 0xFC, b3 = (c3 << 3) & 0xF8;

      // Pack 4 pixels (12 bytes) into 3 × 32-bit words
      WRITE_PERI_REG(regAddr,     r0 | (g0 << 8) | (b0 << 16) | (r1 << 24));
      WRITE_PERI_REG(regAddr + 4, g1 | (b1 << 8) | (r2 << 16) | (g2 << 24));
      WRITE_PERI_REG(regAddr + 8, b2 | (r3 << 8) | (g3 << 16) | (b3 << 24));
      regAddr += 12;
    }

    // ESP32-S3: apply register changes before starting transfer
    SET_PERI_REG_MASK(SPI_CMD_REG(SPI_PORT), SPI_UPDATE);
    while (READ_PERI_REG(SPI_CMD_REG(SPI_PORT)) & SPI_UPDATE);
    // Fire SPI transfer
    SET_PERI_REG_MASK(SPI_CMD_REG(SPI_PORT), SPI_USR);

    len -= PIXELS_PER_BURST;
  }

  // ── Tail: remaining pixels (< 20) ──
  if (len > 0) {
    while (READ_PERI_REG(SPI_CMD_REG(SPI_PORT)) & SPI_USR);
    WRITE_PERI_REG(SPI_MOSI_DLEN_REG(SPI_PORT), (len * 24) - 1);

    uint32_t regIdx  = 0;
    uint32_t byteOff = 0;
    uint32_t word    = 0;

    for (uint32_t i = 0; i < len; i++) {
      uint16_t c = *data++;
      uint8_t rgb[3] = {
        (uint8_t)((c >> 8) & 0xF8),   // R
        (uint8_t)((c >> 3) & 0xFC),   // G
        (uint8_t)((c << 3) & 0xF8)    // B
      };
      for (int j = 0; j < 3; j++) {
        word |= ((uint32_t)rgb[j] << (byteOff * 8));
        byteOff++;
        if (byteOff == 4) {
          WRITE_PERI_REG(SPI_W0_REG(SPI_PORT) + regIdx * 4, word);
          regIdx++;
          word = 0;
          byteOff = 0;
        }
      }
    }
    // Flush any remaining partial word
    if (byteOff > 0) {
      WRITE_PERI_REG(SPI_W0_REG(SPI_PORT) + regIdx * 4, word);
    }

    SET_PERI_REG_MASK(SPI_CMD_REG(SPI_PORT), SPI_UPDATE);
    while (READ_PERI_REG(SPI_CMD_REG(SPI_PORT)) & SPI_UPDATE);
    SET_PERI_REG_MASK(SPI_CMD_REG(SPI_PORT), SPI_USR);
  }

  // Wait for the last burst to finish
  while (READ_PERI_REG(SPI_CMD_REG(SPI_PORT)) & SPI_USR);
}

// ============================================================================
// ASYNC FLUSH TASK
// ============================================================================

/**
 * FreeRTOS task: waits for flush notifications, sends pixels via
 * direct SPI FIFO register bursts, then signals LVGL buffer is free.
 *
 * Runs on Core 0 so SPI transfer overlaps with LVGL rendering on Core 1.
 */
static void flushTask(void *param) {
  for (;;) {
    // Block until flush_cb sends a notification
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // Read request (safe: FreeRTOS notification includes memory barrier)
    lv_disp_drv_t *drv     = flush_req.drv;
    int32_t x1             = flush_req.area.x1;
    int32_t y1             = flush_req.area.y1;
    uint32_t w             = flush_req.area.x2 - x1 + 1;
    uint32_t h             = flush_req.area.y2 - y1 + 1;
    uint32_t len           = w * h;
    lv_color_t *color_p    = flush_req.color_p;

    // Begin SPI transaction + set display write window
    tft.startWrite();
    tft.setAddrWindow(x1, y1, w, h);

    // Push pixels via direct SPI FIFO burst (20px/burst, ~2.5× faster)
    pushPixels_rgb666((const uint16_t *)color_p, len);

    tft.endWrite();

    // Signal LVGL: this buffer is free for rendering
    lv_disp_flush_ready(drv);
  }
}

// ============================================================================
// LVGL FLUSH CALLBACKS
// ============================================================================

/**
 * Async flush callback (used when flush task is available).
 * Posts flush request and returns immediately → LVGL renders into
 * the other buffer while SPI transfer is in progress.
 */
static void tft_flush_async(lv_disp_drv_t *drv, const lv_area_t *area,
                            lv_color_t *color_p) {
  flush_req.drv     = drv;
  flush_req.area    = *area;
  flush_req.color_p = color_p;
  // Notify flush task (includes memory barrier for multi-core safety)
  xTaskNotifyGive(flushTaskHandle);
}

/**
 * Synchronous flush callback (fallback if async task creation fails).
 * Uses direct SPI FIFO burst — still much faster than spi.transfer() per pixel.
 */
static void tft_flush_sync(lv_disp_drv_t *drv, const lv_area_t *area,
                           lv_color_t *color_p) {
  uint32_t w   = (area->x2 - area->x1 + 1);
  uint32_t h   = (area->y2 - area->y1 + 1);
  uint32_t len = w * h;

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  pushPixels_rgb666((const uint16_t *)color_p, len);
  tft.endWrite();

  lv_disp_flush_ready(drv);
}

// ============================================================================
// PUBLIC FUNCTIONS
// ============================================================================

void tft_display_init(void) {
  Serial.println("[TFT] Initializing ILI9488 (SPI FIFO Burst + Async Double Buffer)...");

  // ── 1. Backlight OFF during init ──
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  // ── 2. Hardware Reset ──
  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, HIGH);
  delay(50);
  digitalWrite(TFT_RST, LOW);
  delay(150);
  digitalWrite(TFT_RST, HIGH);
  delay(300);

  // ── 3. Initialize TFT_eSPI ──
  tft.begin();
  tft.setRotation(2); // Portrait, matching SquareLine Studio layout
  Serial.println("[TFT] SPI initialized — using direct FIFO burst for RGB666 pixels");

  // Debug: Read Display IDs
  uint8_t id1 = tft.readcommand8(0xDA);
  uint8_t id2 = tft.readcommand8(0xDB);
  uint8_t id3 = tft.readcommand8(0xDC);
  Serial.printf("[TFT] Display ID: 0x%02X 0x%02X 0x%02X\n", id1, id2, id3);
  if (id1 == 0 && id2 == 0 && id3 == 0) {
    Serial.println("[TFT] WARNING: Display ID all zeros! Check SPI wiring.");
    Serial.printf("[TFT] Pins: MOSI=%d, SCLK=%d, CS=%d, DC=%d, RST=%d, BL=%d\n",
                  TFT_MOSI, TFT_SCLK, TFT_CS, TFT_DC, TFT_RST, TFT_BL);
  }

  // ── 4. Clear screen to black, turn on backlight ──
  // (Không test màu — UI boot screen sẽ hiển thị ngay)
  tft.fillScreen(TFT_BLACK);
  digitalWrite(TFT_BL, HIGH);

  Serial.printf("[TFT] Display: %dx%d, SPI: %d MHz, RGB666 (18-bit)\n",
                SCREEN_WIDTH, SCREEN_HEIGHT, SPI_FREQUENCY / 1000000);
  Serial.printf("[TFT] FIFO burst: %d pixels/burst, SPI_PORT=%d\n",
                PIXELS_PER_BURST, SPI_PORT);

  // ── 5. Allocate LVGL draw buffers (PSRAM preferred, double buffering) ──
  size_t buf_lines  = DRAW_BUF_LINES;
  size_t buf_pixels = SCREEN_WIDTH * buf_lines;
  size_t buf_bytes  = buf_pixels * sizeof(lv_color_t);

  // Try PSRAM first (8MB available on ESP32-S3 R8N16)
  buf1 = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  buf2 = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!buf1 || !buf2) {
    // Fallback: smaller buffers in internal DMA-capable RAM
    Serial.println("[TFT] PSRAM alloc failed, fallback to internal RAM");
    if (buf1) { heap_caps_free(buf1); buf1 = NULL; }
    if (buf2) { heap_caps_free(buf2); buf2 = NULL; }
    buf_lines  = DRAW_BUF_LINES_FALLBACK;
    buf_pixels = SCREEN_WIDTH * buf_lines;
    buf_bytes  = buf_pixels * sizeof(lv_color_t);
    buf1 = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    buf2 = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  }

  if (!buf1) {
    Serial.println("[TFT] FATAL: Cannot allocate any draw buffer!");
    return;
  }

  const char *memType = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0 ? "PSRAM" : "SRAM";
  Serial.printf("[TFT] Buffers: %d lines × %d px = %d KB (%s, %s)\n",
                buf_lines, buf_pixels, buf_bytes / 1024,
                buf2 ? "double" : "single", memType);

  // ── 6. Init LVGL draw buffer ──
  lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_pixels);

  // ── 7. Create async flush task ──
  if (buf2) {
    // Double buffering available → create async flush task
    BaseType_t taskOk = xTaskCreatePinnedToCore(
        flushTask,
        "TFT_Flush",
        FLUSH_TASK_STACK,
        NULL,
        FLUSH_TASK_PRIORITY,
        &flushTaskHandle,
        FLUSH_TASK_CORE
    );
    asyncFlushEnabled = (taskOk == pdPASS && flushTaskHandle != NULL);
  }

  if (asyncFlushEnabled) {
    Serial.printf("[TFT] Async flush task on Core %d (priority %d)\n",
                  FLUSH_TASK_CORE, FLUSH_TASK_PRIORITY);
  } else {
    Serial.println("[TFT] Sync flush mode (no async task)");
  }

  // ── 8. Register LVGL display driver ──
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res      = SCREEN_WIDTH;
  disp_drv.ver_res      = SCREEN_HEIGHT;
  disp_drv.flush_cb     = asyncFlushEnabled ? tft_flush_async : tft_flush_sync;
  disp_drv.draw_buf     = &draw_buf;
  disp_drv.antialiasing = 1;
  lv_disp_drv_register(&disp_drv);

  Serial.println("[TFT] Display initialized OK — SPI FIFO Burst + Async Double Buffer");
}

void tft_set_backlight(uint8_t brightness) {
  if (brightness > 0) {
    digitalWrite(TFT_BL, HIGH);
  } else {
    digitalWrite(TFT_BL, LOW);
  }
}
