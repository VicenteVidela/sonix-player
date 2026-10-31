#ifndef CHIME_H
#define CHIME_H

#include <stdbool.h>

// Sounds for the Meloetta pages, written to the external PCM (audio.h) the
// metronome's way. Like the metronome they never sound over music: with a track
// or a station playing they stay silent, and anything that wants the output
// takes it back.
//
// Each returns at once, the sound playing on a thread of its own, and is false
// when nothing will be heard: something is playing, or a sound already is.

// The timer's: one short digital beep-beep. Synthesised.
bool chime_beep(void);

// The meditation bell: a recorded singing bowl, struck `strikes` times
// CHIME_BOWL_GAP_MS apart, each left to ring out to silence. Read from
// <resources>/gui/meloetta/bowl.ogg; silent without it.
bool chime_bowl(int strikes);

#define CHIME_BOWL_GAP_MS 2000

#endif /* CHIME_H */
