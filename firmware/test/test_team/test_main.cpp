// The agent team view's scene: parsing `tm`, who sits where, who walks.
//   pio test -d firmware -e native -f test_team
#include <unity.h>
#include <string.h>
#include "pet/team_core.h"

void setUp(void) { team_reset(); }
void tearDown(void) {}

static void set(const char* tm) {
    TeamDeskIn in[TEAM_MAX];
    team_set(in, team_parse(tm, in, TEAM_MAX));
}

static void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 100) team_step(100);
}

static void test_parse(void) {
    TeamDeskIn in[TEAM_DESKS];
    TEST_ASSERT_EQUAL(2, team_parse("a3w20magnemite|07i00Clawdmeter", in, TEAM_DESKS));
    TEST_ASSERT_EQUAL_UINT8(0xa3, in[0].id);
    TEST_ASSERT_EQUAL_CHAR('w', in[0].state);
    TEST_ASSERT_EQUAL_UINT8(2, in[0].subs);
    TEST_ASSERT_EQUAL_STRING("magnemite", in[0].name);
    TEST_ASSERT_EQUAL_STRING("Clawdmeter", in[1].name);
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
    run(2000);
    TEST_ASSERT_EQUAL(TEAM_SEAT, team_lead_phase(0));     // a short thought stays seated
    run(2000);
    TEST_ASSERT_EQUAL(TEAM_TO_BOARD, team_lead_phase(0));
    run(15000);
    TEST_ASSERT_EQUAL(TEAM_BOARD, team_lead_phase(0));
    set("01w00api");
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

static void test_render_stays_in_bounds(void) {
    static uint16_t buf[TEAM_W * TEAM_H + 16];
    memset(buf, 0xAB, sizeof(buf));
    set("01w90abcdefghijkl|02t40b|03q00c|04i00d|05w20e|06t10f|07d40g|08b00h|09w42i");
    for (int i = 0; i < 600; i++) {
        team_step(100);
        team_render(buf, i % 2);
    }
    for (int i = 0; i < 16; i++) TEST_ASSERT_EQUAL_HEX16(0xABAB, buf[TEAM_W * TEAM_H + i]);
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
    RUN_TEST(test_render_stays_in_bounds);
    return UNITY_END();
}
