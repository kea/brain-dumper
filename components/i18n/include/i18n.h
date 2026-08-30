#pragma once

#include <stdint.h>

/*
 * Interface language.
 *
 * One table per language, looked up by symbolic id. The active language comes
 * from `lang=` in /sdcard/config.ini and can change at runtime (Settings ->
 * Reload config), so nothing may cache a translated pointer across renders:
 * tr() is cheap, call it where the string is used.
 *
 * Everything here is read-only after i18n_set_lang(), which runs once on the
 * main task before the UI exists, so no locking is needed.
 */

typedef enum {
    BD_LANG_EN = 0,
    BD_LANG_FR,
    BD_LANG_DE,
    BD_LANG_ES,
    BD_LANG_IT,
    BD_LANG_COUNT,
} bd_lang_t;

typedef enum {
    /* Button hints. The glyph is added by the UI; these are the words. */
    STR_HINT_LONG,          /* "long", as in "<glyph> long: REC" */
    STR_HINT_MENU,
    STR_HINT_REC,
    STR_HINT_STOP,
    STR_HINT_OK,
    STR_HINT_SCROLL,
    STR_HINT_OPEN,
    STR_HINT_BACK,
    STR_HINT_PLAY,
    STR_HINT_PAGE,
    STR_HINT_CHANGE,
    STR_HINT_CHOOSE,
    STR_HINT_REBOOT,

    /* Home */
    STR_NO_SD_2LINE,        /* two lines, the panel is 200 px wide */
    STR_NOTES_ONE_FMT,      /* one %d */
    STR_NOTES_MANY_FMT,     /* one %d */
    STR_PENDING_FMT,        /* one %d */
    STR_ALL_TRANSCRIBED,

    /* Recording */
    STR_REC,

    /* Tag picker */
    STR_TAG_TITLE,

    /* Menu */
    STR_MENU_TITLE,
    STR_MENU_NOTES,
    STR_MENU_SYNC,
    STR_MENU_SETTINGS,
    STR_MENU_TRANSFER,
    STR_MENU_INFO,

    /* Note list and detail */
    STR_NO_NOTES,
    STR_NOTE_NOT_FOUND,
    STR_STT_FAILED_LONG,
    STR_STT_WAITING,

    /* Delete confirmation */
    STR_DELETE_Q,
    STR_CANCEL,
    STR_DELETE,

    /* Settings */
    STR_SET_VOLUME,
    STR_SET_MIC,
    STR_SET_WIFI,
    STR_SET_SD,
    STR_SET_RELOAD_CFG,
    STR_SET_POWER_OFF,
    STR_WIFI_CONNECTING,
    STR_WIFI_ABSENT,
    STR_WIFI_UNSET,
    STR_OK,
    STR_RETRY,

    /* Info */
    STR_INFO_SD,
    STR_INFO_FREE,
    STR_INFO_IP,
    STR_INFO_BATT,
    STR_INFO_CLIMATE,
    STR_ABSENT,
    STR_NOT_AVAILABLE,
    STR_ENV_FMT,            /* two %d: temperature, relative humidity */

    /* USB transfer */
    STR_TRANSFER_TITLE,
    STR_TRANSFER_BODY,

    /* Transient messages */
    STR_MSG_NO_SD,
    STR_MSG_CANNOT_RECORD,
    STR_MSG_ERROR,
    STR_MSG_NOTE_CREATE_FAILED,
    STR_MSG_MIC_UNAVAILABLE,
    STR_MSG_TOO_SHORT,
    STR_MSG_DISCARDED,
    STR_MSG_SYNC,
    STR_MSG_IN_PROGRESS,
    STR_MSG_NOTHING_TO_DO,
    STR_MSG_SD_UNAVAILABLE,
    STR_MSG_WIFI,
    STR_MSG_CONNECTING,
    STR_MSG_SD,
    STR_MSG_MOUNTED,
    STR_MSG_CONFIG,
    STR_MSG_RELOADED,
    STR_MSG_SAVED,
    STR_MSG_DELETED,
    STR_MSG_AUDIO_UNAVAILABLE,

    /* Tag names, in the order of NOTE_TAGS */
    STR_TAG_INBOX,
    STR_TAG_IDEA,
    STR_TAG_TODO,
    STR_TAG_WORK,
    STR_TAG_PERSONAL,

    STR_COUNT,
} bd_str_t;

/*
 * Select a language from a two-letter code ("en", "fr", "de", "es", "it").
 * Case-insensitive, and tolerant of a locale tail: "it_IT" and "it-IT" both
 * pick Italian. Anything else falls back to English with a warning.
 */
void i18n_set_lang(const char *code);

bd_lang_t   i18n_lang(void);
const char *i18n_lang_code(void);   /* "it" */
const char *i18n_lang_name(void);   /* the endonym, e.g. "Italiano" */

/* The string for the active language; never NULL. Falls back to English if a
 * table has a hole in it, which is why the English table is the complete one. */
const char *tr(bd_str_t id);

/*
 * The display name of a stored tag.
 *
 * Tags are written to the card in their canonical English form so a card stays
 * readable whatever language the device is set to; only the display is
 * translated. An unknown tag - one an older firmware or a hand-edited .ini put
 * there - is returned unchanged.
 */
const char *i18n_tag(const char *canonical);

/*
 * ASCII stand-in for one Latin-1 code point, for the panel's built-in font.
 *
 * Language-aware because the conventions differ and a shared table would read
 * wrong in most of them: Italian writes "perche'" for "perché", German writes
 * "Loeschen" for "Löschen", and everyone else simply drops the accent.
 * Returns NULL for a code point with no sensible ASCII form.
 */
const char *i18n_fold_cp(uint32_t cp);
