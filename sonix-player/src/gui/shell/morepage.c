#include "morepage.h"

#include "lvgl/lvgl.h"

#include "src/gui/audio/dacpage.h"
#include "src/gui/audio/metronomepage.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/flappybird/flappybird.h"
#include "src/gui/library/filespage.h"
#include "src/gui/meloetta/clockspage.h"
#include "src/gui/meloetta/meditationpage.h"
#include "src/gui/meloetta/moonpage.h"
#include "src/gui/meloetta/homepage.h"
#include "src/gui/meloetta/petpage.h"
#include "src/gui/meloetta/weatherpage.h"
#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"

lv_obj_t *morepage_screen;

void morepage_init(gui_config_t *cfg) {
	// In Vicho's order, left to right and then down (the Meloetta fork). The
	// fork's screens sit among the stock ones rather than behind a tile of
	// their own.
	const grid_entry_t entries[] = {
		// An action rather than a screen: the shelf reads the Ebook folder again
		// every time it is opened, so a book copied onto the card turns up
		// without a restart.
		{"books", &icon_menu_books, NULL, ebookpage_open},
		{"pet", &icon_menu_pet, &petpage_screen, NULL},
		{"weather", &icon_menu_weather, &weatherpage_screen, NULL},
		{"moon", &icon_menu_moon, &moonpage_screen, NULL},
		{"home", &icon_menu_home, &homepage_screen, NULL},
		{"clocks", &icon_menu_clocks, &clockspage_screen, NULL},
		{"meditation", &icon_menu_meditation, &meditationpage_screen, NULL},
		// An action for the same reason the shelf is one: the listing is read
		// when the page is entered, so a card that changed under the player
		// shows what is on it now.
		{"file_explorer", &icon_menu_file_explorer, NULL, filespage_open},
#if GB_CORE
		// The Game Boy emulator. Present only when the binary was built with
		// GB=1 (see the Makefile).
		{"gearboy", &icon_menu_gearboy, &gearboypage_screen, NULL},
#endif
		// An action: the artwork is read from the resource tree on the way in.
		{"flappy_bird", &icon_menu_flappy_bird, NULL, flappybird_open},
		{"dac", &icon_menu_dac, &dacpage_screen, NULL},
		{"metronome", &icon_menu_metronome, &metronomepage_screen, NULL},
	};

	// Two columns by three rows, like the Music and Wireless pages, so a tile
	// here is exactly the size of a tile there. The grid is laid out as full
	// even with fewer entries, so the only tile sits where it would with six --
	// top left -- instead of floating in the middle of a page of its own.
	lv_obj_t *grid = gridpage_build(morepage_screen, cfg, entries, (int)(sizeof(entries) / sizeof(entries[0])), 2, 3, true);
	// More tiles than the six that fit: the rest of the grid scrolls into view.
	gridpage_set_scrolling(grid);

	settingsrow_title(morepage_screen, cfg, "more");
}
