// The agent team view's scene (team_core.h). Not behind PET_ASCII: it is
// pure, and the native tests build it.
#include "team_core.h"
#include "pet_font.h"
#include <string.h>
#include <math.h>

// ──────────────── geometry (480x480) ────────────────
static const int WALL_H   = 80;      // the back wall; the floor starts here
static const int HALL_Y   = 86;      // where people stand along the wall (top of the head)
static const int ROW_TOP  = 134;     // first desk row
static const int ROW_H    = 160;
static const int COL_W    = 160;
static const int DOOR_X   = 34;
static const int BOARD_X0 = 160, BOARD_X1 = 320, BOARD_Y0 = 14, BOARD_Y1 = 62;
static const int COOLER_X = 386;
static const int PLANT_X  = 444;
static const int BAR_Y    = 460;     // the status strip
static const float SPEED  = 92.0f;   // px per second

#define RGB(r, g, b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))

static const uint16_t C_FLOOR_A = RGB(38, 35, 32);
static const uint16_t C_FLOOR_B = RGB(41, 38, 35);
static const uint16_t C_WALL    = RGB(62, 58, 52);
static const uint16_t C_WALL_T  = RGB(54, 50, 45);
static const uint16_t C_BASE    = RGB(92, 84, 74);
static const uint16_t C_BOARD   = RGB(240, 237, 228);
static const uint16_t C_FRAME   = RGB(150, 144, 134);
static const uint16_t C_DOOR    = RGB(122, 88, 60);
static const uint16_t C_DOOR_D  = RGB(96, 68, 46);
static const uint16_t C_DESK    = RGB(168, 126, 88);
static const uint16_t C_DESK_HI = RGB(190, 148, 106);
static const uint16_t C_DESK_F  = RGB(118, 86, 60);
static const uint16_t C_CHAIR   = RGB(70, 72, 82);
static const uint16_t C_CHAIR_D = RGB(52, 54, 62);
static const uint16_t C_DARK    = RGB(18, 17, 16);
static const uint16_t C_SCREEN  = RGB(30, 32, 36);
static const uint16_t C_KEYS    = RGB(205, 200, 190);
static const uint16_t C_PANTS   = RGB(52, 64, 92);
static const uint16_t C_SHOE    = RGB(28, 26, 26);
static const uint16_t C_DIM     = RGB(150, 144, 132);
static const uint16_t C_ALERT   = RGB(255, 138, 91);
static const uint16_t C_LEAF    = RGB(88, 156, 90);
static const uint16_t C_LEAF_D  = RGB(62, 124, 72);
static const uint16_t C_POT     = RGB(176, 104, 70);
static const uint16_t C_COOLER  = RGB(222, 224, 226);
static const uint16_t C_COOLER_D= RGB(176, 180, 184);
static const uint16_t C_WATER   = RGB(120, 190, 235);
static const uint16_t C_WATER_HI= RGB(190, 228, 250);
static const uint16_t C_WHITE   = 0xFFFF;

static const uint16_t SHIRT[TEAM_DESKS] = {
    RGB(217, 119, 87), RGB(91, 155, 213), RGB(107, 191, 115),
    RGB(176, 124, 198), RGB(226, 192, 75), RGB(79, 193, 180),
};
static const uint16_t SKIN[3] = { RGB(244, 206, 170), RGB(204, 152, 110), RGB(146, 100, 70) };
static const uint16_t HAIR[4] = { RGB(44, 34, 28), RGB(104, 68, 40), RGB(204, 170, 96), RGB(136, 136, 140) };

// ──────────────── state ────────────────
typedef struct {
    uint8_t phase;                 // TeamPhase
    uint8_t office;                // the office it is in now
    float   x, y;                  // centre x, top of the head
    int16_t wp[5][2];
    uint8_t wp_n, wp_i;
    uint8_t look;                  // skin, hair colour, hair style
    bool    small;
    uint16_t shirt;
    int32_t timer_ms;              // a sub's next trip to the board, or its stay there
    bool    walking;
    float   dist;                  // walked so far: the stride
} Actor;

// A session. Its desk is `slot`: sessions sit in the order the app sends
// them, six to an office, and move up when one before them leaves.
typedef struct {
    bool    used, closing;
    uint8_t id;
    uint8_t slot;
    uint8_t color;
    char    state;
    uint8_t subs_want, quiet;
    char    name[TEAM_NAME];
    int32_t think_ms;              // how long the state has been `t`
    Actor   lead;
    Actor   sub[TEAM_SEATS];
} Desk;

typedef struct { int16_t x, y; uint8_t len; uint16_t color; } Stroke;

static Desk     desks[TEAM_MAX];
static Stroke   strokes[22];
static int      stroke_n;
static int32_t  stroke_ms;
static uint32_t clock_ms;
static uint32_t rng = 0x2545F491u;
static long     wall_secs = -1;

static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

static int desk_cx(int slot)  { return COL_W / 2 + (slot % TEAM_DESKS % 3) * COL_W; }
static int desk_top(int slot) { return ROW_TOP + (slot % TEAM_DESKS / 3) * ROW_H; }
// The aisle a desk's people walk: between the columns, the middle column
// alternating by row.
static int desk_aisle(int slot) {
    const int d = slot % TEAM_DESKS, col = d % 3;
    return col == 0 ? 160 : col == 2 ? 320 : (d / 3 ? 320 : 160);
}
static void seat_of(int slot, int who, int* x, int* y) {
    if (who < 0) { *x = desk_cx(slot); *y = desk_top(slot) + 6; }
    else         { *x = desk_cx(slot) - 42 + who * 28; *y = desk_top(slot) + 74; }
}

// The session at a desk: the one that sits there, else one walking out of it.
static Desk* at_slot(int slot, bool closing_too) {
    Desk* leaving = NULL;
    for (int i = 0; i < TEAM_MAX; i++) {
        if (!desks[i].used || desks[i].slot != slot) continue;
        if (!desks[i].closing) return &desks[i];
        leaving = &desks[i];
    }
    return closing_too ? leaving : NULL;
}

// ──────────────── parsing ────────────────
static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool digit(char c) { return c >= '0' && c <= '9'; }

int team_parse(const char* tm, TeamDeskIn* out, int max) {
    int n = 0;
    const char* p = tm ? tm : "";
    while (*p && n < max) {
        const char* end = strchr(p, '|');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= 5 && strchr("wtqdib", p[2]) && hex(p[0]) >= 0 && hex(p[1]) >= 0 && digit(p[3]) && digit(p[4])) {
            TeamDeskIn* d = &out[n++];
            d->id = (uint8_t)(hex(p[0]) * 16 + hex(p[1]));
            d->state = p[2];
            d->subs = (uint8_t)(p[3] - '0');
            d->quiet = (uint8_t)(p[4] - '0');
            if (d->quiet > d->subs) d->quiet = d->subs;
            size_t k = len - 5;
            if (k > TEAM_NAME - 1) k = TEAM_NAME - 1;
            memcpy(d->name, p + 5, k);
            d->name[k] = '\0';
        }
        if (!end) break;
        p = end + 1;
    }
    return n;
}

// ──────────────── people ────────────────
static bool present(const Actor* a) { return a->phase != TEAM_OFF && a->phase != TEAM_LEAVE; }
static bool at_cooler(const Actor* a) { return a->phase == TEAM_TO_COOLER || a->phase == TEAM_COOLER; }
static bool by_wall(const Actor* a) { return a->y < ROW_TOP - 4; }

static void add_wp(Actor* a, int x, int y) {
    if (a->wp_n >= 5) return;
    a->wp[a->wp_n][0] = (int16_t)x;
    a->wp[a->wp_n][1] = (int16_t)y;
    a->wp_n++;
}

// To a spot by the wall (the door, the board, the cooler): up the aisle
// unless already there.
static void go_spot(Actor* a, int aisle, int spot_x, int spot_y, TeamPhase phase) {
    a->wp_n = a->wp_i = 0;
    if (!by_wall(a)) {
        add_wp(a, aisle, (int)a->y);
        add_wp(a, aisle, spot_y);
    }
    add_wp(a, spot_x, spot_y);
    a->phase = (uint8_t)phase;
}

// To a seat: along the wall to its aisle and down. From another desk, up to
// the wall first unless the aisle is the same.
static void go_seat(Actor* a, int slot, int who, TeamPhase phase) {
    int sx, sy;
    seat_of(slot, who, &sx, &sy);
    const int ax = desk_aisle(slot);
    a->wp_n = a->wp_i = 0;
    const int near = a->x < TEAM_W / 2 ? 160 : 320;
    if (!by_wall(a) && near != ax) {
        add_wp(a, near, (int)a->y);
        add_wp(a, near, HALL_Y);
        add_wp(a, ax, HALL_Y);
    } else {
        add_wp(a, ax, (int)a->y);
    }
    add_wp(a, ax, sy);
    add_wp(a, sx, sy);
    a->phase = (uint8_t)phase;
}

static void enter(Actor* a, const Desk* d, int who) {
    memset(a, 0, sizeof(*a));
    a->office = (uint8_t)(d->slot / TEAM_DESKS);
    a->x = (float)DOOR_X;
    a->y = (float)HALL_Y;
    a->small = who >= 0;
    a->look = (uint8_t)(rnd() % 36);
    const uint16_t s = SHIRT[d->color];
    // Sub-agents wear a lighter shade of their lead's colour.
    a->shirt = who < 0 ? s
        : (uint16_t)((((s >> 11) + 31) / 2) << 11 | ((((s >> 5) & 63) + 63) / 2) << 5 | (((s & 31) + 31) / 2));
    a->timer_ms = 15000 + (int32_t)(rnd() % 30000);
    go_seat(a, d->slot, who, TEAM_ENTER);
}

static void leave(Actor* a, int slot) {
    if (!present(a)) return;
    go_spot(a, desk_aisle(slot), DOOR_X, HALL_Y, TEAM_LEAVE);
}

// The session got another desk: its people walk over, through the door when
// the desk is in the other office.
static void move_desk(Desk* d, int slot) {
    const int old = d->slot;
    d->slot = (uint8_t)slot;
    for (int who = -1; who < TEAM_SEATS; who++) {
        Actor* a = who < 0 ? &d->lead : &d->sub[who];
        if (!present(a)) continue;
        if (a->office != slot / TEAM_DESKS) go_spot(a, desk_aisle(old), DOOR_X, HALL_Y, TEAM_MOVE_OUT);
        else if (a->phase == TEAM_SEAT || a->phase == TEAM_ENTER || a->phase == TEAM_RETURN)
            go_seat(a, slot, who, TEAM_RETURN);
        // At the board or the cooler: they come back to the new desk.
    }
}

// Move along the path; true when the last waypoint is reached.
static bool walk(Actor* a, float dist) {
    a->walking = false;
    while (a->wp_i < a->wp_n && dist > 0) {
        const float tx = a->wp[a->wp_i][0], ty = a->wp[a->wp_i][1];
        const float dx = tx - a->x, dy = ty - a->y;
        const float ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        if (ax + ay < 0.01f) { a->wp_i++; continue; }
        a->walking = true;
        // One axis at a time: x, then y.
        const float s = ax > 0.01f ? (ax < dist ? ax : dist) : (ay < dist ? ay : dist);
        if (ax > 0.01f) a->x += dx < 0 ? -s : s;
        else            a->y += dy < 0 ? -s : s;
        a->dist += s;
        dist -= s;
    }
    return a->wp_i >= a->wp_n;
}

static void cooler_spot(int slot, int who, int* x, int* y) {
    // Leads in a row by the cooler, sub-agents a step in front of them.
    static const int16_t LEAD_X[TEAM_DESKS] = { 344, 412, 322, 432, 300, 454 };
    if (who < 0) { *x = LEAD_X[slot % TEAM_DESKS]; *y = HALL_Y; return; }
    *x = COOLER_X - 80 + (int)(rnd() % 96);
    *y = HALL_Y + 22 + (int)(rnd() % 3) * 4;
    if (*x > COOLER_X - 22 && *x < COOLER_X + 22) *x += 44;   // not in front of the tap
}

static int subs_at_cooler(const Desk* d) {
    int n = 0;
    for (int s = 0; s < TEAM_SEATS; s++) n += at_cooler(&d->sub[s]);
    return n;
}
static int subs_present(const Desk* d) {
    int n = 0;
    for (int s = 0; s < TEAM_SEATS; s++) n += present(&d->sub[s]);
    return n;
}

static void step_actor(Actor* a, int who, const Desk* d, uint32_t dt) {
    if (a->phase == TEAM_OFF) return;
    const float dist = SPEED * dt / 1000.0f;
    const int slot = d->slot, aisle = desk_aisle(slot);
    const int on_break = subs_at_cooler(d);
    switch (a->phase) {
    case TEAM_ENTER:
    case TEAM_RETURN:
        if (walk(a, dist)) { a->phase = TEAM_SEAT; a->walking = false; }
        break;
    case TEAM_MOVE_OUT:
        // Out of one office's door and in at the other's.
        if (walk(a, dist)) {
            a->office = (uint8_t)(slot / TEAM_DESKS);
            go_seat(a, slot, who, TEAM_ENTER);
        }
        break;
    case TEAM_TO_BOARD:
        if (walk(a, dist)) {
            a->phase = TEAM_BOARD;
            a->walking = false;
            if (who >= 0) a->timer_ms = 5000 + (int32_t)(rnd() % 5000);
        }
        break;
    case TEAM_TO_COOLER:
        if (walk(a, dist)) { a->phase = TEAM_COOLER; a->walking = false; }
        break;
    case TEAM_LEAVE:
        if (walk(a, dist)) a->phase = TEAM_OFF;
        break;
    case TEAM_BOARD: {
        // The lead stays while it thinks; a sub-agent for a few seconds.
        bool back = d->closing;
        if (who < 0) back = back || d->state != 't';
        else { a->timer_ms -= (int32_t)dt; back = back || a->timer_ms <= 0; }
        if (back) {
            go_seat(a, slot, who, TEAM_RETURN);
            if (who >= 0) a->timer_ms = 25000 + (int32_t)(rnd() % 40000);
        }
        break;
    }
    case TEAM_COOLER:
        // Back to work when the session wakes, or this sub-agent does.
        if (who < 0 ? d->state != 'b' : on_break > d->quiet) go_seat(a, slot, who, TEAM_RETURN);
        break;
    case TEAM_SEAT:
        if (who < 0) {
            int x, y;
            if (d->state == 'b') {
                cooler_spot(slot, who, &x, &y);
                go_spot(a, aisle, x, y, TEAM_TO_COOLER);
            } else if (d->state == 't' && d->think_ms > 3000) {
                // A thought that lasts takes the lead to the board.
                go_spot(a, aisle, BOARD_X0 + 20 + (slot % TEAM_DESKS) * 24, HALL_Y, TEAM_TO_BOARD);
            }
        } else if (on_break < d->quiet) {
            int x, y;
            cooler_spot(slot, who, &x, &y);
            go_spot(a, aisle, x, y, TEAM_TO_COOLER);
        } else if (d->state == 'w' || d->state == 't' || d->state == 'd') {
            a->timer_ms -= (int32_t)dt;
            if (a->timer_ms <= 0)
                go_spot(a, aisle, BOARD_X0 + 12 + (int)(rnd() % 136), HALL_Y + (int)(rnd() % 3) * 3, TEAM_TO_BOARD);
        }
        break;
    default: break;
    }
}

// ──────────────── the offices ────────────────
void team_reset(void) {
    memset(desks, 0, sizeof(desks));
    stroke_n = 0;
    stroke_ms = 0;
    clock_ms = 0;
}

static Desk* by_id(uint8_t id) {
    for (int i = 0; i < TEAM_MAX; i++)
        if (desks[i].used && !desks[i].closing && desks[i].id == id) return &desks[i];
    return NULL;
}

int team_desk_of(uint8_t id) {
    const Desk* d = by_id(id);
    return d ? d->slot : -1;
}

static Desk* free_desk(void) {
    for (int i = 0; i < TEAM_MAX; i++) if (!desks[i].used) return &desks[i];
    for (int i = 0; i < TEAM_MAX; i++) if (desks[i].closing) return &desks[i];   // hurry them out
    return NULL;
}

// The shirt colour fewest sessions wear.
static uint8_t pick_color(void) {
    int count[TEAM_DESKS] = { 0 };
    for (int i = 0; i < TEAM_MAX; i++) if (desks[i].used && !desks[i].closing) count[desks[i].color]++;
    int best = 0;
    for (int c = 1; c < TEAM_DESKS; c++) if (count[c] < count[best]) best = c;
    return (uint8_t)best;
}

void team_set(const TeamDeskIn* in, int n) {
    if (n > TEAM_MAX) n = TEAM_MAX;
    // Who is gone leaves first, so the desks they free can be taken.
    for (int i = 0; i < TEAM_MAX; i++) {
        Desk* d = &desks[i];
        if (!d->used || d->closing) continue;
        bool seen = false;
        for (int k = 0; k < n; k++) seen = seen || in[k].id == d->id;
        if (seen) continue;
        d->closing = true;
        d->subs_want = 0;
        d->quiet = 0;
        leave(&d->lead, d->slot);
        for (int s = 0; s < TEAM_SEATS; s++) leave(&d->sub[s], d->slot);
    }
    // The k-th session sits at the k-th desk.
    for (int k = 0; k < n; k++) {
        Desk* d = by_id(in[k].id);
        if (!d) {
            d = free_desk();
            if (!d) continue;
            memset(d, 0, sizeof(*d));
            d->color = pick_color();
            d->used = true;
            d->id = in[k].id;
            d->slot = (uint8_t)k;
            enter(&d->lead, d, -1);
        } else if (d->slot != k) {
            move_desk(d, k);
        }
        if (d->state != in[k].state) d->think_ms = 0;
        d->state = in[k].state;
        d->subs_want = in[k].subs;
        memcpy(d->name, in[k].name, TEAM_NAME);
        d->name[TEAM_NAME - 1] = '\0';

        const int want = d->subs_want > TEAM_SEATS ? TEAM_SEATS : d->subs_want;
        d->quiet = in[k].quiet > want ? (uint8_t)want : in[k].quiet;
        int have = subs_present(d);
        for (int i = 0; i < TEAM_SEATS && have < want; i++)
            if (d->sub[i].phase == TEAM_OFF) { enter(&d->sub[i], d, i); have++; }
        // The ones on a break go first: they were the quiet ones.
        for (int pass = 0; pass < 2; pass++)
            for (int i = TEAM_SEATS - 1; i >= 0 && have > want; i--)
                if (present(&d->sub[i]) && (pass == 1 || at_cooler(&d->sub[i]))) { leave(&d->sub[i], d->slot); have--; }
    }
}

static void settle(Actor* a, int slot, int who, bool to_cooler) {
    if (a->phase == TEAM_OFF) return;
    if (a->phase == TEAM_LEAVE) { a->phase = TEAM_OFF; return; }
    const bool moved = a->office != slot / TEAM_DESKS;
    a->office = (uint8_t)(slot / TEAM_DESKS);
    if (!moved && a->wp_n > 0 && (a->phase == TEAM_TO_BOARD || a->phase == TEAM_BOARD)) {
        a->x = a->wp[a->wp_n - 1][0];
        a->y = a->wp[a->wp_n - 1][1];
        a->phase = TEAM_BOARD;
    } else if (to_cooler) {
        if (!moved && a->wp_n > 0 && at_cooler(a)) { a->x = a->wp[a->wp_n - 1][0]; a->y = a->wp[a->wp_n - 1][1]; }
        else { int x, y; cooler_spot(slot, who, &x, &y); a->x = (float)x; a->y = (float)y; }
        a->phase = TEAM_COOLER;
    } else {
        int sx, sy;
        seat_of(slot, who, &sx, &sy);
        a->x = (float)sx;
        a->y = (float)sy;
        a->phase = TEAM_SEAT;
    }
    a->wp_i = a->wp_n;
    a->walking = false;
}

void team_snap(void) {
    for (int i = 0; i < TEAM_MAX; i++) {
        Desk* d = &desks[i];
        if (!d->used) continue;
        if (d->closing) { d->used = false; continue; }
        settle(&d->lead, d->slot, -1, d->state == 'b');
        int quiet = d->quiet;
        for (int s = TEAM_SEATS - 1; s >= 0; s--) {
            const bool brk = present(&d->sub[s]) && quiet > 0;
            if (brk) quiet--;
            settle(&d->sub[s], d->slot, s, brk);
        }
    }
}

void team_step(uint32_t dt) {
    clock_ms += dt;
    bool at_board = false;
    uint16_t pen = C_DARK;
    for (int i = 0; i < TEAM_MAX; i++) {
        Desk* d = &desks[i];
        if (!d->used) continue;
        if (d->state == 't') d->think_ms += (int32_t)dt;
        step_actor(&d->lead, -1, d, dt);
        for (int s = 0; s < TEAM_SEATS; s++) step_actor(&d->sub[s], s, d, dt);
        if (d->lead.phase == TEAM_BOARD) { at_board = true; pen = d->lead.shirt; }
        for (int s = 0; s < TEAM_SEATS; s++)
            if (d->sub[s].phase == TEAM_BOARD) { at_board = true; pen = d->sub[s].shirt; }
        if (d->closing) {
            bool gone = d->lead.phase == TEAM_OFF;
            for (int s = 0; s < TEAM_SEATS; s++) gone = gone && d->sub[s].phase == TEAM_OFF;
            if (gone) d->used = false;
        }
    }
    // Whoever is at the board writes on it; a full board is wiped.
    if (at_board) {
        stroke_ms += (int32_t)dt;
        if (stroke_ms >= 700) {
            stroke_ms = 0;
            if (stroke_n >= (int)(sizeof(strokes) / sizeof(strokes[0]))) stroke_n = 0;
            Stroke* s = &strokes[stroke_n++];
            s->x = (int16_t)(BOARD_X0 + 10 + rnd() % 100);
            s->y = (int16_t)(BOARD_Y0 + 7 + (rnd() % 6) * 6);
            s->len = (uint8_t)(12 + rnd() % 34);
            s->color = pen;
        }
    }
}

int team_bar_y(void) { return BAR_Y; }
void team_set_time(long seconds) { wall_secs = seconds; }

int team_sessions(void) {
    int n = 0;
    for (int i = 0; i < TEAM_MAX; i++) n += desks[i].used && !desks[i].closing;
    return n;
}
int team_offices(void) {
    const int n = team_sessions();
    return n <= TEAM_DESKS ? 1 : (n + TEAM_DESKS - 1) / TEAM_DESKS;
}
int team_agents(void) {
    int n = 0;
    for (int i = 0; i < TEAM_MAX; i++) if (desks[i].used && !desks[i].closing) n += desks[i].subs_want;
    return n;
}
TeamPhase team_lead_phase(int slot) {
    const Desk* d = at_slot(slot, true);
    return d ? (TeamPhase)d->lead.phase : TEAM_OFF;
}
int team_subs_present(int slot) {
    const Desk* d = at_slot(slot, false);
    return d ? subs_present(d) : 0;
}
int team_subs_at_cooler(int slot) {
    const Desk* d = at_slot(slot, false);
    return d ? subs_at_cooler(d) : 0;
}

void team_desk_info(int office, int desk, TeamDeskInfo* out) {
    memset(out, 0, sizeof(*out));
    out->name = "";
    if (desk < 0 || desk >= TEAM_DESKS) return;
    out->name_cx = desk_cx(desk);
    out->name_y = desk_top(desk) + 111;
    const Desk* d = at_slot(office * TEAM_DESKS + desk, false);
    if (!d) return;
    out->live = true;
    out->name = d->name;
    out->state = d->state;
    out->more = d->subs_want > TEAM_SEATS ? d->subs_want - TEAM_SEATS : 0;
}

// ──────────────── drawing ────────────────
static uint16_t* fb;

static inline uint16_t mix(uint16_t a, uint16_t b, int t) {   // t 0..255 of b over a
    const int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    const int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return (uint16_t)(((ar + ((br - ar) * t >> 8)) << 11) | ((ag + ((bg - ag) * t >> 8)) << 5) | (ab + ((bb - ab) * t >> 8)));
}

static inline void px(int x, int y, uint16_t c, int a) {
    if ((unsigned)x >= TEAM_W || (unsigned)y >= TEAM_H || a <= 0) return;
    uint16_t* p = &fb[y * TEAM_W + x];
    *p = a >= 255 ? c : mix(*p, c, a);
}

static void rect(int x, int y, int w, int h, uint16_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > TEAM_W) w = TEAM_W - x;
    if (y + h > TEAM_H) h = TEAM_H - y;
    for (int j = 0; j < h; j++) {
        uint16_t* row = &fb[(y + j) * TEAM_W + x];
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

static void shade(int x, int y, int w, int h, uint16_t c, int a) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) px(x + i, y + j, c, a);
}

// A filled ellipse with a soft edge; `a` is its opacity.
static void oval(float cx, float cy, float rx, float ry, uint16_t c, int a) {
    const int x0 = (int)floorf(cx - rx - 1), x1 = (int)ceilf(cx + rx + 1);
    const int y0 = (int)floorf(cy - ry - 1), y1 = (int)ceilf(cy + ry + 1);
    const float r = rx < ry ? rx : ry;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            const float cov = (1.0f - sqrtf(dx * dx + dy * dy)) * r + 0.5f;
            if (cov <= 0) continue;
            px(x, y, c, cov >= 1 ? a : (int)(cov * a));
        }
}
static void disc(float cx, float cy, float r, uint16_t c) { oval(cx, cy, r, r, c, 255); }

// A clock hand from (cx, cy) at `angle` (0 = up, clockwise).
static void hand(float cx, float cy, float angle, float len, float r, uint16_t c) {
    const float dx = sinf(angle), dy = -cosf(angle);
    for (float t = 0; t <= len; t += 0.5f) disc(cx + dx * t, cy + dy * t, r, c);
}

// A rounded rectangle with soft corners.
static void rrect(int x, int y, int w, int h, int r, uint16_t c) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    rect(x + r, y, w - 2 * r, h, c);
    rect(x, y + r, r, h - 2 * r, c);
    rect(x + w - r, y + r, r, h - 2 * r, c);
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            const float dx = r - i - 0.5f, dy = r - j - 0.5f;
            const float cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
            if (cov <= 0) continue;
            const int a = cov >= 1 ? 255 : (int)(cov * 255);
            px(x + i, y + j, c, a);
            px(x + w - 1 - i, y + j, c, a);
            px(x + i, y + h - 1 - j, c, a);
            px(x + w - 1 - i, y + h - 1 - j, c, a);
        }
}

static void glyph(int x, int y, char ch, int scale, uint16_t c) {
    const uint8_t u = (uint8_t)ch;
    const uint8_t* g = PET_FONT[(u >= 0x20 && u <= 0x7E) ? u - 0x20 : '?' - 0x20];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 8; row++)
            if ((g[col] >> row) & 1) rect(x + col * scale, y + row * scale, scale, scale, c);
}

// 0..255 and back, once every `period` ms.
static int pulse(uint32_t period) {
    const uint32_t t = clock_ms % period;
    const uint32_t half = period / 2;
    return (int)((t < half ? t : period - t) * 255 / half);
}

typedef enum { POSE_STAND, POSE_SEAT, POSE_SLEEP, POSE_BACK, POSE_CUP } Pose;

static void person(const Actor* a, Pose pose) {
    const float k = a->small ? 0.8f : 1.0f;
    const uint16_t skin = SKIN[a->look % 3], hair = HAIR[(a->look / 3) % 4];
    const int style = (a->look / 12) % 3;             // 0 short, 1 long, 2 cropped
    const bool seated = pose == POSE_SEAT || pose == POSE_SLEEP;
    const int stride = a->walking ? (int)(a->dist / 9.0f) & 1 : -1;
    const float x = a->x;
    float y = a->y + (stride == 1 ? -1.0f : 0.0f) + (pose == POSE_SLEEP ? 5.0f : 0.0f);
    const uint16_t shirt_d = mix(a->shirt, C_DARK, 70);

    if (!seated) {
        oval(x, a->y + 35 * k, 11 * k, 3.5f * k, C_DARK, 90);
        // Legs: one lifts on each stride.
        for (int s = 0; s < 2; s++) {
            const int lift = stride == s ? 2 : 0;
            const int lx = (int)(x + (s ? 1 : -6) * k), ly = (int)(y + 26 * k);
            const int lh = (int)(9 * k) - lift;
            rrect(lx, ly, (int)(5 * k + 0.5f), lh, 2, C_PANTS);
            rect(lx, ly + lh - 2, (int)(5 * k + 0.5f), 2, C_SHOE);
        }
    }
    // Arms swing against the legs; seated, they reach for the desk.
    for (int s = 0; s < 2; s++) {
        const int swing = stride < 0 ? 0 : (stride == s ? -2 : 2);
        const int ax = (int)(x + (s ? 9 : -12) * k);
        const int ah = (int)((seated ? 8 : 12) * k);
        if (pose == POSE_CUP && s == 1) continue;
        rrect(ax, (int)(y + 15 * k) + swing, (int)(3 * k + 1), ah, 2, shirt_d);
        if (!seated) disc(ax + 1.5f * k + 0.5f, y + 15 * k + swing + ah, 1.8f * k, skin);
    }
    const int bw = (int)(18 * k), bh = (int)(15 * k);
    rrect((int)(x - 9 * k), (int)(y + 13 * k), bw, bh, (int)(5 * k), a->shirt);
    shade((int)(x - 9 * k) + 2, (int)(y + 13 * k) + bh - 4, bw - 4, 3, C_DARK, 40);
    if (pose == POSE_CUP) {
        // A raised arm and a paper cup.
        rrect((int)(x + 8 * k), (int)(y + 14 * k), (int)(3 * k + 1), (int)(7 * k), 2, shirt_d);
        rect((int)(x + 10 * k), (int)(y + 11 * k), (int)(5 * k), (int)(6 * k), C_WHITE);
        rect((int)(x + 10 * k), (int)(y + 11 * k), (int)(5 * k), 1, C_WATER);
    }

    const float hx = x, hy = y + 7 * k, r = 7.2f * k;
    if (style == 1) rrect((int)(hx - r), (int)hy, (int)(2 * r + 0.5f), (int)(r + 5 * k), 3, hair);
    disc(hx, hy, r, hair);
    if (pose == POSE_BACK) return;
    disc(hx, hy + 1.6f * k, r - (style == 2 ? 1.0f : 1.7f) * k, skin);
    if (style == 0) oval(hx - 2 * k, hy - 3.6f * k, 5 * k, 2.6f * k, hair, 255);      // a fringe
    const int ey = (int)(hy + 1.5f * k), eo = (int)(3 * k);
    if (pose == POSE_SLEEP) {
        rect((int)hx - eo - 1, ey + 1, 3, 1, C_DARK);
        rect((int)hx + eo - 1, ey + 1, 3, 1, C_DARK);
    } else if ((clock_ms + a->look * 331u) % 3700 > 140) {       // a blink now and then
        rect((int)hx - eo, ey, 2, 2, C_DARK);
        rect((int)hx + eo - 1, ey, 2, 2, C_DARK);
    }
}

static void draw_room(int office) {
    rect(0, 0, TEAM_W, WALL_H, C_WALL);
    rect(0, 0, TEAM_W, 8, C_WALL_T);
    for (int ty = 0; ty * 40 + WALL_H < TEAM_H; ty++)
        for (int tx = 0; tx < TEAM_W / 40; tx++)
            rect(tx * 40, WALL_H + ty * 40, 40, 40, (tx + ty) & 1 ? C_FLOOR_A : C_FLOOR_B);
    rect(0, WALL_H - 5, TEAM_W, 5, C_BASE);
    shade(0, WALL_H, TEAM_W, 5, C_DARK, 70);

    // The door people come and go by.
    rrect(DOOR_X - 20, 12, 40, WALL_H - 12, 3, C_DOOR_D);
    rect(DOOR_X - 17, 15, 34, WALL_H - 15, C_DOOR);
    rrect(DOOR_X - 12, 20, 24, 22, 2, C_DOOR_D);
    rrect(DOOR_X - 12, 47, 24, 26, 2, C_DOOR_D);
    disc(DOOR_X + 12, 46, 2.5f, RGB(226, 192, 110));

    // The clock: the app's time when it sends one, else no hands.
    disc(104, 38, 15, C_FRAME);
    disc(104, 38, 13, C_BOARD);
    for (int h = 0; h < 12; h++) {
        const float a = h * 0.5235988f;
        disc(104 + sinf(a) * 11, 38 - cosf(a) * 11, h % 3 ? 0.7f : 1.1f, C_FRAME);
    }
    if (wall_secs >= 0) {
        const float m = (wall_secs % 3600) / 3600.0f * 6.2831853f;
        const float h = (wall_secs % 43200) / 43200.0f * 6.2831853f;
        hand(104, 38, h, 6.5f, 1.3f, C_DARK);
        hand(104, 38, m, 10.0f, 0.9f, C_DARK);
    }
    disc(104, 38, 1.6f, C_ALERT);

    // The whiteboard and what has been written on it.
    rrect(BOARD_X0 - 4, BOARD_Y0 - 4, BOARD_X1 - BOARD_X0 + 8, BOARD_Y1 - BOARD_Y0 + 8, 4, C_FRAME);
    rect(BOARD_X0, BOARD_Y0, BOARD_X1 - BOARD_X0, BOARD_Y1 - BOARD_Y0, C_BOARD);
    rrect(BOARD_X0 + 50, BOARD_Y1 + 4, 60, 4, 2, C_BASE);
    rect(BOARD_X0 + 58, BOARD_Y1 + 1, 10, 3, SHIRT[0]);
    rect(BOARD_X0 + 72, BOARD_Y1 + 1, 10, 3, SHIRT[1]);
    rect(BOARD_X0 + 86, BOARD_Y1 + 1, 10, 3, SHIRT[2]);
    for (int i = 0; i < stroke_n; i++) {
        int len = strokes[i].len;
        if (strokes[i].x + len > BOARD_X1 - 8) len = BOARD_X1 - 8 - strokes[i].x;
        rrect(strokes[i].x, strokes[i].y, len, 2, 1, strokes[i].color);
    }

    // The water cooler: a bottle on a stand; it bubbles when someone is there.
    bool company = false;
    for (int i = 0; i < TEAM_MAX; i++) {
        if (!desks[i].used) continue;
        for (int who = -1; who < TEAM_SEATS; who++) {
            const Actor* a = who < 0 ? &desks[i].lead : &desks[i].sub[who];
            company = company || (a->phase == TEAM_COOLER && a->office == office);
        }
    }
    oval(COOLER_X, WALL_H + 1, 16, 4, C_DARK, 90);
    rrect(COOLER_X - 11, 12, 22, 30, 7, C_WATER);
    rrect(COOLER_X - 8, 15, 4, 22, 2, C_WATER_HI);
    rect(COOLER_X - 5, 40, 10, 4, C_WATER);
    if (company) {
        const int t = (int)(clock_ms % 1800);
        disc(COOLER_X + 3, 38 - t / 80.0f, 1.8f, C_WATER_HI);
        disc(COOLER_X - 1, 38 - ((t + 900) % 1800) / 80.0f, 1.3f, C_WATER_HI);
    }
    rrect(COOLER_X - 14, 42, 28, WALL_H - 42, 4, C_COOLER);
    rect(COOLER_X - 14, 44, 28, 3, C_COOLER_D);
    rrect(COOLER_X - 9, 52, 18, 12, 2, C_COOLER_D);
    disc(COOLER_X - 4, 56, 2, RGB(90, 150, 230));
    disc(COOLER_X + 4, 56, 2, RGB(230, 100, 90));
    rect(COOLER_X - 8, 63, 16, 2, C_DARK);

    // A plant.
    oval(PLANT_X, WALL_H + 1, 14, 4, C_DARK, 90);
    disc(PLANT_X, 36, 11, C_LEAF_D);
    disc(PLANT_X - 10, 46, 9, C_LEAF);
    disc(PLANT_X + 10, 44, 9, C_LEAF_D);
    disc(PLANT_X + 2, 26, 8, C_LEAF);
    disc(PLANT_X - 3, 42, 7, C_LEAF);
    rrect(PLANT_X - 11, 56, 22, WALL_H - 56, 4, C_POT);
    rect(PLANT_X - 13, 56, 26, 5, mix(C_POT, C_WHITE, 50));
}

static void draw_desk(int slot) {
    const Desk* d = at_slot(slot, false);
    const bool live = d != NULL;
    const bool seated = live && d->lead.phase == TEAM_SEAT && d->lead.office == slot / TEAM_DESKS;
    const int cx = desk_cx(slot), top = desk_top(slot);
    // An empty desk fades into the floor.
    const int fade = live ? 0 : 150;
    #define DIMMED(c) mix((c), C_FLOOR_A, fade)

    // The chair, then whoever is in it, then the desk over their legs.
    oval(cx, top + 62, 50, 7, C_DARK, 70);
    rrect(cx - 15, top + 2, 30, 36, 8, DIMMED(C_CHAIR_D));
    rrect(cx - 12, top + 5, 24, 30, 6, DIMMED(C_CHAIR));
    if (seated) person(&d->lead, d->state == 'i' ? POSE_SLEEP : POSE_SEAT);

    rrect(cx - 48, top + 32, 96, 30, 4, DIMMED(C_DESK_F));
    rrect(cx - 48, top + 32, 96, 24, 4, DIMMED(C_DESK));
    rect(cx - 44, top + 34, 88, 2, DIMMED(C_DESK_HI));

    // The monitor: lit in the session's colour while it works.
    uint16_t screen = C_SCREEN;
    if (live && seated) {
        if (d->state == 'w') screen = mix(C_SCREEN, SHIRT[d->color], 210);
        else if (d->state == 'q') screen = mix(C_SCREEN, C_ALERT, pulse(900));
        else if (d->state == 't' || d->state == 'd') screen = mix(C_SCREEN, SHIRT[d->color], 70);
    }
    rect(cx - 30, top + 38, 6, 5, DIMMED(C_DARK));
    rrect(cx - 36, top + 42, 18, 4, 2, DIMMED(C_DARK));
    rrect(cx - 43, top + 12, 32, 27, 4, DIMMED(C_DARK));
    rrect(cx - 40, top + 15, 26, 20, 2, DIMMED(screen));
    if (live && seated && d->state == 'w') {
        // Lines of code scrolling by.
        for (int i = 0; i < 4; i++) {
            const int k = (int)((clock_ms / 260 + i) % 7);
            rect(cx - 38 + (k & 1) * 3, top + 18 + i * 4, 8 + (k * 5) % 13, 2, mix(SHIRT[d->color], C_DARK, 190));
        }
    }
    // Keyboard, hands, a mug.
    rrect(cx - 9, top + 41, 26, 9, 2, DIMMED(C_KEYS));
    rect(cx - 7, top + 44, 22, 1, DIMMED(C_DIM));
    rect(cx - 7, top + 47, 22, 1, DIMMED(C_DIM));
    if (live && seated && d->state != 'i') {
        const uint16_t skin = SKIN[d->lead.look % 3];
        const int tap = d->state == 'w' ? (int)((clock_ms / 110) % 4) : 1;
        disc(cx - 4 + (tap == 0) * 2, top + 44 + (tap == 1), 2.6f, skin);
        disc(cx + 8 - (tap == 2) * 2, top + 44 + (tap == 3), 2.6f, skin);
    }
    disc(cx + 34, top + 45, 5, DIMMED(C_WHITE));
    disc(cx + 34, top + 45, 3.2f, DIMMED(RGB(110, 72, 48)));
    #undef DIMMED

    if (!live) return;
    // The sub-agents' bench, in front of the desk.
    bool any = false;
    for (int s = 0; s < TEAM_SEATS; s++) {
        if (d->sub[s].phase != TEAM_SEAT) continue;
        any = true;
        person(&d->sub[s], POSE_SEAT);
    }
    if (!any && !(live && d->subs_want > 0)) return;
    oval(cx, top + 108, 62, 5, C_DARK, 70);
    rrect(cx - 60, top + 94, 120, 13, 4, C_DESK_F);
    rrect(cx - 60, top + 94, 120, 9, 4, C_DESK);
    rect(cx - 56, top + 95, 112, 1, C_DESK_HI);
    for (int s = 0; s < TEAM_SEATS; s++) {
        if (d->sub[s].phase != TEAM_SEAT) continue;
        int sx, sy;
        seat_of(slot, s, &sx, &sy);
        const bool busy = d->state == 'w' || d->state == 't' || d->state == 'd';
        rrect(sx - 8, top + 88, 16, 10, 2, C_DIM);
        rect(sx - 6, top + 90, 12, 6, busy ? mix(C_SCREEN, d->sub[s].shirt, 120 + pulse(700 + s * 130) / 2) : C_SCREEN);
    }
}

static void draw_walker(const Actor* a) {
    person(a, a->phase == TEAM_BOARD ? POSE_BACK : a->phase == TEAM_COOLER ? POSE_CUP : POSE_STAND);
}

void team_render(uint16_t* buf, int office) {
    fb = buf;
    draw_room(office);
    for (int i = 0; i < TEAM_DESKS; i++) draw_desk(office * TEAM_DESKS + i);
    // People on their feet in this office, the ones nearer the wall first.
    const Actor* up[TEAM_MAX * (TEAM_SEATS + 1)];
    int n = 0;
    for (int i = 0; i < TEAM_MAX; i++) {
        if (!desks[i].used) continue;
        for (int who = -1; who < TEAM_SEATS; who++) {
            const Actor* a = who < 0 ? &desks[i].lead : &desks[i].sub[who];
            if (a->office == office && a->phase != TEAM_OFF && a->phase != TEAM_SEAT) up[n++] = a;
        }
    }
    for (int i = 1; i < n; i++) {
        const Actor* a = up[i];
        int j = i;
        for (; j > 0 && up[j - 1]->y > a->y; j--) up[j] = up[j - 1];
        up[j] = a;
    }
    for (int i = 0; i < n; i++) draw_walker(up[i]);

    // What each lead has over its head.
    for (int i = 0; i < TEAM_MAX; i++) {
        const Desk* d = &desks[i];
        if (!d->used || d->closing || d->lead.office != office) continue;
        const Actor* a = &d->lead;
        const int cx = (int)a->x, top = (int)a->y;
        if (d->state == 'q' && a->phase == TEAM_SEAT) {
            const int bob = pulse(1000) / 80;
            oval(cx + 21, top - 9 - bob, 11, 12, C_DARK, 80);
            rrect(cx + 10, top - 22 - bob, 20, 22, 7, C_ALERT);
            disc(cx + 11, top + 2 - bob, 2.5f, C_ALERT);
            glyph(cx + 15, top - 19 - bob, '?', 2, C_DARK);
        } else if (d->state == 'i' && a->phase == TEAM_SEAT) {
            const int t = (int)(clock_ms % 2400);
            glyph(cx + 12 + t / 300, top - 6 - t / 150, 'z', 2, mix(C_FLOOR_A, C_WHITE, 220 - t / 12));
            const int t2 = (t + 1200) % 2400;
            glyph(cx + 12 + t2 / 300, top - 6 - t2 / 150, 'z', 1, mix(C_FLOOR_A, C_WHITE, 220 - t2 / 12));
        } else if (a->phase == TEAM_BOARD || (d->state == 'd' && a->phase == TEAM_SEAT)) {
            // Thinking: at the board, or at the desk while the sub-agents work.
            for (int k = 0; k < 3; k++) {
                const bool on = (clock_ms / 350) % 4 > (uint32_t)k;
                disc(cx + 14 + k * 7, top + 2, on ? 2.4f : 1.4f, on ? C_WHITE : C_DIM);
            }
        }
    }

    shade(0, BAR_Y, TEAM_W, TEAM_H - BAR_Y, C_DARK, 215);
}
