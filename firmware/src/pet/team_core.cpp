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
static const uint16_t C_PAPER   = RGB(244, 241, 232);
static const uint16_t C_PAPER_E = RGB(196, 190, 176);
static const uint16_t C_DONE    = RGB(110, 200, 120);
static const uint16_t C_FAIL    = RGB(232, 92, 80);
static const uint16_t C_GEAR    = RGB(214, 208, 196);
static const uint16_t C_GEAR_2  = RGB(150, 160, 176);
static const int32_t  THINK_WALK_MS = 6000;   // thinking this long before the walk to the board
static const int32_t  BOARD_STAY_MS = 4000;   // and there at least this long

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
    int16_t wait_ms;               // stood aside for someone this long
} Actor;

// A session. Its desk is `slot`: sessions sit in the order the app sends
// them, six to an office, and move up when one before them leaves.
typedef struct {
    bool    used, closing;
    uint8_t id;
    uint8_t slot;
    uint8_t color;
    char    state;                 // the finer one (`tx`)
    int8_t  ctx;
    char    mode;
    uint8_t subs_want, quiet;
    char    name[TEAM_NAME];
    int32_t think_ms;              // how long the state has been what it is
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

static uint8_t  skin = TEAM_SKIN_DESKS;
static bool     dark = false;
static uint16_t* cache = NULL;
static int      cache_key = -1;
static uint32_t style_seed = 0;

static int per_office(void) { return skin == TEAM_SKIN_CUBICLES ? TEAM_CUBICLES : TEAM_DESKS; }

// ── the cubicle skin: a + of partitions, one session to a quarter ──
enum { CUB_ROWS = 0, CUB_ISLAND, CUB_L };       // how a cubicle's desks stand
static const int CUB_MID_X = 240, CUB_MID_Y = 290;
static const int CUB_AISLE_L = 219, CUB_AISLE_R = 261;   // either side of the upright partition

// Neighbouring cubicles never match, and the mix changes with the seed.
static int cub_style(int slot) { return (int)((style_seed + (uint32_t)slot + (uint32_t)(slot / TEAM_CUBICLES)) % 3); }
static void cub_box(int slot, int* x0, int* y0, int* w, int* h) {
    const int d = slot % TEAM_CUBICLES;
    *x0 = d % 2 ? 282 : 4;
    *w = 194;
    *y0 = d / 2 ? CUB_MID_Y + 12 : ROW_TOP - 2;
    *h = d / 2 ? BAR_Y - 4 - *y0 : CUB_MID_Y - 4 - *y0;
}
// The sub-agents' column in an L: along the outer wall.
static int cub_l_col(int slot) {
    int x0, y0, w, h;
    cub_box(slot, &x0, &y0, &w, &h);
    return slot % 2 ? x0 + w - 22 : x0 + 22;
}

static int desk_cx(int slot) {
    if (skin != TEAM_SKIN_CUBICLES) return COL_W / 2 + (slot % TEAM_DESKS % 3) * COL_W;
    int x0, y0, w, h;
    cub_box(slot, &x0, &y0, &w, &h);
    if (cub_style(slot) != CUB_L) return x0 + w / 2;
    return slot % 2 ? x0 + 86 : x0 + w - 86;
}
static int desk_top(int slot) {
    if (skin != TEAM_SKIN_CUBICLES) return ROW_TOP + (slot % TEAM_DESKS / 3) * ROW_H;
    int x0, y0, w, h;
    cub_box(slot, &x0, &y0, &w, &h);
    return y0 + (cub_style(slot) == CUB_ISLAND ? 12 : 2);
}
// The aisle a desk's people walk: between the columns, the middle column
// alternating by row.
static int desk_aisle(int slot) {
    if (skin == TEAM_SKIN_CUBICLES) return slot % 2 ? CUB_AISLE_R : CUB_AISLE_L;
    const int d = slot % TEAM_DESKS, col = d % 3;
    return col == 0 ? 160 : col == 2 ? 320 : (d / 3 ? 320 : 160);
}
static void seat_of(int slot, int who, int* x, int* y) {
    if (who < 0) { *x = desk_cx(slot); *y = desk_top(slot) + 6; }
    else if (skin == TEAM_SKIN_CUBICLES && cub_style(slot) == CUB_L) {
        *x = cub_l_col(slot);
        *y = desk_top(slot) + 4 + who * 34;
    } else if (skin == TEAM_SKIN_CUBICLES && cub_style(slot) == CUB_ISLAND) {
        // Across the island from the lead, backs to us.
        *x = desk_cx(slot) - 42 + who * 28;
        *y = desk_top(slot) + 70;
    }
    else         { *x = desk_cx(slot) - 42 + who * 28; *y = desk_top(slot) + 74; }
}
// The aisle nearest a spot on the floor.
static int aisle_near(float x) {
    if (skin == TEAM_SKIN_CUBICLES) return x < CUB_MID_X ? CUB_AISLE_L : CUB_AISLE_R;
    return x < TEAM_W / 2 ? 160 : 320;
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
            d->ctx = -1;
            d->mode = '-';
            d->model = '-';
        }
        if (!end) break;
        p = end + 1;
    }
    return n;
}

void team_parse_extra(const char* tx, TeamDeskIn* in, int n) {
    if (!tx) return;
    const size_t len = strlen(tx);
    for (int i = 0; i < n && (size_t)(i * 4 + 4) <= len; i++) {
        const char* p = tx + i * 4;
        if (strchr("wtqdcfeib", p[0])) in[i].state = p[0];
        if (p[1] >= '0' && p[1] <= '9') in[i].ctx = (int8_t)(p[1] - '0');
        else if (p[1] >= 'a' && p[1] <= 'z') in[i].ctx = (int8_t)(p[1] - 'a' + 10);
        in[i].mode = p[2];
        in[i].model = p[3];
    }
}

// Busy enough for the sub-agents' screens to be lit and for them to wander.
static bool busy_state(char s) { return s == 'w' || s == 't' || s == 'd' || s == 'c'; }

// ──────────────── people ────────────────
static bool present(const Actor* a) { return a->phase != TEAM_OFF && a->phase != TEAM_LEAVE; }
static bool at_cooler(const Actor* a) { return a->phase == TEAM_TO_COOLER || a->phase == TEAM_COOLER; }
static bool by_wall(const Actor* a) { return a->y < ROW_TOP - 8; }

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
// Sub-agents keep to one side of the aisle or the other, so people going
// opposite ways pass instead of walking through each other.
static int lane(int who) { return who < 0 ? 0 : (who % 2 ? 5 : -5); }

static void go_seat(Actor* a, int slot, int who, TeamPhase phase) {
    int sx, sy;
    seat_of(slot, who, &sx, &sy);
    const int base = desk_aisle(slot), ax = base + lane(who);
    a->wp_n = a->wp_i = 0;
    const int near = aisle_near(a->x);
    if (!by_wall(a) && near != base) {
        add_wp(a, near + lane(who), (int)a->y);
        add_wp(a, near + lane(who), HALL_Y);
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
    a->office = (uint8_t)(d->slot / per_office());
    a->x = (float)DOOR_X;
    a->y = (float)HALL_Y;
    a->small = who >= 0;
    // The same session is the same person every time it comes in.
    uint32_t h = (uint32_t)d->id * 2654435761u + (uint32_t)(who + 2) * 40503u;
    h ^= h >> 13;
    a->look = (uint8_t)((h >> 4) % 36);
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
        if (a->office != slot / per_office()) go_spot(a, desk_aisle(old), DOOR_X, HALL_Y, TEAM_MOVE_OUT);
        else if (a->phase == TEAM_SEAT || a->phase == TEAM_ENTER || a->phase == TEAM_RETURN)
            go_seat(a, slot, who, TEAM_RETURN);
        // At the board or the cooler: they come back to the new desk.
    }
}

// Someone else on their feet is right here and goes first: the one earlier
// in the list. Two people never stand aside for each other.
static bool must_yield(const Actor* a) {
    for (int i = 0; i < TEAM_MAX; i++) {
        if (!desks[i].used) continue;
        for (int who = -1; who < TEAM_SEATS; who++) {
            const Actor* o = who < 0 ? &desks[i].lead : &desks[i].sub[who];
            if (o >= a) continue;
            if (o->office != a->office || !o->walking) continue;
            const float dx = o->x - a->x, dy = o->y - a->y;
            if (dx > -13 && dx < 13 && dy > -10 && dy < 10) return true;
        }
    }
    return false;
}

// Move along the path; true when the last waypoint is reached.
static bool walk(Actor* a, float dist) {
    // A moment's pause lets the other get ahead; then on regardless.
    if (a->wp_i < a->wp_n && must_yield(a) && a->wait_ms < 600) {
        a->wait_ms = (int16_t)(a->wait_ms + (int)(dist * 1000.0f / SPEED));
        a->walking = false;
        return false;
    }
    if (!must_yield(a)) a->wait_ms = 0;
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

// Where someone is standing by the wall, or heading to stand.
static bool standing_at(const Actor* a, int* x, int* y) {
    if (a->phase == TEAM_BOARD || a->phase == TEAM_COOLER) { *x = (int)a->x; *y = (int)a->y; return true; }
    if ((a->phase == TEAM_TO_BOARD || a->phase == TEAM_TO_COOLER) && a->wp_n > 0) {
        *x = a->wp[a->wp_n - 1][0];
        *y = a->wp[a->wp_n - 1][1];
        return true;
    }
    return false;
}

// A place to stand at the board (`cooler` false) or the water cooler: the
// one farthest from everyone already standing in that office. One row along
// the wall, shoulder to shoulder; a second row, half a step over, only once
// the first is taken.
static void stand_spot(const Actor* self, int office, bool cooler, bool small, int* x, int* y) {
    static const int16_t BOARD_XS[] = { 226, 182, 270, 160, 248, 204, 292 };
    static const int16_t COOLER_XS[] = { 348, 436, 326, 458, 304, 414 };
    const int16_t* xs = cooler ? COOLER_XS : BOARD_XS;
    const int n = cooler ? 6 : 7;
    int best = -1, best_x = xs[0], best_y = HALL_Y;
    for (int row = 0; row < 2; row++) {
        for (int k = 0; k < n; k++) {
            const int cx = xs[k] + row * 11, cy = HALL_Y + row * 18 + (small ? 8 : 0);
            // The second row must keep clear of the cubicles' upright partition.
            if (row == 1 && skin == TEAM_SKIN_CUBICLES && cx > 216 && cx < 264) continue;
            int nearest = 999;
            for (int i = 0; i < TEAM_MAX; i++) {
                if (!desks[i].used) continue;
                for (int who = -1; who < TEAM_SEATS; who++) {
                    const Actor* o = who < 0 ? &desks[i].lead : &desks[i].sub[who];
                    int ox, oy;
                    if (o == self || o->office != office || !standing_at(o, &ox, &oy)) continue;
                    const int dx = ox > cx ? ox - cx : cx - ox, dy = oy > cy ? oy - cy : cy - oy;
                    // A head shorter or taller on the same line is the same line.
                    const int dist = dx + (dy <= 8 ? 0 : dy);
                    if (dist < nearest) nearest = dist;
                }
            }
            // The earlier in the list, the better, when nobody is near.
            const int score = (nearest > 22 ? 22 : nearest) * 100 - row * 50 - k;
            if (score > best) { best = score; best_x = cx; best_y = cy; }
        }
        if (best >= 22 * 100 - 50) break;       // a clear spot in the first row
    }
    *x = best_x;
    *y = best_y;                                 // a sub-agent's is lower: feet on one line
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

// The lead belongs at the whiteboard: a thought that has lasted, or a plan
// being made (plan mode), which is board work whatever the tool.
static bool wants_board(const Desk* d) {
    if (d->state == 't') return d->think_ms > THINK_WALK_MS;
    return d->mode == 'p' && d->state == 'w' && d->think_ms > THINK_WALK_MS;
}

static void step_actor(Actor* a, int who, const Desk* d, uint32_t dt) {
    if (a->phase == TEAM_OFF) return;
    const float dist = SPEED * dt / 1000.0f;
    const int slot = d->slot, aisle = desk_aisle(slot) + lane(who);
    const int on_break = subs_at_cooler(d);
    switch (a->phase) {
    case TEAM_ENTER:
    case TEAM_RETURN:
        if (walk(a, dist)) { a->phase = TEAM_SEAT; a->walking = false; }
        break;
    case TEAM_MOVE_OUT:
        // Out of one office's door and in at the other's.
        if (walk(a, dist)) {
            a->office = (uint8_t)(slot / per_office());
            go_seat(a, slot, who, TEAM_ENTER);
        }
        break;
    case TEAM_TO_BOARD:
        if (walk(a, dist)) {
            a->phase = TEAM_BOARD;
            a->walking = false;
            a->timer_ms = who >= 0 ? 5000 + (int32_t)(rnd() % 5000) : BOARD_STAY_MS;
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
        // Not straight back, though, unless someone is asking for them.
        bool back = d->closing;
        a->timer_ms -= (int32_t)dt;
        if (who < 0) back = back || (!wants_board(d) && (a->timer_ms <= 0 || d->state == 'q'));
        else back = back || a->timer_ms <= 0;
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
                stand_spot(a, a->office, true, false, &x, &y);
                go_spot(a, aisle, x, y, TEAM_TO_COOLER);
            } else if (wants_board(d)) {
                // A thought that lasts takes the lead to the board.
                stand_spot(a, a->office, false, false, &x, &y);
                go_spot(a, aisle, x, y, TEAM_TO_BOARD);
            }
        } else if (on_break < d->quiet) {
            int x, y;
            stand_spot(a, a->office, true, true, &x, &y);
            go_spot(a, aisle, x, y, TEAM_TO_COOLER);
        } else if (busy_state(d->state)) {
            a->timer_ms -= (int32_t)dt;
            if (a->timer_ms <= 0) {
                int x, y;
                stand_spot(a, a->office, false, true, &x, &y);
                go_spot(a, aisle, x, y, TEAM_TO_BOARD);
            }
        }
        break;
    default: break;
    }
}

// ──────────────── the offices ────────────────
void team_reset(void) {
    memset(desks, 0, sizeof(desks));
    dark = false;
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
// The session's own colour (from its id) when no other is worn less, so it
// usually wears the same one each time.
static uint8_t pick_color(uint8_t id) {
    int count[TEAM_DESKS] = { 0 };
    for (int i = 0; i < TEAM_MAX; i++) if (desks[i].used && !desks[i].closing) count[desks[i].color]++;
    int best = id % TEAM_DESKS;
    for (int c = 0; c < TEAM_DESKS; c++) if (count[c] < count[best]) best = c;
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
            d->color = pick_color(in[k].id);
            d->used = true;
            d->id = in[k].id;
            d->slot = (uint8_t)k;
            enter(&d->lead, d, -1);
        } else if (d->slot != k) {
            move_desk(d, k);
        }
        if (d->state != in[k].state) d->think_ms = 0;
        d->state = in[k].state;
        d->ctx = in[k].ctx;
        d->mode = in[k].mode;
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
    const bool moved = a->office != slot / per_office();
    a->office = (uint8_t)(slot / per_office());
    if (!moved && a->wp_n > 0 && (a->phase == TEAM_TO_BOARD || a->phase == TEAM_BOARD)) {
        a->x = a->wp[a->wp_n - 1][0];
        a->y = a->wp[a->wp_n - 1][1];
        a->phase = TEAM_BOARD;
    } else if (to_cooler) {
        if (!moved && a->wp_n > 0 && at_cooler(a)) { a->x = a->wp[a->wp_n - 1][0]; a->y = a->wp[a->wp_n - 1][1]; }
        else { int x, y; stand_spot(a, a->office, true, who >= 0, &x, &y); a->x = (float)x; a->y = (float)y; }
        a->phase = TEAM_COOLER;
    } else {
        int sx, sy;
        seat_of(slot, who, &sx, &sy);
        a->x = (float)sx;
        a->y = (float)sy;
        a->phase = TEAM_SEAT;
    }
    // Where they were put is where they are: no path left to be read later.
    a->wp[0][0] = (int16_t)a->x;
    a->wp[0][1] = (int16_t)a->y;
    a->wp_n = a->wp_i = 1;
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
        if (d->think_ms < 3600000) d->think_ms += (int32_t)dt;
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

int team_standing(int office, int* xs, int* ys, int max) {
    int n = 0;
    for (int i = 0; i < TEAM_MAX; i++) {
        if (!desks[i].used) continue;
        for (int who = -1; who < TEAM_SEATS; who++) {
            const Actor* a = who < 0 ? &desks[i].lead : &desks[i].sub[who];
            if (a->office != office || (a->phase != TEAM_BOARD && a->phase != TEAM_COOLER)) continue;
            if (xs && ys && n < max) { xs[n] = (int)a->x; ys[n] = (int)a->y; }
            n++;
        }
    }
    return n;
}

void team_set_cache(uint16_t* buf) { cache = buf; cache_key = -1; }
void team_set_dark(bool on) { dark = on; }
bool team_dark(void) { return dark; }

int team_desk_at(int office, int x, int y) {
    if (dark) return -1;
    for (int i = 0; i < per_office(); i++) {
        const int slot = office * per_office() + i;
        if (!at_slot(slot, false)) continue;
        const int cx = desk_cx(slot), top = desk_top(slot);
        if (x >= cx - 52 && x <= cx + 52 && y >= top - 8 && y <= top + 66) return i;
    }
    return -1;
}

int team_office_waiting(void) {
    int best = -1;
    for (int i = 0; i < TEAM_MAX; i++) {
        if (!desks[i].used || desks[i].closing || desks[i].state != 'q') continue;
        const int o = desks[i].slot / per_office();
        if (best < 0 || o < best) best = o;
    }
    return best;
}

int team_bar_y(void) { return BAR_Y; }
int team_skin(void) { return skin; }
int team_per_office(void) { return per_office(); }
void team_set_seed(uint32_t seed) { style_seed = seed; }
// Desks are numbered through the offices, so a change of skin moves people
// between them: call team_snap() after it.
void team_set_skin(int s) { skin = s == TEAM_SKIN_CUBICLES ? TEAM_SKIN_CUBICLES : TEAM_SKIN_DESKS; }
void team_set_time(long seconds) { wall_secs = seconds; }

int team_sessions(void) {
    int n = 0;
    for (int i = 0; i < TEAM_MAX; i++) n += desks[i].used && !desks[i].closing;
    return n;
}
int team_offices(void) {
    const int n = team_sessions();
    const int per = per_office();
    return n <= per ? 1 : (n + per - 1) / per;
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
    out->ctx = -1;
    if (desk < 0 || desk >= per_office()) return;
    const int slot = office * per_office() + desk;
    out->name_cx = desk_cx(slot);
    out->name_y = desk_top(slot) + 111;
    if (skin == TEAM_SKIN_CUBICLES) {
        int x0, y0, w, h;
        cub_box(slot, &x0, &y0, &w, &h);
        out->name_y = y0 + h - 20;
    }
    const Desk* d = at_slot(slot, false);
    if (!d) return;
    out->live = true;
    out->id = d->id;
    out->ctx = d->ctx;
    out->mode = d->mode;
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

typedef enum { POSE_STAND, POSE_SEAT, POSE_SLEEP, POSE_BACK, POSE_CUP, POSE_SEAT_BACK } Pose;

static void person(const Actor* a, Pose pose) {
    const float k = a->small ? 0.8f : 1.0f;
    const uint16_t skin = SKIN[a->look % 3], hair = HAIR[(a->look / 3) % 4];
    const int style = (a->look / 12) % 3;             // 0 short, 1 long, 2 cropped
    const bool seated = pose == POSE_SEAT || pose == POSE_SLEEP || pose == POSE_SEAT_BACK;
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
    if (pose == POSE_BACK || pose == POSE_SEAT_BACK) return;
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

static void draw_partitions(void);

// The parts of the room that never change (kept in the cache when there is one).
static void draw_room_fixed(void) {
    rect(0, 0, TEAM_W, WALL_H, C_WALL);
    rect(0, 0, TEAM_W, 8, C_WALL_T);
    for (int ty = 0; ty * 40 + WALL_H < TEAM_H; ty++)
        for (int tx = 0; tx < TEAM_W / 40; tx++)
            rect(tx * 40, WALL_H + ty * 40, 40, 40, (tx + ty) & 1 ? C_FLOOR_A : C_FLOOR_B);
    rect(0, WALL_H - 5, TEAM_W, 5, C_BASE);
    shade(0, WALL_H, TEAM_W, 5, C_DARK, 70);
    if (skin == TEAM_SKIN_CUBICLES) draw_partitions();

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

    // The whiteboard.
    rrect(BOARD_X0 - 4, BOARD_Y0 - 4, BOARD_X1 - BOARD_X0 + 8, BOARD_Y1 - BOARD_Y0 + 8, 4, C_FRAME);
    rect(BOARD_X0, BOARD_Y0, BOARD_X1 - BOARD_X0, BOARD_Y1 - BOARD_Y0, C_BOARD);
    rrect(BOARD_X0 + 50, BOARD_Y1 + 4, 60, 4, 2, C_BASE);
    rect(BOARD_X0 + 58, BOARD_Y1 + 1, 10, 3, SHIRT[0]);
    rect(BOARD_X0 + 72, BOARD_Y1 + 1, 10, 3, SHIRT[1]);
    rect(BOARD_X0 + 86, BOARD_Y1 + 1, 10, 3, SHIRT[2]);

    // The water cooler: a bottle on a stand.
    oval(COOLER_X, WALL_H + 1, 16, 4, C_DARK, 90);
    rrect(COOLER_X - 11, 12, 22, 30, 7, C_WATER);
    rrect(COOLER_X - 8, 15, 4, 22, 2, C_WATER_HI);
    rect(COOLER_X - 5, 40, 10, 4, C_WATER);
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

// What changes on the wall: the clock's hands, the writing on the board,
// the cooler's bubbles when someone is at it.
static void draw_room_live(int office) {
    if (wall_secs >= 0) {
        const float m = (wall_secs % 3600) / 3600.0f * 6.2831853f;
        const float h = (wall_secs % 43200) / 43200.0f * 6.2831853f;
        hand(104, 38, h, 6.5f, 1.3f, C_DARK);
        hand(104, 38, m, 10.0f, 0.9f, C_DARK);
    }
    disc(104, 38, 1.6f, C_ALERT);
    for (int i = 0; i < stroke_n; i++) {
        int len = strokes[i].len;
        if (strokes[i].x + len > BOARD_X1 - 8) len = BOARD_X1 - 8 - strokes[i].x;
        rrect(strokes[i].x, strokes[i].y, len, 2, 1, strokes[i].color);
    }
    bool company = false;
    for (int i = 0; i < TEAM_MAX; i++) {
        if (!desks[i].used) continue;
        for (int who = -1; who < TEAM_SEATS; who++) {
            const Actor* a = who < 0 ? &desks[i].lead : &desks[i].sub[who];
            company = company || (a->phase == TEAM_COOLER && a->office == office);
        }
    }
    if (company && !dark) {
        const int t = (int)(clock_ms % 1800);
        disc(COOLER_X + 3, 38 - t / 80.0f, 1.8f, C_WATER_HI);
        disc(COOLER_X - 1, 38 - ((t + 900) % 1800) / 80.0f, 1.3f, C_WATER_HI);
    }
}

// A gear: teeth round a disc with a hole, turned by `angle`.
static void gear(float cx, float cy, float r, int teeth, float angle, uint16_t c, uint16_t hole) {
    for (int k = 0; k < teeth; k++) {
        const float a = angle + k * 6.2831853f / teeth;
        disc(cx + cosf(a) * r * 0.92f, cy + sinf(a) * r * 0.92f, r * 0.27f, c);
    }
    disc(cx, cy, r * 0.8f, c);
    disc(cx, cy, r * 0.3f, hole);
}

// Thinking: a larger and a smaller gear, meshed, turning against each other.
static void gears(int x, int y) {
    const float a = clock_ms / 520.0f;
    gear(x + 12.5f, y - 9.0f, 5.0f, 6, -a * 1.6f + 0.52f, C_GEAR_2, C_DARK);
    gear(x, y, 7.5f, 8, a, C_GEAR, C_DARK);
}

// A round badge over a head, with a tail towards it.
static void badge(int cx, int top, int bob, uint16_t c) {
    oval(cx + 21, top - 9 - bob, 11, 12, C_DARK, 80);
    rrect(cx + 10, top - 22 - bob, 20, 22, 7, c);
    disc(cx + 11, top + 2 - bob, 2.5f, c);
}

// The context used, as a stack of paper on the desk: full when it is as
// tall as the monitor.
static void paper_stack(int x, int base, int ctx, uint8_t id, bool shrinking) {
    int h = ctx * 42 / TEAM_CTX_FULL;
    if (ctx > 0 && h < 2) h = 2;
    // Compacting: the stack sinks and comes back, sheets going out of it.
    if (shrinking) h = h * (255 - pulse(1600) * 3 / 4) / 255;
    oval(x + 10, base + 1, 13, 3, C_DARK, 70);
    for (int k = 0; k * 2 < h; k++) {
        const int y = base - 2 - k * 2;
        const int dx = (int)((k * 7 + id) % 3) - 1;      // not quite squared up
        rect(x + dx, y, 20, 2, C_PAPER);
        rect(x + dx, y + 1, 20, 1, C_PAPER_E);
    }
    if (h < 2) return;
    // The top sheet, which says how near the limit it is.
    const int top = base - ((h + 1) / 2) * 2;
    const uint16_t c = ctx >= TEAM_CTX_FULL * 9 / 10 ? C_FAIL : ctx >= TEAM_CTX_FULL * 3 / 4 ? C_ALERT : C_WHITE;
    rect(x - 1, top - 1, 20, 2, c);
    rect(x + 2, top - 1, 9, 1, mix(c, C_DARK, 60));
}

static const uint16_t C_PART    = RGB(126, 138, 152);
static const uint16_t C_PART_HI = RGB(160, 172, 186);
static const uint16_t C_PART_F  = RGB(88, 98, 112);

// A partition seen from above and a little in front: a top edge and a face.
static void panel_h(int x, int y, int w) {
    shade(x, y + 12, w, 4, C_DARK, 70);
    rect(x, y + 4, w, 9, C_PART_F);
    rect(x, y, w, 5, C_PART);
    rect(x, y, w, 1, C_PART_HI);
}
static void panel_v(int x, int y, int h) {
    shade(x + 6, y + 4, 4, h, C_DARK, 60);
    rect(x, y, 6, h, C_PART);
    rect(x, y, 1, h, C_PART_HI);
    rect(x, y + h, 6, 9, C_PART_F);
}

// The + that makes four cubicles, with a gap either side of its centre for
// the aisles. The top two are open to the wall, where people stand.
static void draw_partitions(void) {
    panel_v(CUB_MID_X - 3, ROW_TOP - 10, BAR_Y - 14 - (ROW_TOP - 10));
    panel_h(0, CUB_MID_Y - 4, CUB_AISLE_L - 17);
    panel_h(CUB_AISLE_R + 17, CUB_MID_Y - 4, TEAM_W - CUB_AISLE_R - 17);
}

static void draw_bench(int cx, int y, uint16_t top_c, uint16_t front_c, uint16_t hi_c) {
    rrect(cx - 60, y, 120, 13, 4, front_c);
    rrect(cx - 60, y, 120, 9, 4, top_c);
    rect(cx - 56, y + 1, 112, 1, hi_c);
}

// A cubicle's sub-agent desks: a row, the far side of an island, or the
// upright of an L. They are furniture: there whether or not anyone sits.
static void draw_cubicle_subs(int slot, const Desk* d, int fade) {
    const int cx = desk_cx(slot), top = desk_top(slot), style = cub_style(slot);
    const bool busy = d && busy_state(d->state);
    const uint16_t c_top = mix(C_DESK, C_FLOOR_A, fade), c_front = mix(C_DESK_F, C_FLOOR_A, fade);
    const uint16_t c_hi = mix(C_DESK_HI, C_FLOOR_A, fade);
    #define SEATED(i) (d && d->sub[i].phase == TEAM_SEAT && d->sub[i].office == slot / per_office())
    #define SCREEN(i) (busy ? mix(C_SCREEN, d->sub[i].shirt, 120 + pulse(700 + (i) * 130) / 2) : C_SCREEN)
    if (style == CUB_ROWS) {
        for (int i = 0; i < TEAM_SEATS; i++) if (SEATED(i)) person(&d->sub[i], POSE_SEAT);
        oval(cx, top + 108, 62, 5, C_DARK, 70);
        draw_bench(cx, top + 94, c_top, c_front, c_hi);
    } else if (style == CUB_ISLAND) {
        // Pushed up against the lead's desk, the sub-agents on the near side.
        oval(cx, top + 76, 62, 5, C_DARK, 70);
        draw_bench(cx, top + 60, c_top, c_front, c_hi);
    }
    for (int i = 0; i < TEAM_SEATS; i++) {
        int sx, sy;
        seat_of(slot, i, &sx, &sy);
        if (style == CUB_L) {
            if (SEATED(i)) person(&d->sub[i], POSE_SEAT);
            rrect(sx - 16, sy + 20, 32, 12, 3, c_front);
            rrect(sx - 16, sy + 20, 32, 8, 3, c_top);
            if (SEATED(i)) {
                rrect(sx - 8, sy + 14, 16, 10, 2, C_DIM);
                rect(sx - 6, sy + 16, 12, 6, SCREEN(i));
            }
        } else if (SEATED(i)) {
            const int ly = style == CUB_ISLAND ? top + 58 : top + 88;
            rrect(sx - 8, ly, 16, 10, 2, C_DIM);
            rect(sx - 6, ly + 2, 12, 6, SCREEN(i));
            if (style == CUB_ISLAND) person(&d->sub[i], POSE_SEAT_BACK);
        }
    }
    #undef SEATED
    #undef SCREEN
}

static void draw_desk(int slot) {
    const Desk* d = dark ? NULL : at_slot(slot, false);
    const bool live = d != NULL;
    const bool seated = live && d->lead.phase == TEAM_SEAT && d->lead.office == slot / per_office();
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
        else if (d->state == 'c') screen = mix(C_SCREEN, C_ALERT, 60 + pulse(1600) / 4);
        else if (d->state == 'f') screen = mix(C_SCREEN, C_DONE, 150);
        else if (d->state == 'e') screen = mix(C_SCREEN, C_FAIL, 150);
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
    if (live && seated && d->state == 'f') {
        // A tick on the screen: done.
        for (int i = 0; i < 4; i++) rect(cx - 33 + i, top + 25 + i, 2, 2, C_DARK);
        for (int i = 0; i < 8; i++) rect(cx - 29 + i, top + 28 - i, 2, 2, C_DARK);
    } else if (live && seated && d->state == 'e') {
        rect(cx - 28, top + 18, 3, 9, C_DARK);
        rect(cx - 28, top + 29, 3, 3, C_DARK);
    } else if (live && seated && d->state == 'c') {
        // A bar closing in: the context being squeezed.
        const int w = 22 - pulse(1600) * 16 / 255;
        rect(cx - 27 - w / 2, top + 23, w, 4, mix(C_ALERT, C_DARK, 60));
    }
    // Keyboard, hands, and the context's paper (a mug when it isn't known).
    rrect(cx - 9, top + 41, 26, 9, 2, DIMMED(C_KEYS));
    rect(cx - 7, top + 44, 22, 1, DIMMED(C_DIM));
    rect(cx - 7, top + 47, 22, 1, DIMMED(C_DIM));
    if (live && seated && d->state != 'i' && d->state != 'f' && d->state != 'e') {
        const uint16_t skin = SKIN[d->lead.look % 3];
        const int tap = d->state == 'w' ? (int)((clock_ms / 110) % 4) : 1;
        disc(cx - 4 + (tap == 0) * 2, top + 44 + (tap == 1), 2.6f, skin);
        disc(cx + 8 - (tap == 2) * 2, top + 44 + (tap == 3), 2.6f, skin);
    }
    if (live && d->ctx >= 0) {
        paper_stack(cx + 22, top + 54, d->ctx, d->id, d->state == 'c');
    } else {
        disc(cx + 34, top + 45, 5, DIMMED(C_WHITE));
        disc(cx + 34, top + 45, 3.2f, DIMMED(RGB(110, 72, 48)));
    }
    #undef DIMMED

    if (skin == TEAM_SKIN_CUBICLES) { draw_cubicle_subs(slot, d, fade); return; }
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
        const bool busy = busy_state(d->state);
        rrect(sx - 8, top + 88, 16, 10, 2, C_DIM);
        rect(sx - 6, top + 90, 12, 6, busy ? mix(C_SCREEN, d->sub[s].shirt, 120 + pulse(700 + s * 130) / 2) : C_SCREEN);
    }
}

static void draw_walker(const Actor* a) {
    person(a, a->phase == TEAM_BOARD ? POSE_BACK : a->phase == TEAM_COOLER ? POSE_CUP : POSE_STAND);
}

void team_render(uint16_t* buf, int office) {
    // The fixed parts of the room: copied from the cache when there is one.
    if (cache) {
        if (cache_key != skin) {
            fb = cache;
            draw_room_fixed();
            cache_key = skin;
        }
        memcpy(buf, cache, (size_t)TEAM_W * TEAM_H * sizeof(uint16_t));
        fb = buf;
    } else {
        fb = buf;
        draw_room_fixed();
    }
    draw_room_live(office);
    for (int i = 0; i < per_office(); i++) draw_desk(office * per_office() + i);
    if (dark) {
        // Shut: the lights are off and nobody is in.
        shade(0, 0, TEAM_W, BAR_Y, C_DARK, 150);
        shade(0, BAR_Y, TEAM_W, TEAM_H - BAR_Y, C_DARK, 215);
        return;
    }
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
        const bool seat = a->phase == TEAM_SEAT;
        if (d->state == 'q' && seat) {
            const int bob = pulse(1000) / 80;
            badge(cx, top, bob, C_ALERT);
            glyph(cx + 15, top - 19 - bob, '?', 2, C_DARK);
        } else if (d->state == 'e' && seat) {
            badge(cx, top, 0, C_FAIL);
            glyph(cx + 15, top - 19, '!', 2, C_DARK);
        } else if (d->state == 'f' && seat) {
            // Done, your move: a tick, and it nods for attention at first.
            const int bob = d->think_ms < 6000 ? pulse(800) / 64 : 0;
            badge(cx, top, bob, C_DONE);
            for (int i = 0; i < 4; i++) rect(cx + 14 + i, top - 13 + i - bob, 2, 3, C_DARK);
            for (int i = 0; i < 8; i++) rect(cx + 18 + i, top - 10 - i - bob, 2, 3, C_DARK);
        } else if (d->state == 'i' && seat) {
            const int t = (int)(clock_ms % 2400);
            glyph(cx + 12 + t / 300, top - 6 - t / 150, 'z', 2, mix(C_FLOOR_A, C_WHITE, 220 - t / 12));
            const int t2 = (t + 1200) % 2400;
            glyph(cx + 12 + t2 / 300, top - 6 - t2 / 150, 'z', 1, mix(C_FLOOR_A, C_WHITE, 220 - t2 / 12));
        } else if (d->state == 'c' && seat) {
            // Compacting: pressed in from both sides.
            const int in = pulse(1600) / 64;
            glyph(cx + 6 + in, top - 14, '>', 2, C_ALERT);
            glyph(cx + 24 - in, top - 14, '<', 2, C_ALERT);
            rect(cx + 19, top - 13, 2, 12, C_ALERT);
        } else if (a->phase == TEAM_BOARD || ((d->state == 'd' || d->state == 't') && seat)) {
            // Thinking: at the board, or at the desk (while the sub-agents
            // work, or before the walk over).
            gears(cx + 19, top - 1);
        }
    }

    shade(0, BAR_Y, TEAM_W, TEAM_H - BAR_Y, C_DARK, 215);
}
