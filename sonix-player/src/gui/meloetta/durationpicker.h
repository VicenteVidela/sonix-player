#ifndef DURATIONPICKER_H
#define DURATIONPICKER_H

#include "lvgl/lvgl.h"

// How long, for the Meloetta pages' timers: the hours and minutes wheels of the
// player's own sleep timers (settingsrow.c), with a row of presets under them.
// A preset turns the wheels; wheels turned to a preset's value light it.

#define DURATIONPICKER_PRESETS 8

typedef struct {
	lv_obj_t *box; // the whole thing, for hiding while a countdown runs
	lv_obj_t *hours, *minutes;
	lv_obj_t *pills[DURATIONPICKER_PRESETS];
	int presets[DURATIONPICKER_PRESETS];
	int count;
	void (*changed)(int minutes); // after any change, by wheel or by preset
} durationpicker_t;

void durationpicker_create(durationpicker_t *p, lv_obj_t *parent, const int *presets, int count,
						   void (*changed)(int minutes));

// In minutes, up to 23 h 59 min.
void durationpicker_set(durationpicker_t *p, int minutes);
int durationpicker_get(const durationpicker_t *p);

// Again after a theme or accent change: the colours are hand-set.
void durationpicker_repaint(durationpicker_t *p);

#endif /* DURATIONPICKER_H */
