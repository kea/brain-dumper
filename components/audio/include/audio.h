#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/*
 * ES8311 mono codec: one I2S port shared by the microphone and the speaker,
 * so recording and playback are mutually exclusive by construction.
 *
 * Recordings are written straight to the SD card as 16 kHz / 16 bit / mono
 * WAV, which is exactly what every Whisper-family STT server wants and needs
 * no transcoding on the way out.
 */

typedef enum {
    AUDIO_IDLE,
    AUDIO_RECORDING,
    AUDIO_PLAYING,
} audio_state_t;

esp_err_t     audio_init(void);
audio_state_t audio_state(void);

/* --- Recording ------------------------------------------------------- */

esp_err_t audio_record_start(const char *wav_path, uint32_t max_seconds);

/* Stops and finalises the WAV header. Safe to call when not recording. */
esp_err_t audio_record_stop(uint32_t *duration_ms, uint32_t *bytes);

uint32_t  audio_record_elapsed_ms(void);

/* Peak level of the last block, 0..100. Drives the VU meter on screen. */
uint8_t   audio_level(void);

/* --- Playback -------------------------------------------------------- */

esp_err_t audio_play_start(const char *wav_path);
void      audio_play_stop(void);
uint32_t  audio_play_position_ms(void);
uint32_t  audio_play_duration_ms(void);

/* 0..100. Persisted by the caller; the codec is reopened per session. */
void      audio_set_volume(uint8_t percent);
uint8_t   audio_volume(void);

/* Microphone gain in dB (ES8311 PGA), 0..50. */
void      audio_set_mic_gain(float db);
