#include "homepage.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/net/http.h"

lv_obj_t *homepage_screen;

#define DEFAULT_RELAY "http://192.168.100.11:8091"
#define TIMEOUT_SECS 12
#define RESPONSE_LIMIT (16 * 1024)
#define POLL_MS 10000
#define CONFIRM_SECS 3 // shutting down takes a second tap within this long

#define PLAYERS 1
static const char *player_keys[PLAYERS] = {"bose"};

typedef struct {
	char name[48];
	char state[24];
	char title[96];
	int volume; // -1 when HA does not say
} player_t;

typedef struct {
	bool ok;
	char kinggroot[16]; // on, sleeping, off
	char monitors[32];
	char speakers[16];
	player_t players[PLAYERS];
} home_t;

static home_t data;
static time_t fetched;
static bool busy;
static bool failed;
static char relay[128];
static time_t shutdown_armed;
// The last power tap and when: the page says so until KingGroot's state
// catches up (a sleep takes a few seconds to show), or for a minute at most.
static const char *pending;
static time_t pending_at;
#define PENDING_SECS 60

static lv_obj_t *status_label;
static lv_obj_t *cards[1 + PLAYERS];
static lv_obj_t *kg_state_label;
static lv_obj_t *monitors_label;
static lv_obj_t *speakers_switch;
static lv_obj_t *shutdown_label;
static lv_obj_t *wake_btn, *sleep_btn, *shutdown_btn;
static lv_obj_t *player_name[PLAYERS];
static lv_obj_t *player_state[PLAYERS];
static lv_obj_t *player_title[PLAYERS];
static lv_obj_t *buttons[24];
static bool button_accent[24];
static int button_count;
static lv_timer_t *poll_timer;

// ---------------------------------------------------------------------------
// talking to CL4P-TP, on a thread of its own
// ---------------------------------------------------------------------------

static bool parse(const char *text, home_t *out) {
	memset(out, 0, sizeof(*out));
	json_doc_t doc;
	if (!text || !json_parse(text, &doc)) {
		return false;
	}
	int root = json_root(&doc);
	// An action answers {"ok": name, "state": {...}}; /state answers the state.
	int st = json_get(&doc, root, "state");
	if (st < 0) {
		st = root;
	}
	json_obj_str(&doc, st, "kinggroot", out->kinggroot, sizeof(out->kinggroot));
	json_obj_str(&doc, st, "monitors", out->monitors, sizeof(out->monitors));
	json_obj_str(&doc, st, "speakers", out->speakers, sizeof(out->speakers));
	int players = json_get(&doc, st, "players");
	for (int i = 0; i < PLAYERS; i++) {
		player_t *p = &out->players[i];
		p->volume = -1;
		int o = players >= 0 ? json_get(&doc, players, player_keys[i]) : -1;
		if (o < 0) {
			snprintf(p->state, sizeof(p->state), "unknown");
			continue;
		}
		json_obj_str(&doc, o, "name", p->name, sizeof(p->name));
		json_obj_str(&doc, o, "state", p->state, sizeof(p->state));
		json_obj_str(&doc, o, "title", p->title, sizeof(p->title));
		p->volume = (int)json_obj_long(&doc, o, "volume", -1);
	}
	out->ok = out->kinggroot[0] != '\0';
	json_free(&doc);
	return out->ok;
}

typedef struct {
	char url[256];
	bool post;
	home_t result;
} job_t;

static void done_cb(void *user);

static void *job_main(void *arg) {
	job_t *job = arg;
	thread_be_background("home");
	char *body = NULL;
	bool ok;
	if (job->post) {
		http_req_t req = {0};
		req.method = "POST";
		req.body = "{}";
		req.content_type = "application/json";
		req.allow_empty_body = true;
		int status = 0;
		ok = http_request(job->url, &req, &body, NULL, RESPONSE_LIMIT, TIMEOUT_SECS, &status) && status == 200;
	} else {
		ok = http_get(job->url, &body, NULL, RESPONSE_LIMIT, TIMEOUT_SECS);
	}
	if (!(ok && parse(body, &job->result))) {
		job->result.ok = false;
	}
	free(body);
	if (!gui_post(done_cb, job)) {
		free(job);
	}
	return NULL;
}

static void refresh_view(void);

static void start(const char *action) {
	if (busy) {
		return;
	}
	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	if (action) {
		snprintf(job->url, sizeof(job->url), "%s/action/%s", relay, action);
		job->post = true;
	} else {
		snprintf(job->url, sizeof(job->url), "%s/state", relay);
	}
	pthread_t t;
	if (pthread_create(&t, NULL, job_main, job) != 0) {
		free(job);
		return;
	}
	pthread_detach(t);
	busy = true;
	refresh_view();
}

static void done_cb(void *user) {
	job_t *job = user;
	busy = false;
	failed = !job->result.ok;
	if (job->result.ok) {
		data = job->result;
		fetched = time(NULL);
	}
	free(job);
	refresh_view();
}

// ---------------------------------------------------------------------------
// the view
// ---------------------------------------------------------------------------

static const char *kg_text(const char *s) {
	if (strcmp(s, "on") == 0) return tr("home_kg_on");
	if (strcmp(s, "sleeping") == 0) return tr("home_kg_sleeping");
	if (strcmp(s, "off") == 0) return tr("home_kg_off");
	return tr("home_unknown");
}

static const char *player_text(const char *s) {
	if (strcmp(s, "playing") == 0) return tr("home_playing");
	if (strcmp(s, "paused") == 0) return tr("home_paused");
	if (strcmp(s, "idle") == 0 || strcmp(s, "on") == 0) return tr("home_idle");
	if (strcmp(s, "off") == 0 || strcmp(s, "standby") == 0) return tr("home_off");
	if (strcmp(s, "unavailable") == 0) return tr("home_unavailable");
	return tr("home_unknown");
}

static void set_accent(lv_obj_t *b, bool accent) {
	if (accent) {
		lv_obj_add_style(b, &theme_style_accent_bg, 0);
		lv_obj_remove_local_style_prop(b, LV_STYLE_BG_COLOR, 0);
	} else {
		lv_obj_remove_style(b, &theme_style_accent_bg, 0);
		lv_obj_set_style_bg_color(b, theme()->surface_pressed, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	}
	lv_obj_set_style_text_color(lv_obj_get_child(b, 0), accent ? lv_color_white() : theme()->text_primary, 0);
	for (int i = 0; i < button_count; i++) {
		if (buttons[i] == b) button_accent[i] = accent;
	}
}

static void refresh_view(void) {
	char buf[160];
	if (busy) {
		snprintf(buf, sizeof(buf), "%s", tr("home_working"));
	} else if (failed) {
		snprintf(buf, sizeof(buf), "%s", tr("home_unreachable"));
	} else if (fetched) {
		struct tm tm;
		localtime_r(&fetched, &tm);
		snprintf(buf, sizeof(buf), tr("home_updated"), tm.tm_hour, tm.tm_min);
	} else {
		buf[0] = '\0';
	}
	lv_label_set_text(status_label, buf);

	bool have = data.ok;
	bool kg_on = have && strcmp(data.kinggroot, "on") == 0;
	if (pending) {
		bool arrived = have && (strcmp(pending, "kg_wake") == 0 ? kg_on : !kg_on);
		if (arrived || time(NULL) - pending_at > PENDING_SECS) {
			pending = NULL;
		}
	}
	const char *kg = !have ? tr("home_unknown") : kg_text(data.kinggroot);
	if (pending) {
		kg = tr(strcmp(pending, "kg_wake") == 0 ? "home_waking" : strcmp(pending, "kg_sleep") == 0 ? "home_going_sleep" : "home_shutting_down");
	}
	lv_label_set_text(kg_state_label, kg);
	// A selector, like the weather pills: the lit button is where KingGroot is
	// (or is going), and tapping another sends it there.
	bool kg_asleep = have && strcmp(data.kinggroot, "sleeping") == 0;
	bool kg_off = have && strcmp(data.kinggroot, "off") == 0;
	const char *at = pending ? pending : kg_on ? "kg_wake" : kg_asleep ? "kg_sleep" : kg_off ? "kg_shutdown" : "";
	set_accent(wake_btn, strcmp(at, "kg_wake") == 0);
	set_accent(sleep_btn, strcmp(at, "kg_sleep") == 0);
	set_accent(shutdown_btn, strcmp(at, "kg_shutdown") == 0);
	snprintf(buf, sizeof(buf), "%s: %s", tr("home_monitors"),
			 !have ? "-" : (strncmp(data.monitors, "Dual", 4) == 0 ? tr("home_monitors_dual") : tr("home_monitors_one")));
	lv_label_set_text(monitors_label, buf);
	if (have && strcmp(data.speakers, "on") == 0) {
		lv_obj_add_state(speakers_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_remove_state(speakers_switch, LV_STATE_CHECKED);
	}
	bool armed = shutdown_armed && time(NULL) - shutdown_armed <= CONFIRM_SECS;
	lv_label_set_text(shutdown_label, tr(armed ? "home_tap_again" : "home_shutdown"));

	for (int i = 0; i < PLAYERS; i++) {
		player_t *p = &data.players[i];
		lv_label_set_text(player_name[i], have && p->name[0] ? p->name : player_keys[i]);
		if (have && p->volume >= 0) {
			snprintf(buf, sizeof(buf), "%s  %d%%", player_text(p->state), p->volume);
		} else {
			snprintf(buf, sizeof(buf), "%s", have ? player_text(p->state) : tr("home_unknown"));
		}
		lv_label_set_text(player_state[i], buf);
		lv_label_set_text(player_title[i], have ? p->title : "");
	}

	for (int i = 0; i < button_count; i++) {
		// Nothing to do where it already is; no sleeping a PC that is off, and
		// no shutting down one that is asleep (the request could not reach it).
		bool pointless = have && ((buttons[i] == wake_btn && kg_on && !pending) ||
								  (buttons[i] == sleep_btn && !kg_on) ||
								  (buttons[i] == shutdown_btn && !kg_on));
		if (busy || (pointless && !button_accent[i])) { // the lit one stays bright
			lv_obj_add_state(buttons[i], LV_STATE_DISABLED);
		} else {
			lv_obj_remove_state(buttons[i], LV_STATE_DISABLED);
		}
	}
}

static void refresh_theme(void) {
	for (int i = 0; i < 1 + PLAYERS; i++) {
		lv_obj_set_style_bg_color(cards[i], theme()->surface, 0);
	}
	for (int i = 0; i < button_count; i++) {
		if (button_accent[i]) continue;
		lv_obj_set_style_bg_color(buttons[i], theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(lv_obj_get_child(buttons[i], 0), theme()->text_primary, 0);
	}
	lv_obj_invalidate(homepage_screen);
}

// ---------------------------------------------------------------------------
// taps
// ---------------------------------------------------------------------------

static void action_cb(lv_event_t *e) {
	const char *action = lv_event_get_user_data(e);
	shutdown_armed = 0;
	if (strncmp(action, "kg_", 3) == 0) {
		pending = action;
		pending_at = time(NULL);
	}
	start(action);
}

static void shutdown_cb(lv_event_t *e) {
	(void)e;
	time_t now = time(NULL);
	if (shutdown_armed && now - shutdown_armed <= CONFIRM_SECS) {
		shutdown_armed = 0;
		pending = "kg_shutdown";
		pending_at = now;
		start("kg_shutdown");
	} else {
		shutdown_armed = now;
		refresh_view();
	}
}

static void speakers_cb(lv_event_t *e) {
	lv_obj_t *sw = lv_event_get_target(e);
	start(lv_obj_has_state(sw, LV_STATE_CHECKED) ? "speakers_on" : "speakers_off");
}

static void status_clicked_cb(lv_event_t *e) {
	(void)e;
	start(NULL);
}

static void poll_cb(lv_timer_t *t) {
	(void)t;
	if (shutdown_armed && time(NULL) - shutdown_armed > CONFIRM_SECS) {
		shutdown_armed = 0;
		refresh_view();
	}
	start(NULL);
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh_view();
	start(NULL);
	lv_timer_resume(poll_timer);
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	lv_timer_pause(poll_timer);
	shutdown_armed = 0;
}

// ---------------------------------------------------------------------------
// building it
// ---------------------------------------------------------------------------

static lv_obj_t *plain(lv_obj_t *parent) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_set_style_bg_opa(o, 0, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_set_scrollable(o, false);
	return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, bool dim) {
	lv_obj_t *l = lv_label_create(parent);
	lv_label_set_text(l, "");
	lv_obj_add_style(l, dim ? &theme_style_text_dim : &theme_style_text, 0);
	lv_obj_set_style_text_font(l, font, 0);
	return l;
}

static lv_obj_t *card(lv_obj_t *parent, int index) {
	lv_obj_t *c = cards[index] = plain(parent);
	lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_bg_color(c, theme()->surface, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(c, 16, 0);
	lv_obj_set_style_pad_all(c, 14, 0);
	lv_obj_set_style_pad_row(c, 10, 0);
	return c;
}

static lv_obj_t *row(lv_obj_t *parent) {
	lv_obj_t *r = plain(parent);
	lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(r, 8, 0);
	return r;
}

// A button that fills its share of a row. `label` is shown as is when `raw`,
// otherwise it is a language key.
static lv_obj_t *button(lv_obj_t *parent, const char *label, bool raw, lv_event_cb_t cb, const void *user, bool accent) {
	lv_obj_t *b = lv_button_create(parent);
	lv_obj_set_height(b, 56);
	lv_obj_set_flex_grow(b, 1);
	lv_obj_set_style_pad_hor(b, 6, 0);
	lv_obj_set_style_radius(b, 28, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	if (accent) {
		lv_obj_add_style(b, &theme_style_accent_bg, 0);
	} else {
		lv_obj_set_style_bg_color(b, theme()->surface_pressed, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	}
	lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_DISABLED);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)user);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_text(l, raw ? label : tr(label));
	lv_obj_set_style_text_font(l, &font_ui_20, 0);
	lv_obj_set_style_text_color(l, accent ? lv_color_white() : theme()->text_primary, 0);
	lv_obj_center(l);
	if (button_count < (int)(sizeof(buttons) / sizeof(buttons[0]))) {
		button_accent[button_count] = accent;
		buttons[button_count++] = b;
	}
	return b;
}

static char player_actions[PLAYERS][6][24];

void homepage_init(gui_config_t *cfg) {
	snprintf(relay, sizeof(relay), "%s", config_get("home", "relay", DEFAULT_RELAY));

	lv_obj_add_style(homepage_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(homepage_screen, false);
	settingsrow_title(homepage_screen, cfg, "home");

	int top = settingsrow_content_top(cfg);
	int32_t width = (int32_t)cfg->screen_width - 2 * cfg->padding;
	lv_obj_t *page = plain(homepage_screen);
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
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_clickable(status_label, true);
	lv_obj_set_ext_click_area(status_label, 12);
	lv_obj_add_event_cb(status_label, status_clicked_cb, LV_EVENT_CLICKED, NULL);

	// KingGroot: what it is doing, then wake / sleep / shut down, then the
	// monitors and the speakers.
	lv_obj_t *kg = card(page, 0);
	lv_obj_t *head = row(kg);
	lv_obj_t *name = text(head, &font_ui_24_bold, false);
	lv_label_set_text(name, "KingGroot");
	kg_state_label = text(head, &font_ui_20, true);

	lv_obj_t *power = row(kg);
	wake_btn = button(power, "home_wake", false, action_cb, "kg_wake", false);
	sleep_btn = button(power, "home_sleep", false, action_cb, "kg_sleep", false);
	shutdown_btn = button(power, "home_shutdown", false, shutdown_cb, NULL, false);
	shutdown_label = lv_obj_get_child(shutdown_btn, 0);

	lv_obj_t *mon = row(kg);
	monitors_label = text(mon, &font_ui_18, false);
	lv_obj_set_flex_grow(monitors_label, 1);
	lv_obj_t *mb = button(mon, "home_switch", false, action_cb, "monitors", false);
	lv_obj_set_flex_grow(mb, 0);
	lv_obj_set_width(mb, 130);

	lv_obj_t *spk = row(kg);
	lv_obj_t *spk_label = text(spk, &font_ui_18, false);
	lv_label_set_text(spk_label, tr("home_speakers"));
	speakers_switch = lv_switch_create(spk);
	lv_obj_add_style(speakers_switch, &theme_style_switch, 0);
	lv_obj_add_style(speakers_switch, &theme_style_switch_checked, LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_add_event_cb(speakers_switch, speakers_cb, LV_EVENT_VALUE_CHANGED, NULL);

	// The players: name and what they are doing, the song, then the controls.
	static const char *ops[6] = {"on", "off", "voldown", "playpause", "next", "volup"};
	for (int i = 0; i < PLAYERS; i++) {
		for (int k = 0; k < 6; k++) {
			snprintf(player_actions[i][k], sizeof(player_actions[i][k]), "%s_%s", player_keys[i], ops[k]);
		}
		lv_obj_t *c = card(page, 1 + i);
		lv_obj_t *h = row(c);
		player_name[i] = text(h, &font_ui_24_bold, false);
		player_state[i] = text(h, &font_ui_20, true);
		player_title[i] = text(c, &font_ui_18, true);
		lv_obj_set_width(player_title[i], LV_PCT(100));
		lv_label_set_long_mode(player_title[i], LV_LABEL_LONG_DOT);

		lv_obj_t *r1 = row(c);
		button(r1, "-", true, action_cb, player_actions[i][2], false);
		button(r1, "home_playpause", false, action_cb, player_actions[i][3], true);
		button(r1, "home_next", false, action_cb, player_actions[i][4], false);
		button(r1, "+", true, action_cb, player_actions[i][5], false);
		lv_obj_t *r2 = row(c);
		button(r2, "home_turn_on", false, action_cb, player_actions[i][0], false);
		button(r2, "home_turn_off", false, action_cb, player_actions[i][1], false);
	}

	poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
	lv_timer_pause(poll_timer);

	theme_register_refresh(refresh_theme);
	lv_obj_add_event_cb(homepage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(homepage_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	refresh_view();
}
