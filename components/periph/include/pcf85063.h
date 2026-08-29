#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

/*
 * NXP PCF85063A real time clock (I2C 0x51), backed by the board's coin cell.
 * It is what keeps wall-clock time across deep sleep and battery swaps, so
 * every note gets a real timestamp even when the device has never seen a
 * network.
 */

esp_err_t pcf85063_init(void);

/* True when the oscillator-stop flag is set: the clock lost power and the
 * time it reports is meaningless until someone sets it. */
bool pcf85063_time_is_valid(void);

esp_err_t pcf85063_get_time(struct tm *out);
esp_err_t pcf85063_set_time(const struct tm *in);

/* Copy the RTC into the ESP32's own clock (and vice versa). */
esp_err_t pcf85063_to_system_time(void);
esp_err_t pcf85063_from_system_time(void);

/*
 * Countdown timer on the 1/60 Hz source: pulls INT (GPIO5) low after
 * `minutes`, which is an ext1 deep-sleep wake source. Used for the periodic
 * "wake up, try to drain the transcription queue, go back to sleep" cycle.
 */
esp_err_t pcf85063_start_countdown_minutes(uint8_t minutes);
esp_err_t pcf85063_stop_countdown(void);

/* Read and clear the timer/alarm interrupt flags. */
bool      pcf85063_irq_pending(void);
esp_err_t pcf85063_clear_irq(void);
