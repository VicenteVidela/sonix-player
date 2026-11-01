#include "morepage.h"

#include "lvgl/lvgl.h"

#include "src/gui/audio/dacpage.h"
#include "src/gui/audio/metronomepage.h"
#include "src/gui/ebook/ebookpage.h"
#include "src/gui/library/audiobooks.h"
#include "src/gui/library/filespage.h"
#include "src/gui/meloetta/homepage.h"
#include "src/gui/meloetta/budspage.h"
#include "src/gui/meloetta/notespage.h"
#include "src/gui/shell/gridpage.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/settingsrow.h"

lv_obj_t *morepage_screen;

void morepage_init(gui_config_t *cfg) {
	// Vicho's order (Meloetta fork, 2026-11-01), left to right and then down.
	// The everyday apps and the games moved to the Apps tile.
	const grid_entry_t entries[] = {
		{"audiobooks", &icon_menu_audiobooks, &audiobooks_screen, NULL},
		// An action rather than a screen: the shelf reads the Ebook folder again
		// every time it is opened, so a book copied onto the card turns up
		// without a restart.
		{"books", &icon_menu_books, NULL, ebookpage_open},
		// Next to Books: reading and writing. An action for the same reason, the
		// folder is made on the way in if the card has none.
		{"notes", &icon_menu_notes, NULL, notespage_open},
		{"buds", &icon_menu_buds, &budspage_screen, NULL},
		{"home", &icon_menu_home, &homepage_screen, NULL},
		// An action for the same reason the shelf is one: the listing is read
		// when the page is entered, so a card that changed under the player
		// shows what is on it now.
		{"file_explorer", &icon_menu_file_explorer, NULL, filespage_open},
		{"dac", &icon_menu_dac, &dacpage_screen, NULL},
		// Not in Vicho's list of 2026-11-01; kept at the end until they say.
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
