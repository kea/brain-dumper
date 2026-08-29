#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "lvgl.h"

/*
 * LVGL v9 bound to the e-paper in I1 (1 bit per pixel) mode.
 *
 * The render buffer is 5008 bytes (5000 px bytes + an 8-byte palette header)
 * instead of the 80 KB RGB565 buffer the vendor examples use, and the flush
 * callback is a memcpy rather than a per-pixel conversion.
 *
 * LVGL is not thread safe: everything that touches an lv_obj_t must run
 * between ui_lock() and ui_unlock().
 */

esp_err_t ui_port_init(void);

bool ui_lock(int timeout_ms);   /* timeout_ms < 0 = wait forever */
void ui_unlock(void);

/* Force the panel to do a de-ghosting full refresh on the next redraw.
 * Worth calling when swapping between screens. */
void ui_request_full_refresh(void);

/* Nudge the LVGL task to redraw now instead of waiting for its next tick. */
void ui_notify(void);

/*
 * Frames that have actually reached the glass.
 *
 * The redraw runs in the LVGL task, so ui_render() returning only means the
 * frame is queued. Sample ui_frame_count() before a render and wait on it
 * afterwards when what comes next would disturb the panel - cutting its power
 * mid-refresh leaves the image half-written.
 */
uint32_t ui_frame_count(void);
bool ui_wait_frame(uint32_t before, int timeout_ms);
