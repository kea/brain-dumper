#include "ui.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "audio.h"
#include "board.h"
#include "epd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "storage.h"
#include "ui_port.h"

static const char *TAG = "ui";

#define TTF_PATH   BD_SD_MOUNT_POINT "/font.ttf"
#define TTF_MAX    (512 * 1024)
#define TTF_SIZE   17         /* transcripts */
#define TTF_LIST   20         /* note list rows, matched to the menu font */

/*
 * Vertical budget. 200 px at ~183 DPI is not much, and I1 rendering has no
 * antialiasing to fall back on: the software renderer thresholds at 50 %
 * luminance, so anything under 14 px loses its thin strokes outright. The
 * whole scale is therefore built up from 14 rather than down from it.
 */
#define STATUS_H   20
#define HINT_H     20

/* Home keeps the status bar; every other screen trades it for 22 px of
 * content, which is the difference between five list rows and six. */
#define BODY_Y_HOME 22
#define BODY_H_HOME 158
#define BODY_H_FULL (EPD_H - HINT_H)

#define ROW_H      25
#define HEAD_Y     2
#define SEP_Y      24
#define LIST_Y     28
#define TEXT_Y     28
#define TEXT_H     (BODY_H_FULL - TEXT_Y)
#define TEXT_PAGE  147        /* seven lines of TTF_SIZE, so paging never clips a row */

/*
 * The buttons are named on screen by where they sit on the board, not by what
 * the schematic calls them. Which glyph belongs to which button is the one
 * assumption here that the firmware cannot check for itself; every hint is
 * derived from these two lines, so swapping them flips the whole UI.
 *
 * The arrows identify a button, they do not indicate a direction — so no hint
 * pairs them with a word like "su" or "giu" that would invite the other
 * reading.
 */
#define GLYPH_BOOT LV_SYMBOL_UP     /* BOOT is the upper button */
#define GLYPH_PWR  LV_SYMBOL_DOWN   /* PWR is the lower one */

#define B_CLICK    GLYPH_BOOT
#define B_DOUBLE   GLYPH_BOOT GLYPH_BOOT
#define B_LONG     GLYPH_BOOT " lungo"
#define P_CLICK    GLYPH_PWR
#define P_LONG     GLYPH_PWR " lungo"

static lv_obj_t *s_screen;
static lv_obj_t *s_status;
static lv_obj_t *s_rule;
static lv_obj_t *s_body;
static lv_obj_t *s_hint;

static const lv_font_t *s_font_small = &lv_font_montserrat_14;
static const lv_font_t *s_font_ui    = &lv_font_montserrat_16;
static const lv_font_t *s_font_med   = &lv_font_montserrat_20;
static const lv_font_t *s_font_big   = &lv_font_montserrat_28;
static const lv_font_t *s_font_text  = NULL;   /* set in ui_init() */
static const lv_font_t *s_font_list  = NULL;   /* note titles: needs the accents */
static bool             s_unicode_ok;

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

/*
 * LVGL's bundled Montserrat covers 0x20-0x7F only, so an Italian transcript
 * would render "perche" with a hole in it. If the user drops a TTF on the
 * card we render transcripts with it and everything just works; otherwise we
 * fold the accents down to ASCII for display only. The .txt file on the card
 * always keeps the original UTF-8.
 */
static void load_ttf(void)
{
    struct stat st;
    if (!storage_mounted() || stat(TTF_PATH, &st) != 0) {
        return;
    }
    if (st.st_size <= 0 || st.st_size > TTF_MAX) {
        ESP_LOGW(TAG, "font.ttf is %ld bytes, skipping", (long)st.st_size);
        return;
    }
    uint8_t *data = heap_caps_malloc((size_t)st.st_size, MALLOC_CAP_SPIRAM);
    if (!data) {
        return;
    }
    FILE *f = fopen(TTF_PATH, "rb");
    if (!f || fread(data, 1, (size_t)st.st_size, f) != (size_t)st.st_size) {
        if (f) {
            fclose(f);
        }
        free(data);
        return;
    }
    fclose(f);

    lv_font_t *font = lv_tiny_ttf_create_data(data, (size_t)st.st_size, TTF_SIZE);
    if (!font) {
        free(data);
        return;
    }
    s_font_text = font;
    s_unicode_ok = true;
    ESP_LOGI(TAG, "using %s at %d px for transcripts (full UTF-8)", TTF_PATH, TTF_SIZE);

    /* Note titles are transcript text too, so the list needs the same coverage
     * as the detail screen or every accent turns into a missing glyph. It gets
     * its own instance because a row is a fixed ROW_H tall and the card's font
     * is whatever the user dropped there: if its metrics do not fit, back off
     * a size rather than let the rows collide. */
    lv_font_t *list = lv_tiny_ttf_create_data(data, (size_t)st.st_size, TTF_LIST);
    if (list && list->line_height > ROW_H) {
        lv_tiny_ttf_destroy(list);
        list = lv_tiny_ttf_create_data(data, (size_t)st.st_size, ROW_H - 5);
    }
    if (list) {
        s_font_list = list;
        ESP_LOGI(TAG, "list rows at %d px (line height %d, row %d)",
                 (int)TTF_LIST, (int)list->line_height, ROW_H);
    }
}

/* Fold the Latin-1 supplement down to ASCII, in place, UTF-8 aware. */
static void fold_ascii(char *s)
{
    static const struct { uint16_t cp; const char *ascii; } map[] = {
        { 0xE0, "a'" }, { 0xE1, "a'" }, { 0xE2, "a" },  { 0xE8, "e'" },
        { 0xE9, "e'" }, { 0xEA, "e" },  { 0xEC, "i'" }, { 0xED, "i'" },
        { 0xEE, "i" },  { 0xF2, "o'" }, { 0xF3, "o'" }, { 0xF4, "o" },
        { 0xF9, "u'" }, { 0xFA, "u'" }, { 0xFB, "u" },  { 0xE7, "c" },
        { 0xF1, "n" },  { 0xC0, "A'" }, { 0xC8, "E'" }, { 0xC9, "E'" },
        { 0xCC, "I'" }, { 0xD2, "O'" }, { 0xD9, "U'" },
    };

    char *r = s, *w = s;
    while (*r) {
        unsigned char c = (unsigned char)*r;
        if (c < 0x80) {
            *w++ = *r++;
            continue;
        }
        if ((c & 0xE0) == 0xC0 && (r[1] & 0xC0) == 0x80) {
            uint16_t cp = (uint16_t)(((c & 0x1F) << 6) | (r[1] & 0x3F));
            const char *rep = NULL;
            for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
                if (map[i].cp == cp) {
                    rep = map[i].ascii;
                    break;
                }
            }
            r += 2;
            if (rep) {
                while (*rep) {
                    *w++ = *rep++;
                }
            } else {
                *w++ = '?';
            }
            continue;
        }
        /* 3- and 4-byte sequences: quotes, dashes, emoji. One '?' each. */
        int len = ((c & 0xF0) == 0xE0) ? 3 : 4;
        while (len-- && *r) {
            r++;
        }
        *w++ = '?';
    }
    *w = '\0';
}

/* ------------------------------------------------------------------ */
/* Widget helpers                                                      */
/* ------------------------------------------------------------------ */

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_white(), 0);
    lv_obj_set_style_text_color(o, lv_color_black(), 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *str, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_label_set_text(l, str);
    return l;
}

/* One selectable row: inverted (black fill, white text) when selected, which
 * is the only highlight that survives a 1-bit panel. */
static void row(lv_obj_t *parent, const char *label, bool selected,
                const lv_font_t *font)
{
    lv_obj_t *r = plain(parent);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_height(r, ROW_H);
    lv_obj_set_style_pad_left(r, 3, 0);
    lv_obj_set_style_pad_right(r, 3, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r, selected ? lv_color_black() : lv_color_white(), 0);

    lv_obj_t *l = lv_label_create(r);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, selected ? lv_color_white() : lv_color_black(), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    /* DOTS truncates to the label's *size*, so the height has to be set too:
     * left to grow with its content the label wraps instead, and a two-line
     * row spills over the ones below it. */
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_height(l, ROW_H);
    lv_label_set_text(l, label);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
}

/* Windowed list: `top` comes from the model, which clamp_list() keeps in
 * range, so a menu longer than the panel scrolls instead of overflowing. */
static lv_obj_t *list_box(lv_obj_t *parent, int y)
{
    lv_obj_t *list = plain(parent);
    lv_obj_set_size(list, lv_pct(100), ROW_H * APP_LIST_ROWS);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    return list;
}

static void render_rows(lv_obj_t *parent, int y, const char *const *items,
                        int count, int sel, int top)
{
    lv_obj_t *list = list_box(parent, y);
    for (int i = top; i < count && i < top + APP_LIST_ROWS; i++) {
        row(list, items[i], i == sel, s_font_med);
    }
}

static void separator(lv_obj_t *parent, int y)
{
    lv_obj_t *line = plain(parent);
    lv_obj_set_width(line, lv_pct(100));
    lv_obj_set_height(line, 1);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
}

static void centered(lv_obj_t *parent, const char *str, const lv_font_t *font, int y)
{
    lv_obj_t *l = text(parent, str, font);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
}

/* Home is the only screen with a status bar; everywhere else the body starts
 * at the top of the panel. */
static void set_chrome(bool with_status)
{
    if (with_status) {
        lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_rule, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_body, EPD_W, BODY_H_HOME);
        lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, BODY_Y_HOME);
    } else {
        lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_rule, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_body, EPD_W, BODY_H_FULL);
        lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 0);
    }
}

static void header(const char *title)
{
    centered(s_body, title, s_font_ui, HEAD_Y);
    separator(s_body, SEP_Y);
}

/*
 * The hint bar carries one action per button, left and right, rather than one
 * centred sentence: at 14 px a 200 px line fits about 26 characters, and two
 * short columns say more in that budget than one run-on line.
 */
static void hint(const char *boot, const char *pwr)
{
    lv_obj_clean(s_hint);
    if (boot && *boot) {
        lv_obj_t *l = text(s_hint, boot, s_font_small);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 3, 0);
    }
    if (pwr && *pwr) {
        lv_obj_t *r = text(s_hint, pwr, s_font_small);
        lv_obj_align(r, LV_ALIGN_RIGHT_MID, -3, 0);
    }
}

static void fmt_duration(char *buf, size_t len, uint32_t ms)
{
    uint32_t s = ms / 1000;
    snprintf(buf, len, "%" PRIu32 ":%02" PRIu32, s / 60, s % 60);
}

/* ------------------------------------------------------------------ */
/* Status bar                                                          */
/* ------------------------------------------------------------------ */

static void render_status(const app_model_t *m)
{
    lv_obj_clean(s_status);

    lv_obj_t *left = text(s_status, m->clock, s_font_small);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 3, 0);

    /* Clock on the left, network and battery on the right, and nothing else:
     * at 14 px the width is the scarce resource, and anything else added here
     * is taken out of the digits that have to stay readable. */
    char right[24];
    const char *net_icon = (m->net == NET_CONNECTED) ? LV_SYMBOL_WIFI
                         : (m->net == NET_CONNECTING) ? LV_SYMBOL_REFRESH
                         : LV_SYMBOL_CLOSE;
    if (m->pending > 0) {
        snprintf(right, sizeof(right), "%s%d %s%d%%", net_icon, m->pending,
                 m->charging ? LV_SYMBOL_CHARGE : "", m->battery);
    } else {
        snprintf(right, sizeof(right), "%s %s%d%%", net_icon,
                 m->charging ? LV_SYMBOL_CHARGE : "", m->battery);
    }
    lv_obj_t *r = text(s_status, right, s_font_small);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -3, 0);
}

/* ------------------------------------------------------------------ */
/* Screens                                                             */
/* ------------------------------------------------------------------ */

static void render_home(const app_model_t *m)
{
    char line[64];

    centered(s_body, "BRAIN\nDUMPER", s_font_big, 6);

    if (!m->sd_ok) {
        centered(s_body, "Nessuna\nmicroSD", s_font_ui, 84);
        hint(B_CLICK " menu", "");
        return;
    }

    /* The note count and the transcription backlog are what Home exists to
     * answer, and they used to be set in the two smallest sizes on the panel.
     * The room came from dropping the temperature line: it is still recorded
     * with every note, which is where it is actually worth something. */
    snprintf(line, sizeof(line), "%d note", notes_count());
    centered(s_body, line, s_font_med, 84);

    if (m->pending > 0) {
        snprintf(line, sizeof(line), "%d da trascrivere", m->pending);
    } else if (notes_count() > 0) {
        snprintf(line, sizeof(line), "tutte trascritte");
    } else {
        line[0] = '\0';
    }
    if (line[0]) {
        centered(s_body, line, s_font_ui, 114);
    }

    hint(B_LONG ": REC", B_CLICK " menu");
}

static void render_recording(const app_model_t *m)
{
    char buf[16];
    fmt_duration(buf, sizeof(buf), m->rec_ms);

    centered(s_body, "REC", s_font_ui, 6);
    centered(s_body, buf, s_font_big, 32);

    /* A coarse VU bar: seven blocks is all a 1-bit panel can show honestly,
     * and it only needs to answer "is the mic hearing me". */
    char meter[16] = { 0 };
    int blocks = (m->level * 7) / 100;
    for (int i = 0; i < 7; i++) {
        strcat(meter, i < blocks ? "#" : ".");
    }
    centered(s_body, meter, s_font_big, 82);

    lv_obj_t *box = plain(s_body);
    lv_obj_set_size(box, 160, 16);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 134);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);

    uint32_t max_s = storage_config()->max_record_s;
    int pct = max_s ? (int)((m->rec_ms / 10) / max_s) : 0;
    if (pct > 100) {
        pct = 100;
    }
    lv_obj_t *fill = plain(box);
    lv_obj_set_size(fill, (158 * pct) / 100, 14);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(fill, lv_color_black(), 0);

    hint(B_LONG ": stop", "");
}

static void render_tag_select(const app_model_t *m)
{
    header("Etichetta");
    render_rows(s_body, LIST_Y, NOTE_TAGS, NOTE_TAG_COUNT, m->sel, m->top);
    hint(B_CLICK " ok", P_CLICK " scorri");
}

static const char *const MENU_ITEMS[] = {
    "Note", "Sincronizza", "Impostazioni", "Trasferim. USB", "Info",
};
const int MENU_ITEM_COUNT = sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]);

static void render_menu(const app_model_t *m)
{
    header("Menu");
    render_rows(s_body, LIST_Y, MENU_ITEMS, MENU_ITEM_COUNT, m->sel, m->top);
    hint(B_CLICK " apri", P_CLICK " scorri");
}

static void render_note_list(const app_model_t *m)
{
    char head[32];
    snprintf(head, sizeof(head), "Note (%d)", m->item_count);
    header(head);

    if (m->item_count == 0) {
        centered(s_body, "Nessuna nota", s_font_ui, 80);
        hint(B_DOUBLE " esci", "");
        return;
    }

    lv_obj_t *list = list_box(s_body, LIST_Y);
    for (int i = m->top; i < m->item_count && i < m->top + APP_LIST_ROWS; i++) {
        const note_t *n = notes_at(i);
        if (!n) {
            continue;
        }
        char label[96];
        char dur[12];
        fmt_duration(dur, sizeof(dur), n->duration_ms);

        /* A leading marker beats a trailing one on a narrow panel: it stays
         * visible when the title gets ellipsised. */
        const char *mark = (n->stt == NOTE_STT_DONE) ? "" :
                           (n->stt == NOTE_STT_FAILED) ? "! " : "~ ";
        snprintf(label, sizeof(label), "%s%s %s", mark, dur, n->title);
        if (!s_unicode_ok) {
            fold_ascii(label);
        }
        row(list, label, i == m->sel, s_font_list);
    }
    hint(B_CLICK " apri", P_CLICK " scorri");
}

static void render_note_detail(const app_model_t *m)
{
    const note_t *n = notes_by_id(m->note_id);
    if (!n) {
        centered(s_body, "Nota non trovata", s_font_ui, 80);
        hint(B_DOUBLE " esci", "");
        return;
    }

    char head[64];
    struct tm tm_local;
    time_t created = n->created;
    localtime_r(&created, &tm_local);
    char when[24];
    strftime(when, sizeof(when), "%d/%m %H:%M", &tm_local);

    char dur[12];
    fmt_duration(dur, sizeof(dur), n->duration_ms);
    snprintf(head, sizeof(head), "%s %s %s", when, dur, n->tag);

    lv_obj_t *h = text(s_body, head, s_font_small);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, 3, HEAD_Y);
    separator(s_body, SEP_Y);

    lv_obj_t *box = plain(s_body);
    lv_obj_set_size(box, lv_pct(100), TEXT_H);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, TEXT_Y);

    const char *content;
    char empty[72];
    if (m->text_len > 0) {
        content = m->text;
    } else {
        snprintf(empty, sizeof(empty), "%s",
                 n->stt == NOTE_STT_FAILED ? "Trascrizione fallita.\nRiprova con Sincronizza."
                                           : "In attesa di\ntrascrizione.");
        content = empty;
    }

    lv_obj_t *l = text(box, content, s_font_text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    /* The label is taller than its parent and gets clipped to it; shifting it
     * up by whole pages is the cheapest scroll an e-paper can afford. */
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 3, -(m->text_page * TEXT_PAGE));

    hint(B_CLICK " play", P_CLICK " pagina");
}

static void render_playing(const app_model_t *m)
{
    char pos[16], total[16], line[40];
    fmt_duration(pos, sizeof(pos), m->play_ms);
    fmt_duration(total, sizeof(total), m->play_total);
    snprintf(line, sizeof(line), "%s / %s", pos, total);

    centered(s_body, LV_SYMBOL_PLAY, s_font_big, 30);
    centered(s_body, line, s_font_ui, 84);

    lv_obj_t *box = plain(s_body);
    lv_obj_set_size(box, 160, 16);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);

    int pct = m->play_total ? (int)((m->play_ms * 100) / m->play_total) : 0;
    lv_obj_t *fill = plain(box);
    lv_obj_set_size(fill, (158 * (pct > 100 ? 100 : pct)) / 100, 14);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(fill, lv_color_black(), 0);

    hint(B_CLICK " stop", "");
}

static const char *const CONFIRM_ITEMS[] = { "Annulla", "Elimina" };

static void render_delete_confirm(const app_model_t *m)
{
    const note_t *n = notes_by_id(m->note_id);
    centered(s_body, "Eliminare?", s_font_big, 10);
    if (n) {
        char title[NOTE_TITLE_MAX + 8];
        snprintf(title, sizeof(title), "%s", n->title);
        if (!s_unicode_ok) {
            fold_ascii(title);
        }
        /* The title is transcript text, so it needs the card font here too. */
        centered(s_body, title, s_font_text, 52);
    }
    render_rows(s_body, 110, CONFIRM_ITEMS, 2, m->sel, 0);
    hint(B_CLICK " ok", P_CLICK " scegli");
}

static void render_settings(const app_model_t *m)
{
    const bd_config_t *cfg = storage_config();
    char buf[6][40];
    const char *items[6];

    snprintf(buf[0], sizeof(buf[0]), "Volume: %d%%", m->volume);
    snprintf(buf[1], sizeof(buf[1]), "Mic: %d dB", m->mic_gain_db);
    snprintf(buf[2], sizeof(buf[2]), "Wi-Fi: %s",
             m->net == NET_CONNECTED ? net_ip_str() :
             m->net == NET_CONNECTING ? "in corso" :
             cfg->wifi_ssid[0] ? "assente" : "non conf.");
    snprintf(buf[3], sizeof(buf[3]), "microSD: %s", m->sd_ok ? "ok" : "riprova");
    snprintf(buf[4], sizeof(buf[4]), "Ricarica config");
    snprintf(buf[5], sizeof(buf[5]), "Spegni");
    for (int i = 0; i < 6; i++) {
        items[i] = buf[i];
    }

    header("Impostazioni");
    render_rows(s_body, LIST_Y, items, 6, m->sel, m->top);
    hint(B_CLICK " cambia", P_CLICK " scorri");
}

static void render_info(const app_model_t *m)
{
    char buf[224];
    char env[32];
    uint64_t freemb = storage_free_bytes() >> 20;

    /* Temperature and humidity read better here than on Home: this is the
     * screen you open to ask how the device is doing, and it is the one place
     * with room to say it at a size worth reading. */
    if (isnan(m->temp_c)) {
        snprintf(env, sizeof(env), "n/d");
    } else {
        snprintf(env, sizeof(env), "%d\xC2\xB0""C  %d%% UR", (int)lroundf(m->temp_c),
                 isnan(m->humidity) ? 0 : (int)lroundf(m->humidity));
    }

    /* Deliberately terse: the STT URL alone would fill the panel, and the one
     * thing worth reading here is whether the pieces are alive. */
    snprintf(buf, sizeof(buf),
             "Brain Dumper\n"
             "ESP32-S3 N8R8\n"
             "SD: %s\n"
             "Liberi: %llu MB\n"
             "IP: %s\n"
             "Batt: %.2f V\n"
             "Clima: %s",
             m->sd_ok ? "ok" : "assente",
             (unsigned long long)freemb,
             net_ip_str(),
             board_battery_voltage(),
             env);

    lv_obj_t *l = text(s_body, buf, s_font_ui);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 3, 4);
    hint(B_DOUBLE " esci", "");
}

static void render_transfer(const app_model_t *m)
{
    (void)m;
    centered(s_body, "TRASFERIMENTO", s_font_ui, 12);
    separator(s_body, 36);
    centered(s_body,
             "La microSD e' ora\n"
             "un disco USB.\n\n"
             "Espellila dal PC,\n"
             "poi premi BOOT\n"
             "per riavviare.",
             s_font_small, 48);
    hint(B_CLICK " riavvia", "");
}

/*
 * What the panel keeps showing for as long as the device sleeps, which may be
 * hours: the title and nothing else. A frozen clock, a note count or a button
 * hint would all still be there in the morning, and every one of them would be
 * describing a device that is no longer in that state.
 *
 * The title sits where Home puts it, so waking up does not move it.
 */
static void render_sleep(const app_model_t *m)
{
    (void)m;
    centered(s_body, "BRAIN\nDUMPER", s_font_big, BODY_Y_HOME + 6);
    hint("", "");
}

static void render_message(const app_model_t *m)
{
    centered(s_body, m->msg, s_font_ui, 62);
    if (m->msg2[0]) {
        centered(s_body, m->msg2, s_font_small, 96);
    }
    hint("", "");
}

/* ------------------------------------------------------------------ */

esp_err_t ui_init(void)
{
    if (!ui_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    s_screen = lv_screen_active();
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_screen, s_font_ui, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_status = plain(s_screen);
    lv_obj_set_size(s_status, EPD_W, STATUS_H);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 0);

    s_rule = plain(s_screen);
    lv_obj_set_size(s_rule, EPD_W, 1);
    lv_obj_align(s_rule, LV_ALIGN_TOP_MID, 0, STATUS_H);
    lv_obj_set_style_bg_opa(s_rule, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_rule, lv_color_black(), 0);

    s_body = plain(s_screen);
    lv_obj_set_size(s_body, EPD_W, BODY_H_FULL);
    lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 0);

    s_hint = plain(s_screen);
    lv_obj_set_size(s_hint, EPD_W, HINT_H);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, 0);

    load_ttf();
    if (!s_font_text) {
        s_font_text = s_font_ui;
    }
    if (!s_font_list) {
        s_font_list = s_font_med;
    }

    ui_unlock();
    return ESP_OK;
}

void ui_render(const app_model_t *m)
{
    if (!ui_lock(2000)) {
        ESP_LOGW(TAG, "render skipped, UI busy");
        return;
    }

    set_chrome(m->state == ST_HOME);
    if (m->state == ST_HOME) {
        render_status(m);
    }
    lv_obj_clean(s_body);

    switch (m->state) {
        case ST_HOME:           render_home(m);            break;
        case ST_RECORDING:      render_recording(m);       break;
        case ST_TAG_SELECT:     render_tag_select(m);      break;
        case ST_MENU:           render_menu(m);            break;
        case ST_NOTE_LIST:      render_note_list(m);       break;
        case ST_NOTE_DETAIL:    render_note_detail(m);     break;
        case ST_PLAYING:        render_playing(m);         break;
        case ST_DELETE_CONFIRM: render_delete_confirm(m);  break;
        case ST_SETTINGS:       render_settings(m);        break;
        case ST_INFO:           render_info(m);            break;
        case ST_TRANSFER:       render_transfer(m);        break;
        case ST_MESSAGE:        render_message(m);         break;
        case ST_SLEEP:          render_sleep(m);           break;
    }

    ui_unlock();
    ui_notify();
}
