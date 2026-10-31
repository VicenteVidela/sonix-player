#ifndef MELOETTAPAGE_H
#define MELOETTAPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Meloetta's own corner of the More page: the screens added in this fork
// (github.com/VicenteVidela/sonix-player, branch meloetta), as a grid of tiles
// like the More page itself. Kept on a page of its own so the stock pages
// change by one tile and merging upstream stays easy.

extern lv_obj_t *meloettapage_screen;

void meloettapage_init(gui_config_t *cfg);

#endif /* MELOETTAPAGE_H */
