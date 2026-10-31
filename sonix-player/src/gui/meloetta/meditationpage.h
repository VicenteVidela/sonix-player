#ifndef MEDITATIONPAGE_H
#define MEDITATIONPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// A meditation timer, from the Meloetta page: a length to choose (wheels or
// presets, durationpicker.h), a singing bowl struck once to begin and three
// times to end (chime.h), a ring that fills as the time goes, and the streak --
// days in a row with a finished session -- under it.
//
// A session stopped early counts for nothing. Like the clocks, a session goes
// on with the page closed. Kept in [meditation] minutes, streak, last_day
// (YYYYMMDD of the last finished session) and total.

extern lv_obj_t *meditationpage_screen;

void meditationpage_init(gui_config_t *cfg);

#endif /* MEDITATIONPAGE_H */
