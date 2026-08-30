#include "web.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "audio.h"
#include "board.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "net.h"
#include "notes.h"
#include "shtc3.h"
#include "storage.h"

static const char *TAG = "web";

/* Same 4 KB the STT client uploads with: big enough that the SDMMC bus is not
 * the bottleneck, small enough that a ten-minute note never lands in RAM. */
#define FILE_CHUNK      4096

/* Long enough for a transcript that fills the screen, short enough that a
 * client that walked away does not hold a socket for the rest of the session. */
#define SOCKET_TIMEOUT_S 10

static httpd_handle_t   s_server;
static volatile uint32_t s_last_req_ms;
static volatile bool     s_seen_request;
static volatile uint32_t s_change_seq;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void touch(void)
{
    s_last_req_ms = now_ms();
    s_seen_request = true;
}

/* ------------------------------------------------------------------ */
/* Response plumbing                                                   */
/* ------------------------------------------------------------------ */

/* httpd_send() is a single write() and may place fewer bytes than asked for,
 * so every raw send loops. Returns false once the peer has gone. */
static bool send_all(httpd_req_t *req, const char *buf, size_t len)
{
    while (len > 0) {
        int sent = httpd_send(req, buf, len);
        if (sent <= 0) {
            return false;
        }
        buf += sent;
        len -= (size_t)sent;
    }
    return true;
}

static esp_err_t fail(httpd_req_t *req, const char *status, const char *reason)
{
    char body[192];
    int n = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", reason);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

/*
 * The catalogue lives in RAM, but transcripts and audio do not.
 *
 * Reading a 19 MB WAV off a one-bit SDMMC bus while the recorder is writing to
 * the same card is how an I2S buffer gets dropped, and a dropped buffer is an
 * audible hole in a recording nobody can take again. Anything that touches the
 * card steps aside while audio is running - and there is no card at all while
 * the host has it over USB.
 */
static const char *card_unavailable(void)
{
    if (!storage_mounted()) {
        return "no card: it is either absent or on loan to USB transfer";
    }
    if (audio_state() != AUDIO_IDLE) {
        return "the device is recording or playing";
    }
    return NULL;
}

/* JSON string body, without the quotes. Bytes above 0x7F are already UTF-8 and
 * are valid in a JSON string as they stand, so only the escapes JSON demands
 * are spent. */
static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 7 < out_len; p++) {
        if (*p == '"' || *p == '\\') {
            out[o++] = '\\';
            out[o++] = (char)*p;
        } else if (*p == '\n') {
            out[o++] = '\\'; out[o++] = 'n';
        } else if (*p == '\r') {
            out[o++] = '\\'; out[o++] = 'r';
        } else if (*p == '\t') {
            out[o++] = '\\'; out[o++] = 't';
        } else if (*p < 0x20) {
            o += (size_t)snprintf(out + o, out_len - o, "\\u%04x", *p);
        } else {
            out[o++] = (char)*p;
        }
    }
    out[o] = '\0';
}

static const char *stt_name(note_stt_t s)
{
    switch (s) {
        case NOTE_STT_DONE:    return "done";
        case NOTE_STT_FAILED:  return "failed";
        case NOTE_STT_SKIPPED: return "skipped";
        default:               return "pending";
    }
}

static int note_json(const note_t *n, char *buf, size_t len)
{
    char tag[NOTE_TAG_MAX * 6 + 1];
    char title[NOTE_TITLE_MAX * 6 + 1];
    json_escape(n->tag, tag, sizeof(tag));
    json_escape(n->title, title, sizeof(title));

    char temp[16] = "null";
    char hum[16] = "null";
    if (!isnan(n->temp_c)) {
        snprintf(temp, sizeof(temp), "%.1f", n->temp_c);
    }
    if (!isnan(n->humidity)) {
        snprintf(hum, sizeof(hum), "%.1f", n->humidity);
    }

    return snprintf(buf, len,
        "{\"id\":%" PRIu32 ",\"created\":%lld,\"duration_ms\":%" PRIu32
        ",\"bytes\":%" PRIu32 ",\"temp_c\":%s,\"humidity\":%s,\"stt\":\"%s\""
        ",\"tag\":\"%s\",\"title\":\"%s\",\"has_text\":%s}",
        n->id, (long long)n->created, n->duration_ms, n->bytes,
        temp, hum, stt_name(n->stt), tag, title,
        n->stt == NOTE_STT_DONE ? "true" : "false");
}

/* ------------------------------------------------------------------ */
/* File streaming                                                      */
/* ------------------------------------------------------------------ */

/* snprintf reports the length it wanted, not the length it wrote. Sending that
 * number would walk off the end of the buffer, so a truncated header is no
 * header at all. */
static size_t header_len(int written, size_t cap)
{
    return (written > 0 && (size_t)written < cap) ? (size_t)written : 0;
}

/*
 * Files go out with a real Content-Length and byte ranges, which means writing
 * the header block by hand: httpd_resp_send_chunk() always declares
 * Transfer-Encoding: chunked, and a chunked audio/wav is a file Safari will
 * play from the start and refuse to seek in. The page's scrub bar is a Range
 * request, so this matters more than it looks.
 */
static esp_err_t send_file(httpd_req_t *req, const char *path, const char *ctype,
                           const char *download_name)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return fail(req, "404 Not Found", "no such file on the card");
    }

    long total = (long)st.st_size;
    long from = 0;
    long to = total - 1;
    bool partial = false;

    char range[64];
    if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK) {
        long a = 0, b = -1;
        if (sscanf(range, "bytes=%ld-%ld", &a, &b) >= 1) {
            if (a < 0) {
                from = total + a;      /* bytes=-500: the tail of the file */
                if (from < 0) {
                    from = 0;
                }
            } else {
                from = a;
                if (b >= 0 && b < to) {
                    to = b;
                }
            }
            partial = true;
        }
        if (from > to || from >= total) {
            char hdr[128];
            int n = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 416 Range Not Satisfiable\r\n"
                "Content-Range: bytes */%ld\r\n"
                "Content-Length: 0\r\n\r\n", total);
            send_all(req, hdr, header_len(n, sizeof(hdr)));
            return ESP_OK;
        }
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return fail(req, "500 Internal Server Error", "cannot open the file");
    }
    char *buf = malloc(FILE_CHUNK);
    if (!buf) {
        fclose(f);
        return fail(req, "503 Service Unavailable", "out of memory");
    }

    char crange[64] = "";
    if (partial) {
        snprintf(crange, sizeof(crange), "Content-Range: bytes %ld-%ld/%ld\r\n",
                 from, to, total);
    }
    char disp[96] = "";
    if (download_name) {
        snprintf(disp, sizeof(disp), "Content-Disposition: attachment; filename=\"%s\"\r\n",
                 download_name);
    }

    char hdr[384];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Accept-Ranges: bytes\r\n"
        "Cache-Control: no-store\r\n"
        "%s%s"
        "\r\n",
        partial ? "206 Partial Content" : "200 OK", ctype, to - from + 1,
        crange, disp);

    esp_err_t err = ESP_OK;
    size_t hdr_len = header_len(n, sizeof(hdr));
    if (hdr_len == 0 || !send_all(req, hdr, hdr_len) || fseek(f, from, SEEK_SET) != 0) {
        err = ESP_FAIL;
    }

    long left = to - from + 1;
    while (err == ESP_OK && left > 0) {
        size_t want = (left < FILE_CHUNK) ? (size_t)left : FILE_CHUNK;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) {
            break;                     /* the file shrank under us */
        }
        if (!send_all(req, buf, got)) {
            /* Normal on a phone: seeking in an <audio> element abandons the
             * open range and opens another. */
            ESP_LOGD(TAG, "client left after %ld of %ld bytes", to - from + 1 - left, total);
            err = ESP_FAIL;
        }
        left -= (long)got;
    }

    free(buf);
    fclose(f);
    return err;
}

/* ------------------------------------------------------------------ */
/* Handlers                                                            */
/* ------------------------------------------------------------------ */

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static esp_err_t page_get(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static esp_err_t status_get(httpd_req_t *req)
{
    touch();

    const bd_config_t *cfg = storage_config();
    const esp_app_desc_t *app = esp_app_get_description();

    /* The page renders every timestamp in the device's timezone rather than
     * the phone's, so it needs the offset. newlib here has no tm_gmtoff:
     * read the UTC wall clock back as if it were local and the difference is
     * the offset, DST included. */
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    utc.tm_isdst = -1;
    long tz_offset = (long)(now - mktime(&utc));

    float t = NAN, rh = NAN;
    shtc3_last(&t, &rh);
    char temp[16] = "null";
    char hum[16] = "null";
    if (!isnan(t)) {
        snprintf(temp, sizeof(temp), "%.1f", t);
    }
    if (!isnan(rh)) {
        snprintf(hum, sizeof(hum), "%.1f", rh);
    }

    const char *audio =
        audio_state() == AUDIO_RECORDING ? "recording" :
        audio_state() == AUDIO_PLAYING   ? "playing" : "idle";

    char ssid[BD_CFG_SSID_MAX * 6 + 1];
    json_escape(cfg->wifi_ssid, ssid, sizeof(ssid));

    char body[768];
    int n = snprintf(body, sizeof(body),
        "{\"version\":\"%s\",\"uptime_s\":%lld,\"time\":%lld,\"tz_offset_s\":%ld,"
        "\"battery_pct\":%u,\"battery_v\":%.2f,\"usb_power\":%s,"
        "\"wifi\":{\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d},"
        "\"sd\":{\"mounted\":%s,\"free_bytes\":%llu},"
        "\"notes\":{\"count\":%d,\"pending\":%d},"
        "\"temp_c\":%s,\"humidity\":%s,\"audio\":\"%s\","
        "\"max_record_s\":%u,\"heap_free\":%u}",
        app ? app->version : "?",
        (long long)(esp_timer_get_time() / 1000000),
        (long long)now, tz_offset,
        board_battery_percent(), board_battery_voltage(),
        board_usb_powered() ? "true" : "false",
        ssid, net_ip_str(), net_rssi(),
        storage_mounted() ? "true" : "false",
        (unsigned long long)storage_free_bytes(),
        notes_count(), notes_pending_count(),
        temp, hum, audio,
        (unsigned)cfg->max_record_s,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, n);
}

static esp_err_t list_get(httpd_req_t *req)
{
    touch();

    int count = notes_count();
    note_t *snap = NULL;
    if (count > 0) {
        snap = heap_caps_malloc((size_t)count * sizeof(note_t), MALLOC_CAP_SPIRAM);
        if (!snap) {
            snap = malloc((size_t)count * sizeof(note_t));
        }
        if (!snap) {
            return fail(req, "503 Service Unavailable", "out of memory");
        }
        count = notes_snapshot(snap, count);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* Up to 512 notes at ~200 bytes each is more than fits in one buffer, so
     * the array goes out an element at a time. */
    httpd_resp_sendstr_chunk(req, "[");
    char item[800];
    for (int i = 0; i < count; i++) {
        int n = note_json(&snap[i], item, sizeof(item));
        if (i > 0) {
            httpd_resp_send_chunk(req, ",", 1);
        }
        httpd_resp_send_chunk(req, item, n);
    }
    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_send_chunk(req, NULL, 0);

    free(snap);
    return ESP_OK;
}

/* "/api/notes/42/audio" -> 42, with *tail left pointing at "/audio" or "". */
static uint32_t parse_note_uri(const char *uri, const char **tail)
{
    static const char prefix[] = "/api/notes/";
    if (strncmp(uri, prefix, sizeof(prefix) - 1) != 0) {
        return 0;
    }
    const char *p = uri + sizeof(prefix) - 1;
    char *end = NULL;
    uint32_t id = (uint32_t)strtoul(p, &end, 10);
    if (end == p) {
        return 0;
    }
    *tail = end;
    return id;
}

static bool wants_download(httpd_req_t *req)
{
    char query[32];
    char value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, "dl", value, sizeof(value)) == ESP_OK;
}

static esp_err_t note_get(httpd_req_t *req)
{
    touch();

    const char *tail = "";
    uint32_t id = parse_note_uri(req->uri, &tail);
    if (id == 0) {
        return fail(req, "404 Not Found", "no such route");
    }

    note_t note;
    if (!notes_get(id, &note)) {
        return fail(req, "404 Not Found", "no note with that id");
    }

    /* The query string rides along on tail; compare only the path part. */
    size_t tail_len = strcspn(tail, "?");

    if (tail_len == 0) {
        char body[800];
        int n = note_json(&note, body, sizeof(body));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, body, n);
    }

    const char *busy = card_unavailable();
    if (busy) {
        return fail(req, "503 Service Unavailable", busy);
    }

    char path[NOTE_PATH_MAX];
    char name[32];

    if (tail_len == 5 && !strncmp(tail, "/text", 5)) {
        notes_path(id, "txt", path, sizeof(path));
        snprintf(name, sizeof(name), "%04" PRIu32 ".txt", id);
        return send_file(req, path, "text/plain; charset=utf-8",
                         wants_download(req) ? name : NULL);
    }
    if (tail_len == 6 && !strncmp(tail, "/audio", 6)) {
        notes_path(id, "wav", path, sizeof(path));
        snprintf(name, sizeof(name), "%04" PRIu32 ".wav", id);
        return send_file(req, path, "audio/wav",
                         wants_download(req) ? name : NULL);
    }
    return fail(req, "404 Not Found", "no such route");
}

static esp_err_t note_delete(httpd_req_t *req)
{
    touch();

    const char *tail = "";
    uint32_t id = parse_note_uri(req->uri, &tail);
    if (id == 0 || strcspn(tail, "?") != 0) {
        return fail(req, "404 Not Found", "no such route");
    }

    note_t note;
    if (!notes_get(id, &note)) {
        return fail(req, "404 Not Found", "no note with that id");
    }

    const char *busy = card_unavailable();
    if (busy) {
        return fail(req, "503 Service Unavailable", busy);
    }

    esp_err_t err = notes_delete(id);
    if (err != ESP_OK) {
        return fail(req, "500 Internal Server Error", "the card refused the delete");
    }
    s_change_seq++;

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"deleted\":true}");
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static const httpd_uri_t ROUTES[] = {
    { .uri = "/",             .method = HTTP_GET,    .handler = page_get },
    { .uri = "/api/status",   .method = HTTP_GET,    .handler = status_get },
    { .uri = "/api/notes",    .method = HTTP_GET,    .handler = list_get },
    { .uri = "/api/notes/*",  .method = HTTP_GET,    .handler = note_get },
    { .uri = "/api/notes/*",  .method = HTTP_DELETE, .handler = note_delete },
};

static void start(void)
{
    if (s_server) {
        return;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = sizeof(ROUTES) / sizeof(ROUTES[0]);
    /* One phone, maybe a second tab. Fewer sockets than the lwip default keeps
     * the listener's footprint honest next to LVGL and the audio buffers. */
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = SOCKET_TIMEOUT_S;
    cfg.send_wait_timeout = SOCKET_TIMEOUT_S;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "cannot start the server");
        s_server = NULL;
        return;
    }
    for (size_t i = 0; i < sizeof(ROUTES) / sizeof(ROUTES[0]); i++) {
        httpd_register_uri_handler(s_server, &ROUTES[i]);
    }
    ESP_LOGI(TAG, "listening on http://%s/", net_ip_str());
}

void web_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "stopped");
    }
}

bool web_running(void) { return s_server != NULL; }

uint32_t web_idle_ms(void)
{
    return s_seen_request ? now_ms() - s_last_req_ms : UINT32_MAX;
}

uint32_t web_change_seq(void) { return s_change_seq; }

static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        start();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        web_stop();
    }
}

esp_err_t web_init(void)
{
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_net_event, NULL, NULL), TAG, "ip handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_net_event, NULL, NULL), TAG, "wifi handler");

    /* net_start() may already have an address by the time we get here. */
    if (net_is_connected()) {
        start();
    }
    return ESP_OK;
}
