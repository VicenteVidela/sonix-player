#include "moonpage.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *moonpage_screen;

// The phase moves about half a degree an hour: once a minute is plenty, and
// it also turns the dates over at midnight.
#define REFRESH_MS 60000

#define SYNODIC_DAYS 29.530588853
#define DEG (M_PI / 180.0)

// The moon's own colours, the same in every theme: a picture of the moon,
// not a part of the interface.
#define MOON_LIT 0xEDE6C8
#define MOON_DARK 0x2B2E3A

static lv_obj_t *drawing;
static lv_obj_t *name_label;
static lv_obj_t *lit_label;
static lv_obj_t *full_value;
static lv_obj_t *new_value;
static lv_obj_t *row_labels[2];
static lv_timer_t *refresh_timer;

static double elongation; // of the Moon from the Sun, degrees, 0..360: 0 new, 180 full
static bool south;

// ---------------------------------------------------------------------------
// the sky
// ---------------------------------------------------------------------------

static double wrap360(double a) {
	a = fmod(a, 360.0);
	return a < 0 ? a + 360.0 : a;
}

// The Moon's elongation at a moment: the Moon's ecliptic longitude minus the
// Sun's, each from its mean motion and its largest periodic terms (the
// equation of centre for both, and evection, variation and the annual
// equation for the Moon). Good to a few tenths of a degree, which is under an
// hour of the Moon's motion against the Sun.
static double elongation_at(time_t t) {
	double d = (double)t / 86400.0 + 2440587.5 - 2451545.0; // days from J2000.0

	double M = wrap360(357.529 + 0.98560028 * d); // the Sun's mean anomaly
	double sun = 280.459 + 0.98564736 * d + 1.915 * sin(M * DEG) + 0.020 * sin(2 * M * DEG);

	double L = 218.316 + 13.176396 * d;			 // the Moon's mean longitude
	double Mm = wrap360(134.963 + 13.064993 * d); // its mean anomaly
	double D = wrap360(297.850 + 12.190749 * d);	 // its mean elongation
	double F = wrap360(93.272 + 13.229350 * d);	 // its argument of latitude
	double moon = L + 6.289 * sin(Mm * DEG) + 1.274 * sin((2 * D - Mm) * DEG) + 0.658 * sin(2 * D * DEG) +
				  0.214 * sin(2 * Mm * DEG) - 0.186 * sin(M * DEG) - 0.114 * sin(2 * F * DEG);

	return wrap360(moon - sun);
}

// The next moment after `from` when the elongation is `target` degrees: a
// first guess from the mean motion, then a few corrections, each closing most
// of what is left.
static time_t next_elongation(time_t from, double target) {
	const double per_day = 360.0 / SYNODIC_DAYS;
	double ahead = wrap360(target - elongation_at(from));
	if (ahead < 1.0) {
		ahead += 360.0; // just past it: the next one, not this one
	}
	double t = (double)from + ahead / per_day * 86400.0;
	for (int i = 0; i < 6; i++) {
		double diff = wrap360(target - elongation_at((time_t)t) + 180.0) - 180.0;
		t += diff / per_day * 86400.0;
	}
	return (time_t)t;
}

static const char *phase_key(double e) {
	static const char *const KEYS[] = {"moon_new",		 "moon_waxing_crescent", "moon_first_quarter",
									   "moon_waxing_gibbous", "moon_full", "moon_waning_gibbous",
									   "moon_last_quarter", "moon_waning_crescent"};
	return KEYS[(int)floor(wrap360(e + 22.5) / 45.0) % 8];
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

// The disc, one row of pixels at a time: on each row the lit part runs from
// the terminator to the limb. The terminator sits at cos(elongation) of the
// row's half-width, on the right of the middle while the moon waxes (as seen
// from the north) and on the left while it wanes.
static void draw_cb(lv_event_t *e) {
	lv_obj_t *obj = lv_event_get_current_target(e);
	lv_layer_t *layer = lv_event_get_layer(e);
	if (!layer) {
		return;
	}
	lv_area_t area;
	lv_obj_get_coords(obj, &area);
	int32_t w = lv_area_get_width(&area), h = lv_area_get_height(&area);
	int32_t r = (w < h ? w : h) / 2 - 2;
	int32_t cx = area.x1 + w / 2, cy = area.y1 + h / 2;

	lv_draw_rect_dsc_t disc;
	lv_draw_rect_dsc_init(&disc);
	disc.bg_color = lv_color_hex(MOON_DARK);
	disc.radius = LV_RADIUS_CIRCLE;
	lv_area_t whole = {cx - r, cy - r, cx + r, cy + r};
	lv_draw_rect(layer, &disc, &whole);

	lv_draw_rect_dsc_t lit;
	lv_draw_rect_dsc_init(&lit);
	lit.bg_color = lv_color_hex(MOON_LIT);
	double c = cos(elongation * DEG);
	bool waxing = elongation < 180.0;
	for (int32_t y = -r; y <= r; y++) {
		double half = sqrt((double)r * r - (double)y * y);
		double from, to; // as seen from the north, left to right
		if (waxing) {
			from = half * c;
			to = half;
		} else {
			from = -half;
			to = -half * c;
		}
		if (south) {
			double f = from;
			from = -to;
			to = -f;
		}
		int32_t x1 = (int32_t)lround(from), x2 = (int32_t)lround(to);
		if (x2 <= x1) {
			continue;
		}
		lv_area_t row = {cx + x1, cy + y, cx + x2 - 1, cy + y};
		lv_draw_rect(layer, &lit, &row);
	}
}

// "3 November, 14:20" in local time, with the month from the language file.
static void format_when(time_t t, char *out, size_t size) {
	struct tm local;
	localtime_r(&t, &local);
	const char *months = tr("timeset_month_names");
	char month[32] = "";
	for (int i = 0; i < local.tm_mon && months; i++) {
		months = strchr(months, '\n');
		months = months ? months + 1 : NULL;
	}
	if (months) {
		size_t n = strcspn(months, "\n");
		snprintf(month, sizeof(month), "%.*s", (int)(n < sizeof(month) ? n : sizeof(month) - 1), months);
	}
	snprintf(out, size, "%d %s, %02d:%02d", local.tm_mday, month, local.tm_hour, local.tm_min);
}

static void refresh(void) {
	time_t now = time(NULL);
	elongation = elongation_at(now);
	int percent = (int)lround((1.0 - cos(elongation * DEG)) * 50.0);

	lv_label_set_text(name_label, tr(phase_key(elongation)));
	lv_label_set_text_fmt(lit_label, tr("moon_lit"), percent);

	char when[64];
	format_when(next_elongation(now, 180.0), when, sizeof(when));
	lv_label_set_text(full_value, when);
	format_when(next_elongation(now, 0.0), when, sizeof(when));
	lv_label_set_text(new_value, when);

	lv_obj_invalidate(drawing);
}

static void refresh_cb(lv_timer_t *timer) {
	(void)timer;
	refresh();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
	lv_timer_resume(refresh_timer);
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(refresh_timer);
}

// One line of the table under the moon: what on the left, when on the right.
static lv_obj_t *info_row(lv_obj_t *parent, const char *text, lv_obj_t **label_out) {
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	*label_out = lv_label_create(row);
	lv_label_set_text(*label_out, tr(text));
	lv_obj_add_style(*label_out, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(*label_out, &font_ui_20, 0);

	lv_obj_t *value = lv_label_create(row);
	lv_label_set_text(value, "");
	lv_obj_add_style(value, &theme_style_text, 0);
	lv_obj_set_style_text_font(value, &font_ui_20, 0);
	return value;
}

void moonpage_init(gui_config_t *cfg) {
	south = config_get_int("moon", "south", 1) != 0;

	lv_obj_add_style(moonpage_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(moonpage_screen, false);
	settingsrow_title(moonpage_screen, cfg, "moon");

	int top = settingsrow_content_top(cfg);
	int32_t width = (int32_t)cfg->screen_width - 2 * cfg->padding;
	int32_t height = (int32_t)cfg->screen_height - top - cfg->padding;
	lv_obj_t *page = lv_obj_create(moonpage_screen);
	lv_obj_set_pos(page, cfg->padding, top);
	lv_obj_set_size(page, width, height);
	lv_obj_set_style_bg_opa(page, 0, 0);
	lv_obj_set_style_border_width(page, 0, 0);
	lv_obj_set_style_pad_all(page, 0, 0);
	lv_obj_set_style_pad_row(page, 12, 0);
	lv_obj_set_scrollable(page, false);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// As wide as the page allows while leaving half of it for the words.
	int32_t size = width * 3 / 4;
	if (size > height / 2) {
		size = height / 2;
	}
	drawing = lv_obj_create(page);
	lv_obj_set_size(drawing, size, size);
	lv_obj_set_style_bg_opa(drawing, 0, 0);
	lv_obj_set_style_border_width(drawing, 0, 0);
	lv_obj_set_style_margin_top(drawing, 16, 0);
	lv_obj_set_style_margin_bottom(drawing, 16, 0);
	lv_obj_set_clickable(drawing, false);
	lv_obj_set_scrollable(drawing, false);
	lv_obj_add_event_cb(drawing, draw_cb, LV_EVENT_DRAW_MAIN, NULL);

	name_label = lv_label_create(page);
	lv_obj_add_style(name_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(name_label, &font_ui_36_bold, 0);

	lit_label = lv_label_create(page);
	lv_obj_add_style(lit_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(lit_label, &font_ui_22, 0);
	lv_obj_set_style_margin_bottom(lit_label, 20, 0);

	full_value = info_row(page, "moon_next_full", &row_labels[0]);
	new_value = info_row(page, "moon_next_new", &row_labels[1]);

	refresh_timer = lv_timer_create(refresh_cb, REFRESH_MS, NULL);
	lv_timer_pause(refresh_timer);

	lv_obj_add_event_cb(moonpage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(moonpage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
