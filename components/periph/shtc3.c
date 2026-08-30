#include "shtc3.h"

#include <math.h>

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "shtc3";

#define CMD_WAKEUP      0x3517
#define CMD_SLEEP       0xB098
#define CMD_SOFT_RESET  0x805D
#define CMD_READ_ID     0xEFC8
#define CMD_MEAS_T_RH   0x7866   /* normal mode, T first, no clock stretching */

#define CRC_POLYNOMIAL  0x131

/*
 * The sensor sits next to the PMIC and the e-paper rail, so it reads warm.
 * Waveshare's factory firmware subtracts a flat 4 C; keep that as the default
 * and expose it here so it can be trimmed against a reference thermometer.
 */
#define SELF_HEATING_OFFSET_C 4.0f

static i2c_master_dev_handle_t s_dev;

/* Last good measurement. The read sequence is wake / measure / sleep across
 * three I2C transactions, so it cannot be run from two tasks at once; anything
 * that only wants a recent number - the web server's status page - reads this
 * instead of touching the bus. */
static float s_last_t = NAN;
static float s_last_rh = NAN;

static esp_err_t send_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 1000);
}

static bool crc_ok(const uint8_t *data, uint8_t len, uint8_t checksum)
{
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ CRC_POLYNOMIAL) : (uint8_t)(crc << 1);
        }
    }
    return crc == checksum;
}

static esp_err_t wakeup(void)
{
    esp_err_t err = send_cmd(CMD_WAKEUP);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2));   /* datasheet: 240 us */
    }
    return err;
}

esp_err_t shtc3_init(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BD_I2C_ADDR_SHTC3,
        .scl_speed_hz = BD_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_i2c_bus(), &cfg, &s_dev), TAG, "add dev");

    ESP_RETURN_ON_ERROR(wakeup(), TAG, "wakeup");
    ESP_RETURN_ON_ERROR(send_cmd(CMD_SOFT_RESET), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_RETURN_ON_ERROR(wakeup(), TAG, "wakeup 2");
    uint8_t cmd[2] = { CMD_READ_ID >> 8, CMD_READ_ID & 0xFF };
    uint8_t id[3] = { 0 };
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_dev, cmd, 2, id, 3, 1000), TAG, "read id");
    ESP_RETURN_ON_FALSE(crc_ok(id, 2, id[2]), ESP_ERR_INVALID_CRC, TAG, "id crc");

    /* Bits 5:0 of the ID are 0b000111 on every SHTC3. */
    uint16_t raw_id = (uint16_t)((id[0] << 8) | id[1]);
    ESP_RETURN_ON_FALSE((raw_id & 0x3F) == 0x07, ESP_ERR_NOT_FOUND, TAG,
                        "unexpected id 0x%04x", raw_id);

    send_cmd(CMD_SLEEP);
    ESP_LOGI(TAG, "ready (id=0x%04x)", raw_id);
    return ESP_OK;
}

esp_err_t shtc3_read(float *temp_c, float *humidity_pct)
{
    ESP_RETURN_ON_ERROR(wakeup(), TAG, "wakeup");

    esp_err_t err = send_cmd(CMD_MEAS_T_RH);
    if (err != ESP_OK) {
        send_cmd(CMD_SLEEP);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(15));   /* normal mode conversion: 12.1 ms max */

    uint8_t raw[6] = { 0 };
    err = i2c_master_receive(s_dev, raw, sizeof(raw), 1000);
    send_cmd(CMD_SLEEP);
    ESP_RETURN_ON_ERROR(err, TAG, "read");

    if (!crc_ok(&raw[0], 2, raw[2]) || !crc_ok(&raw[3], 2, raw[5])) {
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t rt = (uint16_t)((raw[0] << 8) | raw[1]);
    uint16_t rh = (uint16_t)((raw[3] << 8) | raw[4]);

    s_last_t  = 175.0f * rt / 65536.0f - 45.0f - SELF_HEATING_OFFSET_C;
    s_last_rh = 100.0f * rh / 65536.0f;

    if (temp_c) {
        *temp_c = s_last_t;
    }
    if (humidity_pct) {
        *humidity_pct = s_last_rh;
    }
    return ESP_OK;
}

bool shtc3_last(float *temp_c, float *humidity_pct)
{
    if (temp_c) {
        *temp_c = s_last_t;
    }
    if (humidity_pct) {
        *humidity_pct = s_last_rh;
    }
    return !isnan(s_last_t);
}
