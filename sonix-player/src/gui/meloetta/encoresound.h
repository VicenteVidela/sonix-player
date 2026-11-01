#ifndef GUI_ENCORESOUND_H
#define GUI_ENCORESOUND_H

#include <stdbool.h>

// Encore's beeps: square waves made on the spot, no sound files. Like
// Flappy Bird's sounds, a thread writes them into the external PCM, which
// stops the music first, so the game asks for it only with [encore] sound on.

typedef struct {
	float hz; // 0 = a rest
	int ms;
	int amp; // 0..32767
} encore_tone_t;

bool encoresound_start(void);
void encoresound_stop(void);

// Plays `n` tones one after another, replacing whatever was sounding.
// Any thread; does nothing while the mixer is off.
void encoresound_play(const encore_tone_t *tones, int n);

#endif /* GUI_ENCORESOUND_H */
