#include "buttons.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "buttons";

static const gpio_num_t s_pins[BTN_COUNT] = {
    [BTN_BOOT] = BD_BTN_BOOT_PIN,
    [BTN_PWR]  = BD_BTN_PWR_PIN,
};

typedef struct {
    bool     pressed;        /* debounced state */
    bool     long_fired;     /* long press already reported for this hold */
    bool     pending_click;  /* a release is waiting out the double-click window */
    bool     double_armed;   /* the press in progress is the second of a pair */
    int64_t  edge_us;        /* last debounced transition */
    int64_t  release_us;     /* when the pending click was released */
} btn_state_t;

static const char *const BTN_NAMES[BTN_COUNT] = {
    [BTN_BOOT] = "BOOT",
    [BTN_PWR]  = "PWR",
};

static const char *const ACTION_NAMES[] = {
    [BTN_EV_CLICK]  = "click",
    [BTN_EV_DOUBLE] = "double",
    [BTN_EV_LONG]   = "long",
};

const char *btn_name(btn_id_t id)
{
    return (id < BTN_COUNT) ? BTN_NAMES[id] : "?";
}

const char *btn_action_name(btn_action_t action)
{
    return (action <= BTN_EV_LONG) ? ACTION_NAMES[action] : "?";
}

static QueueHandle_t s_queue;
static btn_state_t   s_state[BTN_COUNT];
static int64_t       s_last_activity_us;

static void emit(btn_id_t id, btn_action_t action)
{
    btn_event_t ev = { .id = id, .action = action };
    s_last_activity_us = esp_timer_get_time();
    ESP_LOGI(TAG, "%-4s %s", btn_name(id), btn_action_name(action));
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, %s %s dropped", btn_name(id), btn_action_name(action));
    }
}

static void buttons_task(void *arg)
{
    (void)arg;
    for (;;) {
        int64_t now = esp_timer_get_time();

        for (int i = 0; i < BTN_COUNT; i++) {
            btn_state_t *st = &s_state[i];
            bool raw = gpio_get_level(s_pins[i]) == 0;   /* active low */

            if (raw != st->pressed && (now - st->edge_us) >= BTN_DEBOUNCE_MS * 1000) {
                int64_t since_edge_ms = (now - st->edge_us) / 1000;
                st->pressed = raw;
                st->edge_us = now;
                s_last_activity_us = now;

                if (raw) {
                    st->long_fired = false;
                    /* A press that lands inside the window after a release is
                     * the second half of a double click. */
                    if (st->pending_click &&
                        (now - st->release_us) <= BTN_DOUBLE_GAP_MS * 1000) {
                        st->pending_click = false;
                        st->double_armed = true;
                    }
                    ESP_LOGD(TAG, "%-4s down  (idle %lldms%s)", btn_name((btn_id_t)i),
                             (long long)since_edge_ms, st->double_armed ? ", 2nd" : "");
                } else {
                    ESP_LOGD(TAG, "%-4s up    (held %lldms)", btn_name((btn_id_t)i),
                             (long long)since_edge_ms);
                    if (st->long_fired) {
                        /* The long press already fired; the release ends it. */
                        st->pending_click = false;
                        st->double_armed = false;
                    } else if (st->double_armed) {
                        st->double_armed = false;
                        emit((btn_id_t)i, BTN_EV_DOUBLE);
                    } else {
                        st->pending_click = true;
                        st->release_us = now;
                    }
                }
            }

            if (st->pressed && !st->long_fired &&
                (now - st->edge_us) >= BTN_LONG_PRESS_MS * 1000) {
                st->long_fired = true;
                st->pending_click = false;
                st->double_armed = false;
                emit((btn_id_t)i, BTN_EV_LONG);
            }

            /* The window closed with no second press: it really was a single. */
            if (!st->pressed && st->pending_click &&
                (now - st->release_us) > BTN_DOUBLE_GAP_MS * 1000) {
                st->pending_click = false;
                emit((btn_id_t)i, BTN_EV_CLICK);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t buttons_init(void)
{
    gpio_config_t cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BD_BTN_BOOT_PIN) | (1ULL << BD_BTN_PWR_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }

    s_queue = xQueueCreate(8, sizeof(btn_event_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    s_last_activity_us = esp_timer_get_time();
    for (int i = 0; i < BTN_COUNT; i++) {
        s_state[i].edge_us = s_last_activity_us;
    }

    if (xTaskCreate(buttons_task, "buttons", 3072, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

QueueHandle_t buttons_queue(void) { return s_queue; }

uint32_t buttons_idle_ms(void)
{
    return (uint32_t)((esp_timer_get_time() - s_last_activity_us) / 1000);
}

void buttons_wait_all_released(void)
{
    for (;;) {
        bool any = false;
        for (int i = 0; i < BTN_COUNT; i++) {
            if (gpio_get_level(s_pins[i]) == 0) {
                any = true;
            }
        }
        if (!any) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));
    for (int i = 0; i < BTN_COUNT; i++) {
        s_state[i].pending_click = false;
        s_state[i].double_armed = false;
        s_state[i].long_fired = false;
    }
    if (s_queue) {
        xQueueReset(s_queue);
    }
}
