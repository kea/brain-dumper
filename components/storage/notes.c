#include "notes.h"

#include <ctype.h>
#include <dirent.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "board.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "storage.h"

static const char *TAG = "notes";

#define NOTES_DIR BD_SD_MOUNT_POINT "/notes"

const char *const NOTE_TAGS[] = { "Inbox", "Idea", "Todo", "Work", "Personal" };
const int NOTE_TAG_COUNT = sizeof(NOTE_TAGS) / sizeof(NOTE_TAGS[0]);

static note_t         *s_notes;      /* PSRAM, newest first */
static int             s_count;
static uint32_t        s_next_id = 1;
static SemaphoreHandle_t s_lock;

static void lock(void)   { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_lock); }

void notes_path(uint32_t id, const char *ext, char *out, size_t out_len)
{
    snprintf(out, out_len, NOTES_DIR "/%04" PRIu32 ".%s", id, ext);
}

static void default_title(note_t *n)
{
    struct tm tm_local;
    time_t t = n->created;
    localtime_r(&t, &tm_local);
    strftime(n->title, sizeof(n->title), "Nota %d/%m %H:%M", &tm_local);
}

static esp_err_t write_ini(const note_t *n)
{
    char path[NOTE_PATH_MAX];
    notes_path(n->id, "ini", path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f) {
        return ESP_FAIL;
    }
    fprintf(f,
        "id=%" PRIu32 "\n"
        "created=%lld\n"
        "duration_ms=%" PRIu32 "\n"
        "bytes=%" PRIu32 "\n"
        "temp_c=%.1f\n"
        "humidity=%.1f\n"
        "stt=%d\n"
        "tag=%s\n"
        "title=%s\n",
        n->id, (long long)n->created, n->duration_ms, n->bytes,
        isnan(n->temp_c) ? -273.0f : n->temp_c,
        isnan(n->humidity) ? -1.0f : n->humidity,
        (int)n->stt, n->tag, n->title);
    fclose(f);
    return ESP_OK;
}

static bool read_ini(uint32_t id, note_t *n)
{
    char path[NOTE_PATH_MAX];
    notes_path(id, "ini", path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        return false;
    }

    memset(n, 0, sizeof(*n));
    n->id = id;
    n->temp_c = NAN;
    n->humidity = NAN;
    snprintf(n->tag, sizeof(n->tag), "%s", NOTE_TAGS[0]);

    char line[192];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';

        if      (!strcmp(key, "created"))     n->created = (time_t)strtoll(val, NULL, 10);
        else if (!strcmp(key, "duration_ms")) n->duration_ms = (uint32_t)strtoul(val, NULL, 10);
        else if (!strcmp(key, "bytes"))       n->bytes = (uint32_t)strtoul(val, NULL, 10);
        else if (!strcmp(key, "temp_c"))      { float v = strtof(val, NULL); n->temp_c = (v < -270.0f) ? NAN : v; }
        else if (!strcmp(key, "humidity"))    { float v = strtof(val, NULL); n->humidity = (v < 0.0f) ? NAN : v; }
        else if (!strcmp(key, "stt"))         n->stt = (note_stt_t)atoi(val);
        else if (!strcmp(key, "tag"))         snprintf(n->tag, sizeof(n->tag), "%s", val);
        else if (!strcmp(key, "title"))       snprintf(n->title, sizeof(n->title), "%s", val);
    }
    fclose(f);

    if (n->title[0] == '\0') {
        default_title(n);
    }
    return true;
}

static int cmp_newest_first(const void *a, const void *b)
{
    const note_t *na = a, *nb = b;
    if (na->created != nb->created) {
        return (nb->created > na->created) ? 1 : -1;
    }
    return (nb->id > na->id) ? 1 : -1;
}

esp_err_t notes_reload(void)
{
    if (!storage_mounted()) {
        lock();
        s_count = 0;
        unlock();
        return ESP_ERR_INVALID_STATE;
    }

    lock();
    s_count = 0;
    s_next_id = 1;

    DIR *dir = opendir(NOTES_DIR);
    if (!dir) {
        mkdir(NOTES_DIR, 0777);
        unlock();
        return ESP_OK;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && s_count < NOTES_MAX) {
        const char *dot = strrchr(de->d_name, '.');
        if (!dot || strcasecmp(dot, ".ini") != 0) {
            continue;
        }
        uint32_t id = (uint32_t)strtoul(de->d_name, NULL, 10);
        if (id == 0) {
            continue;
        }
        if (read_ini(id, &s_notes[s_count])) {
            s_count++;
        }
        if (id >= s_next_id) {
            s_next_id = id + 1;
        }
    }
    closedir(dir);

    qsort(s_notes, s_count, sizeof(note_t), cmp_newest_first);
    unlock();

    ESP_LOGI(TAG, "%d note(s), next id %" PRIu32 ", %d pending", s_count, s_next_id,
             notes_pending_count());
    return ESP_OK;
}

esp_err_t notes_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "mutex");

    s_notes = heap_caps_calloc(NOTES_MAX, sizeof(note_t), MALLOC_CAP_SPIRAM);
    if (!s_notes) {
        s_notes = calloc(NOTES_MAX, sizeof(note_t));
    }
    ESP_RETURN_ON_FALSE(s_notes, ESP_ERR_NO_MEM, TAG, "catalogue alloc");

    /* No card is a normal state, not an error: the catalogue is simply empty
     * and fills in if the user inserts one and retries from Settings. */
    notes_reload();
    return ESP_OK;
}

int notes_count(void) { return s_count; }

const note_t *notes_at(int index)
{
    return (index >= 0 && index < s_count) ? &s_notes[index] : NULL;
}

int notes_index_of(uint32_t id)
{
    for (int i = 0; i < s_count; i++) {
        if (s_notes[i].id == id) {
            return i;
        }
    }
    return -1;
}

const note_t *notes_by_id(uint32_t id)
{
    int i = notes_index_of(id);
    return (i < 0) ? NULL : &s_notes[i];
}

bool notes_get(uint32_t id, note_t *out)
{
    lock();
    int i = notes_index_of(id);
    if (i >= 0) {
        *out = s_notes[i];
    }
    unlock();
    return i >= 0;
}

int notes_snapshot(note_t *out, int max)
{
    lock();
    int n = (s_count < max) ? s_count : max;
    if (n > 0) {
        memcpy(out, s_notes, (size_t)n * sizeof(note_t));
    }
    unlock();
    return n;
}

esp_err_t notes_create(note_t *out)
{
    ESP_RETURN_ON_FALSE(storage_mounted(), ESP_ERR_INVALID_STATE, TAG, "no card");

    lock();
    if (s_count >= NOTES_MAX) {
        unlock();
        ESP_LOGE(TAG, "catalogue full (%d)", NOTES_MAX);
        return ESP_ERR_NO_MEM;
    }

    memset(out, 0, sizeof(*out));
    out->id = s_next_id++;
    out->created = time(NULL);
    out->stt = NOTE_STT_PENDING;
    out->temp_c = NAN;
    out->humidity = NAN;
    snprintf(out->tag, sizeof(out->tag), "%s", NOTE_TAGS[0]);
    default_title(out);

    esp_err_t err = write_ini(out);
    if (err == ESP_OK) {
        /* Newest first, so the fresh note goes to the front. */
        memmove(&s_notes[1], &s_notes[0], (size_t)s_count * sizeof(note_t));
        s_notes[0] = *out;
        s_count++;
    } else {
        s_next_id--;
    }
    unlock();
    return err;
}

esp_err_t notes_save(const note_t *note)
{
    lock();
    int i = notes_index_of(note->id);
    if (i >= 0) {
        s_notes[i] = *note;
    }
    esp_err_t err = write_ini(note);
    unlock();
    return err;
}

esp_err_t notes_set_tag(uint32_t id, const char *tag)
{
    lock();
    int i = notes_index_of(id);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (i >= 0) {
        snprintf(s_notes[i].tag, sizeof(s_notes[i].tag), "%s", tag);
        err = write_ini(&s_notes[i]);
    }
    unlock();
    return err;
}

esp_err_t notes_delete(uint32_t id)
{
    char path[NOTE_PATH_MAX];
    static const char *exts[] = { "wav", "txt", "ini" };

    lock();
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        notes_path(id, exts[e], path, sizeof(path));
        remove(path);
    }

    int i = notes_index_of(id);
    if (i >= 0) {
        memmove(&s_notes[i], &s_notes[i + 1], (size_t)(s_count - i - 1) * sizeof(note_t));
        s_count--;
    }
    unlock();
    ESP_LOGI(TAG, "deleted note %" PRIu32, id);
    return ESP_OK;
}

esp_err_t notes_save_text(uint32_t id, const char *text)
{
    char path[NOTE_PATH_MAX];
    notes_path(id, "txt", path, sizeof(path));

    FILE *f = fopen(path, "w");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "open %s", path);
    fputs(text, f);
    fclose(f);

    lock();
    int i = notes_index_of(id);
    if (i >= 0) {
        note_t *n = &s_notes[i];
        n->stt = NOTE_STT_DONE;

        /* Title = the first few words, so the list is scannable without
         * opening anything. Stop at a sentence end if one comes early. */
        size_t out = 0;
        bool prev_space = true;
        for (const char *p = text; *p && out < sizeof(n->title) - 1; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '\n' || c == '\r') {
                break;
            }
            if (isspace(c)) {
                if (prev_space) {
                    continue;
                }
                prev_space = true;
                n->title[out++] = ' ';
                continue;
            }
            prev_space = false;
            n->title[out++] = (char)c;
        }
        while (out > 0 && n->title[out - 1] == ' ') {
            out--;
        }
        n->title[out] = '\0';
        if (n->title[0] == '\0') {
            default_title(n);
        }
        write_ini(n);
    }
    unlock();
    return ESP_OK;
}

int notes_load_text(uint32_t id, char *out, size_t out_len)
{
    char path[NOTE_PATH_MAX];
    notes_path(id, "txt", path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }
    size_t n = fread(out, 1, out_len - 1, f);
    fclose(f);
    out[n] = '\0';
    return (int)n;
}

bool notes_has_text(uint32_t id)
{
    char path[NOTE_PATH_MAX];
    struct stat st;
    notes_path(id, "txt", path, sizeof(path));
    return stat(path, &st) == 0 && st.st_size > 0;
}

uint32_t notes_next_pending(void)
{
    uint32_t id = 0;
    lock();
    /* Oldest first: the catalogue is newest-first, so walk it backwards. */
    for (int i = s_count - 1; i >= 0; i--) {
        if (s_notes[i].stt == NOTE_STT_PENDING || s_notes[i].stt == NOTE_STT_FAILED) {
            id = s_notes[i].id;
            break;
        }
    }
    unlock();
    return id;
}

int notes_pending_count(void)
{
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        if (s_notes[i].stt == NOTE_STT_PENDING || s_notes[i].stt == NOTE_STT_FAILED) {
            n++;
        }
    }
    return n;
}
