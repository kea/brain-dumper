#include "ui_port.h"

#include <inttypes.h>
#include <string.h>

#include "epd.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ui_port";

#define LVGL_TICK_MS      10
#define LVGL_IDLE_MS      500     /* longest nap between lv_timer_handler() runs */
#define I1_PALETTE_BYTES  8       /* 2 entries * lv_color32_t */

static SemaphoreHandle_t s_mutex;
static TaskHandle_t      s_task;
static volatile uint32_t s_frames;
static uint8_t           s_render_buf[EPD_BUF_SIZE + I1_PALETTE_BYTES];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;   /* render mode FULL: the area is always the whole screen */

    /* LVGL's I1 layout matches the panel's exactly: row-major, 25 bytes per
     * row, MSB is the leftmost pixel, bit set means luminance > 127 (white).
     * The only thing in the way is the palette header. */
    epd_blit(px_map + I1_PALETTE_BYTES);
    epd_flush(EPD_REFRESH_PARTIAL);
    s_frames++;

    lv_display_flush_ready(disp);
}

static void tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_MS);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t next_ms = LVGL_IDLE_MS;
        if (ui_lock(-1)) {
            next_ms = lv_timer_handler();
            ui_unlock();
        }
        if (next_ms > LVGL_IDLE_MS) {
            next_ms = LVGL_IDLE_MS;
        }
        /* An e-paper redraw costs ~400 ms, so there is nothing to gain from
         * spinning faster than that; ui_notify() cuts the wait short when
         * something actually changed. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(next_ms < 20 ? 20 : next_ms));
    }
}

esp_err_t ui_port_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_mutex, ESP_ERR_NO_MEM, TAG, "mutex");

    lv_init();

    lv_display_t *disp = lv_display_create(EPD_W, EPD_H);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "lv_display_create");

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(disp, s_render_buf, NULL, sizeof(s_render_buf),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, flush_cb);

    const esp_timer_create_args_t tick_args = {
        .callback = tick_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick), TAG, "tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick, LVGL_TICK_MS * 1000), TAG, "tick start");

    if (xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, &s_task, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LVGL %d.%d.%d, I1 buffer %u B",
             lv_version_major(), lv_version_minor(), lv_version_patch(),
             (unsigned)sizeof(s_render_buf));
    return ESP_OK;
}

bool ui_lock(int timeout_ms)
{
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_mutex, ticks) == pdTRUE;
}

void ui_unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

void ui_request_full_refresh(void) { epd_request_full_refresh(); }

void ui_notify(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

uint32_t ui_frame_count(void) { return s_frames; }

bool ui_wait_frame(uint32_t before, int timeout_ms)
{
    /* A full de-ghosting refresh is ~2 s of panel time, so polling every
     * 20 ms costs a handful of wakeups on a path that is about to sleep. */
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (s_frames == before) {
        if (esp_timer_get_time() >= deadline) {
            ESP_LOGW(TAG, "frame %" PRIu32 " never reached the panel", before + 1);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}
