#pragma once

#include <stdbool.h>
#include "esp_err.h"

/*
 * Sensirion SHTC3 temperature/humidity sensor (I2C 0x70).
 * Each note is tagged with the ambient reading taken when recording started —
 * cheap context that makes the catalogue searchable by "that cold morning".
 */

esp_err_t shtc3_init(void);

/* Wakes the sensor, takes one measurement, puts it back to sleep (~1 uA).
 * Takes about 25 ms. */
esp_err_t shtc3_read(float *temp_c, float *humidity_pct);
