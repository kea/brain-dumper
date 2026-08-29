#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * SSD1681-class 200x200 1bpp e-paper (Waveshare 1.54" V2, non-touch).
 *
 * Framebuffer layout: row-major, 25 bytes per row, MSB = leftmost pixel,
 * bit set = white. This is byte-for-byte identical to LVGL's I1 draw buffer,
 * so epd_blit() is a memcpy.
 */

#define EPD_W        200
#define EPD_H        200
#define EPD_STRIDE   (EPD_W / 8)          /* 25 */
#define EPD_BUF_SIZE (EPD_STRIDE * EPD_H) /* 5000 */

typedef enum {
    EPD_REFRESH_PARTIAL,  /* ~400 ms, leaves faint ghosting */
    EPD_REFRESH_FULL,     /* ~2 s, flashes, clears ghosting */
} epd_refresh_t;

/* Powers the panel and initialises SPI. Panel is left in partial-refresh mode. */
esp_err_t epd_init(void);

/* Copy a full 5000-byte 1bpp frame into the driver's framebuffer. */
void epd_blit(const uint8_t *frame);

/* Fill the framebuffer white without touching the panel. */
void epd_clear(void);

/* Push the framebuffer to the panel.
 *
 * The driver counts partial refreshes and forces a full one every
 * EPD_FULL_REFRESH_EVERY frames, so callers can always ask for PARTIAL and
 * still get periodic de-ghosting. */
#define EPD_FULL_REFRESH_EVERY 12
void epd_flush(epd_refresh_t mode);

/* Ask for a full refresh on the next epd_flush(), e.g. on a screen change. */
void epd_request_full_refresh(void);

/* Deep-sleep the panel controller (~1 uA). Any epd_flush() after this needs
 * epd_wake() first. */
void epd_sleep(void);
void epd_wake(void);
