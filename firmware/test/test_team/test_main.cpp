// The agent team view's scene: parsing `tm`, who sits where, who walks.
//   pio test -d firmware -e native -f test_team
#include <unity.h>
#include <string.h>
#include "pet/team_core.h"

void setUp(void) {
    team_reset();
    team_set_skin(TEAM_SKIN_DESKS);
    team_set_seed(0);
}
void tearDown(void) {}

static void set(const char* tm) {
    TeamDeskIn in[TEAM_MAX];
    team_set(in, team_parse(tm, in, TEAM_MAX));
}

// With `tx` too, as the app sends it.
static void setx(const char* tm, const char* tx) {
    TeamDeskIn in[TEAM_MAX];
    const int n = team_parse(tm, in, TEAM_MAX);
    team_parse_extra(tx, in, n);
    team_set(in, n);
}

static void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) team_step(100);
}

static void test_parse(void) {
    TeamDeskIn in[TEAM_DESKS];
    TEST_ASSERT_EQUAL(2, team_parse("a3w20TestApp|07i00ClawdBuddy", in, TEAM_DESKS));
    TEST_ASSERT_EQUAL_UINT8(0xa3, in[0].id);
    TEST_ASSERT_EQUAL_CHAR('w', in[0].state);
    TEST_ASSERT_EQUAL_UINT8(2, in[0].subs);
    TEST_ASSERT_EQUAL_STRING("TestApp", in[0].name);
    TEST_ASSERT_EQUAL_STRING("ClawdBuddy", in[1].name);
    TEST_ASSERT_EQUAL(0, team_parse("", in, TEAM_DESKS));
    TEST_ASSERT_EQUAL(0, team_parse(NULL, in, TEAM_DESKS));
    // Bad entries are skipped; a long name is cut; no name is fine.
    TEST_ASSERT_EQUAL(2, team_parse("zz|01x0bad|02t10a-very-long-project-name|03q00", in, TEAM_DESKS));
    TEST_ASSERT_EQUAL_STRING("a-very-long-", in[0].name);
    TEST_ASSERT_EQUAL_STRING("", in[1].name);
    TEST_ASSERT_EQUAL(1, team_parse("01w00a|02w00b", in, 1));
    // On a break, with quiet sub-agents (never more than there are).
    TEST_ASSERT_EQUAL(1, team_parse("01b25api", in, TEAM_DESKS));
    TEST_ASSERT_EQUAL_CHAR('b', in[0].state);
    TEST_ASSERT_EQUAL_UINT8(2, in[0].quiet);
    TEST_ASSERT_EQUAL_STRING("api", in[0].name);
}

static void test_a_session_walks_in_and_sits(void) {
    set("01w00api");
    const int slot = team_desk_of(0x01);
    TEST_ASSERT_EQUAL(0, slot);
    TEST_ASSERT_EQUAL(TEAM_ENTER, team_lead_phase(slot));
    run(15000);
    TEST_ASSERT_EQUAL(TEAM_SEAT, team_lead_phase(slot));
    TEST_ASSERT_EQUAL(1, team_sessions());
}

static void test_thinking_goes_to_the_board_and_back(void) {
    set("01w00api");
    team_snap();
    set("01t00api");
    run(5000);
    TEST_ASSERT_EQUAL(TEAM_SEAT, team_lead_phase(0));     // a short thought stays seated
    run(2000);
    TEST_ASSERT_EQUAL(TEAM_TO_BOARD, team_lead_phase(0));
    run(4000);
    TEST_ASSERT_EQUAL(TEAM_BOARD, team_lead_phase(0));
    // Not there and straight back: it stays a few seconds at least.
    set("01w00api");
    run(200);
    TEST_ASSERT_EQUAL(TEAM_BOARD, team_lead_phase(0));
    run(15000);
    TEST_ASSERT_EQUAL(TEAM_SEAT, team_lead_phase(0));
    // Someone asking for it brings it back at once.
    set("01t00api");
    run(12000);
    TEST_ASSERT_EQUAL(TEAM_BOARD, team_lead_phase(0));
    set("01q00api");
    run(200);
    TEST_ASSERT_EQUAL(TEAM_RETURN, team_lead_phase(0));
    run(15000);
    TEST_ASSERT_EQUAL(TEAM_SEAT, team_lead_phase(0));
}

static void test_waiting_for_sub_agents_stays_at_the_desk(void) {
    set("01w20api");
    team_snap();
    set("01d20api");
    run(30000);
    TEST_ASSERT_EQUAL(TEAM_SEAT, team_lead_phase(0));
}

static void test_sub_agents_come_and_go(void) {
    set("01w20api");
    TEST_ASSERT_EQUAL(2, team_subs_present(0));
    TEST_ASSERT_EQUAL(2, team_agents());
    set("01w70api");                       // more than the bench seats
    TEST_ASSERT_EQUAL(TEAM_SEATS, team_subs_present(0));
    TEST_ASSERT_EQUAL(7, team_agents());
    set("01w10api");
    TEST_ASSERT_EQUAL(1, team_subs_present(0));
    set("01i00api");
    TEST_ASSERT_EQUAL(0, team_subs_present(0));
}

static void test_the_idle_go_to_the_water_cooler(void) {
    set("01w30api");
    team_snap();
    set("01b30api");                      // idle for a while: the lead takes a break
    run(200);
    TEST_ASSERT_EQUAL(TEAM_TO_COOLER, team_lead_phase(0));
    run(20000);
    TEST_ASSERT_EQUAL(TEAM_COOLER, team_lead_phase(0));
    set("01w32api");                      // back to work; two sub-agents have gone quiet
    run(200);
    TEST_ASSERT_EQUAL(TEAM_RETURN, team_lead_phase(0));
    TEST_ASSERT_EQUAL(2, team_subs_at_cooler(0));
    run(20000);
    TEST_ASSERT_EQUAL(TEAM_SEAT, team_lead_phase(0));
    set("01w31api");                      // one of them picks up again
    run(200);
    TEST_ASSERT_EQUAL(1, team_subs_at_cooler(0));
    set("01w20api");                      // the quiet one timed out: it is the one to go
    TEST_ASSERT_EQUAL(0, team_subs_at_cooler(0));
    TEST_ASSERT_EQUAL(2, team_subs_present(0));
    // Brought up while on a break: already there.
    team_reset();
    set("02b00x");
    team_snap();
    TEST_ASSERT_EQUAL(TEAM_COOLER, team_lead_phase(0));
}

static void test_desks_close_up_when_a_session_leaves(void) {
    set("01w00a|02w00b|03w00c");
    team_snap();
    TEST_ASSERT_EQUAL(1, team_desk_of(0x02));
    set("01w00a|03w00c");                 // b leaves: c moves up a desk
    TEST_ASSERT_EQUAL(-1, team_desk_of(0x02));
    TEST_ASSERT_EQUAL(1, team_desk_of(0x03));
    TEST_ASSERT_EQUAL(2, team_sessions());
    TEST_ASSERT_EQUAL(TEAM_RETURN, team_lead_phase(1));   // c, walking over
    TEST_ASSERT_EQUAL(TEAM_OFF, team_lead_phase(2));
    run(20000);
    TEST_ASSERT_EQUAL(TEAM_SEAT, team_lead_phase(1));
    set("01w00a|03w00c|04w00d");          // the next one takes the next desk
    TEST_ASSERT_EQUAL(2, team_desk_of(0x04));
    set("");
    team_snap();
    TEST_ASSERT_EQUAL(0, team_sessions());
    TEST_ASSERT_EQUAL(1, team_offices());
}

static void test_a_second_office_opens_and_closes(void) {
    set("01w00a|02w00b|03w00c|04w00d|05w00e|06w00f");
    TEST_ASSERT_EQUAL(1, team_offices());
    set("01w00a|02w00b|03w00c|04w00d|05w00e|06w00f|07w20g|08w00h");
    team_snap();
    TEST_ASSERT_EQUAL(2, team_offices());
    TEST_ASSERT_EQUAL(6, team_desk_of(0x07));
    TEST_ASSERT_EQUAL(7, team_desk_of(0x08));
    // One of the first six leaves: everyone after it moves up, and g comes
    // through the door from the second office with its sub-agents.
    set("01w00a|02w00b|04w00d|05w00e|06w00f|07w20g|08w00h");
    TEST_ASSERT_EQUAL(2, team_desk_of(0x04));
    TEST_ASSERT_EQUAL(5, team_desk_of(0x07));
    TEST_ASSERT_EQUAL(6, team_desk_of(0x08));
    TEST_ASSERT_EQUAL(TEAM_MOVE_OUT, team_lead_phase(5));
    TEST_ASSERT_EQUAL(2, team_offices());
    run(40000);
    TEST_ASSERT_EQUAL(TEAM_SEAT, team_lead_phase(5));
    TEST_ASSERT_EQUAL(2, team_subs_present(5));
    TEST_ASSERT_EQUAL(TEAM_SEAT, team_lead_phase(6));
    // Down to six: the second office is gone.
    set("01w00a|02w00b|04w00d|05w00e|06w00f|07w20g");
    TEST_ASSERT_EQUAL(1, team_offices());
    // No more than twelve.
    set("01w00a|02w00b|03w00c|04w00d|05w00e|06w00f|07w00g|08w00h|09w00i|0aw00j|0bw00k|0cw00l|0dw00m");
    TEST_ASSERT_EQUAL(TEAM_MAX, team_sessions());
    TEST_ASSERT_EQUAL(2, team_offices());
}

static void test_nobody_stands_on_anybody(void) {
    // Everyone at the wall at once, in both skins: no two on the same spot.
    for (int skin = 0; skin < 2; skin++) {
        team_reset();
        team_set_skin(skin);
        set("01t42a|02b42b|03t42c|04b42d");
        team_snap();
        team_snap();                      // coming back to the view changes nothing
        run(60000);
        int n = team_standing(0, NULL, NULL, 0);
        TEST_ASSERT_GREATER_OR_EQUAL(8, n);
        int xs[32], ys[32];
        n = team_standing(0, xs, ys, 32);
        for (int i = 0; i < n; i++)
            for (int j = i + 1; j < n; j++) {
                const int dx = xs[i] > xs[j] ? xs[i] - xs[j] : xs[j] - xs[i];
                const int dy = ys[i] > ys[j] ? ys[i] - ys[j] : ys[j] - ys[i];
                TEST_ASSERT_TRUE_MESSAGE(dx >= 10 || dy >= 10, "two people on one spot");
            }
    }
}

static void test_cubicles_seat_four_to_an_office(void) {
    team_set_skin(TEAM_SKIN_CUBICLES);
    TEST_ASSERT_EQUAL(4, team_per_office());
    set("01w00a|02w00b|03w00c|04w00d");
    TEST_ASSERT_EQUAL(1, team_offices());
    set("01w00a|02w00b|03w00c|04w00d|05w20e");
    team_snap();
    TEST_ASSERT_EQUAL(2, team_offices());
    TEST_ASSERT_EQUAL(4, team_desk_of(0x05));
    TeamDeskInfo info;
    team_desk_info(1, 0, &info);
    TEST_ASSERT_TRUE(info.live);
    TEST_ASSERT_EQUAL_STRING("e", info.name);
    team_desk_info(1, 4, &info);          // no fifth cubicle
    TEST_ASSERT_FALSE(info.live);
    // One of the first four leaves: e comes through the door into its place.
    set("01w00a|03w00c|04w00d|05w20e");
    TEST_ASSERT_EQUAL(3, team_desk_of(0x05));
    TEST_ASSERT_EQUAL(TEAM_MOVE_OUT, team_lead_phase(3));
    TEST_ASSERT_EQUAL(1, team_offices());
    run(40000);
    TEST_ASSERT_EQUAL(TEAM_SEAT, team_lead_phase(3));
    TEST_ASSERT_EQUAL(2, team_subs_present(3));
    // Twelve sessions make three offices.
    set("01w00a|02w00b|03w00c|04w00d|05w00e|06w00f|07w00g|08w00h|09w00i|0aw00j|0bw00k|0cw00l");
    TEST_ASSERT_EQUAL(3, team_offices());
    // The same people in the other skin: two offices of six.
    team_set_skin(TEAM_SKIN_DESKS);
    team_snap();
    TEST_ASSERT_EQUAL(2, team_offices());
}

static void test_cubicle_render_stays_in_bounds(void) {
    static uint16_t buf[TEAM_W * TEAM_H + 16];
    for (uint32_t seed = 0; seed < 3; seed++) {       // every arrangement in every cubicle
        team_reset();
        team_set_skin(TEAM_SKIN_CUBICLES);
        team_set_seed(seed);
        memset(buf, 0xAB, sizeof(buf));
        set("01w90abcdefghijkl|02t40b|03q00c|04b42d|05w20e|06d41f");
        for (int i = 0; i < 400; i++) {
            team_step(100);
            team_render(buf, i % 2);
        }
        for (int i = 0; i < 16; i++) TEST_ASSERT_EQUAL_HEX16(0xABAB, buf[TEAM_W * TEAM_H + i]);
    }
}

static void test_render_stays_in_bounds(void) {
    static uint16_t buf[TEAM_W * TEAM_H + 16];
    memset(buf, 0xAB, sizeof(buf));
    setx("01w90abcdefghijkl|02t40b|03q00c|04i00d|05w20e|06t10f|07d40g|08b00h|09w42i", "wz--c9--q0--f5--e---tk--d---b---w-p-");
    for (int i = 0; i < 600; i++) {
        team_step(100);
        team_render(buf, i % 2);
    }
    for (int i = 0; i < 16; i++) TEST_ASSERT_EQUAL_HEX16(0xABAB, buf[TEAM_W * TEAM_H + i]);
}

static void test_the_finer_states_and_the_context(void) {
    TeamDeskIn in[TEAM_DESKS];
    TEST_ASSERT_EQUAL(3, team_parse("01i00a|02t00b|03w00c", in, TEAM_DESKS));
    TEST_ASSERT_EQUAL(-1, in[0].ctx);
    team_parse_extra("f0pfc9--wz-o", in, 3);
    TEST_ASSERT_EQUAL_CHAR('f', in[0].state);
    TEST_ASSERT_EQUAL(0, in[0].ctx);
    TEST_ASSERT_EQUAL_CHAR('p', in[0].mode);
    TEST_ASSERT_EQUAL_CHAR('c', in[1].state);
    TEST_ASSERT_EQUAL(9, in[1].ctx);
    TEST_ASSERT_EQUAL(TEAM_CTX_FULL, in[2].ctx);
    // Too short for the third desk, an unknown state letter, no context:
    // `tm` stands.
    team_parse("01i00a|02t00b|03w00c", in, TEAM_DESKS);
    team_parse_extra("X---e---w", in, 3);
    TEST_ASSERT_EQUAL_CHAR('i', in[0].state);
    TEST_ASSERT_EQUAL_CHAR('e', in[1].state);
    TEST_ASSERT_EQUAL_CHAR('w', in[2].state);
    TEST_ASSERT_EQUAL(-1, in[2].ctx);
    team_parse_extra(NULL, in, 3);

    // Done, failed and compacting all stay at the desk.
    setx("01i00a|02i00b|03t00c", "fk--e---c5--");
    team_snap();
    run(20000);
    for (int slot = 0; slot < 3; slot++) TEST_ASSERT_EQUAL(TEAM_SEAT, team_lead_phase(slot));
    TeamDeskInfo d;
    team_desk_info(0, 0, &d);
    TEST_ASSERT_EQUAL_CHAR('f', d.state);
    TEST_ASSERT_EQUAL(20, d.ctx);
    TEST_ASSERT_EQUAL_UINT8(0x01, d.id);
    // Plan mode: the work is at the board.
    setx("01w00a", "w-p-");
    run(12000);
    TEST_ASSERT_EQUAL(TEAM_BOARD, team_lead_phase(0));
}

static void test_taps_find_desks_and_alerts_find_offices(void) {
    set("01w00a|02w00b|03w00c|04w00d|05w00e|06w00f|07q00g");
    team_snap();
    TEST_ASSERT_EQUAL(1, team_office_waiting());
    TEST_ASSERT_EQUAL(0, team_desk_at(0, 80, 170));      // the first desk
    TEST_ASSERT_EQUAL(4, team_desk_at(0, 240, 330));     // the middle of the second row
    TEST_ASSERT_EQUAL(-1, team_desk_at(0, 80, 40));      // the wall
    TEST_ASSERT_EQUAL(-1, team_desk_at(0, 80, 260));     // a bench and a name
    TEST_ASSERT_EQUAL(0, team_desk_at(1, 80, 170));
    TEST_ASSERT_EQUAL(-1, team_desk_at(1, 240, 170));    // an empty desk
    set("01w00a");
    TEST_ASSERT_EQUAL(-1, team_office_waiting());
}

static void test_a_shut_office_draws_nobody(void) {
    static uint16_t lit[TEAM_W * TEAM_H], shut[TEAM_W * TEAM_H], room[TEAM_W * TEAM_H];
    setx("01w20a|02q00b", "wz--q5--");
    team_snap();
    team_render(lit, 0);
    team_set_dark(true);
    TEST_ASSERT_TRUE(team_dark());
    TEST_ASSERT_EQUAL(-1, team_desk_at(0, 80, 170));
    team_render(shut, 0);
    TEST_ASSERT_TRUE(memcmp(lit, shut, sizeof(lit)) != 0);
    // The same picture whoever was in: nothing of them is drawn.
    team_reset();
    team_set_dark(true);
    team_render(room, 0);
    TEST_ASSERT_EQUAL_MEMORY(room, shut, sizeof(room));
    team_set_dark(false);
    // With the room's fixed parts kept in a cache, the picture is the same.
    static uint16_t cache[TEAM_W * TEAM_H], cached[TEAM_W * TEAM_H];
    setx("01w20a|02q00b", "wz--q5--");
    team_snap();
    team_render(lit, 0);
    team_set_cache(cache);
    team_render(cached, 0);
    team_render(cached, 0);
    TEST_ASSERT_EQUAL_MEMORY(lit, cached, sizeof(lit));
    team_set_cache(NULL);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_parse);
    RUN_TEST(test_a_session_walks_in_and_sits);
    RUN_TEST(test_thinking_goes_to_the_board_and_back);
    RUN_TEST(test_waiting_for_sub_agents_stays_at_the_desk);
    RUN_TEST(test_sub_agents_come_and_go);
    RUN_TEST(test_the_idle_go_to_the_water_cooler);
    RUN_TEST(test_desks_close_up_when_a_session_leaves);
    RUN_TEST(test_a_second_office_opens_and_closes);
    RUN_TEST(test_nobody_stands_on_anybody);
    RUN_TEST(test_cubicles_seat_four_to_an_office);
    RUN_TEST(test_cubicle_render_stays_in_bounds);
    RUN_TEST(test_render_stays_in_bounds);
    RUN_TEST(test_the_finer_states_and_the_context);
    RUN_TEST(test_taps_find_desks_and_alerts_find_offices);
    RUN_TEST(test_a_shut_office_draws_nobody);
    return UNITY_END();
}
