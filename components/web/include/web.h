#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * A small HTTP server on the local network: the note list, transcripts, WAV
 * downloads, one delete, a status document, and a single-page front end for
 * all of it embedded in flash.
 *
 * It exists because the device's own screen is 200x200 pixels and its keyboard
 * is two buttons. Reading a paragraph, or getting a file off the card without
 * unplugging anything, is work the phone already in your pocket does better.
 *
 * Two properties shape the whole component:
 *
 *   - It is reachable only while the device is awake. On battery the device
 *     deep-sleeps after idle_sleep_s and the radio goes with it, so the server
 *     is a companion to a device someone is holding, not a service.
 *   - It has no authentication. Anyone who can reach the device on the network
 *     can read and delete every note on the card. Set web_enable=0 in
 *     config.ini if that is not a trade worth making.
 */

/* Arms the server. It binds when the radio gets an address and unbinds when
 * the association drops, so this is safe to call before Wi-Fi is up. */
esp_err_t web_init(void);

/* Closes the listener. Called on the way into deep sleep so clients get a
 * refused connection rather than a socket that never answers. */
void web_stop(void);

bool web_running(void);

/*
 * Milliseconds since the last request was served, or UINT32_MAX if none has
 * been. The sleep policy consults this: a device that goes dark while someone
 * is halfway through reading a transcript has picked the wrong moment to save
 * power, and the page cannot wake it back up.
 */
uint32_t web_idle_ms(void);

/*
 * Bumped every time a request changes the catalogue. The app task watches it
 * so a note deleted from a phone leaves the device's own list on the next
 * frame, instead of sitting there as a row that opens onto nothing.
 */
uint32_t web_change_seq(void);
