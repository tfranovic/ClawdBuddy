#ifdef PET_ASCII
#include "pet_home.h"
#include "pet_buddy.h"
#include "pet_hooks.h"
#include "pet_stats_core.h"
#include "../theme.h"
#include "../signals.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);

enum { ROW_MOOD, ROW_FED, ROW_ENERGY, ROW_COUNT };
static const uint8_t     ROW_PIPS[ROW_COUNT]  = { PET_MOOD_MAX, PET_FED_PIPS, PET_ENERGY_MAX };
static const char* const ROW_NAMES[ROW_COUNT] = { "mood", "fed", "energy" };

#define PIP      12
#define PIP_GAP  3
#define ROW_H    28
#define LABEL_W  62      // "energy" in Styrene 16, and a gap

static lv_obj_t* box = NULL;
static lv_obj_t* lbl_name = NULL;
static lv_obj_t* lbl_level = NULL;
static lv_obj_t* lbl_row[ROW_COUNT];
static lv_obj_t* pips[ROW_COUNT][PET_FED_PIPS];
static int       shown_vals[ROW_COUNT] = { -1, -1, -1 };
static int       shown_level = -1, shown_species = -1;
static lv_obj_t* lbl_sessions = NULL;   // the last row, under the last tool
static lv_obj_t* lbl_tool = NULL;
static char      shown_tool[36] = "\x01";
static int       shown_sessions = -2;

static lv_color_t row_color(int row) {
    switch (row) {
    case ROW_MOOD: return lv_color_hex(0xFF2D7A);   // the pets' heart pink
    case ROW_FED:  return THEME_AMBER;
    default:       return THEME_GREEN;
    }
}

static lv_obj_t* label(const lv_font_t* font) {
    lv_obj_t* l = lv_label_create(box);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void refresh(bool force) {
    if (!box) return;
    const int level = pet_level(), species = pet_species_idx();
    if (force || level != shown_level || species != shown_species) {
        shown_level = level;
        shown_species = species;
        lv_label_set_text(lbl_name, pet_species_name());
        char buf[16];
        snprintf(buf, sizeof(buf), "Lv %d", level);
        lv_label_set_text(lbl_level, buf);
        lv_obj_align(lbl_level, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    // Sessions with a hook event in the last 5 minutes (payload `n`).
    const int n = signals_sessions();
    if (force || n != shown_sessions) {
        shown_sessions = n;
        if (n < 0) lv_label_set_text(lbl_sessions, "");
        else lv_label_set_text_fmt(lbl_sessions, n == 1 ? "%d active session" : "%d active sessions", n);
    }
    const char* tool = pet_last_tool();
    if (force || strcmp(tool, shown_tool) != 0) {
        snprintf(shown_tool, sizeof(shown_tool), "%s", tool);
        lv_label_set_text(lbl_tool, tool);
    }
    const int vals[ROW_COUNT] = { pet_mood(), pet_fed(), pet_energy() };
    for (int r = 0; r < ROW_COUNT; r++) {
        if (!force && vals[r] == shown_vals[r]) continue;
        shown_vals[r] = vals[r];
        for (int i = 0; i < ROW_PIPS[r]; i++)
            lv_obj_set_style_bg_color(pips[r][i], i < vals[r] ? row_color(r) : THEME_BAR_BG, 0);
    }
}

void pet_home_create(lv_obj_t* parent, int x, int y, int w, int ax, int ay, int aw) {
    if (box) return;
    box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, 32 + ROW_COUNT * ROW_H);
    lv_obj_clear_flag(box, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));

    // The activity rows under the status word: one line each, cut with dots.
    lbl_tool = lv_label_create(parent);
    lbl_sessions = lv_label_create(parent);
    lv_obj_t* const lines[] = { lbl_tool, lbl_sessions };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_text_font(lines[i], &font_styrene_16, 0);
        lv_obj_clear_flag(lines[i], LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_long_mode(lines[i], LV_LABEL_LONG_DOT);
        lv_obj_set_size(lines[i], aw, lv_font_get_line_height(&font_styrene_16));
        lv_obj_set_style_text_align(lines[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(lines[i], ax, ay + i * 17);
        lv_label_set_text(lines[i], "");
    }

    lbl_name = label(&font_styrene_20);
    lv_obj_set_pos(lbl_name, 0, 0);
    lbl_level = label(&font_styrene_20);

    for (int r = 0; r < ROW_COUNT; r++) {
        const int ry = 34 + r * ROW_H;
        lbl_row[r] = label(&font_styrene_16);
        lv_label_set_text(lbl_row[r], ROW_NAMES[r]);
        lv_obj_set_pos(lbl_row[r], 0, ry - (lv_font_get_line_height(&font_styrene_16) - PIP) / 2);
        for (int i = 0; i < ROW_PIPS[r]; i++) {
            lv_obj_t* p = lv_obj_create(box);
            lv_obj_remove_style_all(p);
            lv_obj_set_pos(p, LABEL_W + i * (PIP + PIP_GAP), ry);
            lv_obj_set_size(p, PIP, PIP);
            lv_obj_set_style_radius(p, PIP / 4, 0);
            lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
            lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
            pips[r][i] = p;
        }
    }
    pet_home_repaint();
}

void pet_home_repaint(void) {
    if (!box) return;
    lv_obj_set_style_text_color(lbl_name, THEME_TEXT, 0);
    lv_obj_set_style_text_color(lbl_level, THEME_DIM, 0);
    lv_obj_set_style_text_color(lbl_sessions, THEME_DIM, 0);
    lv_obj_set_style_text_color(lbl_tool, THEME_TEXT, 0);
    for (int r = 0; r < ROW_COUNT; r++) lv_obj_set_style_text_color(lbl_row[r], THEME_DIM, 0);
    refresh(true);
}

void pet_home_tick(void) { refresh(false); }

#endif  // PET_ASCII
