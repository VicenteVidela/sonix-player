#ifndef PET_H
#define PET_H

#include <stdbool.h>
#include <time.h>

// The Meloetta fork's pet: a small music spirit that eats songs. Every play
// that counts (playcount.c, Last.fm's rule) is a meal. It is the classic kind,
// by Vicho's choice: hunger and happiness run down with time, mess left lying
// and neglect make it ill, and an illness left untreated -- or long hunger --
// kills it, after which a new egg starts over.
//
// Everything runs on the wall clock and catches up when asked, so a player
// that slept for a day finds the pet a day older. It sleeps from 23:00 to
// 08:00 local time, and nothing runs down while it does. All of it is kept in
// [pet] in the config. UI thread only.

#define PET_HEARTS 4
#define PET_MAX_POOP 3
#define PET_HATCH_SONGS 3
#define PET_NAME_MAX 24
#define PET_SERIES_MAX 48

typedef enum {
	PET_EGG,
	PET_BABY,
	PET_CHILD,
	PET_TEEN,
	PET_ADULT_GOOD, // raised well: few misses
	PET_ADULT_ROUGH,
	PET_DEAD,
} pet_stage_t;

typedef struct {
	pet_stage_t stage;
	char name[PET_NAME_MAX];
	time_t born;		 // laid, for an egg; hatched otherwise
	time_t died;		 // 0 while alive
	int songs;			 // eaten in this life
	int hunger;			 // 0..PET_HEARTS, PET_HEARTS is full
	int happy;			 // 0..PET_HEARTS
	int poop;			 // 0..PET_MAX_POOP
	bool sick;
	bool asleep;		 // by the clock, now
	int misses;			 // times it was let go hungry, miserable or ill: shapes the adult
	char favorite[PET_SERIES_MAX]; // the series fed most, "" before any
	char last_meal[96];	 // the title of the last song eaten
	int lives;			 // pets so far, this one included
	int hue;			 // 0..359, from the favourite series
} pet_t;

// Reads the pet from the config. Done by the first call of any of these
// anyway; calling it at start only does it sooner.
void pet_init(void);

// Brings it up to now and returns it: ages it, runs its meters down, makes it
// ill or kills it as the time gone by says. Cheap when little time has passed.
const pet_t *pet_get(void);

// A song was played through. `path` is the file (its first folder on the card
// is taken for the series), `title` what to call it -- NULL for the file's own
// name.
void pet_note_play(const char *path, const char *title);

// What the buttons do. Each returns false when it does nothing now: petting
// too soon after the last time, medicine for a pet that is well, and so on.
bool pet_pat(void);
bool pet_medicine(void);
bool pet_clean(void);
bool pet_new_egg(void); // only once it has died

// Something worth telling the user about happened since the last call: it fell
// ill, became hungry, hatched, grew, or died. NULL when nothing did. Returns a
// translation key that takes the pet's name (%s); each event is told once.
const char *pet_take_news(void);

// Days since it hatched (or since it was laid, for an egg).
int pet_age_days(const pet_t *p, time_t now);

#endif /* PET_H */
