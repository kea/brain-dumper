#include "pcf85063.h"

#include <string.h>
#include <sys/time.h>

#include "board.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "pcf85063";

#define REG_CONTROL_1   0x00
#define REG_CONTROL_2   0x01
#define REG_SECONDS     0x04   /* bit 7 = OS, oscillator stopped */
#define REG_TIMER_VALUE 0x10
#define REG_TIMER_MODE  0x11

#define CTRL1_SR        (1 << 4)   /* software reset */
#define CTRL1_STOP      (1 << 5)
#define CTRL2_TF        (1 << 3)   /* timer flag */
#define CTRL2_AF        (1 << 6)   /* alarm flag */

#define TIMER_MODE_TIE  (1 << 0)   /* interrupt enable */
#define TIMER_MODE_TE   (1 << 1)   /* timer enable */
#define TIMER_SRC_1_60HZ (0x3 << 3)

static i2c_master_dev_handle_t s_dev;

static uint8_t bcd2dec(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static esp_err_t reg_write(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[8];
    if (len + 1 > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev, buf, len + 1, 1000);
}

static esp_err_t reg_write_byte(uint8_t reg, uint8_t val)
{
    return reg_write(reg, &val, 1);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 1000);
}

esp_err_t pcf85063_init(void)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BD_I2C_ADDR_RTC,
        .scl_speed_hz = BD_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_i2c_bus(), &cfg, &s_dev), TAG, "add dev");

    uint8_t ctrl1 = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_CONTROL_1, &ctrl1, 1), TAG, "probe");

    /* Clear STOP so the oscillator runs, keep the 24h format and the 12.5 pF
     * load capacitor setting the module's crystal expects. */
    ESP_RETURN_ON_ERROR(reg_write_byte(REG_CONTROL_1, ctrl1 & (uint8_t)~(CTRL1_STOP | CTRL1_SR)),
                        TAG, "ctrl1");

    ESP_LOGI(TAG, "ready (ctrl1=0x%02x, time %s)", ctrl1,
             pcf85063_time_is_valid() ? "valid" : "LOST");
    return ESP_OK;
}

bool pcf85063_time_is_valid(void)
{
    uint8_t sec = 0;
    if (reg_read(REG_SECONDS, &sec, 1) != ESP_OK) {
        return false;
    }
    return (sec & 0x80) == 0;
}

esp_err_t pcf85063_get_time(struct tm *out)
{
    uint8_t r[7];
    ESP_RETURN_ON_ERROR(reg_read(REG_SECONDS, r, sizeof(r)), TAG, "read time");

    memset(out, 0, sizeof(*out));
    out->tm_sec  = bcd2dec(r[0] & 0x7F);
    out->tm_min  = bcd2dec(r[1] & 0x7F);
    out->tm_hour = bcd2dec(r[2] & 0x3F);
    out->tm_mday = bcd2dec(r[3] & 0x3F);
    out->tm_wday = r[4] & 0x07;
    out->tm_mon  = bcd2dec(r[5] & 0x1F) - 1;
    out->tm_year = bcd2dec(r[6]) + 100;   /* struct tm counts from 1900 */
    out->tm_isdst = -1;
    return ESP_OK;
}

esp_err_t pcf85063_set_time(const struct tm *in)
{
    uint8_t r[7] = {
        dec2bcd((uint8_t)in->tm_sec) & 0x7F,   /* clears the OS flag */
        dec2bcd((uint8_t)in->tm_min),
        dec2bcd((uint8_t)in->tm_hour),
        dec2bcd((uint8_t)in->tm_mday),
        (uint8_t)(in->tm_wday & 0x07),
        dec2bcd((uint8_t)(in->tm_mon + 1)),
        dec2bcd((uint8_t)(in->tm_year % 100)),
    };
    return reg_write(REG_SECONDS, r, sizeof(r));
}


/* newlib does not ship timegm(); this is the standard days-from-civil
 * conversion, which is exact and avoids dragging in the TZ machinery. */
static time_t tm_to_utc_epoch(const struct tm *t)
{
    int y = t->tm_year + 1900;
    unsigned m = (unsigned)t->tm_mon + 1;
    unsigned d = (unsigned)t->tm_mday;

    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = (long)era * 146097 + (long)doe - 719468;

    return (time_t)(days * 86400L + t->tm_hour * 3600L + t->tm_min * 60L + t->tm_sec);
}

esp_err_t pcf85063_to_system_time(void)
{
    struct tm t;
    ESP_RETURN_ON_ERROR(pcf85063_get_time(&t), TAG, "get");
    if (!pcf85063_time_is_valid()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* The RTC holds UTC; TZ conversion happens on the way out via localtime(). */
    time_t epoch = tm_to_utc_epoch(&t);
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    return settimeofday(&tv, NULL) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t pcf85063_from_system_time(void)
{
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    return pcf85063_set_time(&utc);
}

esp_err_t pcf85063_start_countdown_minutes(uint8_t minutes)
{
    if (minutes == 0) {
        return pcf85063_stop_countdown();
    }
    /* Disable while reloading, otherwise the counter can latch a stale value. */
    ESP_RETURN_ON_ERROR(reg_write_byte(REG_TIMER_MODE, 0), TAG, "timer off");
    ESP_RETURN_ON_ERROR(reg_write_byte(REG_TIMER_VALUE, minutes), TAG, "timer value");
    ESP_RETURN_ON_ERROR(pcf85063_clear_irq(), TAG, "clear flags");
    return reg_write_byte(REG_TIMER_MODE, TIMER_SRC_1_60HZ | TIMER_MODE_TE | TIMER_MODE_TIE);
}

esp_err_t pcf85063_stop_countdown(void)
{
    return reg_write_byte(REG_TIMER_MODE, 0);
}

bool pcf85063_irq_pending(void)
{
    uint8_t ctrl2 = 0;
    if (reg_read(REG_CONTROL_2, &ctrl2, 1) != ESP_OK) {
        return false;
    }
    return (ctrl2 & (CTRL2_TF | CTRL2_AF)) != 0;
}

esp_err_t pcf85063_clear_irq(void)
{
    uint8_t ctrl2 = 0;
    ESP_RETURN_ON_ERROR(reg_read(REG_CONTROL_2, &ctrl2, 1), TAG, "read ctrl2");
    return reg_write_byte(REG_CONTROL_2, ctrl2 & (uint8_t)~(CTRL2_TF | CTRL2_AF));
}
