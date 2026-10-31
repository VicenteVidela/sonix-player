#ifndef PETPAGE_H
#define PETPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The pet, from the Meloetta page (rules in src/system/meloetta/pet.h): the
// creature drawn as it is now -- its stage, its mood, asleep, ill or gone --
// its hunger and happiness in hearts, what it ate last and likes best, and the
// care it can be given: a pat (a tap on it does the same), cleaning, medicine,
// and a new egg once it has died. A toast says when it hatches, grows, gets
// hungry or ill, or dies, whichever page is up.

extern lv_obj_t *petpage_screen;

void petpage_init(gui_config_t *cfg);

#endif /* PETPAGE_H */
