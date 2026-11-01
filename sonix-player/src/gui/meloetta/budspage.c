#include "budspage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"
#include "src/system/meloetta/soundcore.h"

lv_obj_t *budspage_screen;

#define POLL_MS 500
#define SEGMENTS 5
#define SEG_W 18
#define SEG_H 30
#define COLOR_GOOD lv_color_make(60, 190, 90)
#define COLOR_LOW lv_color_make(220, 60, 50)

// ---------------------------------------------------------------------------
// pills: a row of buttons where the lit one is the current state
// ---------------------------------------------------------------------------

#define MAX_PILLS 20
static lv_obj_t *pills[MAX_PILLS];
static int pill_count;

typedef void (*pick_fn)(int value);

typedef struct {
	lv_obj_t *buttons[5];
	int count;
	pick_fn pick;
} group_t;

static group_t mode_group, nc_group, level_group, transp_group;

static void set_lit(lv_obj_t *b, bool lit) {
	if (lit) {
		lv_obj_add_style(b, &theme_style_accent_bg, 0);
		lv_obj_remove_local_style_prop(b, LV_STYLE_BG_COLOR, 0);
	} else {
		lv_obj_remove_style(b, &theme_style_accent_bg, 0);
		lv_obj_set_style_bg_color(b, theme()->surface_pressed, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	}
	lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lit ? lv_color_white() : theme()->text_primary, 0);
	lv_obj_set_user_data(lv_obj_get_child(b, 0), (void *)(intptr_t)lit);
}

static void group_show(group_t *g, int selected) {
	for (int i = 0; i < g->count; i++) {
		set_lit(g->buttons[i], i == selected);
	}
}

static void pill_cb(lv_event_t *e) {
	group_t *g = lv_event_get_user_data(e);
	lv_obj_t *b = lv_event_get_target(e);
	for (int i = 0; i < g->count; i++) {
		if (g->buttons[i] == b) {
			group_show(g, i); // lit under the finger
			g->pick(i);
			return;
		}
	}
}

static lv_obj_t *plain(lv_obj_t *parent) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_set_style_bg_opa(o, 0, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_set_scrollable(o, false);
	return o;
}

static lv_obj_t *row(lv_obj_t *parent) {
	lv_obj_t *r = plain(parent);
	lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 8, 0);
	return r;
}

// `label` is a language key, or shown as is when `raw`.
static void pill(group_t *g, lv_obj_t *parent, const char *label, bool raw) {
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_set_height(b, 56);
	lv_obj_set_flex_grow(b, 1);
	lv_obj_set_style_pad_hor(b, 4, 0);
	lv_obj_set_style_radius(b, 28, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_add_event_cb(b, pill_cb, LV_EVENT_CLICKED, g);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text(l, raw ? label : tr(label));
	lv_obj_set_style_text_font(l, &font_ui_18, 0);
	lv_obj_center(l);
	g->buttons[g->count++] = b;
	if (pill_count < MAX_PILLS) {
		pills[pill_count++] = b;
	}
	set_lit(b, false);
}

// ---------------------------------------------------------------------------
// the rest of the page
// ---------------------------------------------------------------------------

static lv_obj_t *cards[4];
static int card_count;
static lv_obj_t *status_label;
static lv_obj_t *battery_card, *sound_card, *ldac_card;
static lv_obj_t *col_seg[3][SEGMENTS];
static lv_obj_t *col_value[3];
static lv_obj_t *col_name[3];
static lv_obj_t *nc_box, *level_box, *adaptive_label, *transp_box, *wind_row;
static lv_obj_t *wind_switch, *ldac_switch;
static lv_timer_t *poll_timer;
static uint32_t drawn_serial = 0xFFFFFFFFu;
static bool drawn_connected = true;

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, bool dim) {
	lv_obj_t *l = lv_label_create(parent);
	lv_label_set_text(l, "");
	lv_obj_add_style(l, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(l, font, 0);
	return l;
}

static lv_obj_t *card(lv_obj_t *parent) {
	lv_obj_t *c = plain(parent);
	cards[card_count++] = c;
	lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_bg_color(c, theme()->surface, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(c, 16, 0);
	lv_obj_set_style_pad_all(c, 14, 0);
	lv_obj_set_style_pad_row(c, 10, 0);
	return c;
}

static lv_obj_t *new_switch(lv_obj_t *parent, lv_event_cb_t cb) {
	lv_obj_t *sw = lv_switch_create(parent);
	lv_obj_add_style(sw, &theme_style_switch, 0);
	lv_obj_add_style(sw, &theme_style_switch_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
	return sw;
}

static void set_switch(lv_obj_t *sw, bool on) {
	if (on) {
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(sw, LV_STATE_CHECKED);
	}
}

static void draw_column(int i, int level, bool charging) {
	bool known = level >= 0;
	lv_color_t fill = level <= 1 ? COLOR_LOW : COLOR_GOOD;
	for (int s = 0; s < SEGMENTS; s++) {
		bool on = known && s < level;
		lv_obj_set_style_bg_color(col_seg[i][s], on ? fill : theme()->surface_pressed, 0);
	}
	char buf[24];
	if (!known) {
		snprintf(buf, sizeof(buf), "--");
	} else {
		// The buds count in fifths; the app shows the same steps as percent.
		snprintf(buf, sizeof(buf), "%d%%%s", level * 20, charging ? " +" : "");
	}
	lv_label_set_text(col_value[i], buf);
	lv_obj_set_style_text_color(col_value[i], charging ? theme()->accent : theme()->text_primary, 0);
	lv_obj_set_style_opa(col_name[i], known ? LV_OPA_COVER : LV_OPA_50, 0);
}

static void redraw(const soundcore_state_t *s) {
	char buf[160];
	bool ready = s->connected && s->have_state;
	if (!s->connected) {
		snprintf(buf, sizeof(buf), "%s", tr("buds_not_connected"));
	} else if (!s->have_state) {
		snprintf(buf, sizeof(buf), "%s", tr("buds_reading"));
	} else {
		snprintf(buf, sizeof(buf), "%s", s->name);
		if (s->firmware[0]) {
			size_t n = strlen(buf);
			snprintf(buf + n, sizeof(buf) - n, "  %s %s", tr("buds_firmware"), s->firmware);
		}
		if (!s->both_buds) {
			size_t n = strlen(buf);
			snprintf(buf + n, sizeof(buf) - n, "\n%s", tr("buds_one_bud"));
		}
	}
	lv_label_set_text(status_label, buf);

	lv_obj_set_hidden(battery_card, !ready);
	lv_obj_set_hidden(sound_card, !ready);
	lv_obj_set_hidden(ldac_card, !ready);
	if (!ready) {
		return;
	}

	draw_column(0, s->left, s->left_charging);
	draw_column(1, s->right, s->right_charging);
	draw_column(2, s->box, false);

	// Mode pills are laid out NC, Normal, Transparency.
	int mode_index = s->mode == SOUNDCORE_MODE_NC ? 0 : s->mode == SOUNDCORE_MODE_NORMAL ? 1 : 2;
	group_show(&mode_group, mode_index);

	bool nc = s->mode == SOUNDCORE_MODE_NC;
	lv_obj_set_hidden(nc_box, !nc);
	// NC kind pills: Adaptive, Manual, Transport.
	int nc_index = s->nc == SOUNDCORE_NC_ADAPTIVE ? 0 : s->nc == SOUNDCORE_NC_MANUAL ? 1 : 2;
	group_show(&nc_group, nc_index);
	lv_obj_set_hidden(level_box, !(nc && s->nc == SOUNDCORE_NC_MANUAL));
	group_show(&level_group, s->nc_level >= 1 && s->nc_level <= 5 ? s->nc_level - 1 : -1);
	bool adaptive = nc && s->nc == SOUNDCORE_NC_ADAPTIVE;
	lv_obj_set_hidden(adaptive_label, !adaptive);
	if (adaptive) {
		snprintf(buf, sizeof(buf), tr("buds_adaptive_now"), s->adaptive_level);
		lv_label_set_text(adaptive_label, buf);
	}

	bool transp = s->mode == SOUNDCORE_MODE_TRANSPARENCY;
	lv_obj_set_hidden(transp_box, !transp);
	group_show(&transp_group, s->transparency == SOUNDCORE_TRANSPARENCY_VOCAL ? 1 : 0);

	lv_obj_set_hidden(wind_row, !(nc || transp));
	set_switch(wind_switch, s->wind);

	lv_obj_set_hidden(ldac_switch, s->ldac < 0);
	set_switch(ldac_switch, s->ldac == 1);
}

static void refresh(bool force) {
	soundcore_state_t s;
	bool connected = soundcore_get(&s);
	if (!force && s.serial == drawn_serial && connected == drawn_connected) {
		return;
	}
	drawn_serial = s.serial;
	drawn_connected = connected;
	redraw(&s);
}

// ---------------------------------------------------------------------------
// taps
// ---------------------------------------------------------------------------

static void pick_mode(int i) {
	static const soundcore_mode_t modes[3] = {SOUNDCORE_MODE_NC, SOUNDCORE_MODE_NORMAL, SOUNDCORE_MODE_TRANSPARENCY};
	soundcore_set_mode(modes[i]);
	refresh(true);
}

static void pick_nc(int i) {
	static const soundcore_nc_t kinds[3] = {SOUNDCORE_NC_ADAPTIVE, SOUNDCORE_NC_MANUAL, SOUNDCORE_NC_TRANSPORT};
	soundcore_set_nc(kinds[i]);
	refresh(true);
}

static void pick_level(int i) {
	soundcore_set_nc_level(i + 1);
	refresh(true);
}

static void pick_transp(int i) {
	soundcore_set_transparency(i == 1 ? SOUNDCORE_TRANSPARENCY_VOCAL : SOUNDCORE_TRANSPARENCY_FULL);
	refresh(true);
}

static void wind_cb(lv_event_t *e) {
	soundcore_set_wind(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void ldac_cb(lv_event_t *e) {
	soundcore_set_ldac(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
}

static void status_clicked_cb(lv_event_t *e) {
	(void)e;
	soundcore_refresh();
}

static void poll_cb(lv_timer_t *t) {
	(void)t;
	refresh(false);
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	soundcore_refresh(); // the case battery only comes with the full state
	refresh(true);
	lv_timer_resume(poll_timer);
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll_timer);
}

static void refresh_theme(void) {
	for (int i = 0; i < card_count; i++) {
		lv_obj_set_style_bg_color(cards[i], theme()->surface, 0);
	}
	for (int i = 0; i < pill_count; i++) {
		bool lit = (bool)(intptr_t)lv_obj_get_user_data(lv_obj_get_child(pills[i], 0));
		set_lit(pills[i], lit);
	}
	refresh(true);
	lv_obj_invalidate(budspage_screen);
}

// ---------------------------------------------------------------------------
// building it
// ---------------------------------------------------------------------------

static void build_battery(lv_obj_t *page) {
	battery_card = card(page);
	lv_obj_t *r = row(battery_card);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	for (int i = 0; i < 3; i++) {
		lv_obj_t *col = plain(r);
		lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(col, 8, 0);
		col_name[i] = text(col, &font_ui_20, true);
		lv_obj_t *bar = plain(col);
		lv_obj_set_size(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
		lv_obj_set_style_pad_column(bar, 4, 0);
		for (int s = 0; s < SEGMENTS; s++) {
			lv_obj_t *seg = plain(bar);
			lv_obj_set_size(seg, SEG_W, SEG_H);
			lv_obj_set_style_radius(seg, 4, 0);
			lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
			lv_obj_set_style_bg_color(seg, theme()->surface_pressed, 0);
			col_seg[i][s] = seg;
		}
		col_value[i] = text(col, &font_ui_20, false);
	}
	lv_label_set_text(col_name[0], tr("buds_left"));
	lv_label_set_text(col_name[1], tr("buds_right"));
	lv_label_set_text(col_name[2], tr("buds_case"));
}

static lv_obj_t *sub_box(lv_obj_t *parent) {
	lv_obj_t *b = plain(parent);
	lv_obj_set_size(b, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(b, 10, 0);
	return b;
}

static void build_sound(lv_obj_t *page) {
	sound_card = card(page);
	lv_obj_t *title = text(sound_card, &font_ui_24_bold, false);
	lv_label_set_text(title, tr("buds_sound"));

	mode_group.pick = pick_mode;
	lv_obj_t *r = row(sound_card);
	pill(&mode_group, r, "buds_anc", false);
	pill(&mode_group, r, "buds_normal", false);
	pill(&mode_group, r, "buds_transparency", false);

	nc_box = sub_box(sound_card);
	nc_group.pick = pick_nc;
	r = row(nc_box);
	pill(&nc_group, r, "buds_adaptive", false);
	pill(&nc_group, r, "buds_manual", false);
	pill(&nc_group, r, "buds_transport", false);
	adaptive_label = text(nc_box, &font_ui_18, true);

	level_box = sub_box(nc_box);
	lv_obj_t *lt = text(level_box, &font_ui_20, true);
	lv_label_set_text(lt, tr("buds_strength"));
	level_group.pick = pick_level;
	r = row(level_box);
	pill(&level_group, r, "1", true);
	pill(&level_group, r, "2", true);
	pill(&level_group, r, "3", true);
	pill(&level_group, r, "4", true);
	pill(&level_group, r, "5", true);

	transp_box = sub_box(sound_card);
	transp_group.pick = pick_transp;
	r = row(transp_box);
	pill(&transp_group, r, "buds_full", false);
	pill(&transp_group, r, "buds_vocal", false);

	wind_row = row(sound_card);
	lv_obj_t *wl = text(wind_row, &font_ui_20, false);
	lv_label_set_text(wl, tr("buds_wind"));
	wind_switch = new_switch(wind_row, wind_cb);
}

static void build_ldac(lv_obj_t *page) {
	ldac_card = card(page);
	lv_obj_t *r = row(ldac_card);
	lv_obj_t *l = text(r, &font_ui_24_bold, false);
	lv_label_set_text(l, "LDAC");
	ldac_switch = new_switch(r, ldac_cb);
	lv_obj_t *note = text(ldac_card, &font_ui_18, true);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, LV_PCT(100));
	lv_label_set_text(note, tr("buds_ldac_note"));
}

void budspage_init(gui_config_t *cfg) {
	lv_obj_add_style(budspage_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(budspage_screen, false);
	settingsrow_title(budspage_screen, cfg, "buds");

	int top = settingsrow_content_top(cfg);
	int32_t width = (int32_t)cfg->screen_width - 2 * cfg->padding;
	lv_obj_t *page = plain(budspage_screen);
	lv_obj_set_pos(page, cfg->padding, top);
	lv_obj_set_size(page, width, (int32_t)cfg->screen_height - top);
	lv_obj_set_scrollable(page, true);
	lv_obj_set_scroll_dir(page, LV_DIR_VER);
	lv_obj_set_style_pad_bottom(page, cfg->padding, 0);
	lv_obj_set_style_pad_row(page, 14, 0);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	status_label = text(page, &font_ui_18, true);
	lv_obj_set_width(status_label, LV_PCT(100));
	lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_clickable(status_label, true);
	lv_obj_set_ext_click_area(status_label, 12);
	lv_obj_add_event_cb(status_label, status_clicked_cb, LV_EVENT_CLICKED, NULL);

	build_battery(page);
	build_sound(page);
	build_ldac(page);

	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	theme_register_refresh(refresh_theme);
	lv_obj_add_event_cb(budspage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(budspage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	refresh(true);
}
