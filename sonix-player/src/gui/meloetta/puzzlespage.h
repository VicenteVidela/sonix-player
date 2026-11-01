#ifndef PUZZLESPAGE_H
#define PUZZLESPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Puzzles tile on More: a row per puzzle (Simon Tatham's Portable Puzzle
// Collection, the curated set in src/puzzles/generated-games.h), each opening
// puzzleplay.c. A game left half done is saved and comes back where it was.

extern lv_obj_t *puzzlespage_screen;

void puzzlespage_init(gui_config_t *cfg);

#endif /* PUZZLESPAGE_H */
