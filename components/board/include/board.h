#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "board_config.h"

/* Brings up power rails, LED, I2C bus and the battery ADC. Call once, first. */
esp_err_t board_init(void);

/* Shared I2C master bus (RTC, SHTC3, audio codec all hang off it). */
i2c_master_bus_handle_t board_i2c_bus(void);

void board_power_epd(bool on);
void board_power_audio(bool on);
void board_power_vbat(bool on);   /* false = latch off the whole board */

void board_led(bool on);

/* Battery. Returns -1.0f / 0 when the ADC is not calibrated. */
float   board_battery_voltage(void);
uint8_t board_battery_percent(void);
bool    board_usb_powered(void);

/*
 * Latch the board off. Only works on battery: on USB the rail is fed by the
 * host, so this degrades into a deep sleep that wakes on BOOT/PWR/RTC_INT.
 */
void board_power_off(void) __attribute__((noreturn));

/* Deep sleep with ext1 wake on BOOT, PWR and the RTC alarm line (any low). */
void board_deep_sleep(void) __attribute__((noreturn));

/* Why did we boot? true when we came back from deep sleep via the RTC alarm. */
bool board_woke_from_rtc_alarm(void);
