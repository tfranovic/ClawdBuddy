#ifdef PET_ASCII
#include "team_view.h"
#include "team_core.h"
#include "../ui.h"
#include <esp_heap_caps.h>
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(font_styrene_12);

#define TICK_MS  66          // 15 frames a second

static const uint32_t COL_TEXT = 0xD6D0C4, COL_DIM = 0x8F897D, COL_ALERT = 0xFF8A5B;

static lv_obj_t*      parent = NULL;
static lv_obj_t*      root = NULL;
static lv_obj_t*      img = NULL;
static lv_obj_t*      lbl_name[TEAM_DESKS];
static lv_obj_t*      lbl_bar = NULL;
static lv_timer_t*    timer = NULL;
static lv_image_dsc_t dsc;
static uint16_t*      frame = NULL;      // TEAM_W x TEAM_H
static bool           enabled = false, visible = false;
static int            office = 0;        // the one on screen
static int            pet_bottom = 0;    // above this the meters' taps are the pet's
static char           alert[64];

static void set_text(lv_obj_t* l, const char* s, uint32_t color) {
    if (strcmp(lv_label_get_text(l), s) != 0) lv_label_set_text(l, s);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
}

static void labels(void) {
    char buf[48];
    for (int i = 0; i < TEAM_DESKS; i++) {
        TeamDeskInfo d;
        team_desk_info(office, i, &d);
        // Sub-agents the bench has no seat for are counted after the name.
        if (d.more > 0) snprintf(buf, sizeof(buf), "%s  +%d", d.name, d.more);
        else snprintf(buf, sizeof(buf), "%s", d.name);
        set_text(lbl_name[i], buf,
                 d.state == 'q' ? COL_ALERT : (d.state == 'i' || d.state == 'b') ? COL_DIM : COL_TEXT);
    }
    if (alert[0]) { set_text(lbl_bar, alert, COL_ALERT); return; }
    const int s = team_sessions(), g = team_agents(), n = team_offices();
    if (s == 0) snprintf(buf, sizeof(buf), "Nobody in yet");
    else if (n > 1) snprintf(buf, sizeof(buf), "Office %d of %d:  %d sessions,  %d sub-agent%s", office + 1, n, s, g, g == 1 ? "" : "s");
    else snprintf(buf, sizeof(buf), "%d session%s,  %d sub-agent%s", s, s == 1 ? "" : "s", g, g == 1 ? "" : "s");
    set_text(lbl_bar, buf, COL_DIM);
}

static void paint(void) {
    team_set_time(ui_clock_seconds());
    // An office that emptied is gone: fall back to the last one.
    if (office >= team_offices()) office = team_offices() - 1;
    team_render(frame, office);
    labels();
    lv_obj_invalidate(img);
}

static void tick_cb(lv_timer_t*) {
    if (!visible) return;
    team_step(TICK_MS);
    paint();
}

static lv_obj_t* label(int x, int y, int w, lv_text_align_t align) {
    lv_obj_t* l = lv_label_create(root);
    lv_obj_set_style_text_font(l, &font_styrene_12, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, w, lv_font_get_line_height(&font_styrene_12));
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

// The frame (460 KB) is taken the first time the view comes up.
static bool create(void) {
    if (root) return true;
    if (!parent) return false;
    const size_t bytes = (size_t)TEAM_W * TEAM_H * 2;
    if (!frame) frame = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!frame) frame = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!frame) return false;
    memset(&dsc, 0, sizeof(dsc));
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.w = TEAM_W;
    dsc.header.h = TEAM_H;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.stride = TEAM_W * 2;
    dsc.data = (const uint8_t*)frame;
    dsc.data_size = bytes;

    root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, TEAM_W, TEAM_H);
    lv_obj_clear_flag(root, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    img = lv_image_create(root);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(img, 0, 0);
    lv_image_set_src(img, &dsc);
    for (int i = 0; i < TEAM_DESKS; i++) {
        TeamDeskInfo d;
        team_desk_info(0, i, &d);
        lbl_name[i] = label(d.name_cx - 76, d.name_y, 152, LV_TEXT_ALIGN_CENTER);
    }
    const int bar = team_bar_y();
    lbl_bar = label(8, bar + (TEAM_H - bar - lv_font_get_line_height(&font_styrene_12)) / 2, TEAM_W - 16,
                    LV_TEXT_ALIGN_CENTER);
    timer = lv_timer_create(tick_cb, TICK_MS, NULL);
    return true;
}

void team_view_init(lv_obj_t* scr, int home_bottom) {
    parent = scr;
    pet_bottom = home_bottom;
    team_reset();
}

bool team_view_visible(void) { return visible; }

void team_view_show(bool on) {
    if (on == visible) return;
    if (on) {
        if (!create()) return;
        team_snap();
        paint();
        lv_obj_move_foreground(root);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    } else if (root) {
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    }
    visible = on;
    Serial.printf("[%lu] team view: %s\n", (unsigned long)millis(), on ? "shown" : "hidden");
}

void team_view_on_payload(const UsageData* d) {
    if (d->team_view >= 0) {
        const bool on = d->team_view != 0;
        if (enabled && !on) {
            team_view_show(false);
            team_reset();
        }
        enabled = on;
    }
    if (d->has_team) {
        TeamDeskIn in[TEAM_MAX];
        team_set(in, team_parse(d->team, in, TEAM_MAX));
        // Off screen nobody walks: arrivals are simply there next time.
        if (!visible) team_snap();
    }
}

void team_view_alert(const char* msg) { snprintf(alert, sizeof(alert), "%s", msg ? msg : ""); }

bool team_view_tap(int x, int y) {
    if (!parent) return false;
    const bool side = x < TEAM_W / 3 || x >= TEAM_W - TEAM_W / 3;
    const bool right = x >= TEAM_W - TEAM_W / 3;
    // The views in a ring: the meters, then each office.
    if (visible) {
        if (!side) return true;
        const int next = office + (right ? 1 : -1);
        if (next < 0 || next >= team_offices()) team_view_show(false);
        else { office = next; paint(); }
        return true;
    }
    // One view only: every tap is the pet's, as it was.
    if (!enabled) return false;
    if (y < 0 || y < pet_bottom) return false;     // the pet and its stats: the peek
    if (side) {
        office = right ? 0 : team_offices() - 1;
        team_view_show(true);
    }
    return true;
}

#endif  // PET_ASCII
