#include "clockface.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/src/misc/cache/instance/lv_image_cache.h" // lv_image_cache_drop(), not in lvgl.h since 9.2
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/clocklabel.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/core/respath.h"
#include "src/system/device/clock.h"

// The widget's colours (main.qml), all of them.
#define BLUE 0x1F96FA
#define HUB 0x35A7FF
#define SECOND_BLUE 0x4E8FF5
#define MOON_LIT 0xEBE854
#define MOON_DARK 0x1C1A06
#define FACE 0x0D0D0E
#define FACE_OPA 0xA6
#define SOUTHERN true // the player lives in Chile, like KingGroot

// The numerals' face: the widget asks for a bold serif, and the player carries
// one for the e-book reader. Without it the interface's bold stands in.
#define NUMERAL_FONT SONIX_RESOURCE_DIR "/fonts/ebook/Bookerly-Bold.ttf"

typedef struct {
	lv_obj_t *dial;
	lv_obj_t *time_label; // NULL without figures
	lv_obj_t *date_label;
	const lv_font_t *numeral_font;
	// The moon, rendered smooth into a picture of its own and redrawn only when
	// its size or its phase (to a thousandth) changes.
	lv_draw_buf_t *moon;
	int moon_size, moon_phase;
} face_t;

// ---------------------------------------------------------------------------
// drawing helpers, in the dial's own terms: angles in turns from twelve,
// clockwise; lengths as fractions of the radius
// ---------------------------------------------------------------------------

typedef struct {
	lv_layer_t *layer;
	float cx, cy, r;
} dial_t;

static lv_point_precise_t at(const dial_t *d, float turn, float radius, float across) {
	float a = turn * 2.0f * (float)M_PI;
	float s = sinf(a), c = cosf(a);
	// `radius` out from the centre along the hand's direction, `across` to its
	// right, both times R.
	lv_point_precise_t p = {(lv_value_precise_t)(d->cx + (s * radius + c * across) * d->r),
							(lv_value_precise_t)(d->cy + (-c * radius + s * across) * d->r)};
	return p;
}

static void triangle(const dial_t *d, lv_point_precise_t a, lv_point_precise_t b, lv_point_precise_t c, uint32_t color) {
	lv_draw_triangle_dsc_t t;
	lv_draw_triangle_dsc_init(&t);
	t.color = lv_color_hex(color);
	t.opa = LV_OPA_COVER;
	t.p[0] = a;
	t.p[1] = b;
	t.p[2] = c;
	lv_draw_triangle(d->layer, &t);
}

static void disc(const dial_t *d, float x, float y, float r, uint32_t color, lv_opa_t opa) {
	lv_draw_rect_dsc_t dsc;
	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_color = lv_color_hex(color);
	dsc.bg_opa = opa;
	dsc.radius = LV_RADIUS_CIRCLE;
	lv_area_t a = {(int32_t)lroundf(x - r), (int32_t)lroundf(y - r), (int32_t)lroundf(x + r), (int32_t)lroundf(y + r)};
	lv_draw_rect(d->layer, &dsc, &a);
}

// A circle stroked `width` wide on radius `r`, both in pixels.
static void ring(const dial_t *d, float r, float width) {
	lv_draw_arc_dsc_t arc;
	lv_draw_arc_dsc_init(&arc);
	arc.color = lv_color_hex(BLUE);
	arc.center.x = (int32_t)lroundf(d->cx);
	arc.center.y = (int32_t)lroundf(d->cy);
	arc.radius = (uint16_t)lroundf(r + width / 2.0f);
	arc.width = (int32_t)lroundf(width < 1.0f ? 1.0f : width);
	arc.start_angle = 0;
	arc.end_angle = 360;
	lv_draw_arc(d->layer, &arc);
}

// A tapered blade from the hub, the widget's shape: narrow at the root, widest
// just short of the tip.
static void blade(const dial_t *d, float turn, float len, float w) {
	lv_point_precise_t root_l = at(d, turn, -0.02f, -w * 0.3f), root_r = at(d, turn, -0.02f, w * 0.3f);
	lv_point_precise_t mid_l = at(d, turn, len * 0.86f, -w), mid_r = at(d, turn, len * 0.86f, w);
	lv_point_precise_t tip = at(d, turn, len, 0);
	triangle(d, root_l, mid_l, mid_r, BLUE);
	triangle(d, root_l, mid_r, root_r, BLUE);
	triangle(d, mid_l, tip, mid_r, BLUE);
}

// One numeral along the dial, upright toward the rim: each letter is drawn
// turned with it and stepped along the tangent.
static void numeral(const dial_t *d, const lv_font_t *font, const char *text, float turn) {
	int widths[8], total = 0, n = 0;
	for (; text[n] && n < 8; n++) {
		widths[n] = (int)lv_font_get_glyph_width(font, (uint32_t)text[n], (uint32_t)text[n + 1]);
		total += widths[n];
	}
	float cap = (float)font->line_height * 0.70f;
	float x = -(float)total / 2.0f;
	for (int i = 0; i < n; i++) {
		lv_draw_letter_dsc_t dsc;
		lv_draw_letter_dsc_init(&dsc);
		dsc.unicode = (uint32_t)text[i];
		dsc.font = font;
		dsc.color = lv_color_hex(BLUE);
		dsc.rotation = (int32_t)lroundf(turn * 3600.0f);
		// The point lv_draw_letter takes is the middle of the glyph's baseline,
		// which it also turns around (lv_draw_label.c moves the glyph back by
		// that pivot). Put it half a capital inward of the numeral's centre
		// line, so the letter's middle lands on the line.
		float along = (x + (float)widths[i] / 2.0f) / d->r;
		lv_point_precise_t base = at(d, turn, 0.795f - cap / 2.0f / d->r, along);
		lv_point_t p = {(int32_t)lroundf((float)base.x), (int32_t)lroundf((float)base.y)};
		lv_draw_letter(d->layer, &dsc, &p);
		x += (float)widths[i];
	}
}

// Moon age as a fraction of the synodic month, 0 new, 0.5 full: the widget's
// mean phase, so the two clocks always agree.
static float moon_phase(time_t now) {
	const double synodic = 29.530588853;
	const double known_new = 947182440.0; // 2000-01-06 18:14 UTC
	double p = fmod(((double)now - known_new) / 86400.0, synodic) / synodic;
	return (float)(p < 0 ? p + 1 : p);
}

#define MOON_SAMPLES 4 // per pixel, each way: 16 samples give a smooth limb and terminator

// Paints the moon into `buf`, a square `size` pixels wide: a dark disc, and on
// it the lit part between the limb and the terminator (an ellipse of x-radius
// r*|cos 2*pi*p|). Each pixel is the average of a grid of samples, its alpha
// how many of them fall on the disc -- the widget's canvas is anti-aliased,
// and rows of whole pixels leave steps along both curves.
static void moon_render(lv_draw_buf_t *buf, int size, float p) {
	float k = cosf(2.0f * (float)M_PI * p);
	float side = (p < 0.5f ? 1.0f : -1.0f) * (SOUTHERN ? -1.0f : 1.0f); // +1: lit on the right
	lv_color_t lit = lv_color_hex(MOON_LIT), dark = lv_color_hex(MOON_DARK);
	float r = (float)size / 2.0f;
	for (int py = 0; py < size; py++) {
		uint8_t *row = buf->data + (size_t)py * buf->header.stride;
		for (int px = 0; px < size; px++) {
			int inside = 0, on_lit = 0;
			for (int sy = 0; sy < MOON_SAMPLES; sy++) {
				float y = ((float)py + ((float)sy + 0.5f) / MOON_SAMPLES - r) / r;
				float half = 1.0f - y * y;
				if (half < 0.0f) {
					continue;
				}
				half = sqrtf(half);
				float limb = side * half, term = side * k * half;
				float lo = limb < term ? limb : term, hi = limb < term ? term : limb;
				for (int sx = 0; sx < MOON_SAMPLES; sx++) {
					float x = ((float)px + ((float)sx + 0.5f) / MOON_SAMPLES - r) / r;
					if (x * x + y * y > 1.0f) {
						continue;
					}
					inside++;
					on_lit += x >= lo && x <= hi;
				}
			}
			uint8_t *out = row + px * 4; // ARGB8888 is B, G, R, A in memory
			if (!inside) {
				out[0] = out[1] = out[2] = out[3] = 0;
				continue;
			}
			int mix = on_lit * 255 / inside;
			out[0] = (uint8_t)((lit.blue * mix + dark.blue * (255 - mix)) / 255);
			out[1] = (uint8_t)((lit.green * mix + dark.green * (255 - mix)) / 255);
			out[2] = (uint8_t)((lit.red * mix + dark.red * (255 - mix)) / 255);
			out[3] = (uint8_t)(inside * 255 / (MOON_SAMPLES * MOON_SAMPLES));
		}
	}
}

// The moon a little below twelve, from its picture.
static void moon(const dial_t *d, face_t *f, time_t now) {
	float mx = d->cx, my = d->cy - d->r * 0.5f, mr = d->r * 0.108f;
	int size = (int)lroundf(mr * 2.0f);
	float p = moon_phase(now);
	int key = (int)lroundf(p * 1000.0f);
	if (size < 2) {
		return;
	}
	if (!f->moon || f->moon_size != size) {
		if (f->moon) {
			lv_draw_buf_destroy(f->moon);
		}
		f->moon = lv_draw_buf_create((uint32_t)size, (uint32_t)size, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO);
		f->moon_size = size;
		f->moon_phase = -1;
		if (!f->moon) {
			return;
		}
	}
	if (f->moon_phase != key) {
		moon_render(f->moon, size, p);
		f->moon_phase = key;
		lv_image_cache_drop(f->moon); // same buffer, new picture: no stale copy
	}
	lv_draw_image_dsc_t img;
	lv_draw_image_dsc_init(&img);
	img.src = f->moon;
	lv_area_t a = {(int32_t)lroundf(mx - (float)size / 2.0f), (int32_t)lroundf(my - (float)size / 2.0f), 0, 0};
	a.x2 = a.x1 + size - 1;
	a.y2 = a.y1 + size - 1;
	lv_draw_image(d->layer, &img, &a);
}

static void dial_draw_cb(lv_event_t *e) {
	lv_layer_t *layer = lv_event_get_layer(e);
	face_t *f = lv_event_get_user_data(e);
	if (!layer || !f) {
		return;
	}
	lv_area_t area;
	lv_obj_get_coords(lv_event_get_current_target(e), &area);
	float size = (float)lv_area_get_width(&area);
	dial_t d = {layer, (float)area.x1 + size / 2.0f, (float)area.y1 + size / 2.0f, size / 2.0f};
	float R = d.r;

	disc(&d, d.cx, d.cy, R * 0.905f, FACE, FACE_OPA);

	// spikes, one per hour, the cardinals a little bigger
	for (int i = 0; i < 12; i++) {
		bool big = i % 3 == 0;
		float tip = big ? 0.995f : 0.97f, hw = big ? 0.036f : 0.028f;
		float turn = (float)i / 12.0f;
		triangle(&d, at(&d, turn, 0.89f, -hw), at(&d, turn, tip, 0), at(&d, turn, 0.89f, hw), BLUE);
	}

	ring(&d, R * 0.89f, R * 0.028f);
	ring(&d, R * 0.708f, R * 0.013f);

	static const char *const NUMERALS[] = {"XII", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI"};
	for (int n = 0; n < 12; n++) {
		numeral(&d, f->numeral_font, NUMERALS[n], (float)n / 12.0f);
	}

	time_t now = time(NULL);
	moon(&d, f, now);

	// Hands whenever the time is a real one: the clock may count as unset only
	// because no time zone was chosen, and then the time is still right in UTC.
	if (clock_is_set() || now > 1700000000) {
		struct tm local;
		localtime_r(&now, &local);
		float sec = (float)local.tm_sec; // whole seconds: the hand ticks
		float min = (float)local.tm_min + sec / 60.0f;
		float hr = (float)(local.tm_hour % 12) + min / 60.0f;
		blade(&d, hr / 12.0f, 0.485f, 0.042f);
		blade(&d, min / 60.0f, 0.665f, 0.034f);

		lv_point_precise_t back = at(&d, sec / 60.0f, -0.06f, 0), front = at(&d, sec / 60.0f, 0.755f, 0);
		lv_draw_line_dsc_t line;
		lv_draw_line_dsc_init(&line);
		line.p1 = back;
		line.p2 = front;
		line.width = (int32_t)lroundf(R * 0.008f < 1.0f ? 1.0f : R * 0.008f);
		line.color = lv_color_hex(SECOND_BLUE);
		lv_draw_line(layer, &line);
	}

	// hub: a soft ring and the cap
	disc(&d, d.cx, d.cy, R * 0.08f, HUB, LV_OPA_30);
	disc(&d, d.cx, d.cy, R * 0.062f, HUB, LV_OPA_COVER);
}

// ---------------------------------------------------------------------------
// the face
// ---------------------------------------------------------------------------

// One font per size, kept for good: the Clocks page and the screensaver may ask
// for the same one, and there are never more than a couple of sizes.
static const lv_font_t *numeral_font_for(int px) {
	static struct {
		int px;
		const lv_font_t *font;
	} cache[2];
	for (int i = 0; i < 2; i++) {
		if (cache[i].font && cache[i].px == px) {
			return cache[i].font;
		}
	}
	lv_font_t *font = lv_freetype_font_create(NUMERAL_FONT, LV_FREETYPE_FONT_RENDER_MODE_BITMAP, (uint32_t)px,
											  LV_FREETYPE_FONT_STYLE_NORMAL);
	if (!font) {
		printf("clockface: %s cannot be opened; numerals in the interface bold\n", NUMERAL_FONT);
		return &font_ui_22_bold;
	}
	for (int i = 0; i < 2; i++) {
		if (!cache[i].font) {
			cache[i].px = px;
			cache[i].font = font;
			break;
		}
	}
	return font;
}

// One line of a language list ("Sunday\nMonday\n..."), counted from 0.
static void list_item(const char *list, int index, char *out, size_t size) {
	for (int i = 0; i < index && list; i++) {
		list = strchr(list, '\n');
		list = list ? list + 1 : NULL;
	}
	size_t n = list ? strcspn(list, "\n") : 0;
	snprintf(out, size, "%.*s", (int)(n < size ? n : size - 1), list ? list : "");
}

void clockface_refresh(lv_obj_t *face) {
	face_t *f = face ? lv_obj_get_user_data(face) : NULL;
	if (!f) {
		return;
	}
	lv_obj_invalidate(f->dial);
	if (!f->time_label) {
		return;
	}
	clocklabel_refresh(f->time_label);
	if (!clock_is_set()) {
		lv_label_set_text(f->date_label, "");
		return;
	}
	time_t now = time(NULL);
	struct tm local;
	localtime_r(&now, &local);
	char day[32], month[32], text[96];
	list_item(tr("weather_weekdays"), local.tm_wday, day, sizeof(day));
	list_item(tr("timeset_month_names"), local.tm_mon, month, sizeof(month));
	snprintf(text, sizeof(text), "%s %d %s", day, local.tm_mday, month);
	lv_label_set_text(f->date_label, text);
}

static void face_deleted_cb(lv_event_t *e) {
	face_t *f = lv_event_get_user_data(e);
	if (f && f->moon) {
		lv_draw_buf_destroy(f->moon);
	}
	free(f);
}

lv_obj_t *clockface_create(lv_obj_t *parent, int dial_size, bool figures) {
	face_t *f = calloc(1, sizeof(*f));
	if (!f) {
		return NULL;
	}
	f->numeral_font = numeral_font_for((int)lroundf((float)dial_size / 2.0f * 0.125f));

	lv_obj_t *face = lv_obj_create(parent);
	lv_obj_set_size(face, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(face, 0, 0);
	lv_obj_set_style_border_width(face, 0, 0);
	lv_obj_set_style_pad_all(face, 0, 0);
	lv_obj_set_style_pad_row(face, 10, 0);
	lv_obj_set_scrollable(face, false);
	lv_obj_set_clickable(face, false);
	lv_obj_set_event_bubble(face, true);
	lv_obj_set_flex_flow(face, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(face, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_user_data(face, f);
	lv_obj_add_event_cb(face, face_deleted_cb, LV_EVENT_DELETE, f);

	f->dial = lv_obj_create(face);
	lv_obj_set_size(f->dial, dial_size, dial_size);
	lv_obj_set_style_bg_opa(f->dial, 0, 0);
	lv_obj_set_style_border_width(f->dial, 0, 0);
	lv_obj_set_scrollable(f->dial, false);
	lv_obj_set_clickable(f->dial, false);
	lv_obj_set_event_bubble(f->dial, true);
	lv_obj_add_event_cb(f->dial, dial_draw_cb, LV_EVENT_DRAW_MAIN, f);

	if (figures) {
		f->time_label = clocklabel_create(face, &font_ui_72, 72, &font_ui_32, 32);
		lv_obj_add_style(f->time_label, &theme_style_text, 0);
		f->date_label = lv_label_create(face);
		lv_obj_set_style_text_font(f->date_label, &font_ui_24, 0);
		lv_obj_add_style(f->date_label, &theme_style_text_dim, 0);
	}
	clockface_refresh(face);
	return face;
}
