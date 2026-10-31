#ifndef MOONPAGE_H
#define MOONPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The moon tonight, from the Meloetta page: the moon drawn as it is lit now,
// the phase's name, how much of it is lit, and when the next full and new
// moons are, in local time.
//
// Worked out from the clock alone, nothing from the network: a short series
// for the Sun's and the Moon's longitudes, good to about half an hour on the
// times. The moon is drawn as it looks from the southern hemisphere -- a
// waxing moon lit on the left -- unless [moon] south = 0.

extern lv_obj_t *moonpage_screen;

void moonpage_init(gui_config_t *cfg);

#endif /* MOONPAGE_H */
