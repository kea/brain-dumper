#include "app.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio.h"
#include "board.h"
#include "buttons.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "epd.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "notes.h"
#include "pcf85063.h"
#include "shtc3.h"
#include "storage.h"
#include "stt.h"
#include "ui.h"
#include "ui_port.h"
#include "usb_msc.h"

static const char *TAG = "app";

extern const int MENU_ITEM_COUNT;   /* defined next to the menu labels in ui.c */
#define SETTINGS_ITEM_COUNT 6
#define MIN_NOTE_MS         700

static app_model_t   s_m;
static TaskHandle_t  s_sync_task;
static volatile bool s_render_request;

/* ------------------------------------------------------------------ */
/* Model helpers                                                       */
/* ------------------------------------------------------------------ */

static void refresh_status(void)
{
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(s_m.clock, sizeof(s_m.clock), "%H:%M", &local);

    s_m.battery = board_battery_percent();
    s_m.charging = board_usb_powered();
    s_m.net = net_state();
    s_m.sd_ok = storage_mounted();
    s_m.pending = notes_pending_count();

    /* The SHTC3 takes 25 ms to answer, so it runs on a slow clock of its own
     * rather than on every redraw. Info is the only screen that shows it, and
     * the air in a room does not move faster than this. */
    static int64_t next_env_us;
    int64_t now_us = esp_timer_get_time();
    if (now_us >= next_env_us) {
        next_env_us = now_us + 30 * 1000000;
        float t = NAN, h = NAN;
        if (shtc3_read(&t, &h) == ESP_OK) {
            s_m.temp_c = t;
            s_m.humidity = h;
        }
    }
}

static const char *const STATE_NAMES[] = {
    [ST_HOME] = "HOME", [ST_RECORDING] = "RECORDING", [ST_TAG_SELECT] = "TAG",
    [ST_MENU] = "MENU", [ST_NOTE_LIST] = "LIST", [ST_NOTE_DETAIL] = "DETAIL",
    [ST_PLAYING] = "PLAYING", [ST_DELETE_CONFIRM] = "CONFIRM",
    [ST_SETTINGS] = "SETTINGS", [ST_INFO] = "INFO", [ST_TRANSFER] = "TRANSFER",
    [ST_MESSAGE] = "MESSAGE", [ST_SLEEP] = "SLEEP",
};

static void go(app_state_t state)
{
    if (state != s_m.state) {
        ESP_LOGI(TAG, "%s -> %s", STATE_NAMES[s_m.state], STATE_NAMES[state]);
        s_m.prev_state = s_m.state;
        s_m.state = state;
        s_m.sel = 0;
        s_m.top = 0;
        /* A screen change is where ghosting is most visible, so spend the
         * two seconds on a de-ghosting full refresh here rather than mid-list. */
        ui_request_full_refresh();
    }
}

static void message(const char *line1, const char *line2, uint32_t ms)
{
    snprintf(s_m.msg, sizeof(s_m.msg), "%s", line1 ? line1 : "");
    snprintf(s_m.msg2, sizeof(s_m.msg2), "%s", line2 ? line2 : "");
    s_m.msg_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + ms;
    if (s_m.state != ST_MESSAGE) {
        app_state_t back_to = s_m.state;
        go(ST_MESSAGE);
        s_m.prev_state = back_to;
    }
}

static void clamp_list(void)
{
    if (s_m.item_count <= 0) {
        s_m.sel = 0;
        s_m.top = 0;
        return;
    }
    if (s_m.sel < 0) {
        s_m.sel = s_m.item_count - 1;
    }
    if (s_m.sel >= s_m.item_count) {
        s_m.sel = 0;
    }
    if (s_m.sel < s_m.top) {
        s_m.top = s_m.sel;
    }
    if (s_m.sel >= s_m.top + APP_LIST_ROWS) {
        s_m.top = s_m.sel - APP_LIST_ROWS + 1;
    }
}

static void load_note_text(uint32_t id)
{
    s_m.text_len = notes_load_text(id, s_m.text, APP_TEXT_MAX);
    if (s_m.text_len < 0) {
        s_m.text_len = 0;
        s_m.text[0] = '\0';
    }
    s_m.text_page = 0;
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

static void start_recording(void)
{
    if (!storage_mounted()) {
        message("Nessuna microSD", "Impossibile registrare", 2500);
        return;
    }
    if (audio_state() == AUDIO_PLAYING) {
        audio_play_stop();
    }
    if (audio_state() != AUDIO_IDLE) {
        return;
    }

    note_t note;
    if (notes_create(&note) != ESP_OK) {
        message("Errore", "Creazione nota fallita", 2500);
        return;
    }

    /* Ambient conditions are read once, at the start: the sensor takes 25 ms
     * and the reading would only drift while we hold the mic anyway. */
    float t = NAN, h = NAN;
    if (shtc3_read(&t, &h) == ESP_OK) {
        note.temp_c = t;
        note.humidity = h;
    }
    notes_save(&note);

    char path[NOTE_PATH_MAX];
    notes_path(note.id, "wav", path, sizeof(path));

    if (audio_record_start(path, storage_config()->max_record_s) != ESP_OK) {
        notes_delete(note.id);
        message("Errore", "Microfono non disponibile", 2500);
        return;
    }

    s_m.note_id = note.id;
    s_m.rec_ms = 0;
    s_m.level = 0;
    board_led(true);
    go(ST_RECORDING);
    ESP_LOGI(TAG, "recording note %" PRIu32 " -> %s", note.id, path);
}

static void stop_recording(void)
{
    uint32_t duration = 0, bytes = 0;
    audio_record_stop(&duration, &bytes);
    board_led(false);

    const note_t *existing = notes_by_id(s_m.note_id);
    if (!existing) {
        go(ST_HOME);
        return;
    }
    note_t note = *existing;
    note.duration_ms = duration;
    note.bytes = bytes;

    if (duration < MIN_NOTE_MS) {
        notes_delete(note.id);
        message("Troppo breve", "Nota scartata", 2000);
        return;
    }

    notes_save(&note);
    ESP_LOGI(TAG, "note %" PRIu32 ": %" PRIu32 " ms, %" PRIu32 " B", note.id, duration, bytes);

    /* Ask for a tag while the thought is still fresh. The note is already
     * safely on the card, so bailing out here loses nothing. */
    go(ST_TAG_SELECT);
    if (s_sync_task) {
        xTaskNotifyGive(s_sync_task);
    }
}

/* ------------------------------------------------------------------ */
/* Transcription worker                                                */
/* ------------------------------------------------------------------ */

static bool transcribe_one(void)
{
    uint32_t id = notes_next_pending();
    if (id == 0) {
        return false;
    }

    char path[NOTE_PATH_MAX];
    notes_path(id, "wav", path, sizeof(path));

    char *out = heap_caps_malloc(APP_TEXT_MAX, MALLOC_CAP_SPIRAM);
    if (!out) {
        return false;
    }
    out[0] = '\0';

    stt_result_t res = stt_transcribe(path, out, APP_TEXT_MAX);

    const note_t *existing = notes_by_id(id);
    if (existing) {
        note_t note = *existing;
        if (res.status == STT_OK) {
            notes_save_text(id, out);
        } else if (res.status == STT_ERR_OFFLINE || res.status == STT_ERR_NO_ENDPOINT) {
            /* Not the note's fault; leave it pending so we retry later. */
            free(out);
            return false;
        } else {
            note.stt = NOTE_STT_FAILED;
            notes_save(&note);
        }
    }

    free(out);
    s_render_request = true;
    return true;
}

/* How often to go back to NTP once the clock is already right. The PCF85063
 * drifts by a couple of seconds a day, so this is about not trusting it for
 * weeks, not about precision. */
#define TIME_RESYNC_US (6 * 3600 * 1000000LL)

/*
 * The clock used to be a passenger of the transcription queue: net_time_sync()
 * ran only when a pending note happened to be the thing that started the
 * radio. A device with nothing to send therefore never set its RTC, and since
 * only a write clears the oscillator-stopped flag, it came up "time LOST"
 * every single boot. The time gets its own errand now.
 */
static void maybe_sync_time(void)
{
    static int64_t next_us;   /* 0, so the first chance after boot is taken */
    int64_t now = esp_timer_get_time();
    if (now < next_us) {
        return;
    }

    /* An RTC with no time in it is worth waiting for the radio; a drift
     * correction is not - that one can wait for the next pass. */
    bool lost = !pcf85063_time_is_valid();
    if (!net_is_connected() && (!lost || net_wait_connected(15000) != ESP_OK)) {
        return;
    }

    if (net_time_sync(8000) == ESP_OK) {
        next_us = now + TIME_RESYNC_US;
    } else {
        next_us = now + 60 * 1000000LL;
    }
}

static void sync_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Woken by a new recording or a menu action; the timeout is the
         * fallback for "Wi-Fi came back while we were idle". */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000));

        maybe_sync_time();

        if (!storage_mounted() || notes_next_pending() == 0) {
            continue;
        }
        if (!net_is_connected()) {
            if (net_start() != ESP_OK || net_wait_connected(20000) != ESP_OK) {
                continue;
            }
            /* The radio just came up: rate limiting decides if this is a
             * no-op, but the chance is free. */
            maybe_sync_time();
        }
        while (transcribe_one()) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Menu actions                                                        */
/* ------------------------------------------------------------------ */

static void open_menu_item(int index)
{
    switch (index) {
        case 0:
            notes_reload();
            s_m.item_count = notes_count();
            go(ST_NOTE_LIST);
            break;
        case 1:
            if (s_sync_task) {
                xTaskNotifyGive(s_sync_task);
            }
            message("Sincronizzo", notes_pending_count() ? "in corso..." : "niente da fare", 2000);
            break;
        case 2:
            go(ST_SETTINGS);
            break;
        case 3:
            if (usb_msc_start() == ESP_OK) {
                go(ST_TRANSFER);
            } else {
                message("Errore", "microSD non disponibile", 2500);
            }
            break;
        case 4:
            go(ST_INFO);
            break;
        default:
            break;
    }
}

static void activate_setting(int index)
{
    switch (index) {
        case 0: {
            static const uint8_t steps[] = { 20, 40, 60, 80, 100 };
            int i = 0;
            while (i < 4 && steps[i] <= s_m.volume) {
                i++;
            }
            s_m.volume = steps[i % 5];
            audio_set_volume(s_m.volume);
            break;
        }
        case 1: {
            static const uint8_t steps[] = { 15, 24, 30, 36, 42 };
            int i = 0;
            while (i < 4 && steps[i] <= s_m.mic_gain_db) {
                i++;
            }
            s_m.mic_gain_db = steps[i % 5];
            audio_set_mic_gain((float)s_m.mic_gain_db);
            break;
        }
        case 2:
            net_start();
            message("Wi-Fi", "connessione...", 2000);
            break;
        case 3: {
            /* No card-detect line on this board, so a card inserted after
             * boot only shows up if the user asks for it. */
            esp_err_t err = storage_try_mount();
            if (err == ESP_OK) {
                notes_reload();
                message("microSD", "montata", 1800);
            } else {
                message("microSD", esp_err_to_name(err), 2500);
            }
            break;
        }
        case 4:
            storage_config_reload();
            message("Config", "ricaricata", 1800);
            break;
        case 5:
            board_power_off();
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static void on_select(void)
{
    switch (s_m.state) {
        case ST_HOME:
            go(ST_MENU);
            break;

        case ST_TAG_SELECT:
            notes_set_tag(s_m.note_id, NOTE_TAGS[s_m.sel]);
            message("Salvata", NOTE_TAGS[s_m.sel], 1500);
            s_m.prev_state = ST_HOME;
            break;

        case ST_MENU:
            open_menu_item(s_m.sel);
            break;

        case ST_NOTE_LIST: {
            const note_t *n = notes_at(s_m.sel);
            if (n) {
                s_m.note_id = n->id;
                load_note_text(n->id);
                go(ST_NOTE_DETAIL);
            }
            break;
        }

        case ST_NOTE_DETAIL: {
            char path[NOTE_PATH_MAX];
            notes_path(s_m.note_id, "wav", path, sizeof(path));
            if (audio_play_start(path) == ESP_OK) {
                go(ST_PLAYING);
            } else {
                message("Errore", "Audio non disponibile", 2000);
            }
            break;
        }

        case ST_PLAYING:
            audio_play_stop();
            go(ST_NOTE_DETAIL);
            break;

        case ST_DELETE_CONFIRM:
            if (s_m.sel == 1) {
                notes_delete(s_m.note_id);
                notes_reload();
                s_m.item_count = notes_count();
                message("Eliminata", NULL, 1500);
                s_m.prev_state = ST_NOTE_LIST;
            } else {
                go(ST_NOTE_LIST);
            }
            break;

        case ST_SETTINGS:
            activate_setting(s_m.sel);
            break;

        case ST_TRANSFER:
            usb_msc_stop();   /* reboots */
            break;

        case ST_INFO:
        case ST_MESSAGE:
        case ST_RECORDING:
        case ST_SLEEP:
            break;
    }
}

static void on_back(void)
{
    switch (s_m.state) {
        case ST_TAG_SELECT:
            /* The note keeps the default tag; nothing is lost. */
            go(ST_HOME);
            break;
        case ST_MENU:
        case ST_HOME:
            go(ST_HOME);
            break;
        case ST_NOTE_LIST:
        case ST_SETTINGS:
        case ST_INFO:
            go(ST_MENU);
            break;
        case ST_NOTE_DETAIL:
            go(ST_NOTE_LIST);
            s_m.item_count = notes_count();
            break;
        case ST_PLAYING:
            audio_play_stop();
            go(ST_NOTE_DETAIL);
            break;
        case ST_DELETE_CONFIRM:
            go(ST_NOTE_LIST);
            break;
        case ST_RECORDING:
        case ST_TRANSFER:
        case ST_MESSAGE:
        case ST_SLEEP:
            break;
    }
}

static void on_next(int delta)
{
    switch (s_m.state) {
        case ST_TAG_SELECT:
            s_m.item_count = NOTE_TAG_COUNT;
            s_m.sel += delta;
            clamp_list();
            break;
        case ST_MENU:
            s_m.item_count = MENU_ITEM_COUNT;
            s_m.sel += delta;
            clamp_list();
            break;
        case ST_SETTINGS:
            s_m.item_count = SETTINGS_ITEM_COUNT;
            s_m.sel += delta;
            clamp_list();
            break;
        case ST_DELETE_CONFIRM:
            s_m.item_count = 2;
            s_m.sel += delta;
            clamp_list();
            break;
        case ST_NOTE_LIST:
            s_m.item_count = notes_count();
            s_m.sel += delta;
            clamp_list();
            break;
        case ST_NOTE_DETAIL:
            s_m.text_page += delta;
            if (s_m.text_page < 0) {
                s_m.text_page = 0;
            }
            break;
        case ST_HOME:
            go(ST_MENU);
            break;
        default:
            break;
    }
}

static void on_long_pwr(void)
{
    switch (s_m.state) {
        case ST_NOTE_LIST:
        case ST_NOTE_DETAIL:
            /* Delete lives on PWR-long here because the two-button budget is
             * spent; power off stays available on Home and in Settings. */
            if (notes_count() > 0) {
                s_m.item_count = 2;
                go(ST_DELETE_CONFIRM);
            }
            break;
        case ST_HOME:
            board_power_off();
            break;
        default:
            break;
    }
}

static void handle(const btn_event_t *ev)
{
    ESP_LOGI(TAG, "[%s] %s %s", STATE_NAMES[s_m.state],
             btn_name(ev->id), btn_action_name(ev->action));

    /* Recording is modal on purpose: while the mic is open the only thing the
     * buttons can do is stop it. */
    if (s_m.state == ST_RECORDING) {
        if (ev->id == BTN_BOOT && ev->action == BTN_EV_LONG) {
            stop_recording();
        }
        return;
    }

    if (ev->id == BTN_BOOT && ev->action == BTN_EV_LONG) {
        start_recording();
        return;
    }

    if (s_m.state == ST_MESSAGE) {
        go(s_m.prev_state);
        return;
    }

    if (ev->id == BTN_BOOT) {
        if (ev->action == BTN_EV_CLICK) {
            on_select();
        } else if (ev->action == BTN_EV_DOUBLE) {
            on_back();
        }
    } else {
        if (ev->action == BTN_EV_CLICK) {
            on_next(+1);
        } else if (ev->action == BTN_EV_DOUBLE) {
            on_next(-1);
        } else if (ev->action == BTN_EV_LONG) {
            on_long_pwr();
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main loop                                                           */
/* ------------------------------------------------------------------ */

static bool tick(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (s_m.state == ST_MESSAGE && now_ms >= s_m.msg_until_ms) {
        go(s_m.prev_state);
        return true;
    }

    if (s_m.state == ST_RECORDING) {
        s_m.rec_ms = audio_record_elapsed_ms();
        s_m.level = audio_level();
        if (audio_state() == AUDIO_IDLE) {
            stop_recording();   /* hit max_record_s or the card filled up */
        }
        return true;
    }

    if (s_m.state == ST_PLAYING) {
        s_m.play_ms = audio_play_position_ms();
        s_m.play_total = audio_play_duration_ms();
        if (audio_state() == AUDIO_IDLE) {
            go(ST_NOTE_DETAIL);
        }
        return true;
    }

    if (s_render_request) {
        s_render_request = false;
        return true;
    }

    /* Idle: the clock in the status bar is the only thing that goes stale,
     * and a redraw costs 400 ms of panel time, so once a minute is plenty. */
    static uint32_t last_clock_ms;
    if (s_m.state == ST_HOME && now_ms - last_clock_ms > 60000) {
        last_clock_ms = now_ms;
        return true;
    }
    return false;
}

static void maybe_sleep(void)
{
    uint32_t idle_s = storage_config()->idle_sleep_s;
    if (idle_s == 0 || s_m.state != ST_HOME) {
        return;
    }
    if (audio_state() != AUDIO_IDLE || usb_msc_active()) {
        return;
    }

    /* External power: there is no battery to save, and sleeping would take the
     * USB Serial/JTAG down with it - the port vanishes from the host and the
     * device can only be brought back by pressing a button on it. */
    bool on_usb = board_usb_powered();
    static bool logged_usb;
    if (on_usb != logged_usb) {
        logged_usb = on_usb;
        ESP_LOGI(TAG, "%s", on_usb ? "on USB power, idle sleep suspended"
                                   : "on battery, idle sleep armed");
    }
    if (on_usb) {
        return;
    }

    if (buttons_idle_ms() < idle_s * 1000) {
        return;
    }

    ESP_LOGI(TAG, "idle for %" PRIu32 " s, sleeping", idle_s);

    /* Whatever is on the glass when we cut the power stays there for as long
     * as we sleep, so the last frame is the title alone. go() also asks for a
     * full refresh, which is the de-ghosting pass this image wants anyway. */
    uint32_t frame = ui_frame_count();
    go(ST_SLEEP);
    ui_render(&s_m);

    /* The redraw runs in the LVGL task: sleeping the panel before it finishes
     * would leave the frame half-written on the glass. */
    ui_wait_frame(frame, 6000);

    uint32_t wake_min = storage_config()->wake_interval_min;
    if (wake_min > 0 && notes_pending_count() > 0) {
        /* Only arm the RTC when there is a reason to wake: otherwise the
         * device should stay dark until someone touches it. */
        pcf85063_start_countdown_minutes(wake_min > 255 ? 255 : (uint8_t)wake_min);
    } else {
        pcf85063_stop_countdown();
    }

    net_stop();
    epd_sleep();
    board_deep_sleep();
}

static void app_task(void *arg)
{
    (void)arg;
    bool dirty = true;

    for (;;) {
        bool busy = (s_m.state == ST_RECORDING || s_m.state == ST_PLAYING);
        TickType_t wait = pdMS_TO_TICKS(busy ? 1500 : 400);

        btn_event_t ev;
        if (xQueueReceive(buttons_queue(), &ev, wait) == pdTRUE) {
            handle(&ev);
            dirty = true;
        } else if (tick()) {
            dirty = true;
        }

        if (dirty) {
            refresh_status();
            ui_render(&s_m);
            dirty = false;
        }
        maybe_sleep();
    }
}

esp_err_t app_start(void)
{
    memset(&s_m, 0, sizeof(s_m));
    s_m.state = ST_HOME;
    s_m.prev_state = ST_HOME;
    s_m.temp_c = NAN;
    s_m.humidity = NAN;
    s_m.volume = audio_volume();
    s_m.mic_gain_db = 30;

    s_m.text = heap_caps_malloc(APP_TEXT_MAX, MALLOC_CAP_SPIRAM);
    if (!s_m.text) {
        s_m.text = malloc(APP_TEXT_MAX);
    }
    if (!s_m.text) {
        return ESP_ERR_NO_MEM;
    }
    s_m.text[0] = '\0';

    if (xTaskCreate(sync_task, "stt_sync", 6144, NULL, 3, &s_sync_task) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the transcription worker");
    } else {
        /* Run one pass immediately instead of waiting out the first 60 s
         * timeout: a note recorded in that window would otherwise carry a
         * timestamp from a clock nobody had set yet. */
        xTaskNotifyGive(s_sync_task);
    }
    if (xTaskCreatePinnedToCore(app_task, "app", 6144, NULL, 5, NULL, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

const app_model_t *app_model(void) { return &s_m; }
