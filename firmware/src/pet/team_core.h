#pragma once
#include <stdint.h>
#include <stdbool.h>

// The agent team view: Claude Code sessions as desks in a small office, the
// main agent and its sub-agents as people. Pure (no LVGL, no Arduino): the
// scene's rules and its drawing into a 480x480 RGB565 buffer, which
// team_view.cpp shows with its text. Tested by test_team.
//
// The app sends the desks as `tm`:
// `<2 hex id><state><subs digit><quiet digit><name>` per session, joined
// with `|`. States: w working, t thinking, q waiting for you, d waiting for
// its sub-agents, i idle,
// b idle for a while (on a break). `quiet` is how many of the sub-agents
// have done nothing for a while.
//
// `tx` adds four characters per desk, in the same order: the finer state
// (those six, and c compacting, f turn finished, e stopped on an error), the
// context used (0-9 a-z: thirty-fifths of the window, - unknown), the
// permission mode (p plan, - default) and the model.

#define TEAM_W      480
#define TEAM_H      480
#define TEAM_DESKS  6     // to an office, at most
#define TEAM_CUBICLES 4   // to an office in the cubicle skin

// How the office is drawn (the app's `tv`: 1 = desks, 2 = cubicles).
#define TEAM_SKIN_DESKS    0   // six open desks
#define TEAM_SKIN_CUBICLES 1   // four cubicles around a + of partitions
#define TEAM_MAX    12    // sessions: two offices (what one Bluetooth write holds)
#define TEAM_SEATS  4     // sub-agents drawn at a desk; more show as "+N"
#define TEAM_NAME   13

typedef struct {
    uint8_t id;
    char    state;
    uint8_t subs;
    uint8_t quiet;
    char    name[TEAM_NAME];
    int8_t  ctx;      // context used, 0..TEAM_CTX_FULL; -1 unknown
    char    mode;     // permission mode: 'p' plan, '-' default, ...
    char    model;    // 'f' 'o' 's' 'h' ..., '-' unknown
} TeamDeskIn;

#define TEAM_CTX_FULL 35

// Where a person is in their day.
typedef enum {
    TEAM_OFF = 0,     // not in the office
    TEAM_ENTER,       // walking from the door to their seat
    TEAM_SEAT,
    TEAM_TO_BOARD,
    TEAM_BOARD,       // at the whiteboard
    TEAM_TO_COOLER,
    TEAM_COOLER,      // at the water cooler
    TEAM_RETURN,
    TEAM_LEAVE,       // walking to the door
    TEAM_MOVE_OUT,    // walking to the door, to a desk in the other office
} TeamPhase;

// What team_view.cpp writes under a desk.
typedef struct {
    bool        live;        // a session sits here
    const char* name;
    char        state;
    int         more;        // sub-agents beyond the bench's seats
    int         name_cx, name_y;   // the name: centre x, top y
    uint8_t     id;
    int         ctx;         // context used, 0..TEAM_CTX_FULL; -1 unknown
    char        mode;
} TeamDeskInfo;

// Parse `tm` into at most `max` desks; returns the count. Malformed entries
// are skipped.
int  team_parse(const char* tm, TeamDeskIn* out, int max);
// Apply `tx` to the `n` desks team_parse read. Too short, or with a letter
// it doesn't know, a desk keeps what `tm` said.
void team_parse_extra(const char* tx, TeamDeskIn* in, int n);

void team_reset(void);
// The sessions now in, in desk order: new ones walk in, missing ones walk
// out, the ones after a gap move up a desk, sub-agents come and go.
void team_set(const TeamDeskIn* in, int n);
// Put everyone where they are heading (when the view comes up).
void team_snap(void);
void team_step(uint32_t dt_ms);
// The wall clock: seconds since local midnight, or -1 for no hands.
void team_set_time(long seconds);
void team_render(uint16_t* buf, int office);   // TEAM_W * TEAM_H pixels
// A second TEAM_W * TEAM_H buffer to keep the room's fixed parts in, so a
// frame copies them instead of drawing them. NULL: draw them every frame.
void team_set_cache(uint16_t* buf);
// No word from the computer: the office is shown shut, lights off, nobody in.
void team_set_dark(bool dark);
bool team_dark(void);
// The live desk under a point of an office's picture (the desk itself, not
// its bench or its name), or -1.
int  team_desk_at(int office, int x, int y);
// The first office with a session waiting for you, or -1.
int  team_office_waiting(void);

void      team_desk_info(int office, int desk, TeamDeskInfo* out);
int       team_per_office(void);       // 6 desks or 4 cubicles
int       team_skin(void);
void      team_set_skin(int skin);     // then team_snap()
// Which cubicle gets which arrangement of desks (rows, island, L).
void      team_set_seed(uint32_t seed);
int       team_offices(void);          // 1 with up to six sessions, else one per six
int       team_bar_y(void);            // top of the status strip
int       team_sessions(void);
int       team_agents(void);           // sub-agents, all desks
// A slot is office * TEAM_DESKS + desk: sessions sit in the order the app
// sends them and move up when one before them leaves.
int       team_desk_of(uint8_t id);    // slot, or -1
TeamPhase team_lead_phase(int slot);
int       team_subs_present(int slot); // seated or about, not leaving
int       team_subs_at_cooler(int slot);
// The people standing at the board or the cooler in an office: how many,
// and where (up to `max`; the arrays may be NULL).
int       team_standing(int office, int* xs, int* ys, int max);
