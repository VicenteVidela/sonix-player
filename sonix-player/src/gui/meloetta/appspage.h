#ifndef APPSPAGE_H
#define APPSPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Apps tile on the main menu (Meloetta fork, Vicho 2026-11-01): Pet, Games,
// Clocks, Weather, Moon, Meditation, in that order.

extern lv_obj_t *appspage_screen;

void appspage_init(gui_config_t *cfg);

#endif /* APPSPAGE_H */
