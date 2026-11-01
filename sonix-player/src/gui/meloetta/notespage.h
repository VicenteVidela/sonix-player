#ifndef NOTESPAGE_H
#define NOTESPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// Notes (Meloetta fork): Markdown notes in the card's Notes folder.
//
// The list shows every .md and .txt there, newest first. A note opens drawn --
// headings, bold and italic, lists, quotes, code, and task boxes that tick with
// a tap -- and the pencil turns it into plain text with the keyboard under it.
//
// On home Wi-Fi the folder is swapped with CL4P-TP (the meloetta-notes service,
// [notes] server, default http://192.168.100.11:8092), which keeps the other
// copy in ~/Sync/notes. It happens on its own when the list opens and after
// every save, and on the sync button. Each side's last agreed version is
// remembered as a hash in Notes/.sync, so a note changed in both places is
// kept twice (a "(conflict)" copy) instead of one edit eating the other.

extern lv_obj_t *notespage_screen;
extern lv_obj_t *notesview_screen;

void notespage_init(gui_config_t *cfg);

// The More tile: makes the folder if it is missing and shows the list.
void notespage_open(void);

#endif /* NOTESPAGE_H */
