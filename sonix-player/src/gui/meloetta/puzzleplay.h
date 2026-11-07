#ifndef PUZZLEPLAY_H
#define PUZZLEPLAY_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// One of Simon Tatham's puzzles, full screen: the board in the middle, the
// game's own keys (Solo's digits) under it, then Undo / Redo / Mark / Size /
// New. Mark turns taps into the right button (Mines' flag, Pattern's "empty",
// Solo's pencil marks). The game in progress is saved next to the config when
// the page closes and picked up again when it opens.
//
// The board is drawn into an RGB565 buffer by a small rasteriser here (rects,
// lines, circles; polygons through Tatham's draw-poly.c), with text drawn by
// LVGL into the same buffer. Colours go through a dark-mode transform: the
// puzzles are designed on a light grey board, and their lightness is flipped
// so the board is dark and the ink is light, hues kept.

extern lv_obj_t *puzzleplay_screen;

void puzzleplay_init(gui_config_t *cfg);

// Opens game `index` of the curated list (src/puzzles/generated-games.h).
void puzzleplay_open(int index);

int puzzleplay_game_count(void);
const char *puzzleplay_game_name(int index);
// The short line under its name in the Puzzles menu.
const char *puzzleplay_game_blurb(int index);

#endif /* PUZZLEPLAY_H */
