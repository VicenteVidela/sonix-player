#include "appspage.h"

#include "src/gui/meloetta/clockspage.h"
#include "src/gui/meloetta/fabularium.h"
#include "src/gui/meloetta/gamespage.h"
#include "src/gui/meloetta/moonpage.h"
#include "src/gui/meloetta/petpage.h"
#include "src/gui/meloetta/weatherpage.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"

lv_obj_t *appspage_screen;

void appspage_init(gui_config_t *cfg) {
	// Vicho's order, left to right and then down.
	const grid_entry_t entries[] = {
		{"pet", &icon_menu_pet, &petpage_screen, NULL},
		{"games", &icon_menu_games, &gamespage_screen, NULL},
		{"clocks", &icon_menu_clocks, &clockspage_screen, NULL},
		{"weather", &icon_menu_weather, &weatherpage_screen, NULL},
		{"moon", &icon_menu_moon, &moonpage_screen, NULL},
		{"fabularium", &icon_menu_fabularium, NULL, fabularium_open},
	};
	lv_obj_t *grid = gridpage_build(appspage_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	gridpage_set_scrolling(grid);
	settingsrow_title(appspage_screen, cfg, "apps");
}
