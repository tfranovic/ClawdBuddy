#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// The app's running totals (`ut`): how long it has been up and connected to
// the board, and how much of that no session was working. The app keeps them
// (they survive a reset or a flash of the board) and sends them now and then;
// the board counts on from the last figures in between. Pure: no LVGL, no
// Arduino. Tested by test_totals.

// `ut`: "<uptime s>,<idle s>,<i|w>" (i = idle now). Anything else is ignored.
void     totals_set(const char* ut, uint32_t now_ms);
// The link came up or went: with no link the totals stand still.
void     totals_link(bool live, uint32_t now_ms);
void     totals_reset(void);
bool     totals_known(void);
uint32_t totals_uptime_min(uint32_t now_ms);
uint32_t totals_idle_min(uint32_t now_ms);
// "12h05m".
void     totals_fmt(uint32_t minutes, char* buf, size_t len);
// "12h05m/3h20m" (short) or "Uptime: 12h05m / Idle: 3h20m"; "" when unknown.
void     totals_line(bool short_form, uint32_t now_ms, char* buf, size_t len);
