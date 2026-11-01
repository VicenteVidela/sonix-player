#include "soundcore.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/utils.h"

// ---------------------------------------------------------------------------
// The socket, written out for the same reason airpods.c does: libbluetooth's
// headers are not in the cross toolchain's sysroot.
// ---------------------------------------------------------------------------

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_RFCOMM 3

struct sockaddr_rc {
	sa_family_t rc_family;
	uint8_t rc_bdaddr[6];
	uint8_t rc_channel;
};

#define RETRY_MS 5000
#define CONNECT_ATTEMPTS 3
#define POLL_MS 400
#define REFRESH_MS 60000 // the case battery only comes with the full state
#define BUF_LEN 2048
#define MAC_LEN 32
#define NAME_LEN 64

// The A3947 state body (command 01 01), offsets as OpenSCQ30 reads them:
// tws 0-1, battery levels 2-3, charging 4-5, firmware 6-15, serial 16-31,
// a version 32-36, EQ 37-58, one unknown, HearID 60-107, one unknown, buttons
// 109-124, the ambient cycle 125, sound modes 126-132, six unknown, case 139.
#define ST_TWS_CONNECTED 1
#define ST_LEFT 2
#define ST_RIGHT 3
#define ST_LEFT_CHARGING 4
#define ST_RIGHT_CHARGING 5
#define ST_FIRMWARE 6
#define ST_MODES 126
#define ST_CASE 139

// The seven sound-mode bytes, in and out (06 01 in, 06 81 out):
// ambient mode, manual<<4 | adaptive, transparency, NC mode, wind, environment
// detection, transportation.
#define MODES_LEN 7
#define M_AMBIENT 0
#define M_LEVELS 1
#define M_TRANSPARENCY 2
#define M_NC 3
#define M_WIND 4

static const uint8_t DIR_OUT[5] = {0x08, 0xEE, 0x00, 0x00, 0x00};
static const uint8_t DIR_IN[5] = {0x09, 0xFF, 0x00, 0x00, 0x01};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static bool worker_running;
static bool stopping;

static char want_mac[MAC_LEN];
static char want_name[NAME_LEN];
static char have_mac[MAC_LEN];
static int failures;

static soundcore_state_t g_state;
static uint8_t g_modes[MODES_LEN]; // the last seven bytes, so a write keeps the rest
static bool g_modes_known;

static bool pending_modes;
static uint8_t pending_modes_bytes[MODES_LEN];
static int pending_ldac = -1;
static bool pending_refresh;

static char cached_mac[MAC_LEN];
static int cached_channel;

static uint32_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

static void sleep_ms(int ms) {
	struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

static bool parse_mac(const char *text, uint8_t out[6]) {
	int v[6];
	if (!text || strlen(text) != 17 ||
		sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
		return false;
	}
	for (int i = 0; i < 6; i++) {
		out[5 - i] = (uint8_t)v[i];
	}
	return true;
}

static bool contains_ci(const char *hay, const char *needle) {
	size_t n = strlen(needle);
	for (; *hay; hay++) {
		if (strncasecmp(hay, needle, n) == 0) {
			return true;
		}
	}
	return false;
}

static bool is_soundcore_name(const char *name) {
	return name && (contains_ci(name, "soundcore") || contains_ci(name, "liberty"));
}

// ---------------------------------------------------------------------------
// Reading what arrives
// ---------------------------------------------------------------------------

static int level_or_unknown(uint8_t v) { return v <= 5 ? v : -1; }

// Takes the seven sound-mode bytes into the state. Caller holds the lock.
static void take_modes_locked(const uint8_t *m) {
	if (m[M_AMBIENT] > 2 || m[M_NC] > 2 || m[M_TRANSPARENCY] > 1) {
		printf("soundcore: sound modes look wrong (%02x %02x %02x %02x), ignored\n", m[0], m[1], m[2], m[3]);
		return;
	}
	memcpy(g_modes, m, MODES_LEN);
	g_modes_known = true;
	g_state.mode = (soundcore_mode_t)m[M_AMBIENT];
	g_state.nc = (soundcore_nc_t)m[M_NC];
	g_state.nc_level = (m[M_LEVELS] >> 4) & 0x0F;
	g_state.adaptive_level = m[M_LEVELS] & 0x0F;
	g_state.transparency = (soundcore_transparency_t)m[M_TRANSPARENCY];
	g_state.wind = (m[M_WIND] & 1) != 0;
}

static void parse_state(const uint8_t *b, size_t len) {
	if (len < ST_MODES + MODES_LEN) {
		printf("soundcore: state packet of %u bytes, too short\n", (unsigned)len);
		return;
	}
	pthread_mutex_lock(&lock);
	g_state.both_buds = b[ST_TWS_CONNECTED] != 0;
	g_state.left = level_or_unknown(b[ST_LEFT]);
	g_state.right = level_or_unknown(b[ST_RIGHT]);
	g_state.left_charging = b[ST_LEFT_CHARGING] != 0;
	g_state.right_charging = b[ST_RIGHT_CHARGING] != 0;
	char fw[6];
	memcpy(fw, b + ST_FIRMWARE, 5);
	fw[5] = '\0';
	for (int i = 0; i < 5; i++) {
		if (!isprint((unsigned char)fw[i])) {
			fw[i] = '\0';
			break;
		}
	}
	snprintf(g_state.firmware, sizeof(g_state.firmware), "%s", fw);
	take_modes_locked(b + ST_MODES);
	if (len > ST_CASE) {
		g_state.box = level_or_unknown(b[ST_CASE]);
	}
	bool first = !g_state.have_state;
	g_state.have_state = true;
	g_state.serial++;
	pthread_mutex_unlock(&lock);
	if (first) {
		printf("soundcore: state L=%d R=%d case=%d mode=%d nc=%d level=%d fw=%s\n", g_state.left, g_state.right,
			   g_state.box, g_state.mode, g_state.nc, g_state.nc_level, g_state.firmware);
	}
}

static void handle(uint8_t c0, uint8_t c1, const uint8_t *body, size_t len) {
	if (c0 == 0x01 && c1 == 0x01) {
		parse_state(body, len);
		return;
	}
	pthread_mutex_lock(&lock);
	bool changed = true;
	if (c0 == 0x01 && c1 == 0x03 && len >= 2) { // battery levels
		g_state.left = level_or_unknown(body[0]);
		g_state.right = level_or_unknown(body[1]);
	} else if (c0 == 0x01 && c1 == 0x04 && len >= 2) { // charging
		g_state.left_charging = body[0] != 0;
		g_state.right_charging = body[1] != 0;
	} else if (c0 == 0x01 && c1 == 0x02 && len >= 2) { // the buds to each other
		g_state.both_buds = body[1] != 0;
	} else if (c0 == 0x06 && c1 == 0x01 && len >= MODES_LEN) { // sound modes, e.g. a long press on a bud
		take_modes_locked(body);
	} else if (c0 == 0x01 && c1 == 0x7F && len >= 1) { // LDAC
		g_state.ldac = body[0] ? 1 : 0;
	} else {
		// Acks of what was sent (06 81, 01 FF: same command, empty body) and
		// everything this does not use.
		changed = false;
	}
	if (changed) {
		g_state.serial++;
	}
	pthread_mutex_unlock(&lock);
}

// Pulls whole packets off the front of the buffer. Returns how many bytes are
// left. Anything that does not start with the inbound header is skipped a
// byte at a time until something does.
static size_t drain(uint8_t *buf, size_t have) {
	while (have >= 10) {
		if (memcmp(buf, DIR_IN, sizeof(DIR_IN)) != 0) {
			memmove(buf, buf + 1, --have);
			continue;
		}
		size_t len = (size_t)buf[7] | ((size_t)buf[8] << 8);
		if (len < 10 || len > BUF_LEN) {
			memmove(buf, buf + 1, --have);
			continue;
		}
		if (have < len) {
			break; // the rest is still on its way
		}
		uint8_t sum = 0;
		for (size_t i = 0; i + 1 < len; i++) {
			sum = (uint8_t)(sum + buf[i]);
		}
		if (sum == buf[len - 1]) {
			handle(buf[5], buf[6], buf + 9, len - 10);
		} else {
			printf("soundcore: bad checksum on %02x %02x\n", buf[5], buf[6]);
		}
		memmove(buf, buf + len, have - len);
		have -= len;
	}
	return have;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

static bool send_all(int fd, const uint8_t *data, size_t len) {
	while (len) {
		ssize_t n = write(fd, data, len);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) {
				continue;
			}
			return false;
		}
		data += n;
		len -= (size_t)n;
	}
	return true;
}

static bool send_packet(int fd, uint8_t c0, uint8_t c1, const uint8_t *body, size_t len) {
	uint8_t p[64];
	size_t total = 5 + 2 + 2 + len + 1;
	if (total > sizeof(p)) {
		return false;
	}
	memcpy(p, DIR_OUT, 5);
	p[5] = c0;
	p[6] = c1;
	p[7] = (uint8_t)(total & 0xFF);
	p[8] = (uint8_t)(total >> 8);
	if (len) {
		memcpy(p + 9, body, len);
	}
	uint8_t sum = 0;
	for (size_t i = 0; i + 1 < total; i++) {
		sum = (uint8_t)(sum + p[i]);
	}
	p[total - 1] = sum;
	return send_all(fd, p, total);
}

// ---------------------------------------------------------------------------
// Finding the channel
// ---------------------------------------------------------------------------

// sdptool browse --l2cap lists every record with L2CAP in it (Soundcore's own
// service is not always in the public browse group). Records start at
// "Service Name:" or "Service RecHandle:"; the one carrying the 0cf12d31 UUID
// wins, the plain serial port is the fallback.
static int find_channel(const char *mac) {
	char cmd[160];
	// The player may start with a bare PATH; the firmware's tools are in the
	// usual places. The mac was checked by parse_mac, so it is safe in a shell.
	snprintf(cmd, sizeof(cmd), "PATH=\"$PATH:/usr/bin:/usr/sbin:/bin:/sbin\" sdptool browse --l2cap %s 2>/dev/null", mac);
	FILE *f = popen(cmd, "r");
	if (!f) {
		return -1;
	}
	char line[256];
	int vendor = -1, serial = -1;
	int channel = -1;
	bool is_vendor = false, is_serial = false;
	bool any = false, prev_name = false;
	while (fgets(line, sizeof(line), f)) {
		any = true;
		bool is_name = strstr(line, "Service Name:") != NULL;
		bool is_handle = strstr(line, "Service RecHandle:") != NULL;
		bool blank = line[0] == '\n' || line[0] == '\r' || line[0] == '\0';
		// A record starts at its name, or at its handle when it has no name;
		// records are also separated by a blank line.
		if (blank || is_name || (is_handle && !prev_name)) {
			if (channel > 0 && is_vendor && vendor < 0) vendor = channel;
			if (channel > 0 && is_serial && serial < 0) serial = channel;
			channel = -1;
			is_vendor = is_serial = false;
		}
		prev_name = is_name;
		if (contains_ci(line, "0cf12d31")) is_vendor = true;
		if (strstr(line, "Serial Port") || contains_ci(line, "0x1101")) is_serial = true;
		const char *ch = strstr(line, "Channel:");
		if (ch) {
			channel = atoi(ch + 8);
		}
	}
	if (channel > 0 && is_vendor && vendor < 0) vendor = channel;
	if (channel > 0 && is_serial && serial < 0) serial = channel;
	pclose(f);
	if (!any) {
		printf("soundcore: sdptool gave nothing for %s\n", mac);
	}
	int found = vendor > 0 ? vendor : serial;
	printf("soundcore: SDP %s vendor=%d serial=%d\n", mac, vendor, serial);
	return found > 0 && found <= 30 ? found : -1;
}

static int rfcomm_connect(const char *mac) {
	struct sockaddr_rc addr;
	memset(&addr, 0, sizeof(addr));
	addr.rc_family = AF_BLUETOOTH;
	if (!parse_mac(mac, addr.rc_bdaddr)) {
		return -1;
	}
	int channel = strcmp(cached_mac, mac) == 0 ? cached_channel : -1;
	if (channel <= 0) {
		channel = find_channel(mac);
		if (channel <= 0) {
			return -1;
		}
		snprintf(cached_mac, sizeof(cached_mac), "%s", mac);
		cached_channel = channel;
	}
	addr.rc_channel = (uint8_t)channel;

	int fd = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (fd < 0) {
		printf("soundcore: no RFCOMM socket (%s)\n", strerror(errno));
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		printf("soundcore: connect %s ch %d failed (%s)\n", mac, channel, strerror(errno));
		close(fd);
		cached_channel = -1; // look it up again next time
		return -1;
	}
	return fd;
}

// ---------------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------------

static bool should_stop(const char *mac) {
	pthread_mutex_lock(&lock);
	bool stop = stopping || strcmp(want_mac, mac) != 0;
	pthread_mutex_unlock(&lock);
	return stop;
}

static void run_session(int fd, const char *mac) {
	static uint8_t buf[BUF_LEN];
	size_t have = 0;

	printf("soundcore: session open with %s\n", mac);
	bool ok = send_packet(fd, 0x01, 0x01, NULL, 0) && send_packet(fd, 0x01, 0x7F, NULL, 0);
	uint32_t refreshed_at = now_ms();

	while (ok && !should_stop(mac)) {
		struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
		int ready = poll(&pfd, 1, POLL_MS);
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (ready > 0) {
			if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
				break;
			}
			ssize_t n = read(fd, buf + have, sizeof(buf) - have);
			if (n == 0) {
				break;
			}
			if (n < 0) {
				if (errno == EINTR || errno == EAGAIN) {
					continue;
				}
				break;
			}
			have = drain(buf, have + (size_t)n);
			if (have == sizeof(buf)) {
				have = 0; // nothing parseable in a full buffer: start over
			}
		}

		pthread_mutex_lock(&lock);
		bool send_modes = pending_modes;
		uint8_t modes[MODES_LEN];
		memcpy(modes, pending_modes_bytes, MODES_LEN);
		pending_modes = false;
		int ldac = pending_ldac;
		pending_ldac = -1;
		bool refresh = pending_refresh;
		pending_refresh = false;
		pthread_mutex_unlock(&lock);

		if (send_modes) {
			ok = send_packet(fd, 0x06, 0x81, modes, MODES_LEN);
		}
		if (ok && ldac >= 0) {
			uint8_t v = (uint8_t)ldac;
			ok = send_packet(fd, 0x01, 0xFF, &v, 1);
			printf("soundcore: LDAC -> %d\n", ldac);
		}
		if (ok && (refresh || (uint32_t)(now_ms() - refreshed_at) >= REFRESH_MS)) {
			refreshed_at = now_ms();
			ok = send_packet(fd, 0x01, 0x01, NULL, 0);
		}
	}
	close(fd);
	printf("soundcore: session closed\n");
}

static void clear_state(void) {
	pthread_mutex_lock(&lock);
	uint32_t serial = g_state.serial + 1;
	memset(&g_state, 0, sizeof(g_state));
	g_state.left = g_state.right = g_state.box = -1;
	g_state.ldac = -1;
	g_state.serial = serial;
	g_modes_known = false;
	pending_modes = false;
	pending_ldac = -1;
	pending_refresh = false;
	pthread_mutex_unlock(&lock);
}

// ---------------------------------------------------------------------------
// A session with nothing on the other end, for the desktop build:
// SONIX_SOUNDCORE_FAKE=1 feeds the state packet OpenSCQ30 recorded from a real
// Liberty 4 NC (L 2/5, R 4/5, case 2/5, manual NC at 5). Never set on the device.
// ---------------------------------------------------------------------------

static const uint8_t FAKE_STATE[] = {
	1,	 1,	  2,   4,	0,	 0,	  48,  53,	46,	 56,  56,  48,	53,	 46,  56,  56,	51,	 57,  52,  55,
	66,	 54,  54,  50,	70,	 68,  67,  67,	69,	 69,  69,  56,	48,	 48,  46,  48,	48,	 254, 254, 120,
	120, 120, 120, 120, 120, 120, 120, 120, 120, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0,
	1,	 112, 117, 140, 148, 150, 142, 134, 131, 60,  60,  112, 117, 140, 148, 150, 142, 134, 131, 60,
	60,	 104, 100, 34,	64,	 0,	  112, 117, 140, 148, 150, 142, 134, 131, 60,  60,	112, 117, 140, 148,
	150, 142, 134, 131, 60,	 60,  0,   0,	18,	 17,  102, 17,	101, 17,  50,  17,	51,	 17,  68,  17,
	68,	 0,	  33,  0,	32,	 0x35, 0,  0x50, 1,	 0,	  1,   1,	3,	 0,	  0,   0,	0,	 255, 1,   2,
	49,	 1,	  0,   0,	1,	 0,	  0,   1,	0,	 1,	  80,  1,	0,	 0,	  1,   1,	1,	 1,	  255,
};

static bool fake_session(void) {
	const char *spec = getenv("SONIX_SOUNDCORE_FAKE");
	if (!spec || !spec[0]) {
		return false;
	}
	pthread_mutex_lock(&lock);
	g_state.connected = true;
	snprintf(g_state.name, sizeof(g_state.name), "soundcore Liberty 4 NC");
	g_state.ldac = 1;
	g_state.serial++;
	pthread_mutex_unlock(&lock);
	parse_state(FAKE_STATE, sizeof(FAKE_STATE));
	for (;;) {
		pthread_mutex_lock(&lock);
		bool done = stopping;
		pending_modes = false; // taken as done by the setters already
		pending_ldac = -1;
		pending_refresh = false;
		pthread_mutex_unlock(&lock);
		if (done) {
			break;
		}
		sleep_ms(200);
	}
	return true;
}

static void *soundcore_worker(void *unused) {
	(void)unused;
	thread_be_background("soundcore");

	for (;;) {
		char mac[MAC_LEN];
		char name[NAME_LEN];

		pthread_mutex_lock(&lock);
		if (stopping) {
			pthread_mutex_unlock(&lock);
			break;
		}
		snprintf(mac, sizeof(mac), "%s", want_mac);
		snprintf(name, sizeof(name), "%s", want_name);
		if (strcmp(mac, have_mac) != 0) {
			snprintf(have_mac, sizeof(have_mac), "%s", mac);
			failures = 0;
		}
		int tried = failures;
		pthread_mutex_unlock(&lock);

		if (fake_session()) {
			break;
		}

		if (mac[0] && tried < CONNECT_ATTEMPTS && is_soundcore_name(name)) {
			int fd = rfcomm_connect(mac);
			if (fd >= 0) {
				clear_state();
				pthread_mutex_lock(&lock);
				failures = 0;
				g_state.connected = true;
				snprintf(g_state.name, sizeof(g_state.name), "%s", name);
				g_state.serial++;
				pthread_mutex_unlock(&lock);
				run_session(fd, mac);
				clear_state();
			} else {
				pthread_mutex_lock(&lock);
				int count = ++failures;
				pthread_mutex_unlock(&lock);
				if (count == CONNECT_ATTEMPTS) {
					printf("soundcore: %s does not answer, giving up until it reconnects\n", mac);
				}
			}
		}

		pthread_mutex_lock(&lock);
		bool work = stopping || strcmp(want_mac, have_mac) != 0;
		if (!work) {
			struct timespec deadline;
			deadline_in_ms(&deadline, RETRY_MS);
			pthread_cond_timedwait(&wake, &lock, &deadline);
		}
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// public
// ---------------------------------------------------------------------------

void soundcore_init(void) {
	if (worker_running) {
		return;
	}
	clear_state();
	if (pthread_create(&worker, NULL, soundcore_worker, NULL) != 0) {
		fprintf(stderr, "soundcore: could not start the thread\n");
		return;
	}
	worker_running = true;
}

void soundcore_stop(void) {
	if (!worker_running) {
		return;
	}
	pthread_mutex_lock(&lock);
	stopping = true;
	want_mac[0] = '\0';
	pthread_cond_signal(&wake);
	pthread_mutex_unlock(&lock);
	pthread_join(worker, NULL);
	worker_running = false;
	stopping = false;
}

void soundcore_set_device(const char *mac, const char *name) {
	pthread_mutex_lock(&lock);
	const char *wanted = mac ? mac : "";
	if (strcmp(wanted, want_mac) != 0) {
		snprintf(want_mac, sizeof(want_mac), "%s", wanted);
		snprintf(want_name, sizeof(want_name), "%s", name ? name : "");
		pthread_cond_signal(&wake);
	}
	pthread_mutex_unlock(&lock);
}

bool soundcore_get(soundcore_state_t *out) {
	pthread_mutex_lock(&lock);
	*out = g_state;
	bool connected = g_state.connected;
	pthread_mutex_unlock(&lock);
	return connected;
}

// Queues the current seven bytes after a change to them. Caller holds the lock.
static void queue_modes_locked(void) {
	memcpy(pending_modes_bytes, g_modes, MODES_LEN);
	pending_modes = true;
	take_modes_locked(g_modes); // the state follows at once
	g_state.serial++;
}

void soundcore_set_mode(soundcore_mode_t mode) {
	pthread_mutex_lock(&lock);
	if (g_state.connected && g_modes_known && mode <= SOUNDCORE_MODE_NORMAL) {
		g_modes[M_AMBIENT] = (uint8_t)mode;
		queue_modes_locked();
	}
	pthread_mutex_unlock(&lock);
}

void soundcore_set_nc(soundcore_nc_t nc) {
	pthread_mutex_lock(&lock);
	if (g_state.connected && g_modes_known && nc <= SOUNDCORE_NC_TRANSPORT) {
		g_modes[M_AMBIENT] = SOUNDCORE_MODE_NC;
		g_modes[M_NC] = (uint8_t)nc;
		queue_modes_locked();
	}
	pthread_mutex_unlock(&lock);
}

void soundcore_set_nc_level(int level) {
	if (level < 1 || level > 5) {
		return;
	}
	pthread_mutex_lock(&lock);
	if (g_state.connected && g_modes_known) {
		g_modes[M_AMBIENT] = SOUNDCORE_MODE_NC;
		g_modes[M_NC] = SOUNDCORE_NC_MANUAL;
		g_modes[M_LEVELS] = (uint8_t)((level << 4) | (g_modes[M_LEVELS] & 0x0F));
		queue_modes_locked();
	}
	pthread_mutex_unlock(&lock);
}

void soundcore_set_transparency(soundcore_transparency_t t) {
	pthread_mutex_lock(&lock);
	if (g_state.connected && g_modes_known && t <= SOUNDCORE_TRANSPARENCY_VOCAL) {
		g_modes[M_AMBIENT] = SOUNDCORE_MODE_TRANSPARENCY;
		g_modes[M_TRANSPARENCY] = (uint8_t)t;
		queue_modes_locked();
	}
	pthread_mutex_unlock(&lock);
}

void soundcore_set_wind(bool on) {
	pthread_mutex_lock(&lock);
	if (g_state.connected && g_modes_known) {
		// bit 0 is the switch; bit 1 is "wind detected", which is the buds' to say
		g_modes[M_WIND] = (uint8_t)((g_modes[M_WIND] & 0x02) | (on ? 1 : 0));
		queue_modes_locked();
	}
	pthread_mutex_unlock(&lock);
}

void soundcore_set_ldac(bool on) {
	pthread_mutex_lock(&lock);
	if (g_state.connected) {
		pending_ldac = on ? 1 : 0;
		g_state.ldac = on ? 1 : 0;
		g_state.serial++;
	}
	pthread_mutex_unlock(&lock);
}

void soundcore_refresh(void) {
	pthread_mutex_lock(&lock);
	pending_refresh = true;
	pthread_mutex_unlock(&lock);
}
