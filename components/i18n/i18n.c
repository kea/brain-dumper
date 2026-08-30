#include "i18n.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "notes.h"

static const char *TAG = "i18n";

#include "lang/en.h"
#include "lang/fr.h"
#include "lang/de.h"
#include "lang/es.h"
#include "lang/it.h"

static const struct {
    const char         *code;
    const char         *name;
    const char *const  *table;
} LANGS[BD_LANG_COUNT] = {
    [BD_LANG_EN] = { "en", "English",  S_EN },
    [BD_LANG_FR] = { "fr", "Français", S_FR },
    [BD_LANG_DE] = { "de", "Deutsch",  S_DE },
    [BD_LANG_ES] = { "es", "Español",  S_ES },
    [BD_LANG_IT] = { "it", "Italiano", S_IT },
};

static bd_lang_t s_lang = BD_LANG_EN;

void i18n_set_lang(const char *code)
{
    if (!code || !code[0]) {
        s_lang = BD_LANG_EN;
        return;
    }
    /* "it", "it_IT" and "it-IT" all mean the same thing: only the two letters
     * in front carry any meaning here. */
    for (int i = 0; i < BD_LANG_COUNT; i++) {
        if (!strncasecmp(code, LANGS[i].code, 2) &&
            (code[2] == '\0' || code[2] == '_' || code[2] == '-')) {
            if (s_lang != (bd_lang_t)i) {
                ESP_LOGI(TAG, "interface language: %s (%s)", LANGS[i].name, LANGS[i].code);
            }
            s_lang = (bd_lang_t)i;
            return;
        }
    }
    ESP_LOGW(TAG, "unknown language '%s', falling back to English", code);
    s_lang = BD_LANG_EN;
}

bd_lang_t   i18n_lang(void)      { return s_lang; }
const char *i18n_lang_code(void) { return LANGS[s_lang].code; }
const char *i18n_lang_name(void) { return LANGS[s_lang].name; }

const char *tr(bd_str_t id)
{
    if ((unsigned)id >= (unsigned)STR_COUNT) {
        return "";
    }
    const char *s = LANGS[s_lang].table[id];
    return s ? s : S_EN[id];
}

const char *i18n_tag(const char *canonical)
{
    if (!canonical) {
        return "";
    }
    /* STR_TAG_INBOX..STR_TAG_PERSONAL are laid out in the order of NOTE_TAGS,
     * so the index into one is the offset into the other. */
    const int translated = STR_TAG_PERSONAL - STR_TAG_INBOX + 1;
    for (int i = 0; i < NOTE_TAG_COUNT && i < translated; i++) {
        if (!strcmp(canonical, NOTE_TAGS[i])) {
            return tr((bd_str_t)(STR_TAG_INBOX + i));
        }
    }
    return canonical;   /* hand-edited, or written by an older firmware */
}

/* ------------------------------------------------------------------ */
/* ASCII folding                                                       */
/* ------------------------------------------------------------------ */

/*
 * The base map: drop the accent and keep the letter. Ranges rather than a
 * per-code-point table because that is exactly how the Latin-1 supplement is
 * laid out, and the ranges say why each group belongs together.
 */
static const char *fold_base(uint32_t cp)
{
    if (cp >= 0xC0 && cp <= 0xC5) return "A";
    if (cp == 0xC6)               return "AE";
    if (cp == 0xC7)               return "C";
    if (cp >= 0xC8 && cp <= 0xCB) return "E";
    if (cp >= 0xCC && cp <= 0xCF) return "I";
    if (cp == 0xD1)               return "N";
    if (cp >= 0xD2 && cp <= 0xD6) return "O";
    if (cp == 0xD8)               return "O";
    if (cp >= 0xD9 && cp <= 0xDC) return "U";
    if (cp == 0xDD)               return "Y";
    if (cp == 0xDF)               return "ss";
    if (cp >= 0xE0 && cp <= 0xE5) return "a";
    if (cp == 0xE6)               return "ae";
    if (cp == 0xE7)               return "c";
    if (cp >= 0xE8 && cp <= 0xEB) return "e";
    if (cp >= 0xEC && cp <= 0xEF) return "i";
    if (cp == 0xF1)               return "n";
    if (cp >= 0xF2 && cp <= 0xF6) return "o";
    if (cp == 0xF8)               return "o";
    if (cp >= 0xF9 && cp <= 0xFC) return "u";
    if (cp == 0xFD || cp == 0xFF) return "y";

    /* Punctuation that only decorates: better gone than turned into a '?'. */
    if (cp == 0xA1 || cp == 0xBF)  return "";     /* inverted ! and ? */
    if (cp == 0xB0)                return "";     /* degree sign */
    if (cp == 0xA0)                return " ";    /* no-break space */
    return NULL;
}

const char *i18n_fold_cp(uint32_t cp)
{
    if (s_lang == BD_LANG_IT) {
        /* An Italian reader types the accent as a trailing apostrophe when the
         * keyboard will not give them one, so "perche'" reads as the word it
         * is. This is the fallback the README documents. */
        switch (cp) {
            case 0xE0: case 0xE1: return "a'";
            case 0xE8: case 0xE9: return "e'";
            case 0xEC: case 0xED: return "i'";
            case 0xF2: case 0xF3: return "o'";
            case 0xF9: case 0xFA: return "u'";
            case 0xC0: case 0xC1: return "A'";
            case 0xC8: case 0xC9: return "E'";
            case 0xCC: case 0xCD: return "I'";
            case 0xD2: case 0xD3: return "O'";
            case 0xD9: case 0xDA: return "U'";
            default: break;
        }
    } else if (s_lang == BD_LANG_DE) {
        /* German has a real transliteration and readers expect it: dropping
         * the umlaut outright gives "Loschen", which is a different word. */
        switch (cp) {
            case 0xE4: return "ae";
            case 0xF6: return "oe";
            case 0xFC: return "ue";
            case 0xC4: return "Ae";
            case 0xD6: return "Oe";
            case 0xDC: return "Ue";
            default: break;
        }
    }
    return fold_base(cp);
}
