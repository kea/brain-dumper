#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*
 * The board only exposes two buttons (BOOT and PWR), both active low, so the
 * whole UI has to fit into six gestures. Everything is decoded here from a
 * single polling task; espressif/button would work too but its API churns
 * across major versions and this is 150 lines.
 */
typedef enum {
    BTN_BOOT = 0,
    BTN_PWR,
    BTN_COUNT,
} btn_id_t;

typedef enum {
    BTN_EV_CLICK,
    BTN_EV_DOUBLE,
    BTN_EV_LONG,        /* fires once, while still held */
} btn_action_t;

typedef struct {
    btn_id_t     id;
    btn_action_t action;
} btn_event_t;

#define BTN_LONG_PRESS_MS   700
/*
 * Measured from the first release to the *second press*, not to the second
 * release: the duration of the second press must not eat into the budget, or
 * a deliberate double click degrades into two singles.
 */
#define BTN_DOUBLE_GAP_MS   400
#define BTN_DEBOUNCE_MS     25

esp_err_t buttons_init(void);

/* Queue of btn_event_t. Never NULL after a successful buttons_init(). */
QueueHandle_t buttons_queue(void);

/* Milliseconds since the last button activity; used for the idle timeout. */
uint32_t buttons_idle_ms(void);

/* Blocks until every button is released. Call at boot: the device is usually
 * powered on by holding PWR, and we must not read that as a UI gesture. */
void buttons_wait_all_released(void);

/* Human-readable names, for logs and diagnostics. */
const char *btn_name(btn_id_t id);
const char *btn_action_name(btn_action_t action);
