/**
 * @file tft_display.h
 * TFT Display driver for ILI9488 + LVGL integration
 * Uses TFT_eSPI library on SPI1 (FSPI)
 */

#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize TFT display hardware (ILI9488 via TFT_eSPI)
 * and register LVGL display driver with double-buffered draw buffers.
 * Must be called AFTER lv_init().
 */
void tft_display_init(void);

/**
 * Set backlight brightness (0-255).
 * 0 = off, 255 = full brightness.
 */
void tft_set_backlight(uint8_t brightness);

#ifdef __cplusplus
}
#endif

#endif /* TFT_DISPLAY_H */
