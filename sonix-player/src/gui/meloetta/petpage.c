#include "petpage.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"
#include "src/system/meloetta/pet.h"

lv_obj_t *petpage_screen;

#define FRAME_MS 120	   // the idle bob, while the page is open
#define NEWS_MS 60000	   // how often the pet is looked in on with the page closed
#define POOP_COLOR 0x8A5A3C
#define HEART_COLOR 0xE8506A
#define HALO_COLOR 0xF6C343

static lv_obj_t *name_label, *stage_label, *sprite, *hint_label;
static lv_obj_t *hunger_hearts, *happy_hearts, *meters;
static lv_obj_t *favorite_label, *meal_label;
static lv_obj_t *pat_btn, *clean_btn, *medicine_btn, *egg_btn;
static lv_timer_t *frame_timer, *news_timer;
static unsigned frame; // ticks of the bob, for the animation's phase
static int hunger_shown, happy_shown;

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

static void disc(lv_layer_t *layer, float x, float y, float r, lv_color_t color, lv_opa_t opa) {
	lv_draw_rect_dsc_t dsc;
	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_color = color;
	dsc.bg_opa = opa;
	dsc.radius = LV_RADIUS_CIRCLE;
	lv_area_t a = {(int32_t)lroundf(x - r), (int32_t)lroundf(y - r), (int32_t)lroundf(x + r), (int32_t)lroundf(y + r)};
	lv_draw_rect(layer, &dsc, &a);
}

static void oval(lv_layer_t *layer, float x, float y, float rx, float ry, lv_color_t color, lv_opa_t opa) {
	lv_draw_rect_dsc_t dsc;
	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_color = color;
	dsc.bg_opa = opa;
	dsc.radius = LV_RADIUS_CIRCLE;
	lv_area_t a = {(int32_t)lroundf(x - rx), (int32_t)lroundf(y - ry), (int32_t)lroundf(x + rx), (int32_t)lroundf(y + ry)};
	lv_draw_rect(layer, &dsc, &a);
}

static void line(lv_layer_t *layer, float x1, float y1, float x2, float y2, float w, lv_color_t color) {
	lv_draw_line_dsc_t dsc;
	lv_draw_line_dsc_init(&dsc);
	dsc.p1.x = (lv_value_precise_t)x1;
	dsc.p1.y = (lv_value_precise_t)y1;
	dsc.p2.x = (lv_value_precise_t)x2;
	dsc.p2.y = (lv_value_precise_t)y2;
	dsc.width = (int32_t)lroundf(w < 1.0f ? 1.0f : w);
	dsc.color = color;
	dsc.round_start = 1;
	dsc.round_end = 1;
	lv_draw_line(layer, &dsc);
}

// An arc of the circle (x, y, r), angles in degrees from three o'clock,
// clockwise: 20..160 is a smile, 200..340 a frown.
static void arc(lv_layer_t *layer, float x, float y, float r, int from, int to, float w, lv_color_t color) {
	lv_draw_arc_dsc_t dsc;
	lv_draw_arc_dsc_init(&dsc);
	dsc.color = color;
	dsc.center.x = (int32_t)lroundf(x);
	dsc.center.y = (int32_t)lroundf(y);
	dsc.radius = (uint16_t)lroundf(r);
	dsc.width = (int32_t)lroundf(w < 1.0f ? 1.0f : w);
	dsc.start_angle = from;
	dsc.end_angle = to;
	dsc.rounded = 1;
	lv_draw_arc(layer, &dsc);
}

static void text(lv_layer_t *layer, const char *s, float x, float y, const lv_font_t *font, lv_color_t color) {
	lv_draw_label_dsc_t dsc;
	lv_draw_label_dsc_init(&dsc);
	dsc.text = s;
	dsc.font = font;
	dsc.color = color;
	lv_area_t a = {(int32_t)x, (int32_t)y, (int32_t)x + 80, (int32_t)y + 40};
	lv_draw_label(layer, &dsc, &a);
}

// The note on its head, which grows with it: none for a baby, a quarter note
// for a child, an eighth for a teen, two beamed for a well-raised adult and a
// bent one for a rough one.
static void tuft(lv_layer_t *layer, const pet_t *p, float cx, float top, float s, lv_color_t ink) {
	if (p->stage < PET_CHILD || p->stage == PET_DEAD) {
		return;
	}
	float w = s * 0.045f, h = s * 0.42f;
	float x = cx + s * 0.06f;
	if (p->stage == PET_ADULT_ROUGH) {
		line(layer, x, top + s * 0.04f, x + s * 0.06f, top - h * 0.55f, w, ink);
		line(layer, x + s * 0.06f, top - h * 0.55f, x - s * 0.02f, top - h, w, ink);
		oval(layer, x - s * 0.06f, top - h, s * 0.085f, s * 0.065f, ink, LV_OPA_COVER);
		return;
	}
	line(layer, x, top + s * 0.04f, x, top - h, w, ink);
	oval(layer, x - s * 0.07f, top - h + s * 0.02f, s * 0.085f, s * 0.065f, ink, LV_OPA_COVER);
	if (p->stage == PET_TEEN) {
		line(layer, x, top + s * 0.04f, x + s * 0.13f, top - s * 0.08f, w, ink);
	} else if (p->stage == PET_ADULT_GOOD) {
		float x2 = x + s * 0.2f;
		line(layer, x2, top - s * 0.02f, x2, top - h - s * 0.04f, w, ink);
		oval(layer, x2 - s * 0.07f, top - h - s * 0.02f, s * 0.085f, s * 0.065f, ink, LV_OPA_COVER);
		line(layer, x, top + s * 0.04f, x2, top - s * 0.02f, w * 1.6f, ink);
	}
}

static void sprite_draw_cb(lv_event_t *e) {
	lv_layer_t *layer = lv_event_get_layer(e);
	if (!layer) {
		return;
	}
	const pet_t *p = pet_get();
	lv_area_t area;
	lv_obj_get_coords(lv_event_get_current_target(e), &area);
	float w = (float)lv_area_get_width(&area), h = (float)lv_area_get_height(&area);
	float cx = (float)area.x1 + w / 2.0f;
	float ground = (float)area.y2 - h * 0.08f;
	float phase = (float)frame * 0.35f;
	bool moving = !p->asleep && p->stage != PET_DEAD && !p->sick;
	float bob = moving ? sinf(phase) * h * 0.015f : 0.0f;

	lv_color_t ink = lv_color_make(40, 32, 48);
	lv_color_t white = lv_color_white();
	lv_color_t body = lv_color_hsv_to_rgb((uint16_t)p->hue, 50, 95);
	lv_color_t belly = lv_color_hsv_to_rgb((uint16_t)p->hue, 22, 100);
	if (p->sick) {
		body = lv_color_mix(lv_color_make(150, 190, 110), body, 150);
		belly = lv_color_mix(lv_color_make(200, 220, 170), belly, 150);
	}

	// the ground's shadow
	oval(layer, cx, ground, w * 0.2f, h * 0.025f, lv_color_black(), LV_OPA_20);

	if (p->stage == PET_EGG) {
		// Wobbles more the nearer it is to hatching.
		float shake = p->songs ? sinf(phase * 2.0f) * (float)p->songs * w * 0.008f : 0.0f;
		float ex = cx + shake, ery = h * 0.27f, erx = h * 0.21f, ey = ground - ery;
		oval(layer, ex, ey, erx, ery, lv_color_make(248, 244, 232), LV_OPA_COVER);
		lv_color_t spot = lv_color_hsv_to_rgb((uint16_t)p->hue, 45, 90);
		disc(layer, ex - erx * 0.35f, ey - ery * 0.25f, erx * 0.2f, spot, LV_OPA_COVER);
		disc(layer, ex + erx * 0.4f, ey + ery * 0.1f, erx * 0.15f, spot, LV_OPA_COVER);
		disc(layer, ex - erx * 0.1f, ey + ery * 0.5f, erx * 0.12f, spot, LV_OPA_COVER);
		return;
	}

	float s = h * (p->stage == PET_BABY ? 0.36f : p->stage == PET_CHILD ? 0.44f : p->stage == PET_TEEN ? 0.5f : 0.56f);
	float r = s * 0.5f;
	float by = ground - r + bob;
	if (p->stage == PET_DEAD) {
		by -= h * 0.08f + sinf(phase * 0.5f) * h * 0.02f; // a ghost floats
		body = lv_color_make(235, 238, 245);
		belly = white;
	}

	// arms, from teen on
	if (p->stage >= PET_TEEN && p->stage != PET_DEAD) {
		float sway = moving ? sinf(phase) * r * 0.08f : 0.0f;
		disc(layer, cx - r * 0.98f, by + r * 0.25f + sway, r * 0.2f, body, LV_OPA_COVER);
		disc(layer, cx + r * 0.98f, by + r * 0.25f - sway, r * 0.2f, body, LV_OPA_COVER);
	}
	lv_opa_t body_opa = p->stage == PET_DEAD ? LV_OPA_70 : LV_OPA_COVER;
	disc(layer, cx, by, r, body, body_opa);
	oval(layer, cx, by + r * 0.35f, r * 0.62f, r * 0.45f, belly, body_opa);
	tuft(layer, p, cx, by - r * 0.9f, s, ink);
	if (p->stage == PET_DEAD) {
		arc(layer, cx, by - r * 1.25f, r * 0.45f, 0, 360, r * 0.08f, lv_color_hex(HALO_COLOR));
	}

	// the face
	float eye_y = by - r * 0.12f, eye_dx = r * 0.36f, eye_r = r * 0.12f;
	bool blink = moving && frame % 38 < 2;
	if (p->stage == PET_DEAD || p->sick) {
		for (int side = -1; side <= 1; side += 2) {
			float ex = cx + (float)side * eye_dx, d = eye_r * 0.8f;
			line(layer, ex - d, eye_y - d, ex + d, eye_y + d, r * 0.06f, ink);
			line(layer, ex - d, eye_y + d, ex + d, eye_y - d, r * 0.06f, ink);
		}
	} else if (p->asleep || blink) {
		for (int side = -1; side <= 1; side += 2) {
			float ex = cx + (float)side * eye_dx;
			arc(layer, ex, eye_y - eye_r * 0.4f, eye_r, 30, 150, r * 0.05f, ink);
		}
	} else {
		for (int side = -1; side <= 1; side += 2) {
			float ex = cx + (float)side * eye_dx;
			disc(layer, ex, eye_y, eye_r, ink, LV_OPA_COVER);
			disc(layer, ex + eye_r * 0.35f, eye_y - eye_r * 0.35f, eye_r * 0.35f, white, LV_OPA_COVER);
		}
	}
	float mouth_y = by + r * 0.18f;
	if (p->stage == PET_DEAD || p->sick || p->happy <= 1) {
		arc(layer, cx, mouth_y + r * 0.2f, r * 0.16f, 205, 335, r * 0.05f, ink); // frown
	} else if (p->hunger <= 1 && !p->asleep) {
		oval(layer, cx, mouth_y + r * 0.04f, r * 0.1f, r * 0.12f, ink, LV_OPA_COVER); // agape: feed me
	} else if (!p->asleep) {
		arc(layer, cx, mouth_y - r * 0.06f, r * 0.18f, 25, 155, r * 0.05f, ink); // smile
	}
	if (!p->sick && p->stage != PET_DEAD && p->happy >= 3) {
		lv_color_t blush = lv_color_make(255, 140, 160);
		disc(layer, cx - r * 0.58f, by + r * 0.1f, r * 0.1f, blush, LV_OPA_50);
		disc(layer, cx + r * 0.58f, by + r * 0.1f, r * 0.1f, blush, LV_OPA_50);
	}
	if (p->asleep && p->stage != PET_DEAD) {
		float drift = (float)(frame % 24) * h * 0.003f;
		text(layer, "z", cx + r * 0.9f, by - r * 1.1f - drift, &font_ui_22_bold, theme()->text_secondary);
		text(layer, "Z", cx + r * 1.2f, by - r * 1.55f - drift, &font_ui_28, theme()->text_secondary);
	}
	if (p->sick) {
		disc(layer, cx + r * 0.72f, by - r * 0.55f, r * 0.1f, lv_color_make(120, 190, 240), LV_OPA_COVER); // a sweat drop
	}

	// the mess, beside it
	for (int i = 0; i < p->poop; i++) {
		float px = cx + (i % 2 ? 1.0f : -1.0f) * w * (0.32f + 0.08f * (float)(i / 2)), py = ground;
		lv_color_t c = lv_color_hex(POOP_COLOR);
		oval(layer, px, py - h * 0.03f, w * 0.05f, h * 0.03f, c, LV_OPA_COVER);
		oval(layer, px, py - h * 0.065f, w * 0.036f, h * 0.025f, c, LV_OPA_COVER);
		disc(layer, px + w * 0.005f, py - h * 0.095f, w * 0.018f, c, LV_OPA_COVER);
	}
}

// A row of hearts for one meter: `value` of PET_HEARTS filled.
static void hearts_draw_cb(lv_event_t *e) {
	lv_layer_t *layer = lv_event_get_layer(e);
	const int *value = lv_event_get_user_data(e);
	if (!layer || !value) {
		return;
	}
	lv_area_t area;
	lv_obj_get_coords(lv_event_get_current_target(e), &area);
	float h = (float)lv_area_get_height(&area), step = h * 1.25f;
	for (int i = 0; i < PET_HEARTS; i++) {
		float x = (float)area.x1 + step * (float)i + h / 2.0f, y = (float)area.y1 + h * 0.42f, r = h * 0.26f;
		bool full = i < *value;
		lv_color_t c = full ? lv_color_hex(HEART_COLOR) : theme()->surface_pressed;
		disc(layer, x - r * 0.75f, y, r, c, LV_OPA_COVER);
		disc(layer, x + r * 0.75f, y, r, c, LV_OPA_COVER);
		lv_draw_triangle_dsc_t t;
		lv_draw_triangle_dsc_init(&t);
		t.color = c;
		t.opa = LV_OPA_COVER;
		t.p[0].x = (lv_value_precise_t)(x - r * 1.7f), t.p[0].y = (lv_value_precise_t)(y + r * 0.25f);
		t.p[1].x = (lv_value_precise_t)(x + r * 1.7f), t.p[1].y = (lv_value_precise_t)(y + r * 0.25f);
		t.p[2].x = (lv_value_precise_t)x, t.p[2].y = (lv_value_precise_t)(y + r * 2.1f);
		lv_draw_triangle(layer, &t);
	}
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void enable(lv_obj_t *btn, bool on) {
	lv_obj_set_hidden(btn, false);
	lv_obj_set_style_opa(btn, on ? LV_OPA_COVER : LV_OPA_40, 0);
	if (on) {
		lv_obj_remove_state(btn, LV_STATE_DISABLED);
	} else {
		lv_obj_add_state(btn, LV_STATE_DISABLED);
	}
}

static void show(void) {
	const pet_t *p = pet_get();
	time_t now = time(NULL);
	static const char *const STAGES[] = {"pet_stage_egg",  "pet_stage_baby",  "pet_stage_child", "pet_stage_teen",
										 "pet_stage_good", "pet_stage_rough", "pet_stage_dead"};
	bool alive = p->stage != PET_EGG && p->stage != PET_DEAD;

	lv_label_set_text(name_label, p->stage == PET_EGG ? tr("pet_stage_egg") : p->name);
	if (p->stage == PET_EGG) {
		lv_label_set_text_fmt(stage_label, tr("pet_life_n"), p->lives);
	} else {
		char age[48];
		snprintf(age, sizeof(age), tr("pet_age"), pet_age_days(p, now));
		lv_label_set_text_fmt(stage_label, "%s  -  %s", tr(STAGES[p->stage]), age);
	}

	if (p->stage == PET_EGG) {
		lv_label_set_text_fmt(hint_label, tr("pet_egg_hint"), PET_HATCH_SONGS - p->songs);
	} else if (p->stage == PET_DEAD) {
		lv_label_set_text_fmt(hint_label, tr("pet_rip"), p->name, pet_age_days(p, now));
	} else if (p->sick) {
		lv_label_set_text_fmt(hint_label, tr("pet_news_sick"), p->name);
	} else if (p->asleep) {
		lv_label_set_text(hint_label, tr("pet_sleeping"));
	} else {
		lv_label_set_text(hint_label, "");
	}

	lv_obj_set_hidden(meters, !alive);
	hunger_shown = p->hunger;
	happy_shown = p->happy;
	lv_obj_invalidate(hunger_hearts);
	lv_obj_invalidate(happy_hearts);

	if (p->favorite[0] && p->stage != PET_EGG) {
		lv_label_set_text_fmt(favorite_label, tr("pet_favorite"), p->favorite);
	} else {
		lv_label_set_text(favorite_label, "");
	}
	if (p->last_meal[0]) {
		lv_label_set_text_fmt(meal_label, tr("pet_last_meal"), p->last_meal);
	} else {
		lv_label_set_text(meal_label, "");
	}

	lv_obj_set_hidden(egg_btn, p->stage != PET_DEAD);
	lv_obj_set_hidden(pat_btn, !alive);
	lv_obj_set_hidden(clean_btn, !alive);
	lv_obj_set_hidden(medicine_btn, !alive);
	if (alive) {
		enable(pat_btn, !p->asleep);
		enable(clean_btn, p->poop > 0);
		enable(medicine_btn, p->sick);
	}
	lv_obj_invalidate(sprite);
}

// News from the pet, as a toast: it hatched, grew, got hungry or ill, or died.
static void tell_news(void) {
	const pet_t *p = pet_get();
	const char *key = pet_take_news();
	if (key) {
		char text[128];
		snprintf(text, sizeof(text), tr(key), p->name);
		toast_plain(text);
	}
}

static void news_cb(lv_timer_t *t) {
	(void)t;
	tell_news();
}

static void frame_cb(lv_timer_t *t) {
	(void)t;
	frame++;
	lv_obj_invalidate(sprite);
	if (frame % 40 == 0) {
		show(); // the meters and the words, now and then
		tell_news();
	}
}

static void pat_cb(lv_event_t *e) {
	(void)e;
	if (pet_pat()) {
		frame = 0;
	}
	show();
}

static void clean_cb(lv_event_t *e) {
	(void)e;
	pet_clean();
	show();
}

static void medicine_cb(lv_event_t *e) {
	(void)e;
	pet_medicine();
	show();
}

static void egg_cb(lv_event_t *e) {
	(void)e;
	pet_new_egg();
	show();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	show();
	tell_news();
	lv_timer_resume(frame_timer);
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(frame_timer);
}

static lv_obj_t *plain(lv_obj_t *parent) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_set_style_bg_opa(o, 0, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_set_scrollable(o, false);
	return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, bool dim) {
	lv_obj_t *l = lv_label_create(parent);
	lv_obj_add_style(l, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(l, font, 0);
	lv_label_set_text(l, "");
	return l;
}

static lv_obj_t *meter(lv_obj_t *parent, const char *key, int *value) {
	lv_obj_t *row = plain(parent);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 14, 0);
	lv_obj_t *name = label(row, &font_ui_20, true);
	lv_label_set_text(name, tr(key));
	lv_obj_set_width(name, 110);
	lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_t *hearts = plain(row);
	lv_obj_set_size(hearts, (int32_t)(28 * 1.25f * PET_HEARTS), 28);
	lv_obj_set_clickable(hearts, false);
	lv_obj_add_event_cb(hearts, hearts_draw_cb, LV_EVENT_DRAW_MAIN, value);
	return hearts;
}

static lv_obj_t *button(lv_obj_t *parent, const char *key, lv_event_cb_t cb, bool main) {
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_set_size(b, LV_SIZE_CONTENT, 56);
	lv_obj_set_style_pad_hor(b, 22, 0);
	lv_obj_set_style_radius(b, 28, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	if (main) {
		lv_obj_add_style(b, &theme_style_accent_bg, 0);
	} else {
		lv_obj_set_style_bg_color(b, theme()->surface_pressed, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	}
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text(l, tr(key));
	lv_obj_set_style_text_font(l, &font_ui_20, 0);
	lv_obj_set_style_text_color(l, main ? lv_color_white() : theme()->text_primary, 0);
	lv_obj_center(l);
	return b;
}

void petpage_init(gui_config_t *cfg) {
	pet_init();

	lv_obj_add_style(petpage_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(petpage_screen, false);
	settingsrow_title(petpage_screen, cfg, "pet");

	int top = settingsrow_content_top(cfg);
	int32_t width = (int32_t)cfg->screen_width - 2 * cfg->padding;
	lv_obj_t *page = plain(petpage_screen);
	lv_obj_set_pos(page, cfg->padding, top);
	lv_obj_set_size(page, width, (int32_t)cfg->screen_height - top);
	lv_obj_set_scrollable(page, true);
	lv_obj_set_scroll_dir(page, LV_DIR_VER);
	lv_obj_set_style_pad_bottom(page, cfg->padding, 0);
	lv_obj_set_style_pad_row(page, 10, 0);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	name_label = label(page, &font_ui_36_bold, false);
	stage_label = label(page, &font_ui_18, true);

	sprite = plain(page);
	lv_obj_set_size(sprite, width, (int32_t)cfg->screen_height * 2 / 5);
	lv_obj_set_clickable(sprite, true); // a tap is a pat
	lv_obj_add_event_cb(sprite, sprite_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(sprite, pat_cb, LV_EVENT_CLICKED, NULL);

	hint_label = label(page, &font_ui_20, true);
	lv_obj_set_width(hint_label, LV_PCT(100));
	lv_obj_set_style_text_align(hint_label, LV_TEXT_ALIGN_CENTER, 0);

	meters = plain(page);
	lv_obj_set_size(meters, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(meters, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(meters, 8, 0);
	hunger_hearts = meter(meters, "pet_hunger", &hunger_shown);
	happy_hearts = meter(meters, "pet_happy", &happy_shown);

	lv_obj_t *buttons = plain(page);
	lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(buttons, 10, 0);
	lv_obj_set_style_pad_top(buttons, 6, 0);
	pat_btn = button(buttons, "pet_pat", pat_cb, true);
	clean_btn = button(buttons, "pet_clean", clean_cb, false);
	medicine_btn = button(buttons, "pet_medicine", medicine_cb, false);
	egg_btn = button(buttons, "pet_new_egg", egg_cb, true);

	favorite_label = label(page, &font_ui_18, true);
	meal_label = label(page, &font_ui_18, true);
	lv_obj_set_width(meal_label, LV_PCT(100));
	lv_obj_set_style_text_align(meal_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(meal_label, LV_LABEL_LONG_DOT);

	frame_timer = lv_timer_create(frame_cb, FRAME_MS, NULL);
	lv_timer_pause(frame_timer);
	// Looked in on once a minute with the page closed too, so a pet falling ill
	// says so; not while the panel is dark, when nobody would see the toast.
	news_timer = lv_timer_create(news_cb, NEWS_MS, NULL);
	power_pause_in_standby(news_timer);

	lv_obj_add_event_cb(petpage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(petpage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
