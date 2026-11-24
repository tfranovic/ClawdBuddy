#include "totals.h"
#include <stdio.h>
#include <stdlib.h>

static bool     known = false;
static bool     live = false;
static bool     idle_now = false;
static uint32_t up_s = 0, idle_s = 0;   // at base_ms
static uint32_t base_ms = 0;

// Fold the time since base_ms into the figures.
static void advance(uint32_t now_ms) {
    if (known && live) {
        const uint32_t dt = (uint32_t)(now_ms - base_ms) / 1000u;
        up_s += dt;
        if (idle_now) idle_s += dt;
        base_ms += dt * 1000u;
    } else {
        base_ms = now_ms;
    }
}

void totals_reset(void) {
    known = live = idle_now = false;
    up_s = idle_s = base_ms = 0;
}

void totals_set(const char* ut, uint32_t now_ms) {
    if (!ut || !*ut) return;
    char* end = NULL;
    const unsigned long u = strtoul(ut, &end, 10);
    if (end == ut || *end != ',') return;
    const char* p = end + 1;
    const unsigned long i = strtoul(p, &end, 10);
    if (end == p || *end != ',' || (end[1] != 'i' && end[1] != 'w') || i > u) return;
    up_s = (uint32_t)u;
    idle_s = (uint32_t)i;
    idle_now = end[1] == 'i';
    base_ms = now_ms;
    known = true;
}

void totals_link(bool up, uint32_t now_ms) {
    advance(now_ms);
    live = up;
}

bool totals_known(void) { return known; }

static uint32_t since_base(uint32_t now_ms) {
    return (known && live) ? (uint32_t)(now_ms - base_ms) / 1000u : 0;
}

uint32_t totals_uptime_min(uint32_t now_ms) { return (up_s + since_base(now_ms)) / 60u; }

uint32_t totals_idle_min(uint32_t now_ms) {
    return (idle_s + (idle_now ? since_base(now_ms) : 0)) / 60u;
}

void totals_fmt(uint32_t minutes, char* buf, size_t len) {
    snprintf(buf, len, "%luh%02lum", (unsigned long)(minutes / 60u), (unsigned long)(minutes % 60u));
}

void totals_line(bool short_form, uint32_t now_ms, char* buf, size_t len) {
    if (len) buf[0] = 0;
    if (!known) return;
    char u[16], i[16];
    totals_fmt(totals_uptime_min(now_ms), u, sizeof(u));
    totals_fmt(totals_idle_min(now_ms), i, sizeof(i));
    if (short_form) snprintf(buf, len, "%s/%s", u, i);
    else            snprintf(buf, len, "Uptime: %s / Idle: %s", u, i);
}
