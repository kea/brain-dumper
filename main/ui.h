#pragma once

#include "app.h"
#include "esp_err.h"

/* Builds the LVGL object tree. Call once, after ui_port_init(). */
esp_err_t ui_init(void);

/*
 * Rebuilds the whole body from the model and pushes one frame to the panel.
 *
 * Full rebuilds look wasteful, but an e-paper refresh costs ~400 ms no matter
 * how little changed, so the CPU time spent re-creating a dozen widgets is
 * noise, and it removes every class of stale-widget bug.
 */
void ui_render(const app_model_t *m);
