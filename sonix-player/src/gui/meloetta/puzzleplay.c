#include "puzzleplay.h"

#include <libgen.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/puzzles/puzzles.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"

lv_obj_t *puzzleplay_screen;

// The curated list, in menu order.
#define GAME(x) &x,
static const game *const GAMES[] = {
#include "src/puzzles/generated-games.h"
};
#undef GAME
#define GAME_COUNT ((int)(sizeof(GAMES) / sizeof(GAMES[0])))

#define TIMER_MS 20
#define BUTTON_H 56
#define MAX_KEYS 12

// ---------------------------------------------------------------------------
// The frontend Tatham's mid-end talks to
// ---------------------------------------------------------------------------

struct frontend {
	midend *me;
	int game;
	// the board's pixels
	uint16_t *px;
	int w, h;
	int clip_x0, clip_y0, clip_x1, clip_y1; // clip, x1/y1 exclusive
	uint16_t *palette;
	int ncolours;
	bool dirty;
	// input
	bool mark;		 // taps send the right button
	int pressed;	 // the button a press sent, 0 for none
	int last_x, last_y;
	// timer
	lv_timer_t *timer;
	uint32_t timer_last;
	bool timer_on;
	bool solved;
};

static struct frontend fe_state;
static struct frontend *fe = &fe_state;

static gui_config_t *g_cfg;
static lv_obj_t *title_label;
static lv_obj_t *status_label;
static lv_obj_t *board_area; // where the board may go
static lv_obj_t *canvas;
static lv_obj_t *keys_row;
static lv_obj_t *mark_btn;
static lv_obj_t *undo_btn, *redo_btn;
static lv_obj_t *presets_panel;
static int area_w, area_h;

struct blitter {
	int w, h;
	int x, y; // where it was saved from
	uint16_t *px;
};

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

// --- what Tatham's code expects every front end to provide -----------------

void fatal(const char *fmt, ...) {
	va_list ap;
	fprintf(stderr, "puzzles: fatal: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	abort();
}

// The board colour the games derive their palette from. They are designed on
// light grey; dark_mode() below flips the whole palette afterwards.
void frontend_default_colour(frontend *f, float *output) {
	(void)f;
	output[0] = output[1] = output[2] = 0.85f;
}

void activate_timer(frontend *f) {
	if (!f->timer_on) {
		f->timer_on = true;
		f->timer_last = now_ms();
		lv_timer_resume(f->timer);
	}
}

void deactivate_timer(frontend *f) {
	f->timer_on = false;
	lv_timer_pause(f->timer);
}

void get_random_seed(void **randseed, int *randseedsize) {
	struct {
		struct timespec ts;
		long r;
	} *s = malloc(sizeof(*s));
	clock_gettime(CLOCK_REALTIME, &s->ts);
	s->r = random();
	*randseed = s;
	*randseedsize = (int)sizeof(*s);
}

// --- colours ----------------------------------------------------------------

static uint16_t rgb565(float r, float g, float b) {
	int R = (int)lrintf(r * 31), G = (int)lrintf(g * 63), B = (int)lrintf(b * 31);
	R = R < 0 ? 0 : R > 31 ? 31 : R;
	G = G < 0 ? 0 : G > 63 ? 63 : G;
	B = B < 0 ? 0 : B > 31 ? 31 : B;
	return (uint16_t)((R << 11) | (G << 5) | B);
}

// Lightness flipped, hue and saturation kept: a light board with dark ink
// becomes a dark board with light ink, and Mines' dark blue 1 a light blue.
static void dark_mode(float *r, float *g, float *b) {
	float mx = fmaxf(*r, fmaxf(*g, *b)), mn = fminf(*r, fminf(*g, *b));
	float l = (mx + mn) / 2, nl = 1.0f - l;
	// keep the board a soft dark grey rather than black
	nl = 0.08f + nl * 0.84f;
	if (mx - mn < 1e-4f) {
		*r = *g = *b = nl;
		return;
	}
	// scale each channel's distance from the old lightness to the new
	float s_old = l < 0.5f ? (mx - mn) / (mx + mn) : (mx - mn) / (2 - mx - mn);
	float c_new = (1 - fabsf(2 * nl - 1)) * s_old;
	float c_old = mx - mn;
	float k = c_old > 0 ? c_new / c_old : 0;
	*r = nl + (*r - l) * k;
	*g = nl + (*g - l) * k;
	*b = nl + (*b - l) * k;
}

static void load_palette(void) {
	free(fe->palette);
	int n = 0;
	float *c = midend_colours(fe->me, &n);
	fe->palette = malloc(sizeof(uint16_t) * (size_t)(n > 0 ? n : 1));
	for (int i = 0; i < n; i++) {
		float r = c[3 * i], g = c[3 * i + 1], b = c[3 * i + 2];
		dark_mode(&r, &g, &b);
		fe->palette[i] = rgb565(r, g, b);
	}
	fe->ncolours = n;
	sfree(c);
}

static uint16_t colour(int i) { return i >= 0 && i < fe->ncolours ? fe->palette[i] : 0; }

// --- the rasteriser ---------------------------------------------------------

static inline void plot(int x, int y, uint16_t c) {
	if (x >= fe->clip_x0 && x < fe->clip_x1 && y >= fe->clip_y0 && y < fe->clip_y1) {
		fe->px[y * fe->w + x] = c;
	}
}

static void hline(int x0, int x1, int y, uint16_t c) {
	if (y < fe->clip_y0 || y >= fe->clip_y1) {
		return;
	}
	if (x0 > x1) {
		int t = x0;
		x0 = x1;
		x1 = t;
	}
	if (x0 < fe->clip_x0) x0 = fe->clip_x0;
	if (x1 >= fe->clip_x1) x1 = fe->clip_x1 - 1;
	uint16_t *row = fe->px + y * fe->w;
	for (int x = x0; x <= x1; x++) {
		row[x] = c;
	}
}

static void fe_draw_rect(drawing *dr, int x, int y, int w, int h, int col) {
	(void)dr;
	uint16_t c = colour(col);
	for (int yy = y; yy < y + h; yy++) {
		hline(x, x + w - 1, yy, c);
	}
}

static void fe_draw_line(drawing *dr, int x1, int y1, int x2, int y2, int col) {
	(void)dr;
	uint16_t c = colour(col);
	int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
	int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
	int err = dx + dy;
	for (;;) {
		plot(x1, y1, c);
		if (x1 == x2 && y1 == y2) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x1 += sx; }
		if (e2 <= dx) { err += dx; y1 += sy; }
	}
}

static void fe_draw_circle(drawing *dr, int cx, int cy, int r, int fill, int outline) {
	(void)dr;
	if (fill >= 0) {
		uint16_t c = colour(fill);
		for (int dy = -r; dy <= r; dy++) {
			int dx = (int)floor(sqrt((double)r * r - (double)dy * dy) + 0.5);
			hline(cx - dx, cx + dx, cy + dy, c);
		}
	}
	uint16_t c = colour(outline);
	int x = r, y = 0, err = 1 - r;
	while (x >= y) {
		plot(cx + x, cy + y, c); plot(cx + y, cy + x, c);
		plot(cx - y, cy + x, c); plot(cx - x, cy + y, c);
		plot(cx - x, cy - y, c); plot(cx - y, cy - x, c);
		plot(cx + y, cy - x, c); plot(cx + x, cy - y, c);
		y++;
		if (err < 0) {
			err += 2 * y + 1;
		} else {
			x--;
			err += 2 * (y - x) + 1;
		}
	}
}

// The fonts this firmware carries, by pixel size. Tatham asks for a size in
// pixels; the nearest one is used.
static const lv_font_t *font_for(int size, bool bold) {
	static const struct {
		int px;
		const lv_font_t *font;
		bool bold;
	} FONTS[] = {
		{14, &font_ui_14, false}, {16, &font_ui_16, false}, {18, &font_ui_18, false}, {20, &font_ui_20, false},
		{22, &font_ui_22, false}, {24, &font_ui_24, false}, {26, &font_ui_26, false}, {28, &font_ui_28, false},
		{32, &font_ui_32, false}, {36, &font_ui_36_bold, true}, {64, &font_ui_64_bold, true},
		{20, &font_ui_20_bold, true}, {22, &font_ui_22_bold, true}, {24, &font_ui_24_bold, true},
	};
	const lv_font_t *best = &font_ui_20;
	int best_d = 1 << 30;
	for (size_t i = 0; i < sizeof(FONTS) / sizeof(FONTS[0]); i++) {
		int d = abs(FONTS[i].px - size) * 2 + (FONTS[i].bold != bold ? 1 : 0);
		if (d < best_d) {
			best_d = d;
			best = FONTS[i].font;
		}
	}
	return best;
}

static void fe_draw_text(drawing *dr, int x, int y, int fonttype, int fontsize, int align, int col, const char *text) {
	(void)dr;
	// Tatham's fontsize is the height of the em; the board's digits want to be
	// bold and readable, so fixed-width text (Solo, Mines numbers) goes bold.
	const lv_font_t *font = font_for(fontsize, fonttype == FONT_FIXED);
	lv_point_t size;
	lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	int left = x;
	if (align & ALIGN_HCENTRE) {
		left = x - size.x / 2;
	} else if (align & ALIGN_HRIGHT) {
		left = x - size.x;
	}
	int top;
	if (align & ALIGN_VCENTRE) {
		// the middle of the digits, not of the line: digits sit on the baseline
		int ascent = font->line_height - font->base_line;
		top = y - ascent + (ascent * 4 / 10);
	} else {
		top = y - (font->line_height - font->base_line);
	}

	lv_layer_t layer;
	lv_canvas_init_layer(canvas, &layer);
	lv_draw_label_dsc_t dsc;
	lv_draw_label_dsc_init(&dsc);
	dsc.text = text;
	dsc.font = font;
	uint16_t c = colour(col);
	dsc.color = lv_color_make((uint8_t)(((c >> 11) & 31) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
							  (uint8_t)((c & 31) * 255 / 31));
	lv_area_t area = {left, top, left + size.x, top + size.y};
	lv_draw_label(&layer, &dsc, &area);
	lv_canvas_finish_layer(canvas, &layer);
}

static void fe_draw_update(drawing *dr, int x, int y, int w, int h) {
	(void)dr; (void)x; (void)y; (void)w; (void)h;
	fe->dirty = true;
}

static void fe_clip(drawing *dr, int x, int y, int w, int h) {
	(void)dr;
	fe->clip_x0 = x < 0 ? 0 : x;
	fe->clip_y0 = y < 0 ? 0 : y;
	fe->clip_x1 = x + w > fe->w ? fe->w : x + w;
	fe->clip_y1 = y + h > fe->h ? fe->h : y + h;
}

static void fe_unclip(drawing *dr) {
	(void)dr;
	fe->clip_x0 = fe->clip_y0 = 0;
	fe->clip_x1 = fe->w;
	fe->clip_y1 = fe->h;
}

static void fe_start_draw(drawing *dr) { (void)dr; }

static void fe_end_draw(drawing *dr) {
	(void)dr;
	if (fe->dirty && canvas) {
		lv_obj_invalidate(canvas);
	}
	fe->dirty = false;
}

static void fe_status_bar(drawing *dr, const char *text) {
	(void)dr;
	if (status_label) {
		lv_label_set_text(status_label, text);
	}
}

static blitter *fe_blitter_new(drawing *dr, int w, int h) {
	(void)dr;
	blitter *bl = malloc(sizeof(*bl));
	bl->w = w;
	bl->h = h;
	bl->x = bl->y = 0;
	bl->px = malloc(sizeof(uint16_t) * (size_t)w * (size_t)h);
	return bl;
}

static void fe_blitter_free(drawing *dr, blitter *bl) {
	(void)dr;
	free(bl->px);
	free(bl);
}

static void fe_blitter_save(drawing *dr, blitter *bl, int x, int y) {
	(void)dr;
	bl->x = x;
	bl->y = y;
	for (int j = 0; j < bl->h; j++) {
		for (int i = 0; i < bl->w; i++) {
			int sx = x + i, sy = y + j;
			bl->px[j * bl->w + i] = sx >= 0 && sx < fe->w && sy >= 0 && sy < fe->h ? fe->px[sy * fe->w + sx] : 0;
		}
	}
}

static void fe_blitter_load(drawing *dr, blitter *bl, int x, int y) {
	(void)dr;
	for (int j = 0; j < bl->h; j++) {
		for (int i = 0; i < bl->w; i++) {
			plot(x + i, y + j, bl->px[j * bl->w + i]);
		}
	}
	fe->dirty = true;
}

// Symbols the fonts here may not have (×, ✓ and the like): the first plain
// ASCII choice, else the last.
static char *fe_text_fallback(drawing *dr, const char *const *strings, int nstrings) {
	(void)dr;
	for (int i = 0; i < nstrings; i++) {
		bool ascii = true;
		for (const unsigned char *p = (const unsigned char *)strings[i]; *p; p++) {
			if (*p >= 0x80) {
				ascii = false;
				break;
			}
		}
		if (ascii) {
			return dupstr(strings[i]);
		}
	}
	return dupstr(strings[nstrings - 1]);
}

static const drawing_api API = {
	.version = 1,
	.draw_text = fe_draw_text,
	.draw_rect = fe_draw_rect,
	.draw_line = fe_draw_line,
	.draw_polygon = draw_polygon_fallback,
	.draw_circle = fe_draw_circle,
	.draw_update = fe_draw_update,
	.clip = fe_clip,
	.unclip = fe_unclip,
	.start_draw = fe_start_draw,
	.end_draw = fe_end_draw,
	.status_bar = fe_status_bar,
	.blitter_new = fe_blitter_new,
	.blitter_free = fe_blitter_free,
	.blitter_save = fe_blitter_save,
	.blitter_load = fe_blitter_load,
	.text_fallback = fe_text_fallback,
	// printing and the thick line are left to drawing.c's fallbacks
};

// ---------------------------------------------------------------------------
// Saving the game in progress, next to the config
// ---------------------------------------------------------------------------

static void save_path(int game, char *out, size_t size) {
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", config_path());
	snprintf(out, size, "%s/puzzle-%s.sav", dirname(dir), GAMES[game]->htmlhelp_topic ? GAMES[game]->htmlhelp_topic : GAMES[game]->name);
}

static void write_cb(void *ctx, const void *buf, int len) { fwrite(buf, 1, (size_t)len, (FILE *)ctx); }

static bool read_cb(void *ctx, void *buf, int len) { return fread(buf, 1, (size_t)len, (FILE *)ctx) == (size_t)len; }

static void save_game(void) {
	if (!fe->me) {
		return;
	}
	char path[600], tmp[610];
	save_path(fe->game, path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		return;
	}
	midend_serialise(fe->me, write_cb, f);
	bool ok = fflush(f) == 0;
	ok = fclose(f) == 0 && ok;
	if (ok) {
		rename(tmp, path);
	} else {
		remove(tmp);
	}
}

static bool load_game(void) {
	char path[600];
	save_path(fe->game, path, sizeof(path));
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	const char *err = midend_deserialise(fe->me, read_cb, f);
	fclose(f);
	if (err) {
		printf("puzzles: %s: %s\n", path, err);
	}
	return err == NULL;
}

// ---------------------------------------------------------------------------
// Laying out the board
// ---------------------------------------------------------------------------

static void refresh_buttons(void);

static void layout_board(void) {
	int w = area_w, h = area_h;
	midend_size(fe->me, &w, &h, false, 1.0);
	if (w != fe->w || h != fe->h || !fe->px) {
		free(fe->px);
		fe->px = calloc((size_t)w * (size_t)h, sizeof(uint16_t));
		fe->w = w;
		fe->h = h;
		lv_canvas_set_buffer(canvas, fe->px, w, h, LV_COLOR_FORMAT_RGB565);
		lv_obj_set_size(canvas, w, h);
	}
	lv_obj_center(canvas);
	fe_unclip(NULL);
	load_palette();
	// the area around the board in the board's own background colour
	lv_obj_set_style_bg_color(board_area,
							  lv_color_make((uint8_t)(((colour(0) >> 11) & 31) * 255 / 31),
											(uint8_t)(((colour(0) >> 5) & 63) * 255 / 63),
											(uint8_t)((colour(0) & 31) * 255 / 31)),
							  0);
	midend_force_redraw(fe->me);
	lv_obj_invalidate(canvas);
}

// ---------------------------------------------------------------------------
// The game's own keys (Solo's digits)
// ---------------------------------------------------------------------------

static int key_buttons[MAX_KEYS];

static void key_cb(lv_event_t *e) {
	int button = (int)(intptr_t)lv_event_get_user_data(e);
	midend_process_key(fe->me, 0, 0, button);
	refresh_buttons();
}

static lv_obj_t *small_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user) {
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_set_height(b, BUTTON_H);
	lv_obj_set_flex_grow(b, 1);
	lv_obj_set_style_pad_hor(b, 2, 0);
	lv_obj_set_style_radius(b, 14, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_bg_color(b, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text(l, text);
	lv_obj_set_style_text_font(l, &font_ui_20, 0);
	lv_obj_set_style_text_color(l, theme()->text_primary, 0);
	lv_obj_center(l);
	return b;
}

static void build_keys(void) {
	lv_obj_clean(keys_row);
	int n = 0;
	key_label *keys = midend_request_keys(fe->me, &n);
	if (n > MAX_KEYS) {
		n = MAX_KEYS;
	}
	for (int i = 0; i < n; i++) {
		key_buttons[i] = keys[i].button;
		small_button(keys_row, keys[i].label, key_cb, (void *)(intptr_t)keys[i].button);
	}
	if (keys) {
		free_keys(keys, n);
	}
	lv_obj_set_hidden(keys_row, n == 0);
}

// ---------------------------------------------------------------------------
// Touch
// ---------------------------------------------------------------------------

static bool board_point(lv_event_t *e, int *x, int *y) {
	lv_indev_t *indev = lv_event_get_indev(e);
	if (!indev) {
		return false;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);
	lv_area_t a;
	lv_obj_get_coords(canvas, &a);
	*x = p.x - a.x1;
	*y = p.y - a.y1;
	return true;
}

static void after_move(void) {
	int status = midend_status(fe->me);
	if (status > 0 && !fe->solved) {
		fe->solved = true;
		gui_notify_popup("puzzles_solved");
	} else if (status <= 0) {
		fe->solved = false;
	}
	refresh_buttons();
}

static void board_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	int x, y;
	if (!board_point(e, &x, &y)) {
		return;
	}
	int base = fe->mark ? RIGHT_BUTTON : LEFT_BUTTON;
	if (code == LV_EVENT_PRESSED) {
		fe->pressed = base;
		fe->last_x = x;
		fe->last_y = y;
		midend_process_key(fe->me, x, y, base);
	} else if (code == LV_EVENT_PRESSING && fe->pressed) {
		if (x != fe->last_x || y != fe->last_y) {
			fe->last_x = x;
			fe->last_y = y;
			midend_process_key(fe->me, x, y, fe->pressed + (LEFT_DRAG - LEFT_BUTTON));
		}
	} else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && fe->pressed) {
		if (code == LV_EVENT_PRESS_LOST) {
			x = fe->last_x;
			y = fe->last_y;
		}
		midend_process_key(fe->me, x, y, fe->pressed + (LEFT_RELEASE - LEFT_BUTTON));
		fe->pressed = 0;
		after_move();
	}
}

// ---------------------------------------------------------------------------
// The tool bar
// ---------------------------------------------------------------------------

static void set_mark_look(void) {
	lv_obj_set_style_bg_color(mark_btn, fe->mark ? theme()->accent : theme()->surface_pressed, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(mark_btn, 0), fe->mark ? lv_color_white() : theme()->text_primary, 0);
}

static void refresh_buttons(void) {
	if (!fe->me) {
		return;
	}
	if (midend_can_undo(fe->me)) {
		lv_obj_remove_state(undo_btn, LV_STATE_DISABLED);
	} else {
		lv_obj_add_state(undo_btn, LV_STATE_DISABLED);
	}
	if (midend_can_redo(fe->me)) {
		lv_obj_remove_state(redo_btn, LV_STATE_DISABLED);
	} else {
		lv_obj_add_state(redo_btn, LV_STATE_DISABLED);
	}
	set_mark_look();
}

static void undo_cb(lv_event_t *e) {
	(void)e;
	midend_process_key(fe->me, 0, 0, UI_UNDO);
	after_move();
}

static void redo_cb(lv_event_t *e) {
	(void)e;
	midend_process_key(fe->me, 0, 0, UI_REDO);
	after_move();
}

static void mark_cb(lv_event_t *e) {
	(void)e;
	fe->mark = !fe->mark;
	set_mark_look();
}

static void new_cb(lv_event_t *e) {
	(void)e;
	midend_process_key(fe->me, 0, 0, UI_NEWGAME);
	fe->solved = false;
	layout_board();
	after_move();
}

// --- sizes and difficulties: Tatham's presets, flattened into one list ------

static void close_presets(void) {
	if (presets_panel) {
		lv_obj_delete(presets_panel);
		presets_panel = NULL;
	}
}

static void preset_cb(lv_event_t *e) {
	game_params *params = lv_event_get_user_data(e);
	close_presets();
	midend_set_params(fe->me, params);
	midend_new_game(fe->me);
	fe->solved = false;
	build_keys();
	layout_board();
	after_move();
}

static void presets_close_cb(lv_event_t *e) {
	(void)e;
	close_presets();
}

static void add_presets(lv_obj_t *list, struct preset_menu *menu, const char *prefix, int current) {
	for (int i = 0; i < menu->n_entries; i++) {
		struct preset_menu_entry *entry = &menu->entries[i];
		char name[160];
		snprintf(name, sizeof(name), "%s%s", prefix, entry->title);
		if (entry->submenu) {
			char sub[170];
			snprintf(sub, sizeof(sub), "%s: ", name);
			add_presets(list, entry->submenu, sub, current);
			continue;
		}
		lv_obj_t *b = lv_button_create(list);
		lv_obj_set_size(b, lv_pct(100), BUTTON_H);
		lv_obj_set_style_radius(b, 12, 0);
		lv_obj_set_style_shadow_width(b, 0, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(b, entry->id == current ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_add_event_cb(b, preset_cb, LV_EVENT_CLICKED, entry->params);
		lv_obj_t *l = lv_label_create(b);
		lv_label_set_text(l, name);
		lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
		lv_obj_set_width(l, lv_pct(100));
		lv_obj_set_style_text_font(l, &font_ui_22, 0);
		lv_obj_set_style_text_color(l, entry->id == current ? lv_color_white() : theme()->text_primary, 0);
		lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
	}
}

static void size_cb(lv_event_t *e) {
	(void)e;
	close_presets();
	int limit = 0;
	struct preset_menu *menu = midend_get_presets(fe->me, &limit);
	if (!menu || menu->n_entries == 0) {
		return;
	}
	presets_panel = lv_obj_create(puzzleplay_screen);
	lv_obj_set_size(presets_panel, lv_pct(100), lv_pct(100));
	lv_obj_set_style_bg_color(presets_panel, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(presets_panel, LV_OPA_70, 0);
	lv_obj_set_style_border_width(presets_panel, 0, 0);
	lv_obj_set_style_radius(presets_panel, 0, 0);
	lv_obj_add_event_cb(presets_panel, presets_close_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *list = lv_obj_create(presets_panel);
	lv_obj_set_size(list, lv_pct(90), lv_pct(75));
	lv_obj_center(list);
	lv_obj_set_style_bg_color(list, theme()->surface, 0);
	lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(list, 16, 0);
	lv_obj_set_style_border_width(list, 0, 0);
	lv_obj_set_style_pad_all(list, 10, 0);
	lv_obj_set_style_pad_row(list, 8, 0);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_scroll_dir(list, LV_DIR_VER);
	add_presets(list, menu, "", midend_which_preset(fe->me));
}

// ---------------------------------------------------------------------------
// Opening and closing
// ---------------------------------------------------------------------------

static void close_game(void) {
	close_presets();
	if (!fe->me) {
		return;
	}
	save_game();
	deactivate_timer(fe);
	midend_free(fe->me);
	fe->me = NULL;
	free(fe->px);
	fe->px = NULL;
	fe->w = fe->h = 0;
	free(fe->palette);
	fe->palette = NULL;
	fe->ncolours = 0;
}

static void timer_cb(lv_timer_t *t) {
	(void)t;
	if (!fe->me || !fe->timer_on) {
		return;
	}
	uint32_t now = now_ms();
	float dt = (float)(now - fe->timer_last) / 1000.0f;
	fe->timer_last = now;
	midend_timer(fe->me, dt);
}

int puzzleplay_game_count(void) { return GAME_COUNT; }

const char *puzzleplay_game_name(int index) {
	return index >= 0 && index < GAME_COUNT ? GAMES[index]->name : "";
}

void puzzleplay_open(int index) {
	if (index < 0 || index >= GAME_COUNT) {
		return;
	}
	close_game();
	fe->game = index;
	fe->mark = false;
	fe->pressed = 0;
	fe->solved = false;
	fe->me = midend_new(fe, GAMES[index], &API, fe);
	if (!load_game()) {
		midend_new_game(fe->me);
	}
	fe->solved = midend_status(fe->me) > 0;
	lv_label_set_text(title_label, GAMES[index]->name);
	lv_label_set_text(status_label, "");
	lv_obj_set_hidden(status_label, !midend_wants_statusbar(fe->me));
	build_keys();
	switch_screen(puzzleplay_screen);
	// sizes are only right once the screen's layout has run
	lv_obj_update_layout(puzzleplay_screen);
	area_w = lv_obj_get_content_width(board_area);
	area_h = lv_obj_get_content_height(board_area);
	layout_board();
	refresh_buttons();
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	close_game();
}

void puzzleplay_init(gui_config_t *cfg) {
	g_cfg = cfg;
	fe->timer = lv_timer_create(timer_cb, TIMER_MS, NULL);
	lv_timer_pause(fe->timer);

	lv_obj_add_style(puzzleplay_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(puzzleplay_screen, false);
	title_label = settingsrow_title(puzzleplay_screen, cfg, "puzzles");

	int top = settingsrow_content_top(cfg);
	lv_obj_t *page = lv_obj_create(puzzleplay_screen);
	lv_obj_set_style_bg_opa(page, 0, 0);
	lv_obj_set_style_border_width(page, 0, 0);
	lv_obj_set_style_pad_all(page, 0, 0);
	lv_obj_set_style_pad_row(page, 8, 0);
	lv_obj_set_scrollable(page, false);
	lv_obj_set_pos(page, cfg->padding, top);
	lv_obj_set_size(page, (int32_t)cfg->screen_width - 2 * cfg->padding, (int32_t)cfg->screen_height - top - cfg->padding);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);

	status_label = lv_label_create(page);
	lv_obj_add_style(status_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(status_label, &font_ui_18, 0);
	lv_obj_set_width(status_label, lv_pct(100));
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(status_label, "");

	board_area = lv_obj_create(page);
	lv_obj_set_width(board_area, lv_pct(100));
	lv_obj_set_flex_grow(board_area, 1);
	lv_obj_set_style_border_width(board_area, 0, 0);
	lv_obj_set_style_radius(board_area, 12, 0);
	lv_obj_set_style_pad_all(board_area, 6, 0);
	lv_obj_set_style_bg_opa(board_area, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(board_area, false);

	canvas = lv_canvas_create(board_area);
	lv_obj_set_clickable(canvas, true);
	lv_obj_set_scroll_chain(canvas, false);
	lv_obj_set_gesture_bubble(canvas, false);
	lv_obj_add_event_cb(canvas, board_event_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(canvas, board_event_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(canvas, board_event_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(canvas, board_event_cb, LV_EVENT_PRESS_LOST, NULL);

	keys_row = lv_obj_create(page);
	lv_obj_set_size(keys_row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(keys_row, 0, 0);
	lv_obj_set_style_border_width(keys_row, 0, 0);
	lv_obj_set_style_pad_all(keys_row, 0, 0);
	lv_obj_set_style_pad_gap(keys_row, 4, 0);
	lv_obj_set_scrollable(keys_row, false);
	lv_obj_set_flex_flow(keys_row, LV_FLEX_FLOW_ROW_WRAP);

	lv_obj_t *bar = lv_obj_create(page);
	lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(bar, 0, 0);
	lv_obj_set_style_border_width(bar, 0, 0);
	lv_obj_set_style_pad_all(bar, 0, 0);
	lv_obj_set_style_pad_column(bar, 6, 0);
	lv_obj_set_scrollable(bar, false);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	undo_btn = small_button(bar, tr("puzzles_undo"), undo_cb, NULL);
	redo_btn = small_button(bar, tr("puzzles_redo"), redo_cb, NULL);
	mark_btn = small_button(bar, tr("puzzles_mark"), mark_cb, NULL);
	small_button(bar, tr("puzzles_size"), size_cb, NULL);
	small_button(bar, tr("puzzles_new"), new_cb, NULL);

	lv_obj_add_event_cb(puzzleplay_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
