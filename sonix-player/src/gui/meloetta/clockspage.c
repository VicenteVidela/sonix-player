#include "clockspage.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/meloetta/clockface.h"
#include "src/gui/meloetta/durationpicker.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/tabbar.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/meloetta/chime.h"

lv_obj_t *clockspage_screen;

#define TICK_MS 100
#define LAPS 5

enum { TAB_CLOCK, TAB_STOPWATCH, TAB_TIMER, TABS };

static const int PRESETS[] = {1, 5, 10, 15, 19, 30}; // minutes

static tabbar_t tabs;
static lv_obj_t *panels[TABS];
static int tab;
static bool page_visible;
static lv_timer_t *tick_timer;

static lv_obj_t *face;

// The stopwatch: running time is what was banked before the last start plus
// what has run since.
static bool sw_running;
static int64_t sw_started, sw_banked;
static int64_t sw_laps[LAPS];
static int sw_lap_count;
static lv_obj_t *sw_label, *sw_laps_label, *sw_go_label, *sw_side_label;

// The timer: while it runs, the moment it ends; while paused, what was left.
static int timer_secs; // the length chosen
static bool timer_running, timer_done;
static int64_t timer_end, timer_left;
static lv_obj_t *timer_label, *timer_go_label;
static durationpicker_t timer_picker; // shown only while the timer is set and not started


// ---------------------------------------------------------------------------
// time
// ---------------------------------------------------------------------------

// Milliseconds that keep counting through a suspend, so a timer set before the
// player slept still ends when it should.
static int64_t now_ms(void) {
	struct timespec ts;
#ifdef CLOCK_BOOTTIME
	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
#endif
		clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void format_ms(char *out, size_t size, int64_t ms, bool tenths);

bool clockspage_timer_text(char *out, size_t size, bool *paused) {
	bool part_way = !timer_running && !timer_done && timer_left < timer_secs * 1000LL;
	if (!timer_running && !part_way) {
		return false;
	}
	format_ms(out, size, timer_running ? timer_end - now_ms() : timer_left, false);
	if (paused) {
		*paused = part_way;
	}
	return true;
}

// "4:05", "1:04:05"; with tenths, "4:05.3".
static void format_ms(char *out, size_t size, int64_t ms, bool tenths) {
	if (ms < 0) {
		ms = 0;
	}
	if (!tenths) {
		ms += 999; // a countdown shows 0:00 only when it is over
	}
	int64_t secs = ms / 1000;
	int h = (int)(secs / 3600), m = (int)(secs / 60 % 60), s = (int)(secs % 60);
	int n = h ? snprintf(out, size, "%d:%02d:%02d", h, m, s) : snprintf(out, size, "%d:%02d", m, s);
	if (tenths && n > 0 && (size_t)n < size) {
		snprintf(out + n, size - (size_t)n, ".%d", (int)(ms % 1000 / 100));
	}
}

static void wake_tick(void) { lv_timer_resume(tick_timer); }

// ---------------------------------------------------------------------------
// drawing what is running
// ---------------------------------------------------------------------------

static void show_stopwatch(void) {
	char text[32];
	int64_t total = sw_banked + (sw_running ? now_ms() - sw_started : 0);
	format_ms(text, sizeof(text), total, true);
	lv_label_set_text(sw_label, text);
	lv_label_set_text(sw_go_label, tr(sw_running ? "clocks_pause" : total ? "clocks_resume" : "clocks_start"));
	lv_label_set_text(sw_side_label, tr(sw_running ? "clocks_lap" : "clocks_reset"));

	char laps[LAPS * 40] = "";
	size_t used = 0;
	for (int i = 0; i < sw_lap_count && used < sizeof(laps); i++) {
		char name[24], when[24];
		snprintf(name, sizeof(name), tr("clocks_lap_n"), sw_lap_count - i);
		format_ms(when, sizeof(when), sw_laps[i], true);
		used += (size_t)snprintf(laps + used, sizeof(laps) - used, "%s%s   %s", i ? "\n" : "", name, when);
	}
	lv_label_set_text(sw_laps_label, laps);
}

static void show_timer(void) {
	char text[32];
	int64_t left = timer_running ? timer_end - now_ms() : timer_left;
	if (timer_done) {
		lv_label_set_text(timer_label, tr("clocks_done"));
	} else {
		format_ms(text, sizeof(text), left, false);
		lv_label_set_text(timer_label, text);
	}
	lv_label_set_text(timer_go_label, tr(timer_running					  ? "clocks_pause"
										 : timer_left < timer_secs * 1000LL ? "clocks_resume"
																			: "clocks_start"));
	bool idle = !timer_running && timer_left >= timer_secs * 1000LL;
	lv_obj_set_hidden(timer_picker.box, !idle);
}

static void show_tab(void) {
	switch (tab) {
	case TAB_CLOCK: clockface_refresh(face); break;
	case TAB_STOPWATCH: show_stopwatch(); break;
	case TAB_TIMER: show_timer(); break;
	}
}

// ---------------------------------------------------------------------------
// the beep, over music too: the track is paused for it and plays on after
// ---------------------------------------------------------------------------

static void resume_cb(void *user) {
	(void)user;
	player_interrupt_resume();
}

// On the beep's thread: the music goes back on the UI thread, where the player
// lives.
static void beep_done(void) { gui_post(resume_cb, NULL); }

static void ring(void) {
	bool paused = player_interrupt_pause();
	if (!chime_beep(paused ? beep_done : NULL) && paused) {
		player_interrupt_resume(); // no beep after all: no reason to stay paused
	}
}

// ---------------------------------------------------------------------------
// the tick: ends timers whether or not the page is open
// ---------------------------------------------------------------------------

static void tick_cb(lv_timer_t *t) {
	(void)t;
	int64_t now = now_ms();
	if (timer_running && now >= timer_end) {
		timer_running = false;
		timer_done = true;
		timer_left = timer_secs * 1000LL;
		ring();
		if (!page_visible || tab != TAB_TIMER) {
			toast_plain("clocks_done");
		}
	}
	if (page_visible) {
		static int64_t last_face;
		// The clock only needs a second's resolution; the stopwatch wants tenths.
		if (tab != TAB_CLOCK || now - last_face >= 1000) {
			last_face = now;
			show_tab();
		}
	} else if (!timer_running) {
		lv_timer_pause(tick_timer); // nothing to watch until someone starts something
	}
}

// ---------------------------------------------------------------------------
// the controls
// ---------------------------------------------------------------------------

static void sw_go_cb(lv_event_t *e) {
	(void)e;
	int64_t now = now_ms();
	if (sw_running) {
		sw_banked += now - sw_started;
	} else {
		sw_started = now;
	}
	sw_running = !sw_running;
	show_stopwatch();
}

static void sw_side_cb(lv_event_t *e) {
	(void)e;
	if (sw_running) {
		// Newest first; the oldest drops off the end.
		for (int i = LAPS - 1; i > 0; i--) {
			sw_laps[i] = sw_laps[i - 1];
		}
		sw_laps[0] = sw_banked + now_ms() - sw_started;
		if (sw_lap_count < LAPS) {
			sw_lap_count++;
		}
	} else {
		sw_banked = 0;
		sw_lap_count = 0;
	}
	show_stopwatch();
}

static void timer_go_cb(lv_event_t *e) {
	(void)e;
	int64_t now = now_ms();
	timer_done = false;
	if (timer_running) {
		timer_left = timer_end - now;
		timer_running = false;
	} else {
		if (timer_left <= 0) {
			return; // nothing set: 0 h 00 min
		}
		timer_end = now + timer_left;
		timer_running = true;
		wake_tick();
	}
	show_timer();
}

static void timer_reset_cb(lv_event_t *e) {
	(void)e;
	timer_running = false;
	timer_done = false;
	timer_left = timer_secs * 1000LL;
	show_timer();
}

static void timer_picked(int minutes) {
	timer_secs = minutes * 60;
	config_set_int("clocks", "timer_secs", timer_secs);
	config_save();
	timer_reset_cb(NULL);
}

static void tab_cb(lv_event_t *e) {
	tab = (int)(intptr_t)lv_event_get_user_data(e);
	tabbar_set_active(&tabs, tab);
	for (int i = 0; i < TABS; i++) {
		lv_obj_set_hidden(panels[i], i != tab);
	}
	config_set_int("clocks", "tab", tab);
	config_save();
	show_tab();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	page_visible = true;
	show_tab();
	wake_tick();
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	page_visible = false;
}

static void refresh_theme(void) {
	tabbar_set_active(&tabs, tab);
	durationpicker_repaint(&timer_picker);
	show_tab();
}

// ---------------------------------------------------------------------------
// building
// ---------------------------------------------------------------------------

static lv_obj_t *plain(lv_obj_t *parent) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_set_style_bg_opa(o, 0, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_set_scrollable(o, false);
	return o;
}

static lv_obj_t *panel(lv_obj_t *parent) {
	lv_obj_t *p = plain(parent);
	lv_obj_set_size(p, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(p, 22, 0);
	lv_obj_set_style_pad_top(p, 24, 0);
	return p;
}

static lv_obj_t *big_label(lv_obj_t *parent) {
	lv_obj_t *l = lv_label_create(parent);
	lv_obj_add_style(l, &theme_style_text, 0);
	lv_obj_set_style_text_font(l, &font_ui_64_bold, 0);
	lv_label_set_text(l, "");
	return l;
}

static lv_obj_t *row(lv_obj_t *parent) {
	lv_obj_t *r = plain(parent);
	lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 16, 0);
	return r;
}

// A rounded button with a word on it: the accent for the main action, the
// quieter surface for the one beside it.
static lv_obj_t *button(lv_obj_t *parent, bool main, lv_event_cb_t cb, lv_obj_t **label_out) {
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_set_size(b, 150, 64);
	lv_obj_set_style_radius(b, 32, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	if (main) {
		lv_obj_add_style(b, &theme_style_accent_bg, 0);
	} else {
		lv_obj_set_style_bg_color(b, theme()->surface_pressed, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	}
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *l = lv_label_create(b);
	lv_obj_set_style_text_font(l, &font_ui_22, 0);
	lv_obj_set_style_text_color(l, main ? lv_color_white() : theme()->text_primary, 0);
	lv_obj_center(l);
	if (label_out) {
		*label_out = l;
	}
	return b;
}

void clockspage_init(gui_config_t *cfg) {
	timer_secs = (int)config_get_int("clocks", "timer_secs", 5 * 60);
	timer_left = timer_secs * 1000LL;
	tab = (int)config_get_int("clocks", "tab", TAB_CLOCK);
	if (tab < 0 || tab >= TABS) {
		tab = TAB_CLOCK;
	}

	lv_obj_add_style(clockspage_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(clockspage_screen, false);
	settingsrow_title(clockspage_screen, cfg, "clocks");

	int top = settingsrow_content_top(cfg);
	lv_obj_t *page = plain(clockspage_screen);
	lv_obj_set_pos(page, cfg->padding, top);
	lv_obj_set_size(page, (int32_t)cfg->screen_width - 2 * cfg->padding, (int32_t)cfg->screen_height - top);
	lv_obj_set_scrollable(page, true);
	lv_obj_set_scroll_dir(page, LV_DIR_VER);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_bottom(page, cfg->padding, 0);

	tabbar_create(&tabs, page);
	static const char *const NAMES[TABS] = {"clocks_clock", "clocks_stopwatch", "clocks_timer"};
	for (int i = 0; i < TABS; i++) {
		tabbar_add(&tabs, NAMES[i], tab_cb, (void *)(intptr_t)i);
	}

	// Clock
	panels[TAB_CLOCK] = panel(page);
	face = clockface_create(panels[TAB_CLOCK], (int)cfg->screen_width * 3 / 4, true);

	// Stopwatch
	panels[TAB_STOPWATCH] = panel(page);
	sw_label = big_label(panels[TAB_STOPWATCH]);
	lv_obj_t *sw_buttons = row(panels[TAB_STOPWATCH]);
	button(sw_buttons, false, sw_side_cb, &sw_side_label);
	button(sw_buttons, true, sw_go_cb, &sw_go_label);
	sw_laps_label = lv_label_create(panels[TAB_STOPWATCH]);
	lv_obj_add_style(sw_laps_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(sw_laps_label, &font_ui_22, 0);
	lv_obj_set_style_text_align(sw_laps_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_line_space(sw_laps_label, 8, 0);

	// Timer
	panels[TAB_TIMER] = panel(page);
	timer_label = big_label(panels[TAB_TIMER]);
	durationpicker_create(&timer_picker, panels[TAB_TIMER], PRESETS, (int)(sizeof(PRESETS) / sizeof(PRESETS[0])),
						  timer_picked);
	durationpicker_set(&timer_picker, timer_secs / 60);
	lv_obj_t *timer_buttons = row(panels[TAB_TIMER]);
	lv_obj_t *reset_label;
	button(timer_buttons, false, timer_reset_cb, &reset_label);
	lv_label_set_text(reset_label, tr("clocks_reset"));
	button(timer_buttons, true, timer_go_cb, &timer_go_label);

	for (int i = 0; i < TABS; i++) {
		lv_obj_set_hidden(panels[i], i != tab);
	}
	tabbar_set_active(&tabs, tab);

	tick_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	lv_timer_pause(tick_timer);
	theme_register_refresh(refresh_theme);
	lv_obj_add_event_cb(clockspage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(clockspage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
