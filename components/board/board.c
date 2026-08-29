#include <string.h>

#include "board.h"

#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c_bus;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_adc_cali;
static bool s_adc_ready;

esp_err_t board_init(void)
{
    /* If the previous run ended in USB transfer mode - or crashed there - the
     * shared USB PHY may still be pointed at the OTG core, and that survives a
     * software reset. Claim it back for the Serial/JTAG console: on a normal
     * boot this is the power-on state already, so it costs nothing. */
    usb_serial_jtag_ll_phy_enable_external(false);

    /* Power rails. EPD and audio enables are active low, VBAT latch is active
     * high: driving it low is how the board cuts its own power. */
    gpio_config_t out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BD_PWR_EPD_PIN) | (1ULL << BD_PWR_AUDIO_PIN) |
                        (1ULL << BD_PWR_VBAT_PIN) | (1ULL << BD_LED_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    /* Deep sleep leaves these pads held; until the hold is released
     * gpio_set_level() on them is silently ignored. */
    gpio_deep_sleep_hold_dis();
    rtc_gpio_hold_dis(BD_PWR_VBAT_PIN);
    gpio_hold_dis(BD_PWR_EPD_PIN);
    gpio_hold_dis(BD_PWR_AUDIO_PIN);
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "power gpio");

    board_power_vbat(true);      /* latch ourselves on before anything else */
    board_power_epd(true);
    board_power_audio(true);
    board_led(false);

    /* RTC alarm line: open-drain, pulls low on alarm. */
    gpio_config_t in = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BD_RTC_INT_PIN,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "rtc int gpio");

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = BD_I2C_PORT,
        .sda_io_num = BD_I2C_SDA_PIN,
        .scl_io_num = BD_I2C_SCL_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c bus");

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BD_BAT_ADC_UNIT };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) == ESP_OK) {
        adc_oneshot_chan_cfg_t chan_cfg = {
            .bitwidth = ADC_BITWIDTH_12,
            .atten = ADC_ATTEN_DB_12,
        };
        adc_oneshot_config_channel(s_adc, BD_BAT_ADC_CHANNEL, &chan_cfg);

        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = BD_BAT_ADC_UNIT,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        s_adc_ready = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK;
        if (!s_adc_ready) {
            ESP_LOGW(TAG, "ADC calibration unavailable, battery readings disabled");
        }
    }

    ESP_LOGI(TAG, "board up (i2c=%d/%d)", BD_I2C_SDA_PIN, BD_I2C_SCL_PIN);
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void) { return s_i2c_bus; }

void board_power_epd(bool on)   { gpio_set_level(BD_PWR_EPD_PIN, on ? 0 : 1); }
void board_power_audio(bool on) { gpio_set_level(BD_PWR_AUDIO_PIN, on ? 0 : 1); }
void board_power_vbat(bool on)  { gpio_set_level(BD_PWR_VBAT_PIN, on ? 1 : 0); }
void board_led(bool on)         { gpio_set_level(BD_LED_PIN, on ? 0 : 1); }

float board_battery_voltage(void)
{
    if (!s_adc_ready) {
        return -1.0f;
    }
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, BD_BAT_ADC_CHANNEL, &raw) != ESP_OK) {
        return -1.0f;
    }
    if (adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) != ESP_OK) {
        return -1.0f;
    }
    return mv * 0.001f * BD_BAT_DIVIDER;
}

uint8_t board_battery_percent(void)
{
    float v = board_battery_voltage();
    if (v < 0.0f) {
        return 0;
    }
    if (v <= BD_BAT_EMPTY_V) {
        return 0;
    }
    if (v >= BD_BAT_FULL_V) {
        return 100;
    }
    return (uint8_t)((v - BD_BAT_EMPTY_V) / (BD_BAT_FULL_V - BD_BAT_EMPTY_V) * 100.0f);
}

bool board_usb_powered(void)
{
    /* No dedicated VBUS sense line on this board, so two partial signals stand
     * in for one. The USB Serial/JTAG counts SOF packets, which only a host
     * sends: that catches the cable-to-a-PC case exactly, and says nothing
     * about a charger. The battery only sits above the charge termination
     * voltage while something is feeding the cell, which catches the rest. */
    if (usb_serial_jtag_is_connected()) {
        return true;
    }
    float v = board_battery_voltage();
    return v > (BD_BAT_FULL_V + 0.05f);
}

static void enter_sleep(void) __attribute__((noreturn));

static void enter_sleep(void)
{
    esp_sleep_pd_config(ESP_PD_DOMAIN_MAX, ESP_PD_OPTION_AUTO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

    const uint64_t mask = (1ULL << BD_BTN_BOOT_PIN) |
                          (1ULL << BD_BTN_PWR_PIN) |
                          (1ULL << BD_RTC_INT_PIN);
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW));

    /* The RTC alarm line has no external pull-up strong enough to survive the
     * digital domain being powered down, so keep the RTC-domain one enabled. */
    rtc_gpio_pulldown_dis(BD_RTC_INT_PIN);
    rtc_gpio_pullup_en(BD_RTC_INT_PIN);

    /* Hold the VBAT latch so we do not power ourselves off while asleep, and
     * the two rail enables so they stay off: an unheld pad goes high-Z the
     * moment the digital domain is powered down, and a floating enable lets
     * the panel supply drift back up under a COG that is not driving it. */
    rtc_gpio_hold_en(BD_PWR_VBAT_PIN);
    gpio_hold_en(BD_PWR_EPD_PIN);
    gpio_hold_en(BD_PWR_AUDIO_PIN);
    gpio_deep_sleep_hold_en();

    esp_deep_sleep_start();
}

void board_deep_sleep(void)
{
    board_led(false);
    board_power_epd(false);
    board_power_audio(false);
    enter_sleep();
}

void board_power_off(void)
{
    board_led(false);
    board_power_epd(false);
    board_power_audio(false);
    rtc_gpio_hold_dis(BD_PWR_VBAT_PIN);
    board_power_vbat(false);
    vTaskDelay(pdMS_TO_TICKS(200));
    /* Still alive => we are on USB power. Fall back to deep sleep. */
    enter_sleep();
}

bool board_woke_from_rtc_alarm(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_EXT1) {
        return false;
    }
    return (esp_sleep_get_ext1_wakeup_status() & (1ULL << BD_RTC_INT_PIN)) != 0;
}
