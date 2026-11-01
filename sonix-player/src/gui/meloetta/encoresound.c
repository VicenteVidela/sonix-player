#include "encoresound.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/system/audio/audio.h"
#include "src/system/streaming/radio.h"

#define RATE 44100
#define MIX_FRAMES 441 // 10 ms
#define BUFFER_MS 46
#define MAX_TONES 8
#define EDGE_FRAMES 88 // 2 ms in and out, so a tone does not click

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t mixer;
static bool mixer_live;
static volatile bool stop_flag;

static encore_tone_t queue[MAX_TONES];
static int queue_n, queue_i;
static int tone_frames, tone_pos; // the tone at queue_i
static double phase;

static void tone_begin(void) {
	tone_pos = 0;
	tone_frames = queue_i < queue_n ? queue[queue_i].ms * RATE / 1000 : 0;
}

static void mix(int16_t *out) {
	pthread_mutex_lock(&lock);
	for (int f = 0; f < MIX_FRAMES; f++) {
		int16_t v = 0;
		while (queue_i < queue_n && tone_pos >= tone_frames) {
			queue_i++;
			tone_begin();
		}
		if (queue_i < queue_n) {
			const encore_tone_t *t = &queue[queue_i];
			if (t->hz > 0) {
				int edge = tone_pos < EDGE_FRAMES ? tone_pos : tone_frames - tone_pos;
				int amp = edge < EDGE_FRAMES ? t->amp * edge / EDGE_FRAMES : t->amp;
				v = (int16_t)(phase < 0.5 ? amp : -amp);
				phase += t->hz / RATE;
				if (phase >= 1.0) {
					phase -= 1.0;
				}
			}
			tone_pos++;
		}
		out[f * 2] = out[f * 2 + 1] = v;
	}
	pthread_mutex_unlock(&lock);
}

static void *mixer_main(void *unused) {
	(void)unused;
	audio_stop();
	if (radio_is_playing()) {
		radio_stop();
	}
	if (stop_flag || !audio_external_begin_latency(RATE, 2, 16, BUFFER_MS)) {
		if (!stop_flag) {
			fprintf(stderr, "encoresound: no PCM; the game plays silently\n");
		}
		return NULL;
	}
	int16_t out[MIX_FRAMES * 2];
	while (!stop_flag) {
		mix(out);
		audio_external_write(out, MIX_FRAMES);
	}
	audio_external_end();
	return NULL;
}

bool encoresound_start(void) {
	encoresound_stop();
	stop_flag = false;
	pthread_mutex_lock(&lock);
	queue_n = queue_i = 0;
	tone_begin();
	pthread_mutex_unlock(&lock);
	if (pthread_create(&mixer, NULL, mixer_main, NULL) != 0) {
		return false;
	}
	mixer_live = true;
	return true;
}

void encoresound_play(const encore_tone_t *tones, int n) {
	if (!mixer_live) {
		return;
	}
	if (n > MAX_TONES) {
		n = MAX_TONES;
	}
	pthread_mutex_lock(&lock);
	memcpy(queue, tones, sizeof(*tones) * (size_t)n);
	queue_n = n;
	queue_i = 0;
	tone_begin();
	pthread_mutex_unlock(&lock);
}

void encoresound_stop(void) {
	if (mixer_live) {
		stop_flag = true;
		pthread_join(mixer, NULL);
		mixer_live = false;
	}
}
