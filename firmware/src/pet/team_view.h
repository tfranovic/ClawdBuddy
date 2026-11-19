#pragma once
#include <lvgl.h>
#include "../data.h"

#ifndef PET_ASCII
// A stock build has no team view; ui.cpp's call is a no-op.
static inline void team_view_init(lv_obj_t*, int) {}
#else

// The agent team view on screen (the scene is team_core.cpp; the names and
// the status line are labels over it): a second view
// beside the meters on a 480x480 panel, switched by a tap at either side.
// The app turns it on with `tv` and fills it with `tm`.

void team_view_init(lv_obj_t* scr, int home_bottom);   // ui.cpp, once
void team_view_on_payload(const UsageData* d);
void team_view_alert(const char* msg);                 // NULL or "": none
bool team_view_visible(void);
// A tap on the meters or on the office. True when it was the view's (a
// switch, or a spot that does nothing); false leaves it to the pet's peek.
// y < 0: unknown.
bool team_view_tap(int x, int y);
// Serial/QA: show or hide regardless of `tv`.
void team_view_show(bool on);
#endif
