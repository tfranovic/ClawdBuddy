#pragma once
#include <lvgl.h>

// The pet's stats beside it on the meters (the 480x480 pet layout,
// Layout.pet_home): species and level, then the mood, fed and energy pips.
// The same numbers as the large view (pet_face.cpp), always in sight.
// (ax, ay, aw): the two activity rows under the status word, centred: the
// last session to run a tool and the tool, then the number of active
// sessions.
void pet_home_create(lv_obj_t* parent, int x, int y, int w, int ax, int ay, int aw);
void pet_home_tick(void);       // repaints what changed; cheap to call often
void pet_home_repaint(void);    // after a theme change

#ifndef PET_ASCII
inline void pet_home_create(lv_obj_t*, int, int, int, int, int, int) {}
inline void pet_home_tick(void) {}
inline void pet_home_repaint(void) {}
#endif
