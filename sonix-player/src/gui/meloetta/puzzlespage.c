#include "puzzlespage.h"

#include <stdint.h>

#include "src/gui/meloetta/puzzleplay.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

lv_obj_t *puzzlespage_screen;

static void open_cb(lv_event_t *e) { puzzleplay_open((int)(intptr_t)lv_event_get_user_data(e)); }

// One line under each name, in the player's language. Written out so the tool
// that checks the language files sees every key.
static const char *blurb(int i) {
	switch (i) {
	case 0: return tr("puzzles_solo");
	case 1: return tr("puzzles_mines");
	case 2: return tr("puzzles_net");
	case 3: return tr("puzzles_bridges");
	case 4: return tr("puzzles_lightup");
	case 5: return tr("puzzles_towers");
	case 6: return tr("puzzles_fifteen");
	case 7: return tr("puzzles_samegame");
	case 8: return tr("puzzles_flood");
	default: return "";
	}
}

void puzzlespage_init(gui_config_t *cfg) {
	lv_obj_t *container = settingsrow_page(puzzlespage_screen, cfg, "puzzles");
	for (int i = 0; i < puzzleplay_game_count(); i++) {
		lv_obj_t *value = NULL;
		lv_obj_t *row = settingsrow_add(container, "puzzles", &value, open_cb, (void *)(intptr_t)i);
		lv_label_set_text(settingsrow_name_label(row), puzzleplay_game_name(i));
		if (value) {
			lv_label_set_text(value, blurb(i));
		}
	}
	switcher_attach_back_gesture(puzzlespage_screen);
}
