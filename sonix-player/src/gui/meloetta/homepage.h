#ifndef HOMEPAGE_H
#define HOMEPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Home Assistant from Meloetta: KingGroot (wake, sleep, shut down with a
// second tap, monitors, speakers) and the Bose speaker (volume, play/pause,
// next, on/off).
//
// HA listens on Tailscale only and Meloetta has no Tailscale, so the page talks
// to a small relay on CL4P-TP instead ([home] relay in the config, default
// http://192.168.100.11:8091; CL4P-TP's ~/ai/scripts/meloetta-ha). The relay
// holds the HA token and runs only the actions it lists, for clients on the
// home LAN. Away from home Wi-Fi the line at the top says it cannot reach the
// house. The state is fetched when the page opens and every ten seconds while
// it is open; tapping that line fetches now.

extern lv_obj_t *homepage_screen;

void homepage_init(gui_config_t *cfg);

#endif /* HOMEPAGE_H */
