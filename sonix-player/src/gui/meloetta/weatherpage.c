#include "weatherpage.h"

#include <math.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/config.h"
#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/device/system.h"
#include "src/system/net/http.h"

lv_obj_t *weatherpage_screen;

// name,latitude,longitude for each place, ';' between places.
#define DEFAULT_PLACES "Santiago,-33.45,-70.67;Artificio,-32.397,-71.096"
#define MAX_PLACES 4

// Older than this, opening the page fetches again.
#define STALE_SECS (30 * 60)
#define FETCH_TIMEOUT_SECS 15
#define RESPONSE_LIMIT (64 * 1024)

#define HOURS 8
#define HOUR_STEP 2 // so the strip covers the next sixteen hours
#define DAYS 5

// The icons' own colours, the same in every theme: the sun is yellow and rain
// is blue whatever the accent.
#define SUN_COLOR 0xF6C343
#define MOON_COLOR 0xEDE6C8
#define CLOUD_COLOR 0xB8C2CC
#define RAIN_COLOR 0x5AA9E6
#define SNOW_COLOR 0xF2F6FA
#define BOLT_COLOR 0xF6C343

typedef enum { K_CLEAR, K_PARTLY, K_CLOUDY, K_FOG, K_DRIZZLE, K_RAIN, K_SNOW, K_THUNDER, K_NONE } kind_t;

typedef struct {
	int code;
	bool day;
	double temp;
} wx_slot_t;

typedef struct {
	char name[32];
	char lat[16], lon[16];
} place_t;

typedef struct {
	bool ok;
	int place;
	time_t fetched;
	wx_slot_t now;
	double feels;
	int humidity;
	double wind;
	char hour_label[HOURS][8];
	wx_slot_t hours[HOURS];
	int hour_rain[HOURS];
	int hour_count;
	int day_wday[DAYS];
	int day_code[DAYS];
	double day_min[DAYS], day_max[DAYS];
	int day_rain[DAYS];
	int day_count;
} wx_t;

// All of this belongs to the UI thread.
static place_t places[MAX_PLACES];
static int place_count;
static int current; // the place on screen
static wx_t data[MAX_PLACES];
static bool fetching[MAX_PLACES];
static bool failed[MAX_PLACES];
static bool cache_read[MAX_PLACES];

static lv_obj_t *place_btns[MAX_PLACES];
static lv_obj_t *place_labels[MAX_PLACES];

static lv_obj_t *status_label;
static lv_obj_t *now_icon;
static lv_obj_t *temp_label;
static lv_obj_t *desc_label;
static lv_obj_t *detail_labels[3];
static lv_obj_t *hour_cols[HOURS];
static lv_obj_t *hour_time[HOURS], *hour_icon[HOURS], *hour_temp[HOURS], *hour_rain[HOURS];
static lv_obj_t *day_rows[DAYS];
static lv_obj_t *cards[2]; // the hours and the days, on the theme's surface colour
static lv_obj_t *day_name[DAYS], *day_icon[DAYS], *day_rain[DAYS], *day_temp[DAYS];

// What each icon draws, read by its draw callback: kind in the low byte, the
// day/night flag above it.
static int now_kind, hour_kinds[HOURS], day_kinds[DAYS];

// ---------------------------------------------------------------------------
// the forecast
// ---------------------------------------------------------------------------

static kind_t kind_of(int code) {
	if (code < 0) return K_NONE;
	if (code <= 1) return K_CLEAR;
	if (code == 2) return K_PARTLY;
	if (code == 3) return K_CLOUDY;
	if (code == 45 || code == 48) return K_FOG;
	if (code >= 51 && code <= 57) return K_DRIZZLE;
	if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return K_RAIN;
	if ((code >= 71 && code <= 77) || code == 85 || code == 86) return K_SNOW;
	if (code >= 95) return K_THUNDER;
	return K_CLOUDY;
}

static const char *kind_key(kind_t k) {
	static const char *const KEYS[] = {"weather_clear", "weather_partly", "weather_cloudy", "weather_fog",
									   "weather_drizzle", "weather_rain", "weather_snow", "weather_thunder"};
	return k < K_NONE ? KEYS[k] : "";
}

static const char *cache_path(int place, char *out, size_t size) {
	const char *root = storage_sd_root();
	snprintf(out, size, "%s/.local/weather-%d.json", root ? root : ".", place);
	return out;
}

// [weather] places, or the default pair.
static void read_places(void) {
	char list[256];
	snprintf(list, sizeof(list), "%s", config_get("weather", "places", DEFAULT_PLACES));
	place_count = 0;
	char *save = NULL;
	for (char *one = strtok_r(list, ";", &save); one && place_count < MAX_PLACES; one = strtok_r(NULL, ";", &save)) {
		place_t *p = &places[place_count];
		char *name = one, *lat = strchr(one, ',');
		char *lon = lat ? strchr(lat + 1, ',') : NULL;
		if (!lat || !lon) {
			continue;
		}
		*lat++ = '\0';
		*lon++ = '\0';
		snprintf(p->name, sizeof(p->name), "%s", name);
		snprintf(p->lat, sizeof(p->lat), "%s", lat);
		snprintf(p->lon, sizeof(p->lon), "%s", lon);
		place_count++;
	}
}

static double array_double(const json_doc_t *doc, int arr, int i) {
	return json_double(doc, json_at(doc, arr, i), NAN);
}

// Reads an Open-Meteo answer into `out`. False when the text is not one.
static bool parse(const char *text, wx_t *out) {
	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	int root = json_root(&doc);
	int cur = json_get(&doc, root, "current");
	if (cur < 0) {
		json_free(&doc);
		return false;
	}
	char now_time[24] = "";
	json_obj_str(&doc, cur, "time", now_time, sizeof(now_time));
	out->now.temp = json_double(&doc, json_get(&doc, cur, "temperature_2m"), NAN);
	out->now.code = (int)json_obj_long(&doc, cur, "weather_code", -1);
	out->now.day = json_obj_long(&doc, cur, "is_day", 1) != 0;
	out->feels = json_double(&doc, json_get(&doc, cur, "apparent_temperature"), NAN);
	out->humidity = (int)json_obj_long(&doc, cur, "relative_humidity_2m", -1);
	out->wind = json_double(&doc, json_get(&doc, cur, "wind_speed_10m"), NAN);

	// The hourly arrays start at midnight today; the strip starts at this hour.
	int ht = json_path(&doc, root, "hourly.time");
	int htemp = json_path(&doc, root, "hourly.temperature_2m");
	int hcode = json_path(&doc, root, "hourly.weather_code");
	int hrain = json_path(&doc, root, "hourly.precipitation_probability");
	int hday = json_path(&doc, root, "hourly.is_day");
	int n = json_len(&doc, ht), first = 0;
	char t[24];
	for (; first < n; first++) {
		json_str(&doc, json_at(&doc, ht, first), t, sizeof(t));
		if (strncmp(t, now_time, 13) >= 0) {
			break;
		}
	}
	for (int i = first + HOUR_STEP; i < n && out->hour_count < HOURS; i += HOUR_STEP) {
		int k = out->hour_count++;
		json_str(&doc, json_at(&doc, ht, i), t, sizeof(t));
		snprintf(out->hour_label[k], sizeof(out->hour_label[k]), "%.5s", strlen(t) >= 16 ? t + 11 : "");
		out->hours[k].temp = array_double(&doc, htemp, i);
		out->hours[k].code = (int)json_long(&doc, json_at(&doc, hcode, i), -1);
		out->hours[k].day = json_long(&doc, json_at(&doc, hday, i), 1) != 0;
		out->hour_rain[k] = (int)json_long(&doc, json_at(&doc, hrain, i), -1);
	}

	int dt = json_path(&doc, root, "daily.time");
	int dcode = json_path(&doc, root, "daily.weather_code");
	int dmax = json_path(&doc, root, "daily.temperature_2m_max");
	int dmin = json_path(&doc, root, "daily.temperature_2m_min");
	int drain = json_path(&doc, root, "daily.precipitation_probability_max");
	for (int i = 0; i < json_len(&doc, dt) && out->day_count < DAYS; i++) {
		int k = out->day_count++;
		struct tm tm = {0};
		json_str(&doc, json_at(&doc, dt, i), t, sizeof(t));
		if (sscanf(t, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) == 3) {
			tm.tm_year -= 1900;
			tm.tm_mon -= 1;
			tm.tm_hour = 12;
			mktime(&tm);
		}
		out->day_wday[k] = tm.tm_wday;
		out->day_code[k] = (int)json_long(&doc, json_at(&doc, dcode, i), -1);
		out->day_max[k] = array_double(&doc, dmax, i);
		out->day_min[k] = array_double(&doc, dmin, i);
		out->day_rain[k] = (int)json_long(&doc, json_at(&doc, drain, i), -1);
	}
	json_free(&doc);
	out->ok = !isnan(out->now.temp);
	return out->ok;
}

// ---------------------------------------------------------------------------
// fetching, on a thread of its own
// ---------------------------------------------------------------------------

static void fetched_cb(void *user);

typedef struct {
	int place;
	char url[640];
	char cache[600];
} job_t;

static void *fetch_main(void *arg) {
	job_t *job = arg;
	thread_be_background("weather");
	wx_t *wx = calloc(1, sizeof(*wx));
	char *body = NULL;
	if (wx && http_get(job->url, &body, NULL, RESPONSE_LIMIT, FETCH_TIMEOUT_SECS) && parse(body, wx)) {
		wx->fetched = time(NULL);
		FILE *f = storage_sd_writable() ? fopen(job->cache, "w") : NULL;
		if (f) {
			fputs(body, f);
			fclose(f);
		}
	}
	if (wx) {
		wx->place = job->place;
	}
	free(body);
	free(job);
	if (wx && !gui_post(fetched_cb, wx)) {
		free(wx);
	}
	return NULL;
}

static void refresh_view(void);

static void start_fetch(int place) {
	if (place >= place_count || fetching[place]) {
		return;
	}
	job_t *job = malloc(sizeof(*job));
	if (!job) {
		return;
	}
	job->place = place;
	cache_path(place, job->cache, sizeof(job->cache));
	snprintf(job->url, sizeof(job->url),
			 "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
			 "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
			 "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
			 "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
			 "&timezone=auto&forecast_days=%d",
			 places[place].lat, places[place].lon, DAYS);
	pthread_t t;
	if (pthread_create(&t, NULL, fetch_main, job) != 0) {
		free(job);
		return;
	}
	pthread_detach(t);
	fetching[place] = true;
	refresh_view();
}

static void fetched_cb(void *user) {
	wx_t *wx = user;
	int p = wx->place;
	fetching[p] = false;
	failed[p] = !wx->ok;
	if (wx->ok) {
		data[p] = *wx;
	}
	free(wx);
	if (p == current) {
		refresh_view();
	}
}

// The last answer from the card, when there is nothing newer: what the page
// shows before the first fetch, and with no connection.
static void load_cache(int place) {
	char path[600];
	cache_path(place, path, sizeof(path));
	char *text = NULL;
	size_t len = 0;
	FILE *f = fopen(path, "r");
	struct stat st;
	if (!f || fstat(fileno(f), &st) != 0 || st.st_size <= 0 || st.st_size > RESPONSE_LIMIT) {
		if (f) fclose(f);
		return;
	}
	text = malloc((size_t)st.st_size + 1);
	if (text) {
		len = fread(text, 1, (size_t)st.st_size, f);
		text[len] = '\0';
		wx_t wx;
		if (parse(text, &wx)) {
			wx.fetched = st.st_mtime;
			wx.place = place;
			data[place] = wx;
		}
	}
	free(text);
	fclose(f);
}

// ---------------------------------------------------------------------------
// the icons
// ---------------------------------------------------------------------------

static void circle(lv_layer_t *layer, float x, float y, float r, lv_color_t color) {
	lv_draw_rect_dsc_t dsc;
	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_color = color;
	dsc.radius = LV_RADIUS_CIRCLE;
	lv_area_t a = {(int32_t)lroundf(x - r), (int32_t)lroundf(y - r), (int32_t)lroundf(x + r), (int32_t)lroundf(y + r)};
	lv_draw_rect(layer, &dsc, &a);
}

static void line(lv_layer_t *layer, float x1, float y1, float x2, float y2, float w, lv_color_t color) {
	lv_draw_line_dsc_t dsc;
	lv_draw_line_dsc_init(&dsc);
	dsc.p1.x = (lv_value_precise_t)lroundf(x1);
	dsc.p1.y = (lv_value_precise_t)lroundf(y1);
	dsc.p2.x = (lv_value_precise_t)lroundf(x2);
	dsc.p2.y = (lv_value_precise_t)lroundf(y2);
	dsc.width = (int32_t)lroundf(w < 1.0f ? 1.0f : w);
	dsc.color = color;
	dsc.round_start = 1;
	dsc.round_end = 1;
	lv_draw_line(layer, &dsc);
}

static void sun_or_moon(lv_layer_t *layer, float cx, float cy, float s, bool day) {
	if (day) {
		for (int i = 0; i < 8; i++) {
			float a = (float)i * (float)M_PI / 4.0f;
			line(layer, cx + cosf(a) * s * 0.30f, cy + sinf(a) * s * 0.30f, cx + cosf(a) * s * 0.42f,
				 cy + sinf(a) * s * 0.42f, s * 0.06f, lv_color_hex(SUN_COLOR));
		}
		circle(layer, cx, cy, s * 0.22f, lv_color_hex(SUN_COLOR));
	} else {
		circle(layer, cx, cy, s * 0.26f, lv_color_hex(MOON_COLOR));
		circle(layer, cx + s * 0.13f, cy - s * 0.09f, s * 0.22f, theme()->screen_bg);
	}
}

// A cloud whose flat bottom sits at `base`, `w` wide, centred on `cx`.
static void cloud(lv_layer_t *layer, float cx, float base, float w, lv_color_t color) {
	float r = w * 0.20f;
	circle(layer, cx - w * 0.24f, base - r, r, color);
	circle(layer, cx + w * 0.02f, base - r * 1.55f, r * 1.35f, color);
	circle(layer, cx + w * 0.27f, base - r * 0.95f, r * 0.95f, color);
	lv_draw_rect_dsc_t dsc;
	lv_draw_rect_dsc_init(&dsc);
	dsc.bg_color = color;
	dsc.radius = (int32_t)lroundf(r);
	lv_area_t a = {(int32_t)lroundf(cx - w * 0.44f), (int32_t)lroundf(base - r * 1.4f), (int32_t)lroundf(cx + w * 0.44f),
				   (int32_t)lroundf(base)};
	lv_draw_rect(layer, &dsc, &a);
}

static void icon_draw_cb(lv_event_t *e) {
	lv_layer_t *layer = lv_event_get_layer(e);
	const int *slot = lv_event_get_user_data(e);
	if (!layer || !slot || (*slot & 0xFF) == K_NONE) {
		return;
	}
	kind_t k = (kind_t)(*slot & 0xFF);
	bool day = (*slot >> 8) != 0;
	lv_area_t area;
	lv_obj_get_coords(lv_event_get_current_target(e), &area);
	float s = (float)lv_area_get_width(&area);
	float cx = (float)area.x1 + s / 2.0f, cy = (float)area.y1 + s / 2.0f;
	lv_color_t grey = lv_color_hex(CLOUD_COLOR);

	switch (k) {
	case K_CLEAR:
		sun_or_moon(layer, cx, cy, s, day);
		break;
	case K_PARTLY:
		sun_or_moon(layer, cx - s * 0.14f, cy - s * 0.14f, s * 0.85f, day);
		cloud(layer, cx + s * 0.06f, cy + s * 0.30f, s * 0.70f, grey);
		break;
	case K_CLOUDY:
		cloud(layer, cx, cy + s * 0.22f, s * 0.86f, grey);
		break;
	case K_FOG:
		for (int i = 0; i < 3; i++) {
			float y = cy - s * 0.18f + (float)i * s * 0.18f;
			line(layer, cx - s * 0.36f + (float)(i % 2) * s * 0.08f, y, cx + s * 0.36f - (float)((i + 1) % 2) * s * 0.08f,
				 y, s * 0.07f, grey);
		}
		break;
	case K_DRIZZLE:
	case K_RAIN:
	case K_SNOW:
	case K_THUNDER:
		cloud(layer, cx, cy + s * 0.06f, s * 0.86f, grey);
		for (int i = 0; i < 3; i++) {
			float x = cx - s * 0.20f + (float)i * s * 0.20f, y = cy + s * 0.20f;
			if (k == K_SNOW) {
				circle(layer, x, y + s * 0.06f, s * 0.05f, lv_color_hex(SNOW_COLOR));
			} else if (k == K_DRIZZLE) {
				circle(layer, x, y + s * 0.06f, s * 0.035f, lv_color_hex(RAIN_COLOR));
			} else if (k == K_RAIN) {
				line(layer, x + s * 0.04f, y, x - s * 0.03f, y + s * 0.16f, s * 0.05f, lv_color_hex(RAIN_COLOR));
			}
		}
		if (k == K_THUNDER) {
			lv_draw_triangle_dsc_t t;
			lv_draw_triangle_dsc_init(&t);
			t.color = lv_color_hex(BOLT_COLOR);
			t.opa = LV_OPA_COVER;
			t.p[0].x = (lv_value_precise_t)(cx + s * 0.06f), t.p[0].y = (lv_value_precise_t)(cy + s * 0.08f);
			t.p[1].x = (lv_value_precise_t)(cx - s * 0.12f), t.p[1].y = (lv_value_precise_t)(cy + s * 0.30f);
			t.p[2].x = (lv_value_precise_t)(cx + s * 0.02f), t.p[2].y = (lv_value_precise_t)(cy + s * 0.30f);
			lv_draw_triangle(layer, &t);
			t.p[0].x = (lv_value_precise_t)(cx + s * 0.08f), t.p[0].y = (lv_value_precise_t)(cy + s * 0.24f);
			t.p[1].x = (lv_value_precise_t)(cx - s * 0.06f), t.p[1].y = (lv_value_precise_t)(cy + s * 0.24f);
			t.p[2].x = (lv_value_precise_t)(cx - s * 0.08f), t.p[2].y = (lv_value_precise_t)(cy + s * 0.46f);
			lv_draw_triangle(layer, &t);
		}
		break;
	default:
		break;
	}
}

static lv_obj_t *icon_create(lv_obj_t *parent, int size, int *slot) {
	lv_obj_t *obj = lv_obj_create(parent);
	lv_obj_set_size(obj, size, size);
	lv_obj_set_style_bg_opa(obj, 0, 0);
	lv_obj_set_style_border_width(obj, 0, 0);
	lv_obj_set_style_pad_all(obj, 0, 0);
	lv_obj_set_clickable(obj, false);
	lv_obj_set_scrollable(obj, false);
	*slot = K_NONE;
	lv_obj_add_event_cb(obj, icon_draw_cb, LV_EVENT_DRAW_MAIN, slot);
	return obj;
}

static void set_icon(lv_obj_t *obj, int *slot, int code, bool day) {
	*slot = (int)kind_of(code) | (day ? 0x100 : 0);
	lv_obj_invalidate(obj);
}

// ---------------------------------------------------------------------------
// the page
// ---------------------------------------------------------------------------

static void fmt_temp(char *out, size_t size, double t) {
	if (isnan(t)) {
		snprintf(out, size, "--");
	} else {
		snprintf(out, size, "%d\xC2\xB0", (int)lround(t));
	}
}

// One line of a language list ("Sun\nMon\n..."), `index` counted from 0.
static void list_item(const char *list, int index, char *out, size_t size) {
	for (int i = 0; i < index && list; i++) {
		list = strchr(list, '\n');
		list = list ? list + 1 : NULL;
	}
	size_t n = list ? strcspn(list, "\n") : 0;
	snprintf(out, size, "%.*s", (int)(n < size ? n : size - 1), list ? list : "");
}

static void refresh_view(void) {
	const wx_t *wx = &data[current];
	char buf[96], when[16] = "";
	bool have = wx->ok;
	for (int i = 0; i < place_count; i++) {
		bool on = i == current;
		lv_obj_set_style_bg_color(place_btns[i], on ? theme()->accent : theme()->surface_pressed, 0);
		lv_obj_set_style_text_color(place_labels[i], on ? lv_color_white() : theme()->text_primary, 0);
	}
	if (have) {
		struct tm local;
		localtime_r(&wx->fetched, &local);
		snprintf(when, sizeof(when), "%02d:%02d", local.tm_hour, local.tm_min);
	}
	if (fetching[current]) {
		snprintf(buf, sizeof(buf), "%s", tr("weather_updating"));
	} else if (!have) {
		snprintf(buf, sizeof(buf), "%s", tr(failed[current] ? "weather_never" : "weather_updating"));
	} else {
		snprintf(buf, sizeof(buf), tr(failed[current] ? "weather_offline" : "weather_updated"), when);
	}
	lv_label_set_text(status_label, buf);

	lv_obj_t *now_parts[] = {now_icon, temp_label, desc_label, detail_labels[0], detail_labels[1], detail_labels[2]};
	for (size_t i = 0; i < sizeof(now_parts) / sizeof(now_parts[0]); i++) {
		lv_obj_set_hidden(now_parts[i], !have);
	}
	if (!have) {
		for (int i = 0; i < HOURS; i++) lv_obj_set_hidden(hour_cols[i], true);
		for (int i = 0; i < DAYS; i++) lv_obj_set_hidden(day_rows[i], true);
		return;
	}

	set_icon(now_icon, &now_kind, wx->now.code, wx->now.day);
	fmt_temp(buf, sizeof(buf), wx->now.temp);
	lv_label_set_text(temp_label, buf);
	lv_label_set_text(desc_label, tr(kind_key(kind_of(wx->now.code))));
	char t[16];
	fmt_temp(t, sizeof(t), wx->feels);
	lv_label_set_text_fmt(detail_labels[0], tr("weather_feels"), t);
	lv_label_set_text_fmt(detail_labels[1], tr("weather_humidity"), wx->humidity);
	lv_label_set_text_fmt(detail_labels[2], tr("weather_wind"), (int)lround(isnan(wx->wind) ? 0 : wx->wind));

	for (int i = 0; i < HOURS; i++) {
		bool on = i < wx->hour_count;
		lv_obj_set_hidden(hour_cols[i], !on);
		if (!on) continue;
		lv_label_set_text(hour_time[i], wx->hour_label[i]);
		set_icon(hour_icon[i], &hour_kinds[i], wx->hours[i].code, wx->hours[i].day);
		fmt_temp(t, sizeof(t), wx->hours[i].temp);
		lv_label_set_text(hour_temp[i], t);
		if (wx->hour_rain[i] > 0) {
			lv_label_set_text_fmt(hour_rain[i], "%d%%", wx->hour_rain[i]);
		} else {
			lv_label_set_text(hour_rain[i], "");
		}
	}

	for (int i = 0; i < DAYS; i++) {
		bool on = i < wx->day_count;
		lv_obj_set_hidden(day_rows[i], !on);
		if (!on) continue;
		if (i == 0) {
			lv_label_set_text(day_name[i], tr("weather_today"));
		} else {
			list_item(tr("weather_weekdays"), wx->day_wday[i], buf, sizeof(buf));
			lv_label_set_text(day_name[i], buf);
		}
		set_icon(day_icon[i], &day_kinds[i], wx->day_code[i], true);
		if (wx->day_rain[i] > 0) {
			lv_label_set_text_fmt(day_rain[i], "%d%%", wx->day_rain[i]);
		} else {
			lv_label_set_text(day_rain[i], "");
		}
		char lo[16], hi[16];
		fmt_temp(lo, sizeof(lo), wx->day_min[i]);
		fmt_temp(hi, sizeof(hi), wx->day_max[i]);
		lv_label_set_text_fmt(day_temp[i], "%s / %s", lo, hi);
	}
}

// The colours set by hand, again after a theme or accent change.
static void refresh_theme(void) {
	for (int i = 0; i < 2; i++) {
		lv_obj_set_style_bg_color(cards[i], theme()->surface, 0);
	}
	lv_obj_invalidate(weatherpage_screen);
}

static void status_clicked_cb(lv_event_t *e) {
	(void)e;
	start_fetch(current);
}

// The place on screen: its last answer from the card the first time, and a
// fetch when what there is has gone stale.
static void show_place(int place) {
	current = place;
	if (!cache_read[place]) {
		cache_read[place] = true;
		load_cache(place);
	}
	refresh_view();
	if (!data[place].ok || time(NULL) - data[place].fetched > STALE_SECS) {
		start_fetch(place);
	}
}

static void place_clicked_cb(lv_event_t *e) {
	int place = (int)(intptr_t)lv_event_get_user_data(e);
	if (place == current) {
		return;
	}
	config_set_int("weather", "place", place);
	config_save();
	show_place(place);
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	show_place(current);
}

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

void weatherpage_init(gui_config_t *cfg) {
	read_places();
	current = (int)config_get_int("weather", "place", 0);
	if (current < 0 || current >= place_count) {
		current = 0;
	}

	lv_obj_add_style(weatherpage_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(weatherpage_screen, false);
	settingsrow_title(weatherpage_screen, cfg, "weather");

	int top = settingsrow_content_top(cfg);
	int32_t width = (int32_t)cfg->screen_width - 2 * cfg->padding;
	lv_obj_t *page = plain(weatherpage_screen);
	lv_obj_set_pos(page, cfg->padding, top);
	lv_obj_set_size(page, width, (int32_t)cfg->screen_height - top);
	lv_obj_set_scrollable(page, true);
	lv_obj_set_scroll_dir(page, LV_DIR_VER);
	lv_obj_set_style_pad_bottom(page, cfg->padding, 0);
	lv_obj_set_style_pad_row(page, 14, 0);
	lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// One pill per place, the chosen one in the accent colour.
	lv_obj_t *pills = plain(page);
	lv_obj_set_size(pills, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(pills, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(pills, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(pills, 10, 0);
	for (int i = 0; i < place_count; i++) {
		lv_obj_t *btn = place_btns[i] = lv_obj_create(pills);
		lv_obj_set_size(btn, LV_SIZE_CONTENT, 44);
		lv_obj_set_style_radius(btn, 22, 0);
		lv_obj_set_style_border_width(btn, 0, 0);
		lv_obj_set_style_pad_hor(btn, 20, 0);
		lv_obj_set_style_pad_ver(btn, 0, 0);
		lv_obj_set_scrollable(btn, false);
		lv_obj_add_event_cb(btn, place_clicked_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		place_labels[i] = lv_label_create(btn);
		lv_label_set_text(place_labels[i], places[i].name);
		lv_obj_set_style_text_font(place_labels[i], &font_ui_20, 0);
		lv_obj_center(place_labels[i]);
	}

	status_label = text(page, &font_ui_18, true);
	lv_obj_set_width(status_label, LV_PCT(100));
	lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_clickable(status_label, true);
	lv_obj_set_ext_click_area(status_label, 12);
	lv_obj_add_event_cb(status_label, status_clicked_cb, LV_EVENT_CLICKED, NULL);

	// Now: the icon beside the temperature, the words under them.
	lv_obj_t *now_row = plain(page);
	lv_obj_set_size(now_row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(now_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(now_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(now_row, 18, 0);
	now_icon = icon_create(now_row, 112, &now_kind);
	temp_label = text(now_row, &font_ui_72, false);

	desc_label = text(page, &font_ui_24, false);

	lv_obj_t *details = plain(page);
	lv_obj_set_size(details, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(details, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(details, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	for (int i = 0; i < 3; i++) {
		detail_labels[i] = text(details, &font_ui_16, true);
	}

	// The next hours, as columns across the page.
	lv_obj_t *hours = cards[0] = plain(page);
	lv_obj_set_size(hours, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(hours, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(hours, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_bg_color(hours, theme()->surface, 0);
	lv_obj_set_style_bg_opa(hours, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(hours, 16, 0);
	lv_obj_set_style_pad_all(hours, 10, 0);
	for (int i = 0; i < HOURS; i++) {
		lv_obj_t *col = hour_cols[i] = plain(hours);
		lv_obj_set_size(col, (width - 20) / HOURS, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(col, 4, 0);
		hour_time[i] = text(col, &font_ui_14, true);
		hour_icon[i] = icon_create(col, 34, &hour_kinds[i]);
		hour_temp[i] = text(col, &font_ui_18, false);
		hour_rain[i] = text(col, &font_ui_14, true);
		lv_obj_set_style_text_color(hour_rain[i], lv_color_hex(RAIN_COLOR), 0);
	}

	// The days, one row each.
	lv_obj_t *days = cards[1] = plain(page);
	lv_obj_set_size(days, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(days, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_bg_color(days, theme()->surface, 0);
	lv_obj_set_style_bg_opa(days, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(days, 16, 0);
	lv_obj_set_style_pad_hor(days, 16, 0);
	lv_obj_set_style_pad_ver(days, 6, 0);
	for (int i = 0; i < DAYS; i++) {
		lv_obj_t *row = day_rows[i] = plain(days);
		lv_obj_set_size(row, LV_PCT(100), 44);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(row, 10, 0);
		day_name[i] = text(row, &font_ui_20, false);
		lv_obj_set_flex_grow(day_name[i], 1);
		day_rain[i] = text(row, &font_ui_16, false);
		lv_obj_set_style_text_color(day_rain[i], lv_color_hex(RAIN_COLOR), 0);
		day_icon[i] = icon_create(row, 32, &day_kinds[i]);
		day_temp[i] = text(row, &font_ui_20, false);
		lv_obj_set_width(day_temp[i], 120);
		lv_obj_set_style_text_align(day_temp[i], LV_TEXT_ALIGN_RIGHT, 0);
	}

	theme_register_refresh(refresh_theme);
	lv_obj_add_event_cb(weatherpage_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
