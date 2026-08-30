#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

/*
 * The note catalogue.
 *
 * On disk each note is three sibling files under /sdcard/notes:
 *
 *     0042.wav   16 kHz / 16 bit / mono PCM
 *     0042.txt   UTF-8 transcript (absent until the STT service answers)
 *     0042.ini   key=value metadata, the file the catalogue is built from
 *
 * Keeping the metadata next to the audio means there is no central index to
 * corrupt: the catalogue is rebuilt by scanning the directory at boot, and a
 * note the user copies off the card over USB is self-describing.
 */

#define NOTE_TAG_MAX    24
#define NOTE_TITLE_MAX  56
#define NOTE_PATH_MAX   64
#define NOTES_MAX       512

typedef enum {
    NOTE_STT_PENDING = 0,   /* recorded, waiting for a network + STT service */
    NOTE_STT_DONE,
    NOTE_STT_FAILED,        /* the service answered, but with an error */
    NOTE_STT_SKIPPED,       /* user asked not to transcribe this one */
} note_stt_t;

typedef struct {
    uint32_t   id;
    time_t     created;      /* UTC */
    uint32_t   duration_ms;
    uint32_t   bytes;
    float      temp_c;       /* NAN when the sensor was unavailable */
    float      humidity;
    note_stt_t stt;
    char       tag[NOTE_TAG_MAX];
    char       title[NOTE_TITLE_MAX];   /* first line of the transcript */
} note_t;

/* Tags the UI offers; the first one is the default. */
extern const char *const NOTE_TAGS[];
extern const int         NOTE_TAG_COUNT;

/* Scans /sdcard/notes and builds the in-RAM catalogue (newest first). */
esp_err_t notes_init(void);
esp_err_t notes_reload(void);

int           notes_count(void);
const note_t *notes_at(int index);          /* 0 = newest */
const note_t *notes_by_id(uint32_t id);
int           notes_index_of(uint32_t id);

/*
 * The same lookups, but copied out under the catalogue lock.
 *
 * notes_at() and notes_by_id() hand back pointers straight into the array,
 * which is safe for the app task that owns both the UI and every delete it
 * performs. It is not safe for anyone else: a delete shifts every entry after
 * it, so a pointer taken on another task can end up describing a different
 * note. Tasks that do not own the catalogue take copies.
 */
bool notes_get(uint32_t id, note_t *out);
int  notes_snapshot(note_t *out, int max);      /* returns entries written */

/* Path helper. ext is "wav", "txt" or "ini". */
void notes_path(uint32_t id, const char *ext, char *out, size_t out_len);

/* Reserve the next free id and write its .ini. The .wav is the caller's job. */
esp_err_t notes_create(note_t *out);

/* Persist a note's metadata and refresh its catalogue entry. */
esp_err_t notes_save(const note_t *note);

esp_err_t notes_set_tag(uint32_t id, const char *tag);
esp_err_t notes_delete(uint32_t id);

/* Transcript I/O. notes_save_text() also derives the note title. */
esp_err_t notes_save_text(uint32_t id, const char *text);
int       notes_load_text(uint32_t id, char *out, size_t out_len);  /* bytes, <0 on error */
bool      notes_has_text(uint32_t id);

/* Oldest note still waiting to be transcribed, or 0 when the queue is empty. */
uint32_t  notes_next_pending(void);
int       notes_pending_count(void);
