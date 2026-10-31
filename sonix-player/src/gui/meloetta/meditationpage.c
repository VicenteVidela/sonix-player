#include "meditationpage.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/meloetta/durationpicker.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/meloetta/chime.h"

lv_obj_t *meditationpage_screen;

#define TICK_MS 250

static const int LENGTHS[] = {5, 10, 15, 20, 30}; // minutes

static int minutes;
static bool running;
static int64_t ends_at;
static bool page_visible;
static lv_timer_t *tick_timer;

static lv_obj_t *arc, *time_label, *go_label, *streak_label, *total_label;
static durationpicker_t picker; // hidden while a session runs

static int64_t now_ms(void) {
	struct timespec ts;
#ifdef CLOCK_BOOTTIME
	if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0)
#endif
		clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// A day as YYYYMMDD, `back` days before today, in local time.
static long day_number(int back) {
	time_t now = time(NULL);
	struct tm local;
	localtime_r(&now, &local);
	local.tm_mday -= back;
	local.tm_hour = 12; // away from any daylight-saving edge
	mktime(&local);
	return (long)(local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
}

// The streak as it stands today: a last session before yesterday has broken it,
// whatever the config still says.
static long current_streak(void) {
	long last = config_get_int("meditation", "last_day", 0);
	if (last != day_number(0) && last != day_number(1)) {
		return 0;
	}
	return config_get_int("meditation", "streak", 0);
}

static void record_session(void) {
	long today = day_number(0);
	long last = config_get_int("meditation", "last_day", 0);
	long streak = config_get_int("meditation", "streak", 0);
	if (last != today) {
		streak = last == day_number(1) ? streak + 1 : 1;
	}
	config_set_int("meditation", "streak", streak);
	config_set_int("meditation", "last_day", today);
	config_set_int("meditation", "total", config_get_int("meditation", "total", 0) + 1);
	config_save();
}

static void show(void) {
	int64_t length = minutes * 60000LL;
	int64_t left = running ? ends_at - now_ms() : length;
	if (left < 0) left = 0;
	int64_t secs = (left + 999) / 1000;
	lv_label_set_text_fmt(time_label, "%d:%02d", (int)(secs / 60), (int)(secs % 60));
	lv_arc_set_value(arc, (int32_t)((length - left) * 1000 / (length ? length : 1)));
	lv_label_set_text(go_label, tr(running ? "meditation_stop" : "meditation_start"));
	lv_obj_set_hidden(picker.box, running);
	long streak = current_streak();
	if (streak > 0) {
		lv_label_set_text_fmt(streak_label, tr("meditation_streak"), (int)streak);
	} else {
		lv_label_set_text(streak_label, tr("meditation_no_streak"));
	}
	lv_label_set_text_fmt(total_label, tr("meditation_total"), (int)config_get_int("meditation", "total", 0));
}

static void tick_cb(lv_timer_t *t) {
	(void)t;
	if (running && now_ms() >= ends_at) {
		running = false;
		record_session();
		chime_bowl(3);
		toast_success("meditation_done");
	}
	if (page_visible) {
		show();
	} else if (!running) {
		lv_timer_pause(tick_timer);
	}
}

static void go_cb(lv_event_t *e) {
	(void)e;
	if (running) {
		running = false; // stopped early: no streak, no bell
	} else {
		if (minutes <= 0) {
			return; // nothing set: 0 h 00 min
		}
		running = true;
		ends_at = now_ms() + minutes * 60000LL;
		chime_bowl(1);
		lv_timer_resume(tick_timer);
	}
	show();
}

static void length_picked(int chosen) {
	if (running) {
		return;
	}
	minutes = chosen;
	config_set_int("meditation", "minutes", minutes);
	config_save();
	show();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	page_visible = true;
	show();
	lv_timer_resume(tick_timer);
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	page_visible = false;
}

static void refresh_theme(void) {
	durationpicker_repaint(&picker);
	lv_obj_set_style_arc_color(arc, theme()->surface_pressed, LV_PART_MAIN);
	lv_obj_set_style_arc_color(arc, theme()->accent, LV_PART_INDICATOR);
	show();
}

void meditationpage_init(gui_config_t *cfg) {
	minutes = (int)config_get_int("meditation", "minutes", 10);

	lv_obj_add_style(meditationpage_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(meditationpage_screen, false);
	settingsrow_title(meditationpage_screen, cfg, "meditation");

	int top = settingsrow_content_top(cfg);
	int32_t width = (int32_t)cfg->screen_width - 2 * cfg->padding;
	lv_obj_t *page = lv_obj_create(meditationpage_screen);
	lv_obj_set_pos(page, cfg->padding, top);
	lv_obj_set_size(page, width, (int32_t)cfg->screen_height - top - cfg->padding);
	lv_obj_set_style_bg_opa(page, 0, 0);
	lv_obj_set_style_border_width(page, 0, 0);
	lv_obj_set_style_pad_all(page, 0, 0);
	lv_obj_set_style_pad_row(page, 20, 0);
	// The wheels and the ring together run past the bottom of a short panel;
	// while a session runs the wheels are hidden and it all fits.
	lv_obj_set_scrollable(page, true);
	lv_obj_set_scroll_dir(page, LV_DIR_VER);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	durationpicker_create(&picker, page, LENGTHS, (int)(sizeof(LENGTHS) / sizeof(LENGTHS[0])), length_picked);
	durationpicker_set(&picker, minutes);

	int32_t ring = width * 3 / 5;
	arc = lv_arc_create(page);
	lv_obj_set_size(arc, ring, ring);
	lv_arc_set_bg_angles(arc, 0, 360);
	lv_arc_set_rotation(arc, 270);
	lv_arc_set_range(arc, 0, 1000);
	lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
	lv_obj_set_clickable(arc, false);
	lv_obj_set_style_arc_width(arc, 14, LV_PART_MAIN);
	lv_obj_set_style_arc_width(arc, 14, LV_PART_INDICATOR);
	lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
	time_label = lv_label_create(arc);
	lv_obj_add_style(time_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(time_label, &font_ui_64_bold, 0);
	lv_obj_center(time_label);

	lv_obj_t *go = lv_button_create(page);
	lv_obj_set_size(go, 180, 64);
	lv_obj_set_style_radius(go, 32, 0);
	lv_obj_set_style_shadow_width(go, 0, 0);
	lv_obj_add_style(go, &theme_style_accent_bg, 0);
	lv_obj_add_event_cb(go, go_cb, LV_EVENT_CLICKED, NULL);
	go_label = lv_label_create(go);
	lv_obj_set_style_text_font(go_label, &font_ui_22, 0);
	lv_obj_set_style_text_color(go_label, lv_color_white(), 0);
	lv_obj_center(go_label);

	streak_label = lv_label_create(page);
	lv_obj_add_style(streak_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(streak_label, &font_ui_24, 0);
	total_label = lv_label_create(page);
	lv_obj_add_style(total_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(total_label, &font_ui_18, 0);

	refresh_theme();
	tick_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	lv_timer_pause(tick_timer);
	theme_register_refresh(refresh_theme);
	lv_obj_add_event_cb(meditationpage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(meditationpage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
