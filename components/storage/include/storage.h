#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/sdmmc_host.h"
#include "esp_err.h"

/* Everything the user cares about lives on the microSD: audio, transcripts,
 * and a plain-text config file they can edit from a PC over USB MSC. */

#define BD_CFG_SSID_MAX   33
#define BD_CFG_PASS_MAX   65
#define BD_CFG_URL_MAX    160
#define BD_CFG_STR_MAX    48

typedef struct {
    char     wifi_ssid[BD_CFG_SSID_MAX];
    char     wifi_pass[BD_CFG_PASS_MAX];
    char     stt_url[BD_CFG_URL_MAX];     /* full endpoint, e.g. http://host:8000/v1/audio/transcriptions */
    char     stt_key[BD_CFG_STR_MAX];     /* optional bearer token */
    char     stt_model[BD_CFG_STR_MAX];
    char     stt_lang[8];                 /* ISO-639-1, "" = autodetect */
    char     ui_lang[8];                  /* interface language: en, fr, de, es, it */
    char     timezone[BD_CFG_STR_MAX];    /* POSIX TZ string */
    char     ntp_server[BD_CFG_STR_MAX];
    uint32_t idle_sleep_s;                /* 0 disables the idle deep sleep */
    uint32_t wake_interval_min;           /* RTC countdown between sync wakes */
    uint32_t max_record_s;
    bool     web_enable;                  /* LAN web UI + JSON API on port 80 */
} bd_config_t;

esp_err_t storage_init(void);

/*
 * Probe and mount the card again. The board routes no card-detect line, so
 * inserting a card after boot is invisible to the firmware and the user has
 * to ask for this explicitly from the Settings screen.
 */
esp_err_t storage_try_mount(void);

bool          storage_mounted(void);
sdmmc_card_t *storage_card(void);
uint64_t      storage_free_bytes(void);

/*
 * Hand the card to the USB MSC gadget.
 *
 * Unmounting through the VFS frees the sdmmc_card_t and tears the SDMMC host
 * down, so the card has to be probed again from scratch before TinyUSB can
 * own it. The returned handle belongs to the caller until the next reboot:
 * this is a one-way door, which is why leaving transfer mode reboots.
 */
esp_err_t storage_release_for_usb(sdmmc_card_t **out_card);

/* Parsed /sdcard/config.ini, pre-filled with the menuconfig defaults.
 * Never NULL: with no card we still return the compiled-in defaults. */
const bd_config_t *storage_config(void);

/* Re-read config.ini, e.g. after the user edited it over USB. */
esp_err_t storage_config_reload(void);

/* Write a commented template to /sdcard/config.ini if none exists yet. */
esp_err_t storage_write_default_config(void);
