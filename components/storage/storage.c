#include "storage.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "board.h"
#include "esp_rom_sys.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "storage";

#define CONFIG_PATH BD_SD_MOUNT_POINT "/config.ini"

static sdmmc_card_t *s_card;
static bool          s_mounted;
static bd_config_t   s_cfg;

static void load_defaults(bd_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), "%s", CONFIG_BD_WIFI_SSID);
    snprintf(cfg->wifi_pass, sizeof(cfg->wifi_pass), "%s", CONFIG_BD_WIFI_PASSWORD);
    snprintf(cfg->stt_url, sizeof(cfg->stt_url), "%s", CONFIG_BD_STT_URL);
    snprintf(cfg->stt_model, sizeof(cfg->stt_model), "%s", CONFIG_BD_STT_MODEL);
    snprintf(cfg->stt_lang, sizeof(cfg->stt_lang), "%s", CONFIG_BD_STT_LANGUAGE);
    snprintf(cfg->timezone, sizeof(cfg->timezone), "%s", CONFIG_BD_TIMEZONE);
    snprintf(cfg->ntp_server, sizeof(cfg->ntp_server), "%s", CONFIG_BD_NTP_SERVER);
    cfg->idle_sleep_s = CONFIG_BD_IDLE_SLEEP_S;
    cfg->wake_interval_min = CONFIG_BD_WAKE_INTERVAL_MIN;
    cfg->max_record_s = CONFIG_BD_MAX_RECORD_S;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static void assign(char *dst, size_t dst_len, const char *src)
{
    snprintf(dst, dst_len, "%s", src);
}

static esp_err_t parse_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "no config.ini, using build-time defaults");
        return ESP_ERR_NOT_FOUND;
    }

    char line[256];
    int applied = 0;
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#' || *p == ';') {
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim(p);
        char *val = trim(eq + 1);

        if      (!strcmp(key, "wifi_ssid"))         assign(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), val);
        else if (!strcmp(key, "wifi_pass"))         assign(s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), val);
        else if (!strcmp(key, "stt_url"))           assign(s_cfg.stt_url, sizeof(s_cfg.stt_url), val);
        else if (!strcmp(key, "stt_key"))           assign(s_cfg.stt_key, sizeof(s_cfg.stt_key), val);
        else if (!strcmp(key, "stt_model"))         assign(s_cfg.stt_model, sizeof(s_cfg.stt_model), val);
        else if (!strcmp(key, "stt_lang"))          assign(s_cfg.stt_lang, sizeof(s_cfg.stt_lang), val);
        else if (!strcmp(key, "timezone"))          assign(s_cfg.timezone, sizeof(s_cfg.timezone), val);
        else if (!strcmp(key, "ntp_server"))        assign(s_cfg.ntp_server, sizeof(s_cfg.ntp_server), val);
        else if (!strcmp(key, "idle_sleep_s"))      s_cfg.idle_sleep_s = (uint32_t)strtoul(val, NULL, 10);
        else if (!strcmp(key, "wake_interval_min")) s_cfg.wake_interval_min = (uint32_t)strtoul(val, NULL, 10);
        else if (!strcmp(key, "max_record_s"))      s_cfg.max_record_s = (uint32_t)strtoul(val, NULL, 10);
        else { ESP_LOGW(TAG, "unknown config key '%s'", key); continue; }
        applied++;
    }
    fclose(f);
    ESP_LOGI(TAG, "config.ini: %d settings applied", applied);
    return ESP_OK;
}

esp_err_t storage_write_default_config(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    struct stat st;
    if (stat(CONFIG_PATH, &st) == 0) {
        return ESP_OK;   /* never clobber what the user wrote */
    }

    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        return ESP_FAIL;
    }
    fprintf(f,
        "# Brain Dumper configuration\n"
        "# Edit this file from a PC (hold BOOT at the Transfer menu entry to\n"
        "# expose the card over USB), then reboot the device.\n"
        "\n"
        "wifi_ssid=%s\n"
        "wifi_pass=%s\n"
        "\n"
        "# Local speech-to-text, OpenAI-compatible endpoint. Works with\n"
        "# whisper.cpp server, Speaches / faster-whisper-server, LocalAI.\n"
        "stt_url=%s\n"
        "stt_model=%s\n"
        "stt_lang=%s\n"
        "#stt_key=\n"
        "\n"
        "timezone=%s\n"
        "ntp_server=%s\n"
        "\n"
        "# Seconds of inactivity before deep sleep (0 = never).\n"
        "idle_sleep_s=%u\n"
        "# Minutes between RTC wake-ups to drain the transcription queue.\n"
        "wake_interval_min=%u\n"
        "# Hard cap on a single recording, in seconds.\n"
        "max_record_s=%u\n",
        s_cfg.wifi_ssid, s_cfg.wifi_pass, s_cfg.stt_url, s_cfg.stt_model,
        s_cfg.stt_lang, s_cfg.timezone, s_cfg.ntp_server,
        (unsigned)s_cfg.idle_sleep_s, (unsigned)s_cfg.wake_interval_min,
        (unsigned)s_cfg.max_record_s);
    fclose(f);
    ESP_LOGI(TAG, "wrote template %s", CONFIG_PATH);
    return ESP_OK;
}

/*
 * When the card never answers we cannot tell an empty slot from a wiring
 * problem, and the two need very different fixes. A seated card holds CMD and
 * D0 up through the board's pull-ups strongly enough to beat an internal
 * pull-down; an empty slot does not.
 */
static void log_bus_state(void)
{
    static const gpio_num_t pins[] = { BD_SD_CLK_PIN, BD_SD_CMD_PIN, BD_SD_D0_PIN };
    static const char *const names[] = { "CLK", "CMD", "D0 " };

    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_config_t cfg = {
            .intr_type = GPIO_INTR_DISABLE,
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = 1ULL << pins[i],
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
        };
        gpio_config(&cfg);
        esp_rom_delay_us(500);
        int pulled_down = gpio_get_level(pins[i]);

        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        gpio_config(&cfg);
        esp_rom_delay_us(500);
        int floating = gpio_get_level(pins[i]);

        ESP_LOGW(TAG, "  %s GPIO%-2d  floating=%d  vs internal pulldown=%d  -> %s",
                 names[i], pins[i], floating, pulled_down,
                 pulled_down ? "external pull-up present" : "no external pull-up");
        gpio_reset_pin(pins[i]);
    }
}

static esp_err_t mount_card(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    /* The board only routes CLK/CMD/D0, so slot 1 runs in 1-bit mode. */
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = BD_SD_CLK_PIN;
    slot.cmd = BD_SD_CMD_PIN;
    slot.d0  = BD_SD_D0_PIN;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    /* Cards that enumerate fine but cannot hold 40 MHz over this board's
     * traces fail late and confusingly, so step the clock down and retry
     * rather than reporting "no card" for what is a signal-integrity problem. */
    static const int speeds_khz[] = {
        SDMMC_FREQ_HIGHSPEED,   /* 40 MHz */
        SDMMC_FREQ_DEFAULT,     /* 20 MHz */
        SDMMC_FREQ_PROBING,     /* 400 kHz */
    };

    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(speeds_khz) / sizeof(speeds_khz[0]); i++) {
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = speeds_khz[i];

        err = esp_vfs_fat_sdmmc_mount(BD_SD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
        if (err == ESP_OK) {
            s_mounted = true;
            ESP_LOGI(TAG, "SD mounted at %d kHz", speeds_khz[i]);
            return ESP_OK;
        }
        s_card = NULL;

        if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_NOT_FOUND) {
            /* The card never answered CMD0/ACMD41: it is absent or not seated,
             * and no clock we pick is going to change that. */
            break;
        }
        ESP_LOGW(TAG, "mount at %d kHz failed (%s), retrying slower",
                 speeds_khz[i], esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "no usable microSD: %s", esp_err_to_name(err));
    log_bus_state();
    return err;
}

static void after_mount(void)
{
    ESP_LOGI(TAG, "SD %s, %llu MB", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    mkdir(BD_SD_MOUNT_POINT "/notes", 0777);
    if (parse_config() == ESP_ERR_NOT_FOUND) {
        storage_write_default_config();
    }
}

esp_err_t storage_try_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }
    esp_err_t err = mount_card();
    if (err == ESP_OK) {
        after_mount();
    }
    return err;
}

esp_err_t storage_init(void)
{
    load_defaults(&s_cfg);

    /* Give the card a moment after the rails come up before clocking it. */
    vTaskDelay(pdMS_TO_TICKS(50));

    if (mount_card() == ESP_OK) {
        after_mount();
    }
    /* Never fatal: without a card the device still boots, shows why, and lets
     * the user insert one and retry from Settings. */
    return ESP_OK;
}

bool storage_mounted(void) { return s_mounted; }
sdmmc_card_t *storage_card(void) { return s_card; }

uint64_t storage_free_bytes(void)
{
    if (!s_mounted) {
        return 0;
    }
    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    if (f_getfree("0:", &free_clusters, &fs) != FR_OK || !fs) {
        return 0;
    }
    return (uint64_t)free_clusters * fs->csize * FF_MIN_SS;
}

esp_err_t storage_release_for_usb(sdmmc_card_t **out_card)
{
    ESP_RETURN_ON_FALSE(s_mounted && s_card, ESP_ERR_INVALID_STATE, TAG, "not mounted");

    /* This frees s_card and calls the host's deinit, so nothing of the old
     * handle survives; null it out before anyone can dereference it. */
    esp_err_t err = esp_vfs_fat_sdcard_unmount(BD_SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    ESP_RETURN_ON_ERROR(err, TAG, "unmount");

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = BD_SD_CLK_PIN;
    slot.cmd = BD_SD_CMD_PIN;
    slot.d0  = BD_SD_D0_PIN;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "host init");
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(host.slot, &slot), TAG, "slot init");

    sdmmc_card_t *card = calloc(1, sizeof(sdmmc_card_t));
    ESP_RETURN_ON_FALSE(card, ESP_ERR_NO_MEM, TAG, "card alloc");

    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        free(card);
        sdmmc_host_deinit();
        return err;
    }

    *out_card = card;
    ESP_LOGI(TAG, "card released to USB");
    return ESP_OK;
}

const bd_config_t *storage_config(void) { return &s_cfg; }

esp_err_t storage_config_reload(void)
{
    load_defaults(&s_cfg);
    return s_mounted ? parse_config() : ESP_ERR_INVALID_STATE;
}
