#ifndef CLOCKFACE_H
#define CLOCKFACE_H

#include <stdbool.h>

#include "lvgl/lvgl.h"

// GLaDOS' Clock, the Persona 3 face from KingGroot's desktop
// (~/.local/share/plasma/plasmoids/vicho.clock), drawn again for this screen:
// a double blue ring, radial Roman numerals, a spike per hour, tapered hands, a
// second hand that ticks, and the moon showing today's phase as seen from Chile
// -- lit side mirrored against the north. Same colours and proportions.
//
// `figures` adds the time in figures (12- or 24-hour, as the settings say) and
// the date under the dial: the Clocks page has them, the screensaver shows the
// dial alone.

lv_obj_t *clockface_create(lv_obj_t *parent, int dial_size, bool figures);

// Moves the hands, and the moon and the figures with them: call it once a
// second while the face can be seen.
void clockface_refresh(lv_obj_t *face);

#endif /* CLOCKFACE_H */
