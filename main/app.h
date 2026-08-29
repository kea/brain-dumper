#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "net.h"
#include "notes.h"

/*
 * The device has exactly two buttons, so the whole interaction model is six
 * gestures:
 *
 *   BOOT  click    select / confirm
 *   BOOT  double   back
 *   BOOT  long     start or stop recording, from anywhere
 *   PWR   click    next item
 *   PWR   double   previous item
 *   PWR   long     power off
 *
 * Recording is deliberately reachable with one gesture from every screen:
 * a voice-note device that makes you navigate first has already lost the
 * thought you wanted to capture.
 */

typedef enum {
    ST_HOME,
    ST_RECORDING,
    ST_TAG_SELECT,
    ST_MENU,
    ST_NOTE_LIST,
    ST_NOTE_DETAIL,
    ST_PLAYING,
    ST_DELETE_CONFIRM,
    ST_SETTINGS,
    ST_INFO,
    ST_TRANSFER,
    ST_MESSAGE,        /* transient banner, auto-returns to prev_state */
    ST_SLEEP,          /* the frame left on the glass while the device sleeps */
} app_state_t;

#define APP_TEXT_MAX      4096
#define APP_LIST_ROWS     6

typedef struct {
    app_state_t state;
    app_state_t prev_state;

    /* status bar */
    char        clock[8];
    uint8_t     battery;
    bool        charging;
    net_state_t net;
    float       temp_c;
    float       humidity;
    bool        sd_ok;
    int         pending;

    /* list navigation */
    int         sel;
    int         top;
    int         item_count;

    /* selected note */
    uint32_t    note_id;
    char       *text;            /* PSRAM, APP_TEXT_MAX */
    int         text_len;
    int         text_page;

    /* recording / playback */
    uint32_t    rec_ms;
    uint8_t     level;
    uint32_t    play_ms;
    uint32_t    play_total;

    /* settings */
    uint8_t     volume;
    uint8_t     mic_gain_db;

    /* transient message */
    char        msg[48];
    char        msg2[64];
    uint32_t    msg_until_ms;
} app_model_t;

esp_err_t app_start(void);

/* The model is owned by the app task; the UI only ever reads it, and only
 * from inside ui_render(), which the app task calls. */
const app_model_t *app_model(void);
