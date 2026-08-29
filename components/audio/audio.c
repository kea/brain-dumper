#include "audio.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio";

/*
 * esp_codec_dev rejects an odd channel count, so the codec is always opened
 * as stereo and the mono conversion happens here: we keep the left slot on
 * the way in and duplicate it on the way out. At 16 kHz that is 64 KB/s of
 * memcpy, which is free next to the SD write.
 */
#define CODEC_CHANNELS   2
#define CHUNK_FRAMES     512
#define CHUNK_STEREO_B   (CHUNK_FRAMES * CODEC_CHANNELS * sizeof(int16_t))
#define CHUNK_MONO_B     (CHUNK_FRAMES * sizeof(int16_t))

typedef struct __attribute__((packed)) {
    char     riff[4];
    uint32_t file_size;
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data[4];
    uint32_t data_size;
} wav_header_t;

static esp_codec_dev_handle_t s_codec;
static i2s_chan_handle_t      s_tx, s_rx;

static volatile audio_state_t s_state = AUDIO_IDLE;
static volatile bool          s_stop_request;
static SemaphoreHandle_t      s_done;      /* given when the worker exits */
static SemaphoreHandle_t      s_api_lock;

static char     s_path[96];
static uint32_t s_max_seconds;
static volatile uint32_t s_elapsed_ms;
static volatile uint32_t s_data_bytes;
static volatile uint32_t s_total_ms;
static volatile uint8_t  s_level;
static uint8_t           s_volume = 80;
static float             s_mic_gain_db = 30.0f;

static int16_t *s_stereo;   /* CHUNK_STEREO_B scratch */
static int16_t *s_mono;     /* CHUNK_MONO_B scratch   */

static void wav_header_init(wav_header_t *h, uint32_t data_bytes)
{
    memcpy(h->riff, "RIFF", 4);
    memcpy(h->wave, "WAVE", 4);
    memcpy(h->fmt,  "fmt ", 4);
    memcpy(h->data, "data", 4);
    h->fmt_size = 16;
    h->audio_format = 1;   /* PCM */
    h->channels = BD_AUDIO_CHANNELS;
    h->sample_rate = BD_AUDIO_SAMPLE_RATE;
    h->bits_per_sample = BD_AUDIO_BITS;
    h->block_align = (uint16_t)(BD_AUDIO_CHANNELS * BD_AUDIO_BITS / 8);
    h->byte_rate = BD_AUDIO_SAMPLE_RATE * h->block_align;
    h->data_size = data_bytes;
    h->file_size = data_bytes + sizeof(wav_header_t) - 8;
}

static esp_err_t codec_open(void)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = BD_AUDIO_BITS,
        .channel = CODEC_CHANNELS,
        .sample_rate = BD_AUDIO_SAMPLE_RATE,
    };
    int rc = esp_codec_dev_open(s_codec, &fs);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "codec open (%d)", rc);
    esp_codec_dev_set_out_vol(s_codec, s_volume);
    esp_codec_dev_set_in_gain(s_codec, s_mic_gain_db);
    return ESP_OK;
}

static void record_task(void *arg)
{
    (void)arg;
    FILE *f = NULL;
    wav_header_t header;
    uint32_t data_bytes = 0;
    int64_t started_us = esp_timer_get_time();

    board_power_audio(true);

    if (codec_open() != ESP_OK) {
        goto finish;
    }

    f = fopen(s_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot create %s", s_path);
        goto finish;
    }

    /* Placeholder header, patched with the real sizes when we stop. */
    wav_header_init(&header, 0);
    fwrite(&header, 1, sizeof(header), f);

    /* The first blocks after the ADC powers up are a click and a DC step;
     * throw away ~100 ms so the transcript does not start with a bang. */
    for (int i = 0; i < 3; i++) {
        esp_codec_dev_read(s_codec, s_stereo, CHUNK_STEREO_B);
    }

    started_us = esp_timer_get_time();
    uint32_t max_bytes = s_max_seconds * BD_AUDIO_SAMPLE_RATE * sizeof(int16_t);

    while (!s_stop_request) {
        if (esp_codec_dev_read(s_codec, s_stereo, CHUNK_STEREO_B) != 0) {
            ESP_LOGE(TAG, "codec read failed");
            break;
        }

        int32_t peak = 0;
        for (int i = 0; i < CHUNK_FRAMES; i++) {
            int16_t sample = s_stereo[i * CODEC_CHANNELS];   /* left slot = mic */
            s_mono[i] = sample;
            int32_t mag = sample < 0 ? -sample : sample;
            if (mag > peak) {
                peak = mag;
            }
        }
        s_level = (uint8_t)((peak * 100) / 32768);

        if (fwrite(s_mono, 1, CHUNK_MONO_B, f) != CHUNK_MONO_B) {
            ESP_LOGE(TAG, "SD write failed, stopping");
            break;
        }
        data_bytes += CHUNK_MONO_B;
        s_data_bytes = data_bytes;
        s_elapsed_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);

        if (max_bytes && data_bytes >= max_bytes) {
            ESP_LOGW(TAG, "hit max_record_s");
            break;
        }
    }

finish:
    if (f) {
        wav_header_init(&header, data_bytes);
        fseek(f, 0, SEEK_SET);
        fwrite(&header, 1, sizeof(header), f);
        fclose(f);
    }
    esp_codec_dev_close(s_codec);

    s_data_bytes = data_bytes;
    s_elapsed_ms = (uint32_t)(((uint64_t)data_bytes * 1000) /
                              (BD_AUDIO_SAMPLE_RATE * sizeof(int16_t)));
    s_level = 0;
    s_state = AUDIO_IDLE;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

static void play_task(void *arg)
{
    (void)arg;
    FILE *f = NULL;
    wav_header_t header;

    board_power_audio(true);

    f = fopen(s_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", s_path);
        goto finish;
    }
    if (fread(&header, 1, sizeof(header), f) != sizeof(header) ||
        memcmp(header.riff, "RIFF", 4) != 0) {
        ESP_LOGE(TAG, "%s is not a WAV", s_path);
        goto finish;
    }

    s_total_ms = header.byte_rate ? (header.data_size * 1000u) / header.byte_rate : 0;
    s_elapsed_ms = 0;

    if (codec_open() != ESP_OK) {
        goto finish;
    }

    uint32_t played = 0;
    while (!s_stop_request) {
        size_t n = fread(s_mono, 1, CHUNK_MONO_B, f);
        if (n == 0) {
            break;
        }
        size_t frames = n / sizeof(int16_t);
        for (size_t i = 0; i < frames; i++) {
            s_stereo[i * CODEC_CHANNELS]     = s_mono[i];
            s_stereo[i * CODEC_CHANNELS + 1] = s_mono[i];
        }
        if (esp_codec_dev_write(s_codec, s_stereo, frames * CODEC_CHANNELS * sizeof(int16_t)) != 0) {
            ESP_LOGE(TAG, "codec write failed");
            break;
        }
        played += (uint32_t)n;
        s_elapsed_ms = header.byte_rate ? (played * 1000u) / header.byte_rate : 0;
    }
    esp_codec_dev_close(s_codec);

finish:
    if (f) {
        fclose(f);
    }
    s_state = AUDIO_IDLE;
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

esp_err_t audio_init(void)
{
    s_api_lock = xSemaphoreCreateMutex();
    s_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_api_lock && s_done, ESP_ERR_NO_MEM, TAG, "sync objects");

    s_stereo = heap_caps_malloc(CHUNK_STEREO_B, MALLOC_CAP_DEFAULT);
    s_mono   = heap_caps_malloc(CHUNK_MONO_B, MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(s_stereo && s_mono, ESP_ERR_NO_MEM, TAG, "scratch buffers");

    board_power_audio(true);
    vTaskDelay(pdMS_TO_TICKS(20));

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BD_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, &s_rx), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BD_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BD_I2S_MCLK_PIN,
            .bclk = BD_I2S_BCLK_PIN,
            .ws   = BD_I2S_WS_PIN,
            .dout = BD_I2S_DOUT_PIN,
            .din  = BD_I2S_DIN_PIN,
            .invert_flags = { false, false, false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "i2s tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s rx");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BD_I2S_PORT,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "i2s data if");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BD_I2C_PORT,
        .addr = BD_I2C_ADDR_ES8311,
        .bus_handle = board_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "i2c ctrl if");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BD_AUDIO_PA_PIN,
        .pa_reverted = false,
        .master_mode = false,        /* the ESP32 drives BCLK/WS */
        .use_mclk = true,
        .digital_mic = false,
        /* Without this the right slot carries a copy of the DAC output, which
         * would leak playback into recordings. */
        .no_dac_ref = true,
        .hw_gain = {
            .pa_voltage = 5.0f,
            .codec_dac_voltage = 3.3f,
            .pa_gain = 6.0f,
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311 not found");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "codec dev");

    ESP_LOGI(TAG, "ES8311 ready (%d Hz mono)", BD_AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

audio_state_t audio_state(void) { return s_state; }

esp_err_t audio_record_start(const char *wav_path, uint32_t max_seconds)
{
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;

    if (s_state != AUDIO_IDLE) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    snprintf(s_path, sizeof(s_path), "%s", wav_path);
    s_max_seconds = max_seconds;
    s_stop_request = false;
    s_elapsed_ms = 0;
    s_data_bytes = 0;
    s_level = 0;
    s_state = AUDIO_RECORDING;
    xSemaphoreTake(s_done, 0);

    if (xTaskCreatePinnedToCore(record_task, "rec", 5120, NULL, 7, NULL, 0) != pdPASS) {
        s_state = AUDIO_IDLE;
        err = ESP_ERR_NO_MEM;
    }
out:
    xSemaphoreGive(s_api_lock);
    return err;
}

esp_err_t audio_record_stop(uint32_t *duration_ms, uint32_t *bytes)
{
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    if (s_state == AUDIO_RECORDING) {
        s_stop_request = true;
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(4000)) != pdTRUE) {
            ESP_LOGE(TAG, "recorder did not stop in time");
        }
    }
    if (duration_ms) {
        *duration_ms = s_elapsed_ms;
    }
    if (bytes) {
        *bytes = s_data_bytes;
    }
    xSemaphoreGive(s_api_lock);
    return ESP_OK;
}

uint32_t audio_record_elapsed_ms(void) { return s_elapsed_ms; }
uint8_t  audio_level(void) { return s_level; }

esp_err_t audio_play_start(const char *wav_path)
{
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;

    if (s_state != AUDIO_IDLE) {
        err = ESP_ERR_INVALID_STATE;
        goto out;
    }
    snprintf(s_path, sizeof(s_path), "%s", wav_path);
    s_stop_request = false;
    s_elapsed_ms = 0;
    s_total_ms = 0;
    s_state = AUDIO_PLAYING;
    xSemaphoreTake(s_done, 0);

    if (xTaskCreatePinnedToCore(play_task, "play", 5120, NULL, 7, NULL, 0) != pdPASS) {
        s_state = AUDIO_IDLE;
        err = ESP_ERR_NO_MEM;
    }
out:
    xSemaphoreGive(s_api_lock);
    return err;
}

void audio_play_stop(void)
{
    xSemaphoreTake(s_api_lock, portMAX_DELAY);
    if (s_state == AUDIO_PLAYING) {
        s_stop_request = true;
        xSemaphoreTake(s_done, pdMS_TO_TICKS(3000));
    }
    xSemaphoreGive(s_api_lock);
}

uint32_t audio_play_position_ms(void) { return s_elapsed_ms; }
uint32_t audio_play_duration_ms(void) { return s_total_ms; }

void audio_set_volume(uint8_t percent)
{
    s_volume = percent > 100 ? 100 : percent;
    if (s_codec && s_state == AUDIO_PLAYING) {
        esp_codec_dev_set_out_vol(s_codec, s_volume);
    }
}

uint8_t audio_volume(void) { return s_volume; }

void audio_set_mic_gain(float db)
{
    s_mic_gain_db = db;
    if (s_codec && s_state == AUDIO_RECORDING) {
        esp_codec_dev_set_in_gain(s_codec, db);
    }
}
