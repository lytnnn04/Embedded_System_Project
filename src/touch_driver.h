/**
 * @file touch_driver.h
 * XPT2046 Touch driver for LVGL (via SPI2 / HSPI)
 * Dedicated SPI2 bus for touch - no sharing.
 */

#ifndef TOUCH_DRIVER_H
#define TOUCH_DRIVER_H

#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize touch hardware (XPT2046 on SPI2) and register
 * LVGL input device driver.
 * Must be called AFTER lv_init() and tft_display_init().
 *
 * @param unused  Reserved parameter (pass NULL).
 */
void touch_driver_init(SemaphoreHandle_t unused);

#ifdef __cplusplus
}
#endif

#endif /* TOUCH_DRIVER_H */
