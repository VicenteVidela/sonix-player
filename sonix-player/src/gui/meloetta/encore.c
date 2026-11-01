#include "encore.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/meloetta/encore_art.h"
#include "src/gui/meloetta/encoresound.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/gui/shell/volume_overlay.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/device/power.h"

lv_obj_t *encore_screen;

// ---------------------------------------------------------------------------
// the LCD
// ---------------------------------------------------------------------------
//
// Every segment is made once and only ever lit or dimmed, as on a real LCD:
// an unlit segment stays faintly visible (its "ghost"). Lit segments take the
// theme's accent colour; the board is the theme's page colour.

#define LANES 5 // staff lines, 0 = the top one (F), 4 = the bottom one (E)
#define COLS 6	// steps from the right edge to the singer; 0 is the last
#define DIGITS 4
#define MISSES 3

#define GHOST_OPA 22
#define LINE_OPA 70

#define TICK_MS 15
#define STALL_US 400000
#define MISS_US 1100000
#define MISS_BLINK_US 140000
#define CATCH_US 160000
#define OVER_WAIT_US 1200000

typedef enum { EMPTY, NOTE, SOUR, REST } cell_t;
typedef enum { ST_TITLE, ST_PLAY, ST_MISS, ST_PAUSED, ST_OVER } state_t;

static int panel_w, panel_h;
static int lane_y[LANES];
static int col_x[COLS];

static lv_obj_t *note_img[LANES][COLS];
static lv_obj_t *hazard_img[LANES][COLS];
static lv_obj_t *singer_img[LANES];
static lv_obj_t *miss_img[MISSES];
static lv_obj_t *seg[DIGITS][7];
static lv_obj_t *staff_line[LANES];
static lv_obj_t *pause_btn, *exit_btn, *sound_btn, *sound_label;
static lv_obj_t *game_btn[2], *game_label[2];
static lv_obj_t *resume_btn, *quit_btn;
static lv_obj_t *mode_label, *hi_label, *banner_label, *sub_label;
static lv_timer_t *tick_timer;

static cell_t board[LANES][COLS];
static state_t state;
static bool active;
static int mode; // 0 = Game A, 1 = Game B
static int player;
static int score, misses, best[2];
static bool new_best;
static int since_note, last_note_lane;
static int64_t next_beat_us, last_us, state_until_us, catch_until_us;
static int miss_lane, miss_col;
static cell_t miss_kind;
static bool sound_on;

// What each segment shows now, so a frame only touches what changed.
static uint8_t note_shown[LANES][COLS], hazard_shown[LANES][COLS], singer_shown[LANES], miss_shown[MISSES];
static uint8_t seg_shown[DIGITS][7];

static int64_t now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------------------
// rules
// ---------------------------------------------------------------------------

typedef struct {
	int start_ms, min_ms, step_ms; // the beat, shortened by step_ms a point
	float note_p0, note_p1;		  // chance of a note in a new column, from 0 to 40 points
	float hazard_p0, hazard_p1;
	int max_hazards;
} rules_t;

static const rules_t RULES[2] = {
	{620, 300, 3, 0.45f, 0.70f, 0.12f, 0.35f, 1},
	{480, 240, 3, 0.55f, 0.80f, 0.30f, 0.50f, 2},
};

#define JUMP_PER_BEAT 2 // lanes the singer is asked to cover between two notes, a beat

static int beat_ms(void) {
	const rules_t *r = &RULES[mode];
	int ms = r->start_ms - score * r->step_ms;
	return ms < r->min_ms ? r->min_ms : ms;
}

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static float ramp(float a, float b) {
	float t = score >= 40 ? 1.0f : (float)score / 40.0f;
	return a + (b - a) * t;
}

// The lanes of the staff, top to bottom: F5 D5 B4 G4 E4.
static const float LANE_HZ[LANES] = {698.46f, 587.33f, 493.88f, 392.00f, 329.63f};

static void beep_catch(int lane) {
	encore_tone_t t[] = {{LANE_HZ[lane], 70, 5000}};
	encoresound_play(t, 1);
}

static void beep_tick(void) {
	encore_tone_t t[] = {{1800, 12, 1500}};
	encoresound_play(t, 1);
}

static void beep_miss(void) {
	encore_tone_t t[] = {{311, 90, 5000}, {0, 30, 0}, {233, 90, 5000}, {0, 30, 0}, {155, 160, 5000}};
	encoresound_play(t, 5);
}

static void beep_over(void) {
	encore_tone_t t[] = {{523, 140, 5000}, {392, 140, 5000}, {330, 140, 5000}, {262, 320, 5000}};
	encoresound_play(t, 4);
}

static void beep_bonus(void) {
	encore_tone_t t[] = {{523, 80, 5000}, {659, 80, 5000}, {784, 80, 5000}, {1047, 200, 5000}};
	encoresound_play(t, 4);
}

static void beep_start(void) {
	encore_tone_t t[] = {{784, 80, 5000}, {0, 40, 0}, {1047, 140, 5000}};
	encoresound_play(t, 3);
}

static void spawn(void) {
	const rules_t *r = &RULES[mode];
	int note_lane = -1;
	since_note++;
	if (frand() < ramp(r->note_p0, r->note_p1)) {
		// Within reach of the last note: JUMP_PER_BEAT lanes for each beat between them.
		int reach = since_note * JUMP_PER_BEAT;
		int lo = last_note_lane - reach, hi = last_note_lane + reach;
		lo = lo < 0 ? 0 : lo;
		hi = hi >= LANES ? LANES - 1 : hi;
		note_lane = lo + rand() % (hi - lo + 1);
		board[note_lane][COLS - 1] = NOTE;
		last_note_lane = note_lane;
		since_note = 0;
	}
	int hazards = 0;
	for (int tries = 0; tries < r->max_hazards; tries++) {
		if (frand() >= ramp(r->hazard_p0, r->hazard_p1)) {
			continue;
		}
		int lane = rand() % LANES;
		if (board[lane][COLS - 1] != EMPTY) {
			continue;
		}
		board[lane][COLS - 1] = (rand() & 1) ? SOUR : REST;
		hazards++;
	}
	(void)hazards;
}

static void save_best(void) {
	config_set_int("encore", mode ? "best_b" : "best_a", best[mode]);
	config_save();
}

static void begin_miss(int lane, int col, cell_t kind) {
	misses++;
	miss_lane = lane;
	miss_col = col;
	miss_kind = kind;
	state = ST_MISS;
	state_until_us = now_us() + MISS_US;
	beep_miss();
}

static void go_over(void) {
	state = ST_OVER;
	state_until_us = now_us() + OVER_WAIT_US;
	if (new_best) {
		save_best();
	}
	beep_over();
}

static void add_point(void) {
	if (score < 9999) {
		score++;
	}
	if (score > best[mode]) {
		best[mode] = score;
		new_best = true;
	}
	if (score % 100 == 0 && misses > 0) {
		misses = 0; // the old LCD games' mercy
		beep_bonus();
	}
}

static void catch_note(int lane, int col) {
	board[lane][col] = EMPTY;
	catch_until_us = now_us() + CATCH_US;
	add_point();
	if (score % 100 != 0) {
		beep_catch(lane);
	}
}

// What has reached the singer's column, as it arrives.
static bool arrivals(void) {
	cell_t c = board[player][0];
	if (c == NOTE) {
		catch_note(player, 0);
	} else if (c == SOUR || c == REST) {
		begin_miss(player, 0, c);
		board[player][0] = EMPTY;
		return false;
	}
	return true;
}

static void beat(void) {
	// A note still in the last column was let go.
	for (int l = 0; l < LANES; l++) {
		if (board[l][0] == NOTE) {
			board[l][0] = EMPTY;
			begin_miss(l, 0, NOTE);
			return;
		}
	}
	for (int l = 0; l < LANES; l++) {
		for (int c = 0; c < COLS - 1; c++) {
			board[l][c] = board[l][c + 1];
		}
		board[l][COLS - 1] = EMPTY;
	}
	spawn();
	if (arrivals()) {
		beep_tick();
	}
}

static void move_player(int dir) {
	int p = player + dir;
	if (p < 0 || p >= LANES) {
		return;
	}
	player = p;
	// A note waiting in the last column can be taken any time in its beat.
	// The others only hurt as they arrive: hopping across one is safe.
	if (board[player][0] == NOTE) {
		catch_note(player, 0);
	}
}

static void start_game(int m) {
	mode = m;
	config_set_int("encore", "mode", mode);
	memset(board, 0, sizeof(board));
	player = LANES / 2;
	score = misses = 0;
	new_best = false;
	since_note = 2;
	last_note_lane = LANES / 2;
	state = ST_PLAY;
	next_beat_us = now_us() + 700000; // a breath before the first beat
	catch_until_us = 0;
	beep_start();
}

static void go_title(void) {
	state = ST_TITLE;
	memset(board, 0, sizeof(board));
	player = LANES / 2;
	board[1][3] = NOTE;
	board[3][1] = NOTE;
	board[2][4] = REST;
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------

static void set_lit(lv_obj_t *o, uint8_t *shown, uint8_t want) {
	// 0 = ghost, 1 = lit, 2 = hidden
	if (*shown == want) {
		return;
	}
	*shown = want;
	if (want == 2) {
		lv_obj_set_hidden(o, true);
		return;
	}
	lv_obj_set_hidden(o, false);
	lv_obj_set_style_opa(o, want == 1 ? LV_OPA_COVER : GHOST_OPA, 0);
}

// The seven segments, a to g, for each digit.
static const uint8_t SEVEN[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

static void draw_number(int v) {
	for (int d = DIGITS - 1; d >= 0; d--) {
		int digit = v % 10;
		v /= 10;
		// leading zeros stay dark, but a single 0 shows
		bool blank = (v == 0 && digit == 0 && d < DIGITS - 1);
		for (int s = 0; s < 7; s++) {
			set_lit(seg[d][s], &seg_shown[d][s], !blank && (SEVEN[digit] >> s & 1) ? 1 : 0);
		}
		if (blank) {
			// keep going: the remaining digits are blank too
			for (int dd = d - 1; dd >= 0; dd--) {
				for (int s = 0; s < 7; s++) {
					set_lit(seg[dd][s], &seg_shown[dd][s], 0);
				}
			}
			return;
		}
	}
}

static void show(lv_obj_t *o, bool on) {
	if (on == lv_obj_is_hidden(o)) {
		if (on) {
			lv_obj_set_hidden(o, false);
		} else {
			lv_obj_set_hidden(o, true);
		}
	}
}

static void render(void) {
	int64_t now = now_us();
	bool blink_off = state == ST_MISS && ((state_until_us - now) / MISS_BLINK_US) % 2 == 1;

	for (int l = 0; l < LANES; l++) {
		for (int c = 0; c < COLS; c++) {
			cell_t k = board[l][c];
			bool missing_here = state == ST_MISS && l == miss_lane && c == miss_col;
			if (missing_here) {
				k = miss_kind;
				if (blink_off) {
					k = EMPTY;
				}
			}
			set_lit(note_img[l][c], &note_shown[l][c], k == NOTE ? 1 : (k == EMPTY ? 0 : 2));
			if (k == SOUR || k == REST) {
				const void *src = k == SOUR ? &encore_sour : &encore_rest;
				if (lv_image_get_src(hazard_img[l][c]) != src) {
					lv_image_set_src(hazard_img[l][c], src);
				}
			}
			set_lit(hazard_img[l][c], &hazard_shown[l][c], (k == SOUR || k == REST) ? 1 : 2);
		}
		bool catching = l == player && now < catch_until_us;
		const void *src = catching ? &encore_singer_catch : &encore_singer;
		if (lv_image_get_src(singer_img[l]) != src) {
			lv_image_set_src(singer_img[l], src);
		}
		bool lit = l == player && !(state == ST_MISS && miss_kind != NOTE && blink_off);
		set_lit(singer_img[l], &singer_shown[l], lit ? 1 : 0);
	}
	for (int i = 0; i < MISSES; i++) {
		bool lit = i < misses && !(state == ST_MISS && i == misses - 1 && blink_off);
		set_lit(miss_img[i], &miss_shown[i], lit ? 1 : 0);
	}

	bool title = state == ST_TITLE;
	draw_number(title ? best[mode] : score);
	show(hi_label, title);
	show(exit_btn, title);
	show(sound_btn, title);
	show(game_btn[0], title);
	show(game_btn[1], title);
	show(pause_btn, state == ST_PLAY || state == ST_MISS);
	show(mode_label, !title);
	show(resume_btn, state == ST_PAUSED);
	show(quit_btn, state == ST_PAUSED);
	for (int i = 0; i < MISSES; i++) {
		show(miss_img[i], !title);
	}

	const char *banner = NULL, *sub = NULL;
	if (state == ST_PAUSED) {
		banner = tr("encore_paused");
	} else if (state == ST_OVER) {
		banner = tr("encore_game_over");
		sub = new_best ? tr("encore_new_best") : (now >= state_until_us ? tr("encore_tap_to_continue") : NULL);
	}
	show(banner_label, banner != NULL);
	if (banner && strcmp(lv_label_get_text(banner_label), banner) != 0) {
		lv_label_set_text(banner_label, banner);
	}
	show(sub_label, sub != NULL);
	if (sub && strcmp(lv_label_get_text(sub_label), sub) != 0) {
		lv_label_set_text(sub_label, sub);
	}
}

static void refresh_labels(void) {
	char buf[64];
	for (int i = 0; i < 2; i++) {
		snprintf(buf, sizeof(buf), "%s\n%s %d", tr(i ? "encore_game_b" : "encore_game_a"), tr("encore_hi"), best[i]);
		lv_label_set_text(game_label[i], buf);
	}
	lv_label_set_text(mode_label, tr(mode ? "encore_game_b" : "encore_game_a"));
	lv_label_set_text(sound_label, tr(sound_on ? "encore_sound_on" : "encore_sound_off"));
}

// ---------------------------------------------------------------------------
// time and touch
// ---------------------------------------------------------------------------

static void tick_cb(lv_timer_t *timer) {
	if (!active) {
		lv_timer_pause(timer);
		return;
	}
	int64_t now = now_us();
	int64_t gap = now - last_us;
	last_us = now;

	if (gap > STALL_US && (state == ST_PLAY || state == ST_MISS)) {
		// Back from standby, or the player was busy: wait for a tap.
		state = ST_PAUSED;
	}

	switch (state) {
	case ST_PLAY:
		if (now >= next_beat_us) {
			beat();
			next_beat_us += beat_ms() * 1000;
			if (next_beat_us < now) {
				next_beat_us = now + beat_ms() * 1000;
			}
		}
		break;
	case ST_MISS:
		if (now >= state_until_us) {
			for (int l = 0; l < LANES; l++) {
				board[l][0] = EMPTY;
			}
			if (misses >= MISSES) {
				go_over();
			} else {
				state = ST_PLAY;
				next_beat_us = now + beat_ms() * 1000;
			}
		}
		break;
	default:
		break;
	}
	render();
}

static void press_cb(lv_event_t *e) {
	(void)e;
	if (!active) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	if (state == ST_PLAY) {
		move_player(p.y < panel_h / 2 ? -1 : 1);
		render();
	} else if (state == ST_OVER && now_us() >= state_until_us) {
		go_title();
		refresh_labels();
		render();
	}
}

static void game_cb(lv_event_t *e) {
	start_game((int)(intptr_t)lv_event_get_user_data(e));
	refresh_labels();
	render();
}

static void pause_cb(lv_event_t *e) {
	(void)e;
	if (state == ST_PLAY || state == ST_MISS) {
		state = ST_PAUSED;
		render();
	}
}

static void resume_cb(lv_event_t *e) {
	(void)e;
	if (misses >= MISSES) { // paused on the third miss
		go_over();
		render();
		return;
	}
	state = ST_PLAY;
	next_beat_us = now_us() + beat_ms() * 1000;
	render();
}

static void quit_cb(lv_event_t *e) {
	(void)e;
	if (new_best) {
		save_best();
	}
	go_title();
	refresh_labels();
	render();
}

static void exit_cb(lv_event_t *e) {
	(void)e;
	back_btn_cb(NULL);
}

static void sound_cb(lv_event_t *e) {
	(void)e;
	sound_on = !sound_on;
	config_set_bool("encore", "sound", sound_on);
	config_save();
	if (sound_on) {
		encoresound_start();
	} else {
		encoresound_stop();
	}
	refresh_labels();
}

// ---------------------------------------------------------------------------
// building it
// ---------------------------------------------------------------------------

static lv_obj_t *sprite(int x, int y, const lv_image_dsc_t *src) {
	lv_obj_t *o = lv_image_create(encore_screen);
	lv_image_set_src(o, src);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_clickable(o, false);
	lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_opa(o, GHOST_OPA, 0);
	return o;
}

static lv_obj_t *rect(int x, int y, int w, int h) {
	lv_obj_t *o = lv_obj_create(encore_screen);
	lv_obj_remove_style_all(o);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, 2, 0);
	lv_obj_set_clickable(o, false);
	lv_obj_set_style_opa(o, GHOST_OPA, 0);
	return o;
}

static lv_obj_t *label(int font_px, bool bold) {
	lv_obj_t *l = lv_label_create(encore_screen);
	const lv_font_t *f = bold ? (font_px >= 36 ? &font_ui_36_bold : &font_ui_24_bold) : (font_px >= 24 ? &font_ui_24 : &font_ui_20);
	lv_obj_set_style_text_font(l, f, 0);
	lv_label_set_text(l, "");
	return l;
}

static lv_obj_t *button(int x, int y, int w, int h, lv_event_cb_t cb, void *user, lv_obj_t **label_out) {
	lv_obj_t *b = lv_button_create(encore_screen);
	lv_obj_set_pos(b, x, y);
	lv_obj_set_size(b, w, h);
	lv_obj_set_style_radius(b, 12, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(b, 3, 0);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
	lv_obj_t *l = lv_label_create(b);
	lv_obj_set_style_text_font(l, &font_ui_22_bold, 0);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_center(l);
	lv_label_set_text(l, "");
	if (label_out) {
		*label_out = l;
	}
	return b;
}

// The theme can change between visits: the colours are set on the way in.
static void apply_colours(void) {
	const theme_palette_t *t = theme();
	lv_color_t lit = t->accent, dim = t->text_secondary;
	lv_obj_set_style_bg_color(encore_screen, t->screen_bg, 0);
	for (int l = 0; l < LANES; l++) {
		for (int c = 0; c < COLS; c++) {
			lv_obj_set_style_image_recolor(note_img[l][c], lit, 0);
			lv_obj_set_style_image_recolor(hazard_img[l][c], lit, 0);
		}
		lv_obj_set_style_image_recolor(singer_img[l], lit, 0);
		lv_obj_set_style_bg_color(staff_line[l], dim, 0);
	}
	for (int i = 0; i < MISSES; i++) {
		lv_obj_set_style_image_recolor(miss_img[i], lit, 0);
	}
	for (int d = 0; d < DIGITS; d++) {
		for (int s = 0; s < 7; s++) {
			lv_obj_set_style_bg_color(seg[d][s], lit, 0);
		}
	}
	lv_obj_t *texts[] = {mode_label, hi_label, banner_label, sub_label};
	for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
		lv_obj_set_style_text_color(texts[i], lit, 0);
	}
	lv_obj_t *btns[] = {pause_btn, exit_btn, sound_btn, game_btn[0], game_btn[1], resume_btn, quit_btn};
	for (size_t i = 0; i < sizeof(btns) / sizeof(btns[0]); i++) {
		lv_obj_set_style_border_color(btns[i], lit, 0);
		lv_obj_set_style_bg_color(btns[i], lit, LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(btns[i], LV_OPA_30, LV_STATE_PRESSED);
		lv_obj_set_style_text_color(btns[i], lit, 0);
	}
}

static void screen_loaded_cb(lv_event_t *e) {
	(void)e;
	topbar_set_hidden(true);
	back_btn_force_hidden(true);
	volume_overlay_allow_outside_player(true);
	apply_colours();
	sound_on = config_get_bool("encore", "sound", false);
	if (sound_on) {
		encoresound_start();
	}
	active = true;
	last_us = now_us();
	refresh_labels();
	render();
	lv_timer_resume(tick_timer);
	lv_timer_ready(tick_timer);
}

static void screen_unloaded_cb(lv_event_t *e) {
	(void)e;
	active = false;
	lv_timer_pause(tick_timer);
	encoresound_stop();
	if (new_best) {
		save_best();
		new_best = false;
	}
	go_title();
	volume_overlay_allow_outside_player(false);
	back_btn_force_hidden(false);
	topbar_set_hidden(false);
}

void encore_open(void) {
	best[0] = (int)config_get_int("encore", "best_a", 0);
	best[1] = (int)config_get_int("encore", "best_b", 0);
	mode = (int)config_get_int("encore", "mode", 0) ? 1 : 0;
	srand((unsigned)now_us());
	go_title();
	switch_screen(encore_screen);
}

void encore_init(gui_config_t *cfg) {
	panel_w = (int)cfg->screen_width;
	panel_h = (int)cfg->screen_height;

	lv_obj_set_style_bg_opa(encore_screen, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(encore_screen, false);

	// The staff: from 27% to 80% of the height, the singer on the left.
	int top = panel_h * 27 / 100, bottom = panel_h * 80 / 100;
	for (int l = 0; l < LANES; l++) {
		lane_y[l] = top + (bottom - top) * l / (LANES - 1);
		staff_line[l] = rect(0, lane_y[l] - 1, panel_w, 3);
		lv_obj_set_style_opa(staff_line[l], LINE_OPA, 0);
	}
	int step = (panel_w - 150 - 40) / (COLS - 1);
	for (int c = 0; c < COLS; c++) {
		col_x[c] = 150 + c * step;
	}
	// Note frames: 56 x 92, the head's centre at (20, 74). The singer: 100 x 120, centre (50, 60).
	for (int l = 0; l < LANES; l++) {
		for (int c = 0; c < COLS; c++) {
			note_img[l][c] = sprite(col_x[c] - 20, lane_y[l] - 74, &encore_note);
			hazard_img[l][c] = sprite(col_x[c] - 20, lane_y[l] - 74, &encore_rest);
			lv_obj_set_hidden(hazard_img[l][c], true);
			hazard_shown[l][c] = 2;
		}
		singer_img[l] = sprite(58 - 50, lane_y[l] - 60, &encore_singer);
	}

	// Header: pause or exit on the left, misses, the score on the right.
	int dw = 34, dh = 60, t = 7, gap = 12;
	int x0 = panel_w - 20 - DIGITS * dw - (DIGITS - 1) * gap, y0 = 26;
	for (int d = 0; d < DIGITS; d++) {
		int x = x0 + d * (dw + gap), h2 = dh / 2;
		seg[d][0] = rect(x + t, y0, dw - 2 * t, t);
		seg[d][1] = rect(x + dw - t, y0 + t, t, h2 - t);
		seg[d][2] = rect(x + dw - t, y0 + h2, t, h2 - t);
		seg[d][3] = rect(x + t, y0 + dh - t, dw - 2 * t, t);
		seg[d][4] = rect(x, y0 + h2, t, h2 - t);
		seg[d][5] = rect(x, y0 + t, t, h2 - t);
		seg[d][6] = rect(x + t, y0 + h2 - t / 2, dw - 2 * t, t);
	}
	hi_label = label(24, true);
	lv_label_set_text(hi_label, tr("encore_hi"));
	lv_obj_set_pos(hi_label, x0 - 40, y0 + 30);

	pause_btn = button(16, 24, 64, 64, pause_cb, NULL, NULL);
	lv_label_set_text(lv_obj_get_child(pause_btn, 0), "II");
	exit_btn = button(16, 24, 64, 64, exit_cb, NULL, NULL);
	lv_label_set_text(lv_obj_get_child(exit_btn, 0), "X");
	sound_btn = button(92, 24, 128, 64, sound_cb, NULL, &sound_label);
	lv_obj_set_style_text_font(sound_label, &font_ui_20, 0);

	for (int i = 0; i < MISSES; i++) {
		miss_img[i] = sprite(96 + i * 42, 28, &encore_miss);
	}
	mode_label = label(20, false);
	lv_obj_set_pos(mode_label, 98, 70);

	// Below the staff: the title's buttons, the pause menu, the banners.
	int by = bottom + 70, bw = (panel_w - 3 * 20) / 2, bh = panel_h - by - 16;
	if (bh > 96) {
		bh = 96;
	}
	game_btn[0] = button(20, by, bw, bh, game_cb, (void *)0, &game_label[0]);
	game_btn[1] = button(40 + bw, by, bw, bh, game_cb, (void *)1, &game_label[1]);
	resume_btn = button(20, by, bw, bh, resume_cb, NULL, NULL);
	lv_label_set_text(lv_obj_get_child(resume_btn, 0), tr("encore_resume"));
	quit_btn = button(40 + bw, by, bw, bh, quit_cb, NULL, NULL);
	lv_label_set_text(lv_obj_get_child(quit_btn, 0), tr("encore_quit"));

	banner_label = label(36, true);
	lv_obj_set_width(banner_label, panel_w);
	lv_obj_set_style_text_align(banner_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(banner_label, 0, bottom + 26);
	sub_label = label(24, false);
	lv_obj_set_width(sub_label, panel_w);
	lv_obj_set_style_text_align(sub_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(sub_label, 0, bottom + 80);

	lv_obj_add_event_cb(encore_screen, press_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(encore_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(encore_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	tick_timer = lv_timer_create(tick_cb, TICK_MS, NULL);
	lv_timer_pause(tick_timer);
	power_pause_in_standby(tick_timer);
	go_title();
}
