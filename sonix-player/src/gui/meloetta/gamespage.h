#ifndef GAMESPAGE_H
#define GAMESPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Games tile inside Apps: every game on the player, Gearboy, Flappy Bird
// and Puzzles (Meloetta fork, Vicho 2026-11-01).

extern lv_obj_t *gamespage_screen;

void gamespage_init(gui_config_t *cfg);

#endif /* GAMESPAGE_H */
