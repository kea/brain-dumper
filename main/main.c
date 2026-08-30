/*
 * Brain Dumper - voice notes with local transcription
 * Waveshare ESP32-S3-ePaper-1.54 V2 (non-touch) / ESP32-S3-PICO-1-N8R8
 */

#include <inttypes.h>
#include <stdlib.h>
#include <time.h>

#include "app.h"
#include "audio.h"
#include "board.h"
#include "buttons.h"
#include "epd.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i18n.h"
#include "net.h"
#include "notes.h"
#include "pcf85063.h"
#include "shtc3.h"
#include "storage.h"
#include "stt.h"
#include "ui.h"
#include "ui_port.h"
#include "web.h"

static const char *TAG = "main";

/* How long an unattended RTC-triggered sync is allowed to run before we give
 * up and go back to sleep, so a dead STT server cannot drain the battery. */
#define HEADLESS_BUDGET_MS (4 * 60 * 1000)

/*
 * Woken by the RTC rather than by a person: drain the transcription queue
 * without ever powering the panel, then go straight back to deep sleep.
 * This is what lets a note recorded on a walk be readable by morning.
 */
static void headless_sync(void)
{
    ESP_LOGI(TAG, "RTC wake: unattended sync");
    pcf85063_clear_irq();

    storage_init();
    notes_init();
    pcf85063_to_system_time();

    int64_t deadline = esp_timer_get_time() + (int64_t)HEADLESS_BUDGET_MS * 1000;
    int done = 0;

    if (notes_pending_count() > 0 && net_start() == ESP_OK &&
        net_wait_connected(20000) == ESP_OK) {
        net_time_sync(8000);

        char *text = malloc(APP_TEXT_MAX);
        while (text && esp_timer_get_time() < deadline) {
            uint32_t id = notes_next_pending();
            if (id == 0) {
                break;
            }
            char path[NOTE_PATH_MAX];
            notes_path(id, "wav", path, sizeof(path));

            stt_result_t res = stt_transcribe(path, text, APP_TEXT_MAX);
            if (res.status == STT_OK) {
                notes_save_text(id, text);
                done++;
            } else if (res.status == STT_ERR_OFFLINE || res.status == STT_ERR_NO_ENDPOINT) {
                break;   /* the network, not the note: retry next wake */
            } else {
                const note_t *existing = notes_by_id(id);
                if (existing) {
                    note_t n = *existing;
                    n.stt = NOTE_STT_FAILED;
                    notes_save(&n);
                }
            }
        }
        free(text);
    }

    int left = notes_pending_count();
    ESP_LOGI(TAG, "unattended sync: %d done, %d left", done, left);

    net_stop();
    uint32_t wake_min = storage_config()->wake_interval_min;
    if (left > 0 && wake_min > 0) {
        pcf85063_start_countdown_minutes(wake_min > 255 ? 255 : (uint8_t)wake_min);
    } else {
        pcf85063_stop_countdown();
    }
    board_deep_sleep();
}

void app_main(void)
{
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(buttons_init());

    /* The board is switched on by holding PWR; without this the very first
     * gesture the UI sees is that same press. */
    buttons_wait_all_released();

    if (pcf85063_init() != ESP_OK) {
        ESP_LOGW(TAG, "no RTC: timestamps will restart from the epoch");
    }

    if (board_woke_from_rtc_alarm()) {
        headless_sync();   /* never returns */
    }

    ESP_ERROR_CHECK(storage_init());

    const bd_config_t *cfg = storage_config();
    /* Before the UI exists, so the first frame is already in the user's
     * language rather than in English for one redraw. */
    i18n_set_lang(cfg->ui_lang);
    if (cfg->timezone[0]) {
        setenv("TZ", cfg->timezone, 1);
        tzset();
    }
    /* Do this before notes_init() so anything created early is timestamped. */
    if (pcf85063_to_system_time() != ESP_OK) {
        ESP_LOGW(TAG, "RTC time invalid, waiting for SNTP");
    }

    ESP_ERROR_CHECK(epd_init());
    ESP_ERROR_CHECK(ui_port_init());
    ESP_ERROR_CHECK(ui_init());

    if (shtc3_init() != ESP_OK) {
        ESP_LOGW(TAG, "SHTC3 not responding, notes will have no ambient data");
    }
    if (audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: recording and playback are disabled");
    }

    if (notes_init() != ESP_OK) {
        ESP_LOGE(TAG, "note catalogue unavailable");
    }

    /* Non-blocking: the UI comes up immediately and the radio catches up. */
    net_start();

    /* Only on this boot path. headless_sync() never gets here, which is the
     * point: an unattended wake exists to drain the queue and go back to
     * sleep, not to answer a browser nobody is holding. */
    if (storage_config()->web_enable) {
        web_init();
    }

    ESP_ERROR_CHECK(app_start());

    ESP_LOGI(TAG, "ready, %" PRIu32 " B free heap", esp_get_free_heap_size());
}
