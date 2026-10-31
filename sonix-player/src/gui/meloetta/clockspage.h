#ifndef CLOCKSPAGE_H
#define CLOCKSPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Clocks, from the Meloetta page, in three tabs: the clock face, a stopwatch
// with the last five laps, and a timer set on wheels or
// from presets.
//
// The timer goes on running with the page closed, on time that keeps counting
// through a suspend, and beeps (chime.h) when it ends -- pausing a playing
// track for the beep and playing on after it; one that ends while
// another page is up says so with a toast too. The tab shown and the
// timer's length are kept in [clocks] tab and timer_secs.

extern lv_obj_t *clockspage_screen;

void clockspage_init(gui_config_t *cfg);

#endif /* CLOCKSPAGE_H */
