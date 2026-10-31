#include "pet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/core/config.h"
#include "src/system/device/system.h"

// The rules, in minutes of waking time.
#define STEP_MIN 10				   // how finely time gone by is replayed
#define HUNGER_EVERY (3 * 60)	   // a heart of hunger lost
#define HAPPY_EVERY (4 * 60)	   // a heart of happiness lost
#define STARVED_SICK (12 * 60)	   // hunger at nothing this long: ill
#define MISERABLE_SICK (24 * 60)   // happiness at nothing this long: ill
#define MESS_SICK (6 * 60)		   // the most mess this long: ill
#define SICK_DEATH (24 * 60)	   // ill and untreated this long: dies
#define STARVED_DEATH (36 * 60)	   // hunger at nothing this long: dies
#define PAT_EVERY (30 * 60)		   // seconds between pats that count
#define SONGS_PER_POOP 4
#define SLEEP_FROM 23 // local hour
#define SLEEP_UNTIL 8
#define CATCH_UP_DAYS 30 // a longer absence is replayed as this much
#define SANE_TIME 1700000000 // before this the clock was never set: nothing runs

#define SERIES_KEPT 8

static pet_t pet;
static time_t last_update;
static int acc_hunger, acc_happy;		   // minutes towards the next heart lost
static int zero_hunger, zero_happy, mess_full, sick_for; // minutes each condition has lasted
static time_t last_pat;
static int poop_songs; // songs since the last mess
static char last_series[PET_SERIES_MAX];
static const char *news;
static bool loaded;

// The series fed so far, as "name=count|name=count", the most fed kept.
static char series_counts[SERIES_KEPT * (PET_SERIES_MAX + 8)];

static const char *const NAMES[] = {"Doremi", "Allegra", "Tempo", "Pip", "Coda", "Trill",
									"Melo",	  "Arpi",	 "Sol",	  "Fermi", "Lulla", "Bop"};

// ---------------------------------------------------------------------------
// keeping it
// ---------------------------------------------------------------------------

static void save(void) {
	config_set_int("pet", "stage", pet.stage);
	config_set("pet", "name", pet.name);
	config_set_int("pet", "born", (long)pet.born);
	config_set_int("pet", "died", (long)pet.died);
	config_set_int("pet", "songs", pet.songs);
	config_set_int("pet", "hunger", pet.hunger);
	config_set_int("pet", "happy", pet.happy);
	config_set_int("pet", "poop", pet.poop);
	config_set_bool("pet", "sick", pet.sick);
	config_set_int("pet", "misses", pet.misses);
	config_set("pet", "favorite", pet.favorite);
	config_set("pet", "last_meal", pet.last_meal);
	config_set_int("pet", "lives", pet.lives);
	config_set_int("pet", "hue", pet.hue);
	config_set_int("pet", "updated", (long)last_update);
	config_set_int("pet", "acc_hunger", acc_hunger);
	config_set_int("pet", "acc_happy", acc_happy);
	config_set_int("pet", "zero_hunger", zero_hunger);
	config_set_int("pet", "zero_happy", zero_happy);
	config_set_int("pet", "mess_full", mess_full);
	config_set_int("pet", "sick_for", sick_for);
	config_set_int("pet", "last_pat", (long)last_pat);
	config_set_int("pet", "poop_songs", poop_songs);
	config_set("pet", "last_series", last_series);
	config_set("pet", "series", series_counts);
	config_save();
}

static void copy_str(char *out, size_t size, const char *in) { snprintf(out, size, "%s", in ? in : ""); }

static void lay_egg(time_t now) {
	int lives = pet.lives;
	memset(&pet, 0, sizeof(pet));
	pet.stage = PET_EGG;
	pet.born = now;
	pet.hunger = PET_HEARTS;
	pet.happy = PET_HEARTS;
	pet.lives = lives + 1;
	pet.hue = 160;
	acc_hunger = acc_happy = zero_hunger = zero_happy = mess_full = sick_for = 0;
	poop_songs = 0;
	last_pat = 0;
	last_series[0] = series_counts[0] = '\0';
	last_update = now;
}

void pet_init(void) {
	if (loaded) {
		return;
	}
	loaded = true;
	time_t now = time(NULL);
	pet.stage = (pet_stage_t)config_get_int("pet", "stage", -1);
	if ((int)pet.stage < PET_EGG || pet.stage > PET_DEAD) {
		lay_egg(now);
		save();
		return;
	}
	copy_str(pet.name, sizeof(pet.name), config_get("pet", "name", ""));
	pet.born = (time_t)config_get_int("pet", "born", (long)now);
	pet.died = (time_t)config_get_int("pet", "died", 0);
	pet.songs = (int)config_get_int("pet", "songs", 0);
	pet.hunger = (int)config_get_int("pet", "hunger", PET_HEARTS);
	pet.happy = (int)config_get_int("pet", "happy", PET_HEARTS);
	pet.poop = (int)config_get_int("pet", "poop", 0);
	pet.sick = config_get_bool("pet", "sick", false);
	pet.misses = (int)config_get_int("pet", "misses", 0);
	copy_str(pet.favorite, sizeof(pet.favorite), config_get("pet", "favorite", ""));
	copy_str(pet.last_meal, sizeof(pet.last_meal), config_get("pet", "last_meal", ""));
	pet.lives = (int)config_get_int("pet", "lives", 1);
	pet.hue = (int)config_get_int("pet", "hue", 160);
	last_update = (time_t)config_get_int("pet", "updated", (long)now);
	acc_hunger = (int)config_get_int("pet", "acc_hunger", 0);
	acc_happy = (int)config_get_int("pet", "acc_happy", 0);
	zero_hunger = (int)config_get_int("pet", "zero_hunger", 0);
	zero_happy = (int)config_get_int("pet", "zero_happy", 0);
	mess_full = (int)config_get_int("pet", "mess_full", 0);
	sick_for = (int)config_get_int("pet", "sick_for", 0);
	last_pat = (time_t)config_get_int("pet", "last_pat", 0);
	poop_songs = (int)config_get_int("pet", "poop_songs", 0);
	copy_str(last_series, sizeof(last_series), config_get("pet", "last_series", ""));
	copy_str(series_counts, sizeof(series_counts), config_get("pet", "series", ""));
}

// ---------------------------------------------------------------------------
// time going by
// ---------------------------------------------------------------------------

static bool asleep_at(time_t t) {
	struct tm local;
	localtime_r(&t, &local);
	return local.tm_hour >= SLEEP_FROM || local.tm_hour < SLEEP_UNTIL;
}

static void die(time_t t) {
	pet.stage = PET_DEAD;
	pet.died = t;
	pet.sick = false;
	news = "pet_news_died";
}

static void fall_ill(void) {
	if (!pet.sick) {
		pet.sick = true;
		sick_for = 0;
		pet.misses++;
		news = "pet_news_sick";
	}
}

// One step of waking time for a pet that has hatched.
static void step(time_t t) {
	acc_hunger += STEP_MIN;
	if (acc_hunger >= HUNGER_EVERY) {
		acc_hunger = 0;
		if (pet.hunger > 0 && --pet.hunger == 0) {
			pet.misses++;
			news = "pet_news_hungry";
		}
	}
	acc_happy += STEP_MIN;
	if (acc_happy >= HAPPY_EVERY) {
		acc_happy = 0;
		if (pet.happy > 0 && --pet.happy == 0) {
			pet.misses++;
		}
	}
	zero_hunger = pet.hunger == 0 ? zero_hunger + STEP_MIN : 0;
	zero_happy = pet.happy == 0 ? zero_happy + STEP_MIN : 0;
	mess_full = pet.poop >= PET_MAX_POOP ? mess_full + STEP_MIN : 0;
	if (zero_hunger >= STARVED_SICK || zero_happy >= MISERABLE_SICK || mess_full >= MESS_SICK) {
		fall_ill();
	}
	if (pet.sick) {
		sick_for += STEP_MIN;
	}
	if (sick_for >= SICK_DEATH || zero_hunger >= STARVED_DEATH) {
		die(t);
	}
}

int pet_age_days(const pet_t *p, time_t now) {
	time_t until = p->stage == PET_DEAD && p->died ? p->died : now;
	return until > p->born ? (int)((until - p->born) / 86400) : 0;
}

// Growing up: old enough and fed enough for the next stage.
static void grow(time_t now) {
	int days = pet_age_days(&pet, now);
	pet_stage_t before = pet.stage;
	if (pet.stage == PET_BABY && days >= 1 && pet.songs >= 20) {
		pet.stage = PET_CHILD;
	} else if (pet.stage == PET_CHILD && days >= 3 && pet.songs >= 80) {
		pet.stage = PET_TEEN;
	} else if (pet.stage == PET_TEEN && days >= 7 && pet.songs >= 200) {
		pet.stage = pet.misses <= 2 ? PET_ADULT_GOOD : PET_ADULT_ROUGH;
	}
	if (pet.stage != before) {
		news = "pet_news_grew";
	}
}

static void catch_up(void) {
	pet_init(); // a song can end before the page is ever built
	time_t now = time(NULL);
	if (now < SANE_TIME) {
		return; // the clock has never been set: the pet waits
	}
	if (last_update < SANE_TIME || now < last_update) {
		last_update = now; // the clock was wrong before, or went back
		return;
	}
	if (now - last_update > (time_t)CATCH_UP_DAYS * 86400) {
		last_update = now - (time_t)CATCH_UP_DAYS * 86400;
	}
	bool moved = false;
	while (now - last_update >= STEP_MIN * 60) {
		last_update += STEP_MIN * 60;
		moved = true;
		if (pet.stage == PET_EGG || pet.stage == PET_DEAD || asleep_at(last_update)) {
			continue;
		}
		step(last_update);
	}
	if (pet.stage != PET_EGG && pet.stage != PET_DEAD) {
		grow(now);
	}
	pet.asleep = asleep_at(now);
	if (moved) {
		save();
	}
}

const pet_t *pet_get(void) {
	catch_up();
	return &pet;
}

// ---------------------------------------------------------------------------
// meals
// ---------------------------------------------------------------------------

// The series a file belongs to: its first folder on the card.
static void series_of(const char *path, char *out, size_t size) {
	out[0] = '\0';
	if (!path) {
		return;
	}
	const char *root = storage_sd_root();
	const char *rest = path;
	size_t n = root ? strlen(root) : 0;
	if (n && strncmp(path, root, n) == 0) {
		rest = path + n;
	}
	while (*rest == '/') {
		rest++;
	}
	const char *slash = strchr(rest, '/');
	if (!slash) {
		return; // a file at the top of the card: no series
	}
	size_t len = (size_t)(slash - rest);
	snprintf(out, size, "%.*s", (int)(len < size ? len : size - 1), rest);
}

// One more meal from `series` in the counts, and the favourite from them.
static void count_series(const char *series) {
	char names[SERIES_KEPT + 1][PET_SERIES_MAX];
	int counts[SERIES_KEPT + 1], n = 0;
	char copy[sizeof(series_counts)];
	copy_str(copy, sizeof(copy), series_counts);
	char *save_ptr = NULL;
	for (char *item = strtok_r(copy, "|", &save_ptr); item && n < SERIES_KEPT; item = strtok_r(NULL, "|", &save_ptr)) {
		char *eq = strrchr(item, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		copy_str(names[n], PET_SERIES_MAX, item);
		counts[n++] = atoi(eq + 1);
	}
	int found = -1;
	for (int i = 0; i < n; i++) {
		if (strcmp(names[i], series) == 0) {
			found = i;
		}
	}
	if (found < 0) {
		found = n++;
		copy_str(names[found], PET_SERIES_MAX, series);
		counts[found] = 0;
	}
	counts[found]++;
	// The most fed first, so dropping the last keeps the ones that matter.
	for (int i = 0; i < n; i++) {
		for (int j = i + 1; j < n; j++) {
			if (counts[j] > counts[i]) {
				int c = counts[i];
				counts[i] = counts[j];
				counts[j] = c;
				char t[PET_SERIES_MAX];
				copy_str(t, sizeof(t), names[i]);
				copy_str(names[i], PET_SERIES_MAX, names[j]);
				copy_str(names[j], PET_SERIES_MAX, t);
			}
		}
	}
	if (n > SERIES_KEPT) {
		n = SERIES_KEPT;
	}
	size_t used = 0;
	series_counts[0] = '\0';
	for (int i = 0; i < n && used < sizeof(series_counts); i++) {
		used += (size_t)snprintf(series_counts + used, sizeof(series_counts) - used, "%s%s=%d", i ? "|" : "", names[i],
								 counts[i]);
	}
	copy_str(pet.favorite, sizeof(pet.favorite), names[0]);
	unsigned hash = 5381;
	for (const char *c = names[0]; *c; c++) {
		hash = hash * 33 + (unsigned char)*c;
	}
	pet.hue = (int)(hash % 360);
}

void pet_note_play(const char *path, const char *title) {
	catch_up();
	if (pet.stage == PET_DEAD) {
		return;
	}
	pet.songs++;
	if (title && title[0]) {
		copy_str(pet.last_meal, sizeof(pet.last_meal), title);
	} else {
		// The file's own name, without its folder or its extension.
		const char *base = path ? strrchr(path, '/') : NULL;
		base = base ? base + 1 : (path ? path : "");
		const char *dot = strrchr(base, '.');
		size_t len = dot ? (size_t)(dot - base) : strlen(base);
		snprintf(pet.last_meal, sizeof(pet.last_meal), "%.*s", (int)(len < sizeof(pet.last_meal) ? len : sizeof(pet.last_meal) - 1), base);
	}
	char series[PET_SERIES_MAX];
	series_of(path, series, sizeof(series));
	if (series[0]) {
		count_series(series);
	}

	if (pet.stage == PET_EGG) {
		if (pet.songs >= PET_HATCH_SONGS) {
			pet.stage = PET_BABY;
			pet.born = time(NULL);
			copy_str(pet.name, sizeof(pet.name), NAMES[(unsigned)time(NULL) % (sizeof(NAMES) / sizeof(NAMES[0]))]);
			pet.songs = 0;
			news = "pet_news_hatched";
		}
	} else {
		if (pet.hunger < PET_HEARTS) {
			pet.hunger++;
			acc_hunger = 0;
		}
		// Variety pleases it: a song from another series than the last one.
		if (series[0] && strcmp(series, last_series) != 0 && last_series[0] && pet.happy < PET_HEARTS) {
			pet.happy++;
		}
		if (++poop_songs >= SONGS_PER_POOP) {
			poop_songs = 0;
			if (pet.poop < PET_MAX_POOP) {
				pet.poop++;
			}
		}
		grow(time(NULL));
	}
	copy_str(last_series, sizeof(last_series), series);
	save();
}

// ---------------------------------------------------------------------------
// care
// ---------------------------------------------------------------------------

static bool alive(void) { return pet.stage != PET_EGG && pet.stage != PET_DEAD; }

bool pet_pat(void) {
	catch_up();
	time_t now = time(NULL);
	if (!alive() || pet.asleep || now - last_pat < PAT_EVERY) {
		return false;
	}
	last_pat = now;
	if (pet.happy < PET_HEARTS) {
		pet.happy++;
		acc_happy = 0;
	}
	save();
	return true;
}

bool pet_medicine(void) {
	catch_up();
	if (!alive() || !pet.sick) {
		return false;
	}
	pet.sick = false;
	sick_for = zero_hunger = zero_happy = mess_full = 0;
	save();
	return true;
}

bool pet_clean(void) {
	catch_up();
	if (!alive() || pet.poop == 0) {
		return false;
	}
	pet.poop = 0;
	mess_full = 0;
	save();
	return true;
}

bool pet_new_egg(void) {
	catch_up();
	if (pet.stage != PET_DEAD) {
		return false;
	}
	lay_egg(time(NULL));
	save();
	return true;
}

const char *pet_take_news(void) {
	const char *n = news;
	news = NULL;
	return n;
}
