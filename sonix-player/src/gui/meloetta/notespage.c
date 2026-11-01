#include "notespage.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/core/config.h"
#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/meloetta/notesmd.h"
#include "src/system/net/http.h"

lv_obj_t *notespage_screen;
lv_obj_t *notesview_screen;

#define DEFAULT_SERVER "http://192.168.100.11:8092"
#define FOLDER "Notes"
#define SYNC_FILE ".sync"
#define MAX_NOTES 400
#define MAX_NOTE_BYTES (256 * 1024) // bigger files are left alone, by the list and the sync
#define MAX_EDIT_BYTES (64 * 1024)	// what the editor takes: a textarea re-wraps all of it per key
#define NAME_LEN 200
#define PATH_LEN 512
#define SYNC_TIMEOUT 20
#define SYNC_LIMIT (4 * 1024 * 1024)
#define AUTO_SYNC_GAP 60 // seconds: the list opening twice in a row asks once

static gui_config_t *config;
static char dir[PATH_LEN];
static char server[160];

// ---------------------------------------------------------------------------
// small things
// ---------------------------------------------------------------------------

static bool gesture_not_tap(void) { return player_sheet_drag_active() || switcher_back_drag_active(); }

static lv_obj_t *plain(lv_obj_t *parent) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_set_style_bg_opa(o, 0, 0);
	lv_obj_set_style_border_width(o, 0, 0);
	lv_obj_set_style_radius(o, 0, 0);
	lv_obj_set_style_pad_all(o, 0, 0);
	lv_obj_set_scrollable(o, false);
	return o;
}

static lv_obj_t *corner_button(lv_obj_t *parent, const lv_image_dsc_t *icon, int slot, lv_event_cb_t cb) {
	lv_obj_t *b = lv_btn_create(parent);
	lv_obj_set_size(b, 56, 56);
	lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -config->padding - slot * 64, config->padding + config->top_bar_height);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(b, 0, 0);
	lv_obj_set_style_shadow_width(b, 0, 0);
	lv_obj_set_style_pad_all(b, 0, 0);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *g = lv_image_create(b);
	lv_image_set_src(g, icon);
	lv_obj_add_style(g, &theme_style_icon, 0);
	lv_obj_center(g);
	return b;
}

static bool is_note_name(const char *name) {
	size_t n = strlen(name);
	if (!n || n > NAME_LEN || name[0] == '.' || strchr(name, '/') || strchr(name, '\n')) {
		return false;
	}
	return has_extension(name, ".md") || has_extension(name, ".txt");
}

static void note_path(const char *name, char *out, size_t size) { snprintf(out, size, "%s/%s", dir, name); }

// FNV-1a, 64 bits, as hex: CL4P-TP computes the same over the same bytes.
static void hash_hex(const char *s, size_t n, char out[17]) {
	uint64_t h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; i++) {
		h ^= (unsigned char)s[i];
		h *= 1099511628211ULL;
	}
	snprintf(out, 17, "%016" PRIx64, h);
}

static char *read_all(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		return NULL;
	}
	struct stat st;
	if (fstat(fileno(f), &st) != 0 || st.st_size > MAX_NOTE_BYTES) {
		fclose(f);
		return NULL;
	}
	char *buf = malloc((size_t)st.st_size + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	size_t got = fread(buf, 1, (size_t)st.st_size, f);
	fclose(f);
	buf[got] = 0;
	*len = got;
	return buf;
}

// Written beside the note and renamed over it, so a flat battery mid-write
// leaves the old note and not half of the new one.
static bool write_all(const char *path, const char *data, size_t len) {
	char tmp[PATH_LEN + 16];
	snprintf(tmp, sizeof(tmp), "%s/.tmp-note", dir);
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		return false;
	}
	bool ok = fwrite(data, 1, len, f) == len;
	ok = fflush(f) == 0 && ok;
	fsync(fileno(f));
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

static bool ensure_dir(void) {
	if (!config->sd_root_path || !gui_card_available()) {
		return false;
	}
	snprintf(dir, sizeof(dir), "%s/%s", config->sd_root_path, FOLDER);
	return mkdir(dir, 0755) == 0 || errno == EEXIST;
}

// ---------------------------------------------------------------------------
// the last agreed hashes (Notes/.sync: "<hash> <name>" a line)
// ---------------------------------------------------------------------------

typedef struct {
	char name[NAME_LEN + 1];
	char hash[17];
} base_t;

static base_t *bases;
static int base_count;

static void bases_load(void) {
	free(bases);
	bases = NULL;
	base_count = 0;
	char path[PATH_LEN];
	note_path(SYNC_FILE, path, sizeof(path));
	size_t len = 0;
	char *text = read_all(path, &len);
	if (!text) {
		return;
	}
	bases = calloc(MAX_NOTES, sizeof(base_t));
	for (char *line = strtok(text, "\n"); line && bases && base_count < MAX_NOTES; line = strtok(NULL, "\n")) {
		if (strlen(line) > 17 && line[16] == ' ' && is_note_name(line + 17)) {
			memcpy(bases[base_count].hash, line, 16);
			snprintf(bases[base_count].name, sizeof(bases[base_count].name), "%s", line + 17);
			base_count++;
		}
	}
	free(text);
}

static const char *base_of(const char *name) {
	for (int i = 0; i < base_count; i++) {
		if (strcmp(bases[i].name, name) == 0) {
			return bases[i].hash;
		}
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the folder
// ---------------------------------------------------------------------------

typedef struct {
	char name[NAME_LEN + 1];
	time_t mtime;
	off_t size;
} note_t;

static note_t *notes;
static int note_count;

static int by_newest(const void *a, const void *b) {
	const note_t *x = a, *y = b;
	if (x->mtime != y->mtime) {
		return x->mtime < y->mtime ? 1 : -1;
	}
	return strcasecmp(x->name, y->name);
}

static void scan(void) {
	note_count = 0;
	if (!notes) {
		notes = calloc(MAX_NOTES, sizeof(note_t));
		if (!notes) {
			return;
		}
	}
	DIR *d = opendir(dir);
	if (!d) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(d)) && note_count < MAX_NOTES) {
		if (!is_note_name(de->d_name)) {
			continue;
		}
		char path[PATH_LEN];
		note_path(de->d_name, path, sizeof(path));
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > MAX_NOTE_BYTES) {
			continue;
		}
		note_t *n = &notes[note_count++];
		snprintf(n->name, sizeof(n->name), "%s", de->d_name);
		n->mtime = st.st_mtime;
		n->size = st.st_size;
	}
	closedir(d);
	qsort(notes, (size_t)note_count, sizeof(note_t), by_newest);
}

// "Shopping.md" -> "Shopping"
static void display_name(const char *name, char *out, size_t size) {
	snprintf(out, size, "%s", name);
	char *dot = strrchr(out, '.');
	if (dot && dot != out) {
		*dot = 0;
	}
}

// ---------------------------------------------------------------------------
// sync with CL4P-TP
//
// Request:  SONIX-NOTES 1, then "F <len> <base|-> <name>\n<bytes>\n" for each
//           note and "B <base> <name>\n" for a note deleted here since the
//           last sync, then END.
// Answer:   SONIX-NOTES 1, then "W <len> <name>\n<bytes>\n" to write,
//           "D <name>\n" to delete, "K <hash> <name>\n" for every note both
//           sides now agree on, then END. Nothing is applied without the END.
// ---------------------------------------------------------------------------

typedef struct {
	char name[NAME_LEN + 1];
	time_t mtime;
	off_t size;
} sent_t;

typedef struct {
	char *body;
	size_t body_len;
	char *answer;
	size_t answer_len;
	bool ok;
	bool manual;
	sent_t *sent;
	int sent_count;
} sync_job_t;

static bool syncing;
static bool sync_again;
static time_t last_sync;
static time_t last_sync_ok;
static bool last_sync_failed;

static lv_obj_t *list_status;
static void list_rebuild(void);
static void view_reload_if_open(const char *name, bool deleted);
static bool editing_name(const char *name);

typedef struct {
	char *p;
	size_t len, cap;
} buf_t;

static bool buf_add(buf_t *b, const char *s, size_t n) {
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap : 4096;
		while (cap < b->len + n + 1) {
			cap *= 2;
		}
		char *p = realloc(b->p, cap);
		if (!p) {
			return false;
		}
		b->p = p;
		b->cap = cap;
	}
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = 0;
	return true;
}

static bool buf_fmt(buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool buf_fmt(buf_t *b, const char *fmt, ...) {
	char line[NAME_LEN + 64];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	return n > 0 && (size_t)n < sizeof(line) && buf_add(b, line, (size_t)n);
}

static void sync_done_cb(void *user);

static void *sync_main(void *arg) {
	sync_job_t *job = arg;
	thread_be_background("notes");
	char url[200];
	snprintf(url, sizeof(url), "%s/sync", server);
	http_req_t req = {0};
	req.method = "POST";
	req.body = job->body;
	req.body_len = job->body_len;
	req.content_type = "text/plain; charset=utf-8";
	int status = 0;
	job->ok = http_request(url, &req, &job->answer, &job->answer_len, SYNC_LIMIT, SYNC_TIMEOUT, &status) &&
			  status == 200 && job->answer && strncmp(job->answer, "SONIX-NOTES 1\n", 14) == 0;
	if (!gui_post(sync_done_cb, job)) {
		free(job->body);
		free(job->answer);
		free(job->sent);
		free(job);
	}
	return NULL;
}

static void sync_start(bool manual) {
	if (syncing) {
		sync_again = true;
		return;
	}
	if (!ensure_dir()) {
		if (manual) {
			gui_notify_no_card();
		}
		return;
	}
	scan();
	bases_load();

	sync_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->manual = manual;
	job->sent = calloc((size_t)(note_count ? note_count : 1), sizeof(sent_t));
	buf_t b = {0};
	bool ok = job->sent && buf_add(&b, "SONIX-NOTES 1\n", 14);
	for (int i = 0; ok && i < note_count; i++) {
		char path[PATH_LEN];
		note_path(notes[i].name, path, sizeof(path));
		size_t len = 0;
		char *text = read_all(path, &len);
		if (!text) {
			continue;
		}
		const char *base = base_of(notes[i].name);
		ok = buf_fmt(&b, "F %zu %s %s\n", len, base ? base : "-", notes[i].name) && buf_add(&b, text, len) &&
			 buf_add(&b, "\n", 1);
		free(text);
		sent_t *s = &job->sent[job->sent_count++];
		snprintf(s->name, sizeof(s->name), "%s", notes[i].name);
		s->mtime = notes[i].mtime;
		s->size = notes[i].size;
	}
	// Agreed once and gone now: deleted on the player.
	for (int i = 0; ok && i < base_count; i++) {
		bool present = false;
		for (int j = 0; j < note_count && !present; j++) {
			present = strcmp(notes[j].name, bases[i].name) == 0;
		}
		if (!present) {
			ok = buf_fmt(&b, "B %s %s\n", bases[i].hash, bases[i].name);
		}
	}
	ok = ok && buf_add(&b, "END\n", 4);
	if (!ok) {
		free(b.p);
		free(job->sent);
		free(job);
		return;
	}
	job->body = b.p;
	job->body_len = b.len;

	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&th, &attr, sync_main, job) != 0) {
		free(job->body);
		free(job->sent);
		free(job);
	} else {
		syncing = true;
		last_sync = time(NULL);
		if (manual) {
			toast_busy(tr("notes_syncing"));
		}
	}
	pthread_attr_destroy(&attr);
}

// True when the note on the card is still the one that went to CL4P-TP: an
// answer about a note saved again since must not be applied to it.
static bool unchanged_since_sent(const sync_job_t *job, const char *name) {
	char path[PATH_LEN];
	note_path(name, path, sizeof(path));
	struct stat st;
	bool exists = stat(path, &st) == 0;
	for (int i = 0; i < job->sent_count; i++) {
		if (strcmp(job->sent[i].name, name) == 0) {
			return exists && st.st_mtime == job->sent[i].mtime && st.st_size == job->sent[i].size;
		}
	}
	return !exists; // not sent: fine only if it is still not there
}

typedef struct {
	char name[NAME_LEN + 1];
	char hash[17];
} keep_t;

static bool apply_answer(sync_job_t *job) {
	const char *p = job->answer + 14;
	const char *end = job->answer + job->answer_len;
	// First pass: is it whole?
	const char *q = p;
	bool whole = false;
	while (q < end) {
		const char *nl = memchr(q, '\n', (size_t)(end - q));
		if (!nl) {
			break;
		}
		if (nl - q == 3 && memcmp(q, "END", 3) == 0) {
			whole = true;
			break;
		}
		if (q[0] == 'W') {
			size_t len = strtoul(q + 2, NULL, 10);
			q = nl + 1 + len + 1;
			continue;
		}
		q = nl + 1;
	}
	if (!whole) {
		return false;
	}

	keep_t *keep = calloc(MAX_NOTES, sizeof(keep_t));
	int keep_count = 0;
	bool skipped = false;
	char name[NAME_LEN + 1];
	while (p < end) {
		const char *nl = memchr(p, '\n', (size_t)(end - p));
		if (!nl || (nl - p == 3 && memcmp(p, "END", 3) == 0)) {
			break;
		}
		char kind = p[0];
		if (kind == 'W') {
			char *after;
			size_t len = strtoul(p + 2, &after, 10);
			size_t nlen = (size_t)(nl - after - 1);
			const char *data = nl + 1;
			if (*after == ' ' && nlen <= NAME_LEN && data + len <= end) {
				memcpy(name, after + 1, nlen);
				name[nlen] = 0;
				if (is_note_name(name)) {
					if (editing_name(name) || !unchanged_since_sent(job, name)) {
						skipped = true;
					} else {
						char path[PATH_LEN];
						note_path(name, path, sizeof(path));
						if (write_all(path, data, len)) {
							view_reload_if_open(name, false);
						}
					}
				}
			}
			p = data + len + 1;
			continue;
		}
		size_t llen = (size_t)(nl - p);
		if (kind == 'D' && llen > 2 && llen - 2 <= NAME_LEN) {
			memcpy(name, p + 2, llen - 2);
			name[llen - 2] = 0;
			if (is_note_name(name)) {
				if (editing_name(name) || !unchanged_since_sent(job, name)) {
					skipped = true;
				} else {
					char path[PATH_LEN];
					note_path(name, path, sizeof(path));
					unlink(path);
					view_reload_if_open(name, true);
				}
			}
		} else if (kind == 'K' && llen > 19 && p[18] == ' ' && llen - 19 <= NAME_LEN && keep &&
				   keep_count < MAX_NOTES) {
			keep_t *k = &keep[keep_count];
			memcpy(k->hash, p + 2, 16);
			k->hash[16] = 0;
			memcpy(k->name, p + 19, llen - 19);
			k->name[llen - 19] = 0;
			if (is_note_name(k->name)) {
				keep_count++;
			}
		}
		p = nl + 1;
	}

	// The new hashes, except for notes that were not touched because they
	// changed here meanwhile: those keep the old one, so the next sync sees
	// both sides moved and keeps both.
	buf_t b = {0};
	for (int i = 0; keep && i < keep_count; i++) {
		const char *hash = keep[i].hash;
		if (editing_name(keep[i].name) || !unchanged_since_sent(job, keep[i].name)) {
			// The note may be one written above, which then counts as sent.
			char path[PATH_LEN];
			note_path(keep[i].name, path, sizeof(path));
			size_t len = 0;
			char *text = read_all(path, &len);
			char now[17] = "";
			if (text) {
				hash_hex(text, len, now);
				free(text);
			}
			if (strcmp(now, hash) != 0) {
				hash = base_of(keep[i].name);
				if (!hash) {
					continue;
				}
			}
		}
		buf_fmt(&b, "%s %s\n", hash, keep[i].name);
	}
	char path[PATH_LEN];
	note_path(SYNC_FILE, path, sizeof(path));
	write_all(path, b.p ? b.p : "", b.len);
	free(b.p);
	free(keep);
	if (skipped) {
		sync_again = true;
	}
	return true;
}

static void list_status_refresh(void) {
	if (!list_status) {
		return;
	}
	char text[128];
	if (syncing) {
		snprintf(text, sizeof(text), "%s", tr("notes_syncing"));
	} else if (last_sync_failed) {
		snprintf(text, sizeof(text), "%s", tr("notes_not_synced"));
	} else if (last_sync_ok) {
		struct tm tm;
		localtime_r(&last_sync_ok, &tm);
		char hm[16];
		strftime(hm, sizeof(hm), "%H:%M", &tm);
		snprintf(text, sizeof(text), tr("notes_synced_at"), hm);
	} else {
		text[0] = 0;
	}
	lv_label_set_text(list_status, text);
}

static void sync_done_cb(void *user) {
	sync_job_t *job = user;
	syncing = false;
	bool applied = job->ok && apply_answer(job);
	last_sync_failed = !applied;
	if (applied) {
		last_sync_ok = time(NULL);
	}
	if (job->manual) {
		toast_busy_end();
		if (applied) {
			toast_success(tr("notes_synced"));
		} else {
			toast_error(tr("notes_sync_failed"));
		}
	}
	free(job->body);
	free(job->answer);
	free(job->sent);
	free(job);

	if (lv_screen_active() == notespage_screen) {
		list_rebuild();
	}
	list_status_refresh();
	if (sync_again) {
		sync_again = false;
		sync_start(false);
	}
}

// ---------------------------------------------------------------------------
// the list
// ---------------------------------------------------------------------------

static lv_obj_t *list_col;
static lv_obj_t *list_empty;
static lv_obj_t *list_title;

static char open_name[NAME_LEN + 1]; // the note on the view page
static void view_open(const char *name, bool edit);

static void row_click_cb(lv_event_t *e) {
	if (gesture_not_tap()) {
		return;
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i >= 0 && i < note_count) {
		view_open(notes[i].name, false);
	}
}

static char delete_name[NAME_LEN + 1];

static void delete_ok(void *user) {
	(void)user;
	char path[PATH_LEN];
	note_path(delete_name, path, sizeof(path));
	if (unlink(path) == 0) {
		list_rebuild();
		sync_start(false);
	}
}

static void row_long_cb(lv_event_t *e) {
	if (gesture_not_tap()) {
		return;
	}
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= note_count) {
		return;
	}
	snprintf(delete_name, sizeof(delete_name), "%s", notes[i].name);
	char shown[NAME_LEN + 1];
	display_name(delete_name, shown, sizeof(shown));
	confirm_show(tr("notes_delete_title"), shown, tr("notes_delete"), delete_ok, NULL);
	// The long press ends in a release, and a release over the row is a click.
	lv_indev_wait_release(lv_indev_active());
}

static void list_rebuild(void) {
	scan();
	lv_obj_clean(list_col);
	lv_obj_set_hidden(list_empty, note_count > 0);
	time_t now = time(NULL);
	struct tm today;
	localtime_r(&now, &today);
	for (int i = 0; i < note_count; i++) {
		lv_obj_t *row = lv_obj_create(list_col);
		lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
		lv_obj_add_style(row, &theme_style_card, 0);
		lv_obj_add_style(row, &theme_style_card_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_border_width(row, 0, 0);
		lv_obj_set_style_shadow_width(row, 0, 0);
		lv_obj_set_style_radius(row, 14, 0);
		lv_obj_set_style_pad_hor(row, 18, 0);
		lv_obj_set_style_pad_ver(row, 16, 0);
		lv_obj_set_scrollable(row, false);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(row, 12, 0);
		lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_obj_add_event_cb(row, row_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
		lv_obj_set_event_bubble(row, true);

		char shown[NAME_LEN + 1];
		display_name(notes[i].name, shown, sizeof(shown));
		lv_obj_t *name = lv_label_create(row);
		lv_label_set_text(name, shown);
		lv_obj_add_style(name, &theme_style_text, 0);
		lv_obj_set_style_text_font(name, &font_ui_24, 0);
		lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
		lv_obj_set_flex_grow(name, 1);

		struct tm tm;
		localtime_r(&notes[i].mtime, &tm);
		char when[24];
		if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday) {
			strftime(when, sizeof(when), "%H:%M", &tm);
		} else {
			strftime(when, sizeof(when), "%d/%m", &tm);
		}
		lv_obj_t *date = lv_label_create(row);
		lv_label_set_text(date, when);
		lv_obj_add_style(date, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(date, &font_ui_18, 0);
	}
}

// --- the new-note dialog, in the shape of the file manager's naming dialog

static lv_obj_t *name_layer;
static lv_obj_t *name_field;
static keyboard_t *name_keyboard;

static void name_layer_hide(void) {
	lv_obj_set_hidden(name_layer, true);
	back_btn_hide_on_screen(notespage_screen, false);
}

static void new_cb(lv_event_t *e) {
	(void)e;
	if (!ensure_dir()) {
		gui_notify_no_card();
		return;
	}
	lv_textarea_set_text(name_field, "");
	keyboard_reset(name_keyboard);
	lv_obj_set_hidden(name_layer, false);
	lv_obj_move_foreground(name_layer);
	back_btn_hide_on_screen(notespage_screen, true);
}

static void name_cancel_cb(lv_event_t *e) {
	(void)e;
	name_layer_hide();
}

static void name_accept_cb(lv_event_t *e) {
	(void)e;
	char typed[NAME_LEN + 1];
	snprintf(typed, sizeof(typed), "%s", lv_textarea_get_text(name_field));
	// Trimmed, and nothing a file name on the card or on CL4P-TP cannot carry.
	char *s = typed;
	while (*s == ' ' || *s == '.') {
		s++;
	}
	size_t n = strlen(s);
	while (n && s[n - 1] == ' ') {
		s[--n] = 0;
	}
	for (char *c = s; *c; c++) {
		if (strchr("/\\:*?\"<>|", *c)) {
			*c = '-';
		}
	}
	if (!n || n > NAME_LEN - 4) {
		gui_notify_popup("notes_name_invalid");
		return;
	}
	char file[NAME_LEN + 1];
	if (has_extension(s, ".md") || has_extension(s, ".txt")) {
		snprintf(file, sizeof(file), "%s", s);
	} else {
		snprintf(file, sizeof(file), "%s.md", s);
	}
	char path[PATH_LEN];
	note_path(file, path, sizeof(path));
	struct stat st;
	if (stat(path, &st) == 0) {
		gui_notify_popup("notes_name_taken");
		return;
	}
	char title[NAME_LEN + 1];
	display_name(file, title, sizeof(title));
	char first[NAME_LEN + 8];
	int len = snprintf(first, sizeof(first), "# %s\n\n", title);
	if (!write_all(path, first, (size_t)len)) {
		gui_notify_popup("notes_save_failed");
		return;
	}
	name_layer_hide();
	view_open(file, true);
}

static void sync_cb(lv_event_t *e) {
	(void)e;
	sync_start(true);
	list_status_refresh();
}

static void list_loaded_cb(lv_event_t *e) {
	(void)e;
	if (!ensure_dir()) {
		lv_obj_clean(list_col);
		lv_obj_set_hidden(list_empty, false);
		return;
	}
	list_rebuild();
	if (time(NULL) - last_sync >= AUTO_SYNC_GAP) {
		sync_start(false);
	}
	list_status_refresh();
}

void notespage_open(void) {
	if (!ensure_dir()) {
		gui_notify_no_card();
		return;
	}
	switch_screen(notespage_screen);
}

// ---------------------------------------------------------------------------
// one note: drawn, and the editor over it
// ---------------------------------------------------------------------------

static char *note;
static size_t note_len;
static lv_obj_t *view_col;
static lv_obj_t *view_title;
static lv_obj_t *edit_layer;
static lv_obj_t *edit_field;
static keyboard_t *edit_keyboard;
static bool editing;

static bool editing_name(const char *name) { return editing && strcmp(name, open_name) == 0; }

typedef struct {
	lv_obj_t *group;
	const lv_font_t *font;
	const lv_font_t *bold;
	lv_color_t color;
	bool done; // a ticked task: its text is struck through
} spans_t;

static void span_emit(const char *text, size_t len, int styles, void *user) {
	spans_t *sp = user;
	lv_span_t *span = lv_spangroup_add_span(sp->group);
	char *piece = malloc(len + 1);
	if (!piece) {
		return;
	}
	memcpy(piece, text, len);
	piece[len] = 0;
	lv_span_set_text(span, piece);
	free(piece);
	lv_style_t *st = lv_span_get_style(span);
	lv_style_set_text_font(st, (styles & MD_BOLD) ? sp->bold : sp->font);
	// No italic or monospace face in the player: italic is the quiet colour,
	// code and links the accent.
	lv_color_t c = sp->color;
	if (styles & (MD_CODE_SPAN | MD_LINK)) {
		c = theme()->accent;
	} else if (styles & MD_ITALIC) {
		c = theme()->text_secondary;
	}
	lv_style_set_text_color(st, c);
	int decor = 0;
	if ((styles & MD_STRIKE) || sp->done) {
		decor |= LV_TEXT_DECOR_STRIKETHROUGH;
	}
	if (styles & MD_LINK) {
		decor |= LV_TEXT_DECOR_UNDERLINE;
	}
	if (decor) {
		lv_style_set_text_decor(st, (lv_text_decor_t)decor);
	}
}

static lv_obj_t *rich(lv_obj_t *parent, const char *s, size_t n, const lv_font_t *font, const lv_font_t *bold,
					  lv_color_t color, bool done) {
	lv_obj_t *g = lv_spangroup_create(parent);
	lv_obj_set_clickable(g, false);
	lv_spangroup_set_overflow(g, LV_SPAN_OVERFLOW_CLIP);
	lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_text_line_space(g, 4, 0);
	spans_t sp = {g, font, bold, color, done};
	if (n) {
		md_inline(s, n, span_emit, &sp);
	} else {
		span_emit(" ", 1, 0, &sp);
	}
	lv_spangroup_refresh(g);
	return g;
}

// A row for list items: the marker in a fixed column, the text beside it.
static lv_obj_t *item_row(lv_obj_t *parent, int indent) {
	lv_obj_t *r = plain(parent);
	lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_column(r, 10, 0);
	lv_obj_set_style_pad_left(r, indent * 28, 0);
	return r;
}

static lv_obj_t *marker(lv_obj_t *row, const char *text) {
	lv_obj_t *m = lv_label_create(row);
	lv_label_set_text(m, text);
	lv_obj_set_style_text_font(m, &font_ui_22, 0);
	lv_obj_set_style_text_color(m, theme()->accent, 0);
	lv_obj_set_style_min_width(m, 22, 0);
	return m;
}

static void save_note(void);
static void view_render(void);

static void task_cb(lv_event_t *e) {
	if (gesture_not_tap() || !note) {
		return;
	}
	size_t at = (size_t)(uintptr_t)lv_event_get_user_data(e);
	if (at >= note_len || (note[at] != ' ' && note[at] != 'x' && note[at] != 'X')) {
		return;
	}
	note[at] = note[at] == ' ' ? 'x' : ' ';
	int32_t y = lv_obj_get_scroll_y(view_col);
	save_note();
	view_render();
	lv_obj_update_layout(view_col);
	lv_obj_scroll_to_y(view_col, y, LV_ANIM_OFF);
}

static void view_render(void) {
	lv_obj_clean(view_col);
	if (!note) {
		return;
	}
	const theme_palette_t *t = theme();
	md_block_t b;
	size_t at = 0;
	int blocks = 0;
	while (md_next_block(note, note_len, at, &b)) {
		at = b.next;
		const char *s = note + b.text;
		blocks++;
		switch (b.kind) {
		case MD_BLANK: {
			lv_obj_t *gap = plain(view_col);
			lv_obj_set_size(gap, LV_PCT(100), 8);
			break;
		}
		case MD_H1:
		case MD_H2:
		case MD_H3: {
			const lv_font_t *f = b.kind == MD_H1 ? &font_ui_36_bold : b.kind == MD_H2 ? &font_ui_28 : &font_ui_24_bold;
			lv_obj_t *g = rich(view_col, s, b.len, f, f, b.kind == MD_H2 ? t->accent : t->text_primary, false);
			lv_obj_set_style_margin_top(g, blocks > 1 ? 10 : 0, 0);
			break;
		}
		case MD_PARA:
			rich(view_col, s, b.len, &font_ui_22, &font_ui_22_bold, t->text_primary, false);
			break;
		case MD_BULLET:
		case MD_NUMBER: {
			lv_obj_t *r = item_row(view_col, b.indent);
			char num[16];
			if (b.kind == MD_NUMBER) {
				snprintf(num, sizeof(num), "%d.", b.number);
			}
			marker(r, b.kind == MD_NUMBER ? num : (b.indent ? "\xE2\x97\xA6" : "\xE2\x80\xA2"));
			lv_obj_t *g = rich(r, s, b.len, &font_ui_22, &font_ui_22_bold, t->text_primary, false);
			lv_obj_set_width(g, 0);
			lv_obj_set_flex_grow(g, 1);
			break;
		}
		case MD_TASK: {
			lv_obj_t *r = item_row(view_col, b.indent);
			lv_obj_set_style_pad_ver(r, 6, 0);
			lv_obj_set_clickable(r, true);
			lv_obj_set_ext_click_area(r, 6);
			lv_obj_add_event_cb(r, task_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)b.check_at);
			lv_obj_set_event_bubble(r, true);
			lv_obj_t *box = plain(r);
			lv_obj_set_size(box, 30, 30);
			lv_obj_set_style_radius(box, 8, 0);
			lv_obj_set_style_border_width(box, 3, 0);
			lv_obj_set_style_border_color(box, t->accent, 0);
			if (b.checked) {
				lv_obj_set_style_bg_color(box, t->accent, 0);
				lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
				lv_obj_t *tick = lv_image_create(box);
				lv_image_set_src(tick, &icon_check_small);
				lv_obj_set_style_image_recolor(tick, t->screen_bg, 0);
				lv_obj_set_style_image_recolor_opa(tick, LV_OPA_COVER, 0);
				lv_obj_center(tick);
			}
			lv_obj_t *g = rich(r, s, b.len, &font_ui_22, &font_ui_22_bold,
							   b.checked ? t->text_secondary : t->text_primary, b.checked);
			lv_obj_set_width(g, 0);
			lv_obj_set_flex_grow(g, 1);
			break;
		}
		case MD_QUOTE: {
			lv_obj_t *q = plain(view_col);
			lv_obj_set_size(q, LV_PCT(100), LV_SIZE_CONTENT);
			lv_obj_set_style_border_side(q, LV_BORDER_SIDE_LEFT, 0);
			lv_obj_set_style_border_width(q, 4, 0);
			lv_obj_set_style_border_color(q, t->accent, 0);
			lv_obj_set_style_pad_left(q, 14, 0);
			rich(q, s, b.len, &font_ui_22, &font_ui_22_bold, t->text_secondary, false);
			break;
		}
		case MD_CODE: {
			lv_obj_t *c = plain(view_col);
			lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
			lv_obj_set_style_bg_color(c, t->surface, 0);
			lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
			lv_obj_set_style_radius(c, 10, 0);
			lv_obj_set_style_pad_all(c, 12, 0);
			lv_obj_t *l = lv_label_create(c);
			char *code = malloc(b.len + 1);
			if (code) {
				memcpy(code, s, b.len);
				code[b.len] = 0;
				lv_label_set_text(l, code);
				free(code);
			}
			lv_obj_set_width(l, LV_PCT(100));
			lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
			lv_obj_set_style_text_font(l, &font_ui_18, 0);
			lv_obj_set_style_text_color(l, t->text_primary, 0);
			break;
		}
		case MD_RULE: {
			lv_obj_t *line = plain(view_col);
			lv_obj_set_size(line, LV_PCT(100), 2);
			lv_obj_set_style_bg_color(line, t->text_secondary, 0);
			lv_obj_set_style_bg_opa(line, LV_OPA_50, 0);
			lv_obj_set_style_margin_ver(line, 8, 0);
			break;
		}
		}
	}
	if (!note_len) {
		lv_obj_t *l = lv_label_create(view_col);
		lv_label_set_text(l, tr("notes_empty_note"));
		lv_obj_add_style(l, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(l, &font_ui_20, 0);
	}
}

static bool view_load(const char *name) {
	char path[PATH_LEN];
	note_path(name, path, sizeof(path));
	size_t len = 0;
	char *text = read_all(path, &len);
	if (!text) {
		return false;
	}
	free(note);
	note = text;
	note_len = len;
	snprintf(open_name, sizeof(open_name), "%s", name);
	char shown[NAME_LEN + 1];
	display_name(name, shown, sizeof(shown));
	lv_label_set_text(view_title, shown);
	return true;
}

static void save_note(void) {
	char path[PATH_LEN];
	note_path(open_name, path, sizeof(path));
	if (!write_all(path, note, note_len)) {
		gui_notify_popup("notes_save_failed");
		return;
	}
	sync_start(false);
}

static void edit_show(bool on) {
	editing = on;
	lv_obj_set_hidden(edit_layer, !on);
	back_btn_hide_on_screen(notesview_screen, on);
	if (on) {
		lv_obj_move_foreground(edit_layer);
		lv_textarea_set_text(edit_field, note ? note : "");
		lv_textarea_set_cursor_pos(edit_field, LV_TEXTAREA_CURSOR_LAST);
		keyboard_reset(edit_keyboard);
	}
}

static void edit_cb(lv_event_t *e) {
	(void)e;
	if (note_len > MAX_EDIT_BYTES) {
		confirm_notice(tr("notes_too_long_title"), tr("notes_too_long"));
		return;
	}
	edit_show(true);
}

static void edit_save_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(edit_field);
	size_t len = strlen(text);
	char *copy = malloc(len + 1);
	if (!copy) {
		return;
	}
	memcpy(copy, text, len + 1);
	free(note);
	note = copy;
	note_len = len;
	edit_show(false);
	save_note();
	view_render();
}

static bool edit_dirty(void) {
	const char *text = lv_textarea_get_text(edit_field);
	return strlen(text) != note_len || memcmp(text, note ? note : "", note_len) != 0;
}

static void discard_ok(void *user) {
	(void)user;
	edit_show(false);
}

static void edit_close(void) {
	if (edit_dirty()) {
		confirm_show(tr("notes_discard_title"), NULL, tr("notes_discard"), discard_ok, NULL);
	} else {
		edit_show(false);
	}
}

static void edit_cancel_cb(lv_event_t *e) {
	(void)e;
	edit_close();
}

// The keyboard has no Enter of its own: its accent key is this page's, and
// here it starts a new line.
static void edit_enter_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_add_char(edit_field, '\n');
}

static bool view_back_guard(void) {
	if (editing) {
		edit_close();
		return true;
	}
	return false;
}

static void view_open(const char *name, bool edit) {
	if (!view_load(name)) {
		gui_notify_popup("notes_open_failed");
		return;
	}
	view_render();
	lv_obj_scroll_to_y(view_col, 0, LV_ANIM_OFF);
	switch_screen(notesview_screen);
	if (edit) {
		edit_show(true);
	}
}

static void view_reload_if_open(const char *name, bool deleted) {
	if (strcmp(name, open_name) != 0 || editing) {
		return;
	}
	if (deleted) {
		free(note);
		note = NULL;
		note_len = 0;
		if (lv_screen_active() == notesview_screen) {
			view_render();
		}
		return;
	}
	if (view_load(name) && lv_screen_active() == notesview_screen) {
		view_render();
	}
}

static bool view_back_peek(void) { return editing; }

// The drawn note is let go while the page is away and drawn again on the way
// back. Not while editing: the keyboard's text is the only copy of the edit.
static void view_loaded_cb(lv_event_t *e) {
	(void)e;
	if (!editing && !lv_obj_get_child_count(view_col)) {
		view_render();
	}
}

static void view_unloaded_cb(lv_event_t *e) {
	(void)e;
	if (!editing) {
		lv_obj_clean(view_col);
	}
}

// ---------------------------------------------------------------------------

static lv_obj_t *full_layer(lv_obj_t *screen) {
	lv_obj_t *l = lv_obj_create(screen);
	lv_obj_set_size(l, config->screen_width, config->screen_height);
	lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_add_style(l, &theme_style_screen, 0);
	lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(l, 0, 0);
	lv_obj_set_style_radius(l, 0, 0);
	lv_obj_set_style_pad_all(l, 0, 0);
	lv_obj_set_scrollable(l, false);
	lv_obj_set_hidden(l, true);
	return l;
}

static lv_obj_t *layer_heading(lv_obj_t *layer, const char *tag) {
	lv_obj_t *h = lv_label_create(layer);
	lv_label_set_text(h, tr(tag));
	lv_obj_add_style(h, &theme_style_text, 0);
	lv_obj_set_style_text_font(h, &font_ui_24, 0);
	lv_obj_align(h, LV_ALIGN_TOP_LEFT, config->padding + 14, config->padding + config->top_bar_height + 10);
	return h;
}

static lv_obj_t *column(lv_obj_t *screen, int top) {
	lv_obj_t *c = plain(screen);
	lv_obj_set_pos(c, config->padding, top);
	lv_obj_set_size(c, (int32_t)config->screen_width - 2 * config->padding, (int32_t)config->screen_height - top);
	lv_obj_set_scrollable(c, true);
	lv_obj_set_scroll_dir(c, LV_DIR_VER);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_bottom(c, config->padding * 2, 0);
	lv_obj_set_event_bubble(c, true);
	return c;
}

void notespage_init(gui_config_t *cfg) {
	config = cfg;
	snprintf(server, sizeof(server), "%s", config_get("notes", "server", DEFAULT_SERVER));
	int top = settingsrow_content_top(cfg);

	// --- the list
	lv_obj_add_style(notespage_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(notespage_screen, false);
	list_title = settingsrow_title(notespage_screen, cfg, "notes");
	settingsrow_title_corner_slots(list_title, cfg, 2);
	corner_button(notespage_screen, &icon_plus, 0, new_cb);
	corner_button(notespage_screen, &icon_file_down, 1, sync_cb);

	list_status = lv_label_create(notespage_screen);
	lv_label_set_text(list_status, "");
	lv_obj_add_style(list_status, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_status, &font_ui_16, 0);
	lv_obj_align(list_status, LV_ALIGN_TOP_LEFT, cfg->padding, top - 4);

	list_col = column(notespage_screen, top + 22);
	lv_obj_set_style_pad_row(list_col, 8, 0);

	list_empty = lv_label_create(notespage_screen);
	lv_label_set_text(list_empty, tr("notes_none"));
	lv_obj_add_style(list_empty, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(list_empty, &font_ui_20, 0);
	lv_obj_set_width(list_empty, (int32_t)cfg->screen_width - 4 * cfg->padding);
	lv_label_set_long_mode(list_empty, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(list_empty, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(list_empty, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_hidden(list_empty, true);

	name_layer = full_layer(notespage_screen);
	layer_heading(name_layer, "notes_new");
	corner_button(name_layer, &icon_close, 0, name_cancel_cb);
	name_field = lv_textarea_create(name_layer);
	lv_textarea_set_one_line(name_field, true);
	lv_textarea_set_max_length(name_field, NAME_LEN - 4);
	lv_textarea_set_placeholder_text(name_field, tr("name"));
	lv_obj_set_size(name_field, cfg->screen_width - 2 * cfg->padding, 62);
	lv_obj_set_scrollbar_mode(name_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(name_field, LV_ALIGN_TOP_LEFT, cfg->padding, cfg->padding + cfg->top_bar_height + 60);
	lv_obj_add_style(name_field, &theme_style_card, 0);
	lv_obj_set_style_radius(name_field, 12, 0);
	lv_obj_set_style_border_width(name_field, 0, 0);
	lv_obj_set_style_shadow_width(name_field, 0, 0);
	lv_obj_set_style_pad_all(name_field, 14, 0);
	lv_obj_set_style_text_font(name_field, &font_ui_24, 0);
	keyboard_style_caret(name_field);
	name_keyboard = keyboard_create(name_layer, cfg->screen_width, 316, name_field, NULL, "ok", name_accept_cb, NULL);

	lv_obj_add_event_cb(notespage_screen, list_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);

	// --- one note
	lv_obj_add_style(notesview_screen, &theme_style_screen, 0);
	lv_obj_set_scrollable(notesview_screen, false);
	view_title = settingsrow_title(notesview_screen, cfg, "notes");
	settingsrow_title_corner_slots(view_title, cfg, 1);
	corner_button(notesview_screen, &icon_pencil, 0, edit_cb);
	view_col = column(notesview_screen, top);
	lv_obj_set_style_pad_row(view_col, 6, 0);

	edit_layer = full_layer(notesview_screen);
	layer_heading(edit_layer, "notes_editing");
	corner_button(edit_layer, &icon_close, 0, edit_cancel_cb);
	corner_button(edit_layer, &icon_check, 1, edit_save_cb);
	int field_top = cfg->padding + cfg->top_bar_height + 60;
	int kb_h = 316;
	edit_field = lv_textarea_create(edit_layer);
	lv_textarea_set_one_line(edit_field, false);
	lv_textarea_set_max_length(edit_field, MAX_EDIT_BYTES);
	lv_obj_set_size(edit_field, cfg->screen_width - 2 * cfg->padding, (int32_t)cfg->screen_height - kb_h - field_top - 8);
	lv_obj_align(edit_field, LV_ALIGN_TOP_LEFT, cfg->padding, field_top);
	lv_obj_add_style(edit_field, &theme_style_card, 0);
	lv_obj_set_style_radius(edit_field, 12, 0);
	lv_obj_set_style_border_width(edit_field, 0, 0);
	lv_obj_set_style_shadow_width(edit_field, 0, 0);
	lv_obj_set_style_pad_all(edit_field, 12, 0);
	lv_obj_set_style_text_font(edit_field, &font_ui_20, 0);
	keyboard_style_caret(edit_field);
	edit_keyboard = keyboard_create(edit_layer, cfg->screen_width, kb_h, edit_field, NULL, "notes_enter", edit_enter_cb,
									NULL);

	switcher_set_back_guard(notesview_screen, view_back_guard);
	switcher_set_back_guard_peek(notesview_screen, view_back_peek);
	lv_obj_add_event_cb(notesview_screen, view_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(notesview_screen, view_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
