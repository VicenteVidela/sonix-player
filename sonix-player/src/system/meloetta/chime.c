#include "chime.h"

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/system/audio/audio.h"
#include "src/system/core/respath.h"
#include "src/system/core/utils.h"
#include "src/system/decode/stb_vorbis_decl.h"
#include "src/system/streaming/radio.h"

#define BOWL_PATH SONIX_RESOURCE_DIR "/gui/meloetta/bowl.ogg"

#define RATE 44100
#define CHUNK 512
#define BUFFER_MS 200
#define MAX_STRIKES 4
// Silence before every sound: Bluetooth earbuds take a moment to wake the
// stream, and ramp it in, and would swallow the start of a short beep.
#define LEAD_MS 300

// The beep: one pair, a watch's -- beep-beep, with room between the two so
// they are heard as two.
#define BEEP_HZ 2093.0f // C7: high but not shrill
#define BEEP_MS 90
#define BEEP_GAP_MS 110
#define BEEP_LEVEL 0.16f
#define RAMP_FRAMES 220 // 5 ms in and out, so each beep starts without a click

// What a sound thread plays: a mono buffer and how many times, how far apart.
typedef struct {
	int16_t *pcm; // freed by the thread
	long frames;
	int copies;
	long gap;
	void (*done)(void); // called on the sound's thread once it has finished
} job_t;

static atomic_bool playing;

static bool output_free(void) {
	return audio_get_status() != AUDIO_STATUS_PLAYING && !radio_is_playing() && !radio_is_connecting();
}

// Plays the job's buffer `copies` times, each starting `gap` frames after the
// one before and overlapping it, as stereo. Ends on a few blocks of silence,
// so the last of the sound has left the buffer before the output is given up.
static void *play_main(void *arg) {
	job_t *job = arg;
	thread_set_background_level(true);
	if (audio_external_begin_yielding(RATE, 2, 16, BUFFER_MS)) {
		long lead = RATE * LEAD_MS / 1000;
		long total = lead + job->gap * (job->copies - 1) + job->frames + RATE * BUFFER_MS / 1000 * 2;
		int16_t out[CHUNK * 2];
		for (long frame = 0; frame < total && !audio_external_wanted();) {
			int n = 0;
			for (; n < CHUNK && frame < total; n++, frame++) {
				int32_t v = 0;
				for (int c = 0; c < job->copies; c++) {
					long at = frame - lead - job->gap * c;
					if (at >= 0 && at < job->frames) {
						v += job->pcm[at];
					}
				}
				if (v > 32767) v = 32767;
				if (v < -32768) v = -32768;
				out[n * 2] = out[n * 2 + 1] = (int16_t)v;
			}
			if (audio_external_write(out, n) < 0) {
				break;
			}
		}
		audio_external_end();
	} else {
		printf("chime: no output; silent\n");
	}
	void (*done)(void) = job->done;
	free(job->pcm);
	free(job);
	atomic_store(&playing, false);
	if (done) {
		done();
	}
	return NULL;
}

static bool start(job_t *job) {
	pthread_t t;
	if (pthread_create(&t, NULL, play_main, job) != 0) {
		free(job->pcm);
		free(job);
		atomic_store(&playing, false);
		return false;
	}
	pthread_detach(t);
	return true;
}

static bool claim(void) {
	bool expected = false;
	return output_free() && atomic_compare_exchange_strong(&playing, &expected, true);
}

bool chime_beep(void (*done)(void)) {
	if (!claim()) {
		return false;
	}
	long beep = RATE * BEEP_MS / 1000, gap = RATE * BEEP_GAP_MS / 1000;
	job_t *job = calloc(1, sizeof(*job));
	int16_t *pcm = job ? calloc((size_t)beep, sizeof(int16_t)) : NULL;
	if (!pcm) {
		free(job);
		atomic_store(&playing, false);
		return false;
	}
	for (long i = 0; i < beep; i++) {
		float ramp = 1.0f;
		if (i < RAMP_FRAMES) ramp = (float)i / RAMP_FRAMES;
		else if (beep - i < RAMP_FRAMES) ramp = (float)(beep - i) / RAMP_FRAMES;
		pcm[i] = (int16_t)(sinf(2.0f * (float)M_PI * BEEP_HZ * (float)i / RATE) * ramp * BEEP_LEVEL * 32767.0f);
	}
	job->pcm = pcm;
	job->frames = beep;
	job->done = done;
	job->copies = 2; // the same beep twice
	job->gap = beep + gap;
	return start(job);
}

bool chime_bowl(int strikes) {
	if (!claim()) {
		return false;
	}
	int channels = 0, rate = 0;
	short *pcm = NULL;
	int frames = stb_vorbis_decode_filename(BOWL_PATH, &channels, &rate, &pcm);
	if (frames <= 0 || channels != 1 || rate != RATE) {
		printf("chime: %s cannot be used (%d frames, %d channels, %d Hz)\n", BOWL_PATH, frames, channels, rate);
		free(pcm);
		atomic_store(&playing, false);
		return false;
	}
	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		free(pcm);
		atomic_store(&playing, false);
		return false;
	}
	job->pcm = pcm;
	job->frames = frames;
	job->copies = strikes < 1 ? 1 : strikes > MAX_STRIKES ? MAX_STRIKES : strikes;
	job->gap = (long)RATE * CHIME_BOWL_GAP_MS / 1000;
	return start(job);
}
