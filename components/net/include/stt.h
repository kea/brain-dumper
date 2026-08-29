#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/*
 * Speech-to-text against a local, OpenAI-compatible endpoint:
 *
 *     POST <stt_url>            multipart/form-data
 *          file=<the WAV>, model=..., language=..., response_format=json
 *     200  {"text": "..."}
 *
 * That shape is what whisper.cpp's server, Speaches / faster-whisper-server
 * and LocalAI all speak, so pointing the device at a different box is a
 * one-line change in config.ini and no firmware change at all.
 *
 * The WAV is streamed from the SD card in 4 KB chunks: a ten minute note is
 * 19 MB and must never be buffered in RAM.
 */

typedef enum {
    STT_OK,
    STT_ERR_OFFLINE,
    STT_ERR_NO_ENDPOINT,
    STT_ERR_FILE,
    STT_ERR_HTTP,        /* connected, but the server said no */
    STT_ERR_PARSE,
} stt_status_t;

typedef struct {
    stt_status_t status;
    int          http_status;
    uint32_t     elapsed_ms;
} stt_result_t;

/* Blocking; expect tens of seconds for a long note. `out` receives UTF-8. */
stt_result_t stt_transcribe(const char *wav_path, char *out, size_t out_len);

const char *stt_status_str(stt_status_t s);
