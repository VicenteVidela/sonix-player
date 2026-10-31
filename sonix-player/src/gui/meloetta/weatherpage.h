#ifndef WEATHERPAGE_H
#define WEATHERPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The weather, from the Meloetta page: now (a drawn icon, the temperature, what
// it feels like, humidity and wind), the next hours, and five days.
//
// Straight from Open-Meteo over HTTPS -- no key, no account -- so it works on
// any Wi-Fi, a phone's hotspot included. The places are [weather] places in
// the config, "name,lat,lon;name,lat,lon" (Santiago and Artificio when unset),
// one pill each at the top; the chosen one is remembered in [weather] place.
// Each place's last answer is kept in <card>/.local/weather-<n>.json, so with
// no connection the page shows that one and says how old it is. Showing a place
// fetches again when its answer is more than half an hour old; tapping the
// line under the pills fetches now.

extern lv_obj_t *weatherpage_screen;

void weatherpage_init(gui_config_t *cfg);

#endif /* WEATHERPAGE_H */
