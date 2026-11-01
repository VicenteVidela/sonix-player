#include "gamespage.h"

#include "src/gui/flappybird/flappybird.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/meloetta/puzzlespage.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"

lv_obj_t *gamespage_screen;

void gamespage_init(gui_config_t *cfg) {
	const grid_entry_t entries[] = {
#if GB_CORE
		// Present only when the binary was built with the Game Boy core.
		{"gearboy", &icon_menu_gearboy, &gearboypage_screen, NULL},
#endif
		// An action: the artwork is read from the resource tree on the way in.
		{"flappy_bird", &icon_menu_flappy_bird, NULL, flappybird_open},
		{"puzzles", &icon_menu_puzzles, &puzzlespage_screen, NULL},
	};
	lv_obj_t *grid = gridpage_build(gamespage_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	gridpage_set_scrolling(grid);
	settingsrow_title(gamespage_screen, cfg, "games");
}
