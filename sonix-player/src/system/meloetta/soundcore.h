#ifndef SOUNDCORE_H
#define SOUNDCORE_H

#include <stdbool.h>
#include <stdint.h>

// Anker Soundcore earbuds, from the player: battery, noise cancelling,
// transparency, LDAC. Written for the Liberty 4 NC (A3947), the pair Meloetta
// plays to.
//
// Soundcore headphones speak a small framed protocol over RFCOMM, on the same
// classic Bluetooth link that carries the music -- the channel the Soundcore
// app uses. Every packet is
//
//     direction[5] command[2] length u16 LE (whole packet) body... checksum
//
// with direction 08 EE 00 00 00 going out and 09 FF 00 00 01 coming back, and
// the checksum the byte sum of everything before it. The packets and the
// layout of the A3947's state were worked out by OpenSCQ30
// (github.com/Oppzippy/OpenSCQ30, GPL-3.0); this is the part of it the player
// needs, ported to C.
//
// The RFCOMM channel is not fixed: it is looked up over SDP with sdptool,
// preferring Soundcore's own service UUID (0cf12d31-fac3-4553-bd80-d6832e7xxxxx)
// over the plain serial port.
//
// Only devices whose Bluetooth name says Soundcore or Liberty are tried, so an
// ordinary pair of headphones never sees an SDP query from this.
//
// Threading: one worker owns the socket; callers get copies under a lock, the
// same shape as airpods.c.

typedef enum {
	SOUNDCORE_MODE_NC = 0,			 // noise cancelling
	SOUNDCORE_MODE_TRANSPARENCY = 1, // ambient sound in
	SOUNDCORE_MODE_NORMAL = 2,		 // neither
} soundcore_mode_t;

typedef enum {
	SOUNDCORE_NC_MANUAL = 0,	 // a fixed strength, 1..5
	SOUNDCORE_NC_ADAPTIVE = 1,	 // the buds pick the strength
	SOUNDCORE_NC_TRANSPORT = 2, // tuned for a vehicle
} soundcore_nc_t;

typedef enum {
	SOUNDCORE_TRANSPARENCY_FULL = 0,
	SOUNDCORE_TRANSPARENCY_VOCAL = 1,
} soundcore_transparency_t;

typedef struct {
	bool connected;		 // the RFCOMM session is up
	bool have_state;	 // the first full state packet has arrived
	char name[64];		 // what Bluetooth calls them
	int left, right;	 // battery 0..5, -1 when not known
	int box;			 // the case, 0..5, -1 when not known (shut, or never said)
	bool left_charging, right_charging;
	bool both_buds;		 // both earbuds connected to each other
	soundcore_mode_t mode;
	soundcore_nc_t nc;
	int nc_level;		 // manual strength 1..5 (0 = never said)
	int adaptive_level;	 // what adaptive picked, 0..5, read only
	soundcore_transparency_t transparency;
	bool wind;			 // wind noise reduction
	int ldac;			 // 1 on, 0 off, -1 not known yet
	char firmware[8];	 // "05.88"
	uint32_t serial;	 // bumps on every change, so a poll can skip a redraw
} soundcore_state_t;

void soundcore_init(void);
void soundcore_stop(void);

// The connected audio device, or NULL for none. Cheap to call on every round.
void soundcore_set_device(const char *mac, const char *name);

// A copy of the state; true while the session is up.
bool soundcore_get(soundcore_state_t *out);

// Queued and sent on the worker's thread. Each is taken as done at once so the
// page lights under the finger; whatever the buds report next wins.
void soundcore_set_mode(soundcore_mode_t mode);
void soundcore_set_nc(soundcore_nc_t nc);
void soundcore_set_nc_level(int level); // 1..5, also switches NC to manual
void soundcore_set_transparency(soundcore_transparency_t t);
void soundcore_set_wind(bool on);

// LDAC on or off. The buds drop the link and come back with the other codec,
// so the session closes and reopens by itself a few seconds later.
void soundcore_set_ldac(bool on);

// Asks for the full state again (the case battery only comes with it).
void soundcore_refresh(void);

#endif /* SOUNDCORE_H */
