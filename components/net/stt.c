#include "stt.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "net.h"
#include "storage.h"

static const char *TAG = "stt";

#define BOUNDARY      "----BrainDumperBoundary7Ab3"
#define UPLOAD_CHUNK  4096
#define RESPONSE_MAX  16384

static int append_field(char *buf, int off, int cap, const char *name, const char *value)
{
    return off + snprintf(buf + off, (size_t)(cap - off),
                          "--" BOUNDARY "\r\n"
                          "Content-Disposition: form-data; name=\"%s\"\r\n\r\n"
                          "%s\r\n",
                          name, value);
}

const char *stt_status_str(stt_status_t s)
{
    switch (s) {
        case STT_OK:              return "ok";
        case STT_ERR_OFFLINE:     return "offline";
        case STT_ERR_NO_ENDPOINT: return "no endpoint";
        case STT_ERR_FILE:        return "file error";
        case STT_ERR_HTTP:        return "server error";
        case STT_ERR_PARSE:       return "bad response";
    }
    return "?";
}

stt_result_t stt_transcribe(const char *wav_path, char *out, size_t out_len)
{
    stt_result_t res = { .status = STT_ERR_HTTP };
    int64_t t0 = esp_timer_get_time();
    const bd_config_t *cfg = storage_config();

    if (cfg->stt_url[0] == '\0') {
        res.status = STT_ERR_NO_ENDPOINT;
        return res;
    }
    if (!net_is_connected()) {
        res.status = STT_ERR_OFFLINE;
        return res;
    }

    struct stat st;
    if (stat(wav_path, &st) != 0 || st.st_size <= 44) {
        ESP_LOGE(TAG, "%s missing or empty", wav_path);
        res.status = STT_ERR_FILE;
        return res;
    }

    /* Build the multipart preamble and epilogue up front so we can declare an
     * exact Content-Length; chunked uploads trip up some of these servers. */
    char preamble[768];
    int pre = 0;
    pre = append_field(preamble, pre, sizeof(preamble), "model",
                       cfg->stt_model[0] ? cfg->stt_model : "whisper-1");
    pre = append_field(preamble, pre, sizeof(preamble), "response_format", "json");
    if (cfg->stt_lang[0]) {
        pre = append_field(preamble, pre, sizeof(preamble), "language", cfg->stt_lang);
    }
    pre += snprintf(preamble + pre, sizeof(preamble) - pre,
                    "--" BOUNDARY "\r\n"
                    "Content-Disposition: form-data; name=\"file\"; filename=\"note.wav\"\r\n"
                    "Content-Type: audio/wav\r\n\r\n");

    static const char epilogue[] = "\r\n--" BOUNDARY "--\r\n";
    const int epi = (int)sizeof(epilogue) - 1;

    FILE *f = fopen(wav_path, "rb");
    if (!f) {
        res.status = STT_ERR_FILE;
        return res;
    }

    uint8_t *chunk = heap_caps_malloc(UPLOAD_CHUNK, MALLOC_CAP_DEFAULT);
    char    *body  = heap_caps_malloc(RESPONSE_MAX, MALLOC_CAP_SPIRAM);
    if (!body) {
        body = malloc(RESPONSE_MAX);
    }
    if (!chunk || !body) {
        ESP_LOGE(TAG, "out of memory");
        res.status = STT_ERR_FILE;
        goto cleanup;
    }

    esp_http_client_config_t http_cfg = {
        .url = cfg->stt_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 120000,       /* whisper on CPU is not fast */
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        res.status = STT_ERR_HTTP;
        goto cleanup;
    }

    esp_http_client_set_header(client, "Content-Type",
                               "multipart/form-data; boundary=" BOUNDARY);
    if (cfg->stt_key[0]) {
        char auth[BD_CFG_STR_MAX + 8];
        snprintf(auth, sizeof(auth), "Bearer %s", cfg->stt_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    int total = pre + (int)st.st_size + epi;
    ESP_LOGI(TAG, "POST %s (%d KB audio)", cfg->stt_url, (int)(st.st_size / 1024));

    esp_err_t err = esp_http_client_open(client, total);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(err));
        res.status = STT_ERR_OFFLINE;
        goto close_client;
    }

    if (esp_http_client_write(client, preamble, pre) != pre) {
        goto close_client;
    }

    size_t sent = 0;
    for (;;) {
        size_t n = fread(chunk, 1, UPLOAD_CHUNK, f);
        if (n == 0) {
            break;
        }
        int written = esp_http_client_write(client, (const char *)chunk, n);
        if (written != (int)n) {
            ESP_LOGE(TAG, "upload stalled at %u/%ld bytes", (unsigned)sent, (long)st.st_size);
            goto close_client;
        }
        sent += n;
    }

    if (esp_http_client_write(client, epilogue, epi) != epi) {
        goto close_client;
    }

    int content_len = esp_http_client_fetch_headers(client);
    res.http_status = esp_http_client_get_status_code(client);

    int read = esp_http_client_read_response(client, body, RESPONSE_MAX - 1);
    if (read < 0) {
        read = 0;
    }
    body[read] = '\0';

    if (res.http_status != 200) {
        ESP_LOGE(TAG, "HTTP %d: %.200s", res.http_status, body);
        res.status = STT_ERR_HTTP;
        goto close_client;
    }
    ESP_LOGD(TAG, "response (%d/%d bytes): %.120s", read, content_len, body);

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        res.status = STT_ERR_PARSE;
        goto close_client;
    }
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(text) && text->valuestring) {
        snprintf(out, out_len, "%s", text->valuestring);
        res.status = STT_OK;
    } else {
        ESP_LOGE(TAG, "no \"text\" field in response");
        res.status = STT_ERR_PARSE;
    }
    cJSON_Delete(root);

close_client:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

cleanup:
    fclose(f);
    free(chunk);
    free(body);
    res.elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    ESP_LOGI(TAG, "%s in %" PRIu32 " ms", stt_status_str(res.status), res.elapsed_ms);
    return res;
}
