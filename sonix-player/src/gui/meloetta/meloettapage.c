#include "meloettapage.h"

#include "lvgl/lvgl.h"

#include "src/gui/meloetta/moonpage.h"
#include "src/gui/meloetta/weatherpage.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"

lv_obj_t *meloettapage_screen;

void meloettapage_init(gui_config_t *cfg) {
	const grid_entry_t entries[] = {
		{"moon", &icon_menu_moon, &moonpage_screen, NULL},
		{"weather", &icon_menu_weather, &weatherpage_screen, NULL},
	};

	// The More page's two by three, so the tiles are the size of its tiles.
	gridpage_build(meloettapage_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);

	settingsrow_title(meloettapage_screen, cfg, "meloetta");
}
