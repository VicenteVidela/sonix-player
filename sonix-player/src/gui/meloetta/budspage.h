#ifndef BUDSPAGE_H
#define BUDSPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The earbuds, from Meloetta: battery of each bud and the case, noise
// cancelling / normal / transparency (with the cancelling kind and strength,
// and the transparency kind), wind noise reduction, and LDAC.
//
// Talks to src/system/meloetta/soundcore.c, which opens Soundcore's own RFCOMM
// channel next to the music whenever a Soundcore pair is the audio device.
// With anything else connected the page says so and shows nothing to press.

extern lv_obj_t *budspage_screen;

void budspage_init(gui_config_t *cfg);

#endif /* BUDSPAGE_H */
