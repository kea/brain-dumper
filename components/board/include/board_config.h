/*
 * Pin map for Waveshare ESP32-S3-ePaper-1.54 V2 (non-touch)
 * MCU module: ESP32-S3-PICO-1-N8R8  (8 MB flash QIO, 8 MB PSRAM octal)
 *
 * Source: 02_Example/ESP-IDF/V2/11_FactoryProgram/components/port_bsp/epaper_config.h
 * of https://github.com/waveshareteam/ESP32-S3-ePaper-1.54
 */
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "hal/adc_types.h"

/* ---- e-Paper (SSD1681-class, 200x200 1bpp) ---------------------------- */
#define BD_EPD_SPI_HOST     SPI2_HOST
#define BD_EPD_WIDTH        200
#define BD_EPD_HEIGHT       200
#define BD_EPD_BUSY_PIN     GPIO_NUM_8
#define BD_EPD_RST_PIN      GPIO_NUM_9
#define BD_EPD_DC_PIN       GPIO_NUM_10
#define BD_EPD_CS_PIN       GPIO_NUM_11
#define BD_EPD_SCK_PIN      GPIO_NUM_12
#define BD_EPD_MOSI_PIN     GPIO_NUM_13

/* ---- Power rails (active LOW for EPD/audio, active HIGH for VBAT) ------ */
#define BD_PWR_EPD_PIN      GPIO_NUM_6
#define BD_PWR_AUDIO_PIN    GPIO_NUM_42
#define BD_PWR_VBAT_PIN     GPIO_NUM_17

/* ---- Buttons (both active LOW) ---------------------------------------- */
#define BD_BTN_BOOT_PIN     GPIO_NUM_0
#define BD_BTN_PWR_PIN      GPIO_NUM_18

/* ---- Status LED (active LOW) ------------------------------------------ */
#define BD_LED_PIN          GPIO_NUM_3

/* ---- I2C bus: PCF85063 RTC + SHTC3 + ES8311 codec ---------------------- */
#define BD_I2C_PORT         I2C_NUM_0
#define BD_I2C_SDA_PIN      GPIO_NUM_47
#define BD_I2C_SCL_PIN      GPIO_NUM_48
#define BD_I2C_FREQ_HZ      400000
#define BD_I2C_ADDR_RTC     0x51    /* PCF85063A */
#define BD_I2C_ADDR_SHTC3   0x70
#define BD_I2C_ADDR_ES8311  0x30    /* 8-bit form; esp_codec_dev shifts it down to 0x18 */

/* ---- RTC interrupt line (also a deep-sleep wake source) ---------------- */
#define BD_RTC_INT_PIN      GPIO_NUM_5

/* ---- microSD, SDMMC slot 1 in 1-bit mode ------------------------------ */
#define BD_SD_CLK_PIN       GPIO_NUM_39
#define BD_SD_CMD_PIN       GPIO_NUM_41
#define BD_SD_D0_PIN        GPIO_NUM_40
#define BD_SD_MOUNT_POINT   "/sdcard"

/* ---- Audio: ES8311 mono codec on I2S0 --------------------------------- */
#define BD_I2S_PORT         I2S_NUM_0
#define BD_I2S_MCLK_PIN     GPIO_NUM_14
#define BD_I2S_BCLK_PIN     GPIO_NUM_15
#define BD_I2S_WS_PIN       GPIO_NUM_38
#define BD_I2S_DOUT_PIN     GPIO_NUM_45   /* ESP32 -> codec (speaker) */
#define BD_I2S_DIN_PIN      GPIO_NUM_16   /* codec -> ESP32 (mic)     */
#define BD_AUDIO_PA_PIN     GPIO_NUM_46   /* speaker amplifier enable */

/* ---- Battery sense: 1:2 divider on GPIO4, gated by BD_PWR_VBAT_PIN ----- */
#define BD_BAT_ADC_UNIT     ADC_UNIT_1
#define BD_BAT_ADC_CHANNEL  ADC_CHANNEL_3   /* GPIO4 */
#define BD_BAT_DIVIDER      2.0f
#define BD_BAT_FULL_V       4.12f
#define BD_BAT_EMPTY_V      3.30f

/* ---- Recording format (what the STT backends expect) ------------------ */
#define BD_AUDIO_SAMPLE_RATE   16000
#define BD_AUDIO_BITS          16
#define BD_AUDIO_CHANNELS      1
