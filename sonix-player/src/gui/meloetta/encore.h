#ifndef GUI_ENCORE_H
#define GUI_ENCORE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Encore: an original Game & Watch-style LCD game, on the Games page.
//
// Five staff lines, the singer on the left hops between them (tap the top
// half: up, the bottom half: down). Notes step in from the right on the beat:
// catch them; dodge the sour notes and the rests. Three misses and the game
// is over; every 100 points clears the misses. Game A and Game B, each with its
// best score in the config ([encore] best_a, best_b); [encore] sound turns the
// beeps on, which stops the music.

extern lv_obj_t *encore_screen;

void encore_init(gui_config_t *cfg);

// Opens the game on its title screen.
void encore_open(void);

#endif /* GUI_ENCORE_H */
