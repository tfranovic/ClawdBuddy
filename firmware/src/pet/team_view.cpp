#ifdef PET_ASCII
#include "team_view.h"
#include "team_core.h"
#include "../ui.h"
#include "../ble.h"
#include <esp_heap_caps.h>
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_16);

#define TICK_MS      66      // 15 frames a second
#define CARD_ASK_MS  3000    // a detail card on screen is asked for again this often
#define CARD_LINES   4

static const uint32_t COL_TEXT = 0xD6D0C4, COL_DIM = 0x8F897D, COL_ALERT = 0xFF8A5B;
static const uint32_t COL_DONE = 0x6EC878, COL_FAIL = 0xE85C50;

static lv_obj_t*      parent = NULL;
static lv_obj_t*      root = NULL;
static lv_obj_t*      img = NULL;
static lv_obj_t*      lbl_name[TEAM_DESKS];
static lv_obj_t*      lbl_usage = NULL;   // the strip's left: the plan's usage
static lv_obj_t*      lbl_bar = NULL;     // its right: the alert, or who is in
static lv_obj_t*      card = NULL;        // one desk's details, over the scene
static lv_obj_t*      card_title = NULL;
static lv_obj_t*      card_line[CARD_LINES];
static lv_timer_t*    timer = NULL;
static lv_image_dsc_t dsc;
static uint16_t*      frame = NULL;      // TEAM_W x TEAM_H
static uint16_t*      room = NULL;       // the room's fixed parts (team_set_cache)
static bool           enabled = false, visible = false;
static int            office = 0;        // the one on screen
static int            pet_bottom = 0;    // above this the meters' taps are the pet's
static char           alert[64];
static bool           alert_seen = false;   // the alert's office has been looked for
static int            office_before = -1;   // where to go back to when the alert ends
static int            link_state = 2;             // 0 not connected, 1 no fresh data, 2 live
static char           usage[40];
static bool           usage_hot = false;    // a limit is used up
static int            card_id = -1;         // the desk whose card is up
static uint32_t       card_asked_ms = 0;
static uint32_t       spent_us = 0, frames = 0;

static void set_text(lv_obj_t* l, const char* s, uint32_t color) {
    if (strcmp(lv_label_get_text(l), s) != 0) lv_label_set_text(l, s);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
}

static uint32_t state_color(char s) {
    return s == 'q' ? COL_ALERT : s == 'f' ? COL_DONE : s == 'e' ? COL_FAIL
         : (s == 'i' || s == 'b') ? COL_DIM : COL_TEXT;
}

static const char* state_words(char s) {
    switch (s) {
    case 'w': return "Working";
    case 't': return "Thinking";
    case 'q': return "Waiting for you";
    case 'd': return "Waiting for sub-agents";
    case 'c': return "Compacting";
    case 'f': return "Done";
    case 'e': return "Stopped on an error";
    case 'b': return "On a break";
    default:  return "Idle";
    }
}

static void labels(void) {
    char buf[64];
    const bool shut = team_dark();
    for (int i = 0; i < TEAM_DESKS; i++) {
        TeamDeskInfo d;
        team_desk_info(office, i, &d);
        // The skin decides where a name goes (and a cubicle office has four).
        lv_obj_set_pos(lbl_name[i], d.name_cx - 76, d.name_y);
        // Sub-agents the bench has no seat for are counted after the name.
        if (shut) buf[0] = '\0';
        else if (d.more > 0) snprintf(buf, sizeof(buf), "%s  +%d", d.name, d.more);
        else snprintf(buf, sizeof(buf), "%s", d.name);
        set_text(lbl_name[i], buf, state_color(d.state));
    }
    set_text(lbl_usage, shut ? "" : usage, usage_hot ? COL_ALERT : COL_DIM);
    if (link_state == 0) { set_text(lbl_bar, "Not connected", COL_DIM); return; }
    if (link_state == 1) { set_text(lbl_bar, "No word from the computer", COL_DIM); return; }
    if (alert[0]) { set_text(lbl_bar, alert, COL_ALERT); return; }
    const int s = team_sessions(), g = team_agents(), n = team_offices();
    if (s == 0) snprintf(buf, sizeof(buf), "Nobody in yet");
    else if (n > 1) snprintf(buf, sizeof(buf), "Office %d of %d:  %d sessions,  %d sub-agent%s", office + 1, n, s, g, g == 1 ? "" : "s");
    else snprintf(buf, sizeof(buf), "%d session%s,  %d sub-agent%s", s, s == 1 ? "" : "s", g, g == 1 ? "" : "s");
    set_text(lbl_bar, buf, COL_DIM);
}

static void card_close(void) {
    card_id = -1;
    if (card) lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
}

// The card for a desk of the office on screen: what the board knows at
// once (the name, the state), and a request to the app for the rest.
static void card_open(int desk) {
    TeamDeskInfo d;
    team_desk_info(office, desk, &d);
    if (!d.live || !card) return;
    card_id = d.id;
    set_text(card_title, d.name, COL_TEXT);
    set_text(card_line[0], state_words(d.state), state_color(d.state));
    char buf[32] = "";
    if (d.ctx >= 0) snprintf(buf, sizeof(buf), "context %d%%", d.ctx * 100 / TEAM_CTX_FULL);
    set_text(card_line[1], buf, COL_DIM);
    for (int i = 2; i < CARD_LINES; i++) set_text(card_line[i], "", COL_DIM);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(card);
    ble_send_desk(d.id);
    card_asked_ms = millis();
}

// `td`: "a3|title|state|about|team|foot". The id alone: the desk is gone.
static void card_fill(const char* td) {
    if (card_id < 0 || strlen(td) < 2) return;
    unsigned id = 0;
    if (sscanf(td, "%2x", &id) != 1 || (int)id != card_id) return;
    if (td[2] != '|') { card_close(); return; }
    char text[208];
    snprintf(text, sizeof(text), "%s", td + 3);
    char* part = text;
    for (int i = -1; i < CARD_LINES; i++) {
        char* end = part ? strchr(part, '|') : NULL;
        if (end) *end = '\0';
        const char* s = part ? part : "";
        if (i < 0) set_text(card_title, s, COL_TEXT);
        else if (i == 0) lv_label_set_text(card_line[0], s);      // keeps the state's colour
        else set_text(card_line[i], s, i == 1 ? COL_TEXT : COL_DIM);
        part = end ? end + 1 : NULL;
    }
}

static void paint(void) {
    team_set_time(ui_clock_seconds());
    // An office that emptied is gone: fall back to the last one.
    if (office >= team_offices()) office = team_offices() - 1;
    // An alert for a desk in another office: go and show it, once.
    if (alert[0] && !alert_seen) {
        const int w = team_office_waiting();
        if (w >= 0) {
            alert_seen = true;
            if (w != office) { office_before = office; office = w; card_close(); }
        }
    }
    team_render(frame, office);
    labels();
    lv_obj_invalidate(img);
}

static void tick_cb(lv_timer_t*) {
    if (!visible) return;
    const uint32_t t0 = micros();
    team_step(TICK_MS);
    paint();
    // What a frame costs, for the serial log: the scene is drawn in software.
    spent_us += micros() - t0;
    if (++frames >= 300) {
        Serial.printf("[%lu] team view: %lu us a frame\n", (unsigned long)millis(), (unsigned long)(spent_us / frames));
        spent_us = frames = 0;
    }
    if (card_id >= 0 && millis() - card_asked_ms >= CARD_ASK_MS) {
        ble_send_desk((uint8_t)card_id);
        card_asked_ms = millis();
    }
}

static lv_obj_t* label(lv_obj_t* on, const lv_font_t* font, int x, int y, int w, lv_text_align_t align) {
    lv_obj_t* l = lv_label_create(on);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

// The frame (460 KB) is taken the first time the view comes up, and as much
// again for the room's fixed parts when there is the memory for it.
static bool create(void) {
    if (root) return true;
    if (!parent) return false;
    const size_t bytes = (size_t)TEAM_W * TEAM_H * 2;
    if (!frame) frame = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!frame) frame = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!frame) return false;
    if (!room) room = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    team_set_cache(room);
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
        lbl_name[i] = label(root, &font_styrene_12, d.name_cx - 76, d.name_y, 152, LV_TEXT_ALIGN_CENTER);
    }
    const int bar = team_bar_y();
    const int by = bar + (TEAM_H - bar - lv_font_get_line_height(&font_styrene_14)) / 2;
    lbl_usage = label(root, &font_styrene_14, 8, by, 150, LV_TEXT_ALIGN_LEFT);
    lbl_bar = label(root, &font_styrene_14, 162, by, TEAM_W - 170, LV_TEXT_ALIGN_RIGHT);

    card = lv_obj_create(root);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, 30, 150);
    lv_obj_set_size(card, TEAM_W - 60, 172);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1A18), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x6A6156), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_clear_flag(card, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    card_title = label(card, &font_styrene_16, 18, 16, TEAM_W - 96, LV_TEXT_ALIGN_LEFT);
    for (int i = 0; i < CARD_LINES; i++)
        card_line[i] = label(card, &font_styrene_14, 18, 50 + i * 26, TEAM_W - 96, LV_TEXT_ALIGN_LEFT);
    timer = lv_timer_create(tick_cb, TICK_MS, NULL);
    return true;
}

void team_view_init(lv_obj_t* scr, int home_bottom) {
    parent = scr;
    pet_bottom = home_bottom;
    team_reset();
    team_set_seed((uint32_t)micros());
}

bool team_view_visible(void) { return visible; }

void team_view_show(bool on) {
    if (on == visible) return;
    if (on) {
        if (!create()) return;
        team_set_dark(link_state != 2);
        team_snap();
        paint();
        lv_obj_move_foreground(root);
        lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    } else if (root) {
        card_close();
        lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    }
    visible = on;
    Serial.printf("[%lu] team view: %s\n", (unsigned long)millis(), on ? "shown" : "hidden");
}

void team_view_link(int state) {
    link_state = state;
    team_set_dark(state != 2);
    if (state != 2) card_close();
    // Back in touch: everyone is simply where they are now.
    else team_snap();
}

// The plan's usage for the strip: the office hides the meters.
static void usage_text(const UsageData* d) {
    const int s = (int)(d->session_pct + 0.5f), w = (int)(d->weekly_pct + 0.5f);
    if (d->codex) snprintf(usage, sizeof(usage), "Codex %d%%", s);
    else if (d->enterprise) snprintf(usage, sizeof(usage), "Spend %d%%", s);
    else if (d->weekly_only) snprintf(usage, sizeof(usage), "Week %d%%", w);
    else if (d->session_reset_mins < 0) snprintf(usage, sizeof(usage), "5h --   week %d%%", w);
    else snprintf(usage, sizeof(usage), "5h %d%%   week %d%%", s, w);
    usage_hot = strcmp(d->status, "limited") == 0;
}

void team_view_on_payload(const UsageData* d) {
    usage_text(d);
    if (d->team_view >= 0) {
        const bool on = d->team_view != 0;
        if (d->team_skin >= 0 && d->team_skin != team_skin()) {
            team_set_skin(d->team_skin);
            team_snap();
            office = 0;
            card_close();
        }
        if (enabled && !on) {
            team_view_show(false);
            team_reset();
        }
        enabled = on;
    }
    if (d->has_team) {
        TeamDeskIn in[TEAM_MAX];
        const int n = team_parse(d->team, in, TEAM_MAX);
        team_parse_extra(d->team_extra, in, n);
        team_set(in, n);
        // Off screen nobody walks: arrivals are simply there next time.
        if (!visible) team_snap();
    }
    if (d->has_detail) card_fill(d->detail);
}

void team_view_alert(const char* msg) {
    snprintf(alert, sizeof(alert), "%s", msg ? msg : "");
    if (alert[0]) return;
    // Over: back to the office that was on screen before it.
    alert_seen = false;
    if (office_before >= 0 && visible) office = office_before;
    office_before = -1;
}

bool team_view_tap(int x, int y) {
    if (!parent) return false;
    const bool side = x < TEAM_W / 3 || x >= TEAM_W - TEAM_W / 3;
    const bool right = x >= TEAM_W - TEAM_W / 3;
    // The views in a ring: the meters, then each office.
    if (visible) {
        // A card closes on any tap; a tap on a desk opens its card.
        if (card_id >= 0) { card_close(); return true; }
        const int desk = y >= 0 ? team_desk_at(office, x, y) : -1;
        if (desk >= 0) { card_open(desk); return true; }
        if (!side) return true;
        office_before = -1;                        // the user chose where to look
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
