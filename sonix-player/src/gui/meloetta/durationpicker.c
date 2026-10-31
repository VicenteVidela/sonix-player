#include "durationpicker.h"

#include <stdint.h>
#include <stdio.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"

// The wheels are made the way settingsrow.c makes the sleep timers': the same
// font, spacing and three visible rows, so the two read as one control.

static void fill_numbers(char *out, size_t size, int last, const char *format) {
	size_t used = 0;
	for (int value = 0; value <= last && used < size; value++) {
		used += (size_t)snprintf(out + used, size - used, format, value);
		if (value != last) {
			used += (size_t)snprintf(out + used, size - used, "\n");
		}
	}
}

static void paint_roller(lv_obj_t *roller) {
	lv_obj_set_style_text_color(roller, theme()->text_secondary, 0);
	lv_obj_set_style_bg_color(roller, theme()->surface_pressed, LV_PART_SELECTED);
	lv_obj_set_style_text_color(roller, theme()->accent, LV_PART_SELECTED);
}

static void paint_pills(durationpicker_t *p) {
	int now = durationpicker_get(p);
	for (int i = 0; i < p->count; i++) {
		settingsrow_pill_active(p->pills[i], p->presets[i] == now);
	}
}

static void wheel_cb(lv_event_t *e) {
	durationpicker_t *p = lv_event_get_user_data(e);
	paint_pills(p);
	if (p->changed) {
		p->changed(durationpicker_get(p));
	}
}

static void preset_cb(lv_event_t *e) {
	durationpicker_t *p = lv_obj_get_user_data(lv_obj_get_parent(lv_event_get_current_target(e)));
	if (!p) {
		return;
	}
	durationpicker_set(p, (int)(intptr_t)lv_event_get_user_data(e));
	if (p->changed) {
		p->changed(durationpicker_get(p));
	}
}

static lv_obj_t *roller(lv_obj_t *parent, const char *options, int width, durationpicker_t *p) {
	lv_obj_t *r = lv_roller_create(parent);
	lv_roller_set_options(r, options, LV_ROLLER_MODE_NORMAL);
	lv_obj_set_width(r, width);
	lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(r, 0, 0);
	lv_obj_set_style_shadow_width(r, 0, 0);
	lv_obj_set_style_text_font(r, &font_ui_24, 0);
	lv_obj_set_style_text_line_space(r, 14, 0);
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
	lv_obj_set_style_radius(r, 10, LV_PART_SELECTED);
	lv_obj_set_style_text_font(r, &font_ui_24_bold, LV_PART_SELECTED);
	paint_roller(r);
	lv_obj_add_event_cb(r, wheel_cb, LV_EVENT_VALUE_CHANGED, p);
	// After the font and spacing: the height is worked out from them here.
	lv_roller_set_visible_row_count(r, 3);
	return r;
}

static void unit(lv_obj_t *parent, const char *text) {
	lv_obj_t *l = lv_label_create(parent);
	lv_label_set_text(l, text);
	lv_obj_add_style(l, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(l, &font_ui_22, 0);
}

static lv_obj_t *plain(lv_obj_t *parent) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(o, 0, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_set_scrollable(o, false);
	return o;
}

void durationpicker_create(durationpicker_t *p, lv_obj_t *parent, const int *presets, int count,
						   void (*changed)(int minutes)) {
	// lv_roller keeps the pointer rather than copying the options.
	static char hour_options[24 * 4], minute_options[60 * 4];
	if (!hour_options[0]) {
		fill_numbers(hour_options, sizeof(hour_options), 23, "%d");
		fill_numbers(minute_options, sizeof(minute_options), 59, "%02d");
	}
	p->changed = changed;
	p->count = count < DURATIONPICKER_PRESETS ? count : DURATIONPICKER_PRESETS;

	p->box = plain(parent);
	lv_obj_set_flex_flow(p->box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(p->box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(p->box, 14, 0);

	lv_obj_t *wheels = plain(p->box);
	lv_obj_set_flex_flow(wheels, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(wheels, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(wheels, 10, 0);
	p->hours = roller(wheels, hour_options, 76, p);
	unit(wheels, "h");
	p->minutes = roller(wheels, minute_options, 86, p);
	unit(wheels, "min");

	lv_obj_t *row = plain(p->box);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(row, 10, 0);
	lv_obj_set_user_data(row, p);
	for (int i = 0; i < p->count; i++) {
		char text[16];
		p->presets[i] = presets[i];
		if (presets[i] % 60 == 0) {
			snprintf(text, sizeof(text), "%d h", presets[i] / 60);
		} else {
			snprintf(text, sizeof(text), "%d min", presets[i]);
		}
		p->pills[i] = settingsrow_pill_text(row, text, presets[i], preset_cb);
	}
}

void durationpicker_set(durationpicker_t *p, int minutes) {
	if (minutes < 0) minutes = 0;
	if (minutes > 23 * 60 + 59) minutes = 23 * 60 + 59;
	lv_roller_set_selected(p->hours, (uint32_t)(minutes / 60), LV_ANIM_ON);
	lv_roller_set_selected(p->minutes, (uint32_t)(minutes % 60), LV_ANIM_ON);
	paint_pills(p);
}

int durationpicker_get(const durationpicker_t *p) {
	return (int)lv_roller_get_selected(p->hours) * 60 + (int)lv_roller_get_selected(p->minutes);
}

void durationpicker_repaint(durationpicker_t *p) {
	paint_roller(p->hours);
	paint_roller(p->minutes);
	paint_pills(p);
}
