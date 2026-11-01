#include "screensaver.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/cover.h"
#include "src/gui/nowplaying/coverloader.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/meloetta/clockface.h"
#include "src/gui/meloetta/clockspage.h"
#include "src/system/core/lang.h"
#include "src/gui/shell/clocklabel.h"
#include "src/gui/shell/icons.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/scrolltext.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/topbar.h"
#include "src/system/device/clock.h"
#include "src/system/core/config.h"
#include "src/system/playback/device_state.h"
#include "src/system/device/power.h"
#include "src/system/device/screensaverpics.h"

// Movement that turns a press into a drag, and how long the slide that follows
// the release lasts. The dismiss threshold itself is a quarter of the screen,
// applied in drag_cb; short of it the panel slides back down, like the control
// centre.
#define DRAG_ENGAGE_PX 8
#define DISMISS_ANIM_MS 180

static lv_obj_t *saver;		 // the whole thing, full screen on the top layer
static lv_obj_t *cover_img;	 // the artwork, filling what is above the strip
static lv_obj_t *cover_placeholder; // the note, for a track that has none
static lv_obj_t *info_strip; // the blurred block along the bottom
static lv_obj_t *cover_box;	 // the artwork's frame, everything above the strip
static lv_obj_t *title_label;
static lv_obj_t *artist_label;
static lv_obj_t *clock_label;
static lv_timer_t *clock_timer;
static lv_obj_t *clock_face; // the Clock source: the dial, instead of picture and strip
static lv_obj_t *clock_group; // the dial and, under it, what is playing
static lv_obj_t *mini_row, *mini_art_box, *mini_art, *mini_title, *mini_artist;
static lv_obj_t *timer_line; // Meloetta: a running Clocks timer, under the dial
static lv_obj_t *date_line;	 // Meloetta: "Sun 01/11/2026" under the dial (Vicho, 2026-11-01)

// Meloetta: the page underneath, hidden while the screensaver covers it.
// A wake repaints the whole screen bottom layer first, and Now Playing (a
// full-screen blurred backdrop) takes long enough to draw that the panel could
// show it for a frame before the screensaver landed on top: the flicker on
// lock and unlock, and now and then when the page redrew under it. Hidden, it
// costs nothing to draw and there is nothing to flash. Shown again the moment
// a swipe starts uncovering it.
static lv_obj_t *covered_screen;
static void cover_screen(void) {
	lv_obj_t *scr = lv_screen_active();
	if (covered_screen && covered_screen != scr) {
		lv_obj_set_hidden(covered_screen, false);
		covered_screen = NULL;
	}
	if (scr && !covered_screen && !lv_obj_is_hidden(scr)) {
		lv_obj_set_hidden(scr, true);
		covered_screen = scr;
	}
}
static void uncover_screen(void) {
	if (covered_screen) {
		lv_obj_set_hidden(covered_screen, false);
		covered_screen = NULL;
	}
}
#define MINI_ART 72 // the thumbnail beside the title, in pixels
// The Clock source shows the status bar over it, white on the black, and
// gives the bar back as it found it when it goes.
static bool bar_borrowed, bar_was_hidden, bar_was_over_art;

static bool enabled;
static int screen_w, screen_h;
static int strip_h;

// What the strip falls back to when there is no artwork to size it from, and
// the height it always has over a picture from the folder -- there the block
// is built to order rather than borrowed, so there is nothing to measure.
#define DEFAULT_STRIP_H 250

// ---------------------------------------------------------------------------
// Pictures from the card
//
// The other thing the screensaver can show: one of the JPEGs in the card's
// Screensaver folder, a different one each time the screen lights up. Which
// files qualify and which one comes up is screensaverpics.h; what is left here
// is the drawing and the pixels it owns.
//
// Nothing is decoded on this thread. The picture for the next wake is asked of
// the cover loader as the panel goes dark and is waiting, decoded, by the time
// the screen comes back -- so a wake costs a pointer swap rather than a card
// read, a JPEG and a blur with the interface thread standing still and the
// audio decoder sharing the one core with it.
// ---------------------------------------------------------------------------

static screensaver_source_t source;
// The picture on screen and the blurred block over its lower part. Owned here,
// not by the player: freed here whenever a different one is chosen or the
// screensaver goes away.
static cover_image_t image_pic, image_strip;
static char image_file[512]; // what image_pic was built from

// The one being prepared for the next wake: `next_file` while it is out with
// the loader, and the pixels once they have been collected.
static cover_image_t next_pic, next_strip;
static char next_file[512];
static bool next_ready;	  // next_pic/next_strip hold a collected picture
static bool next_pending; // a request is out with the loader

// Resizes the strip and gives the picture whatever it should have.
//
// LVGL draws a background image at its own size, centred, and paints whatever
// is left over in the background colour, so a strip even a few pixels taller
// than the blurred block would show a black bar above it and another below.
// The height is therefore taken from the picture, leaving nothing to paint.
//
// Over an album cover the picture's box stops where the strip starts. Over a
// photograph from the folder it is the whole screen: there the strip is drawn
// ON TOP of a picture that fills the panel, which is what "the blurred part
// hides the bottom of the image" means.
//
// Cheap, and a no-op unless something has actually changed.
static int cover_box_h;

static void set_strip_height(int height) {
	if (height <= 0 || height >= screen_h) {
		return;
	}
	if (height != strip_h) {
		strip_h = height;
		lv_obj_set_height(info_strip, strip_h);
		lv_obj_align(info_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
	}
	int want = source == SCREENSAVER_SOURCE_IMAGES ? screen_h : screen_h - strip_h;
	if (want != cover_box_h) {
		cover_box_h = want;
		lv_obj_set_height(cover_box, cover_box_h);
	}
}

bool screensaver_is_visible(void) { return saver && !lv_obj_is_hidden(saver); }
bool screensaver_get_enabled(void) { return enabled; }

screensaver_source_t screensaver_source(void) { return source; }

static void request_next(void);
static void drop_next(void);

void screensaver_set_source(screensaver_source_t chosen) {
	source = chosen == SCREENSAVER_SOURCE_IMAGES || chosen == SCREENSAVER_SOURCE_CLOCK ? chosen : SCREENSAVER_SOURCE_ALBUM;
	config_set_int("screen", "screensaver_source", (int)source);
	config_save();
	if (source == SCREENSAVER_SOURCE_IMAGES) {
		// Asked for now rather than at the first screen-off, so the first wake
		// after the option is chosen already has a picture to show.
		request_next();
	} else {
		drop_next();
	}
}

void screensaver_set_card_root(const char *root) { screensaverpics_set_root(root); }

bool screensaver_has_images(void) { return screensaverpics_any(screen_w, screen_h); }

// Picks the next picture and puts it out to decode. Called as the panel goes
// dark, which is the whole point: the card read and the JPEG happen on the
// loader thread at background priority over a screen nobody is looking at.
//
// A no-op while one is already in hand or in flight, so the repeated wake-hook
// calls cost nothing.
static void request_next(void) {
	if (source != SCREENSAVER_SOURCE_IMAGES || next_ready || next_pending) {
		return;
	}
	char chosen[sizeof(next_file)];
	if (!screensaverpics_pick(image_file, screen_w, screen_h, chosen, sizeof(chosen))) {
		// The folder gave nothing at all -- emptied, or the card taken out.
		// What is already up stays up: it is still a picture, and black is not.
		return;
	}
	snprintf(next_file, sizeof(next_file), "%s", chosen);
	next_pending = true;
	coverloader_request_screensaver(next_file, screen_w, screen_h, screen_w, DEFAULT_STRIP_H);
}

// Takes delivery of a finished decode, if there is one. Cheap and non-blocking:
// called at every wake and on the tick while the screensaver is up.
static void collect_next(void) {
	if (!next_pending) {
		return;
	}
	bool finished = false;
	cover_image_t pic = {0}, strip = {0};
	bool found = coverloader_take_screensaver(&pic, &strip, &finished);
	if (!finished) {
		return;
	}
	next_pending = false;
	if (!found) {
		// It passed the header check and still would not decode. Forgotten, so
		// the next request picks another rather than retrying this one.
		next_file[0] = '\0';
		return;
	}
	cover_free(&next_pic);
	cover_free(&next_strip);
	next_pic = pic;
	next_strip = strip;
	next_ready = true;
}

static void drop_next(void) {
	if (next_pending) {
		coverloader_release_screensaver();
		next_pending = false;
	}
	cover_free(&next_pic);
	cover_free(&next_strip);
	next_ready = false;
	next_file[0] = '\0';
}

static void drop_image(void) {
	cover_free(&image_pic);
	cover_free(&image_strip);
	image_file[0] = '\0';
}

void screensaver_set_enabled(bool on) {
	enabled = on;
	config_set_bool("screen", "screensaver", on);
	config_save();
	if (!on) {
		if (screensaver_is_visible()) {
			screensaver_hide(); // frees what is held on its way out
		} else {
			drop_next();
		}
	}
}

// ---------------------------------------------------------------------------
// what it shows
// ---------------------------------------------------------------------------

static void refresh_clock(void) {
	clocklabel_refresh(clock_label);
	if (source == SCREENSAVER_SOURCE_CLOCK) {
		clockface_refresh(clock_face);
		if (date_line) {
			time_t now = time(NULL);
			struct tm t;
			localtime_r(&now, &t);
			char date[32];
			strftime(date, sizeof(date), "%a %d/%m/%Y", &t);
			lv_label_set_text(date_line, date);
		}
		// Meloetta: the Clocks timer, if one is going. Faded rather than
		// hidden, so the dial does not jump when it starts or ends.
		char left[24], line[64];
		bool paused = false;
		if (timer_line && clockspage_timer_text(left, sizeof(left), &paused)) {
			snprintf(line, sizeof(line), paused ? "%s  %s  ||" : "%s  %s", tr("clocks_timer"), left);
			lv_label_set_text(timer_line, line);
			lv_obj_set_style_opa(timer_line, LV_OPA_COVER, 0);
		} else if (timer_line) {
			lv_obj_set_style_opa(timer_line, LV_OPA_TRANSP, 0);
		}
	}
}

// What refresh_content() last drew, so the tick below can tell an idle moment
// from a track that changed while the screensaver was up.
static char shown_file[512];
static const void *shown_cover;

static void refresh_content(void);

// Runs while the screensaver is up. The clock is the obvious reason, but the
// important one is the track: the player frees its artwork at every track
// change and calls screensaver_release_artwork(), which blanks the picture, so
// this check is what puts the new one back.
static void clock_timer_cb(lv_timer_t *timer) {
	(void)timer;

	// Takes delivery of a decode that landed after the wake had already given
	// up on it -- a short dark spell, or a slow card. It is only stored, never
	// swapped in: the picture must not change while it is being looked at.
	collect_next();

	if (!screensaver_is_visible()) {
		refresh_clock();
		return;
	}

	device_state_t state;
	device_state_get(&state);
	const void *cover = player_cover_image();

	const char *now = state.live ? state.metadata.title : state.current_file;
	if (strcmp(now, shown_file) != 0 || cover != shown_cover) {
		refresh_content(); // refreshes the clock on its way through
		return;
	}

	refresh_clock();
}

// Puts the prepared picture on screen. Unlike the album source this owns its
// pixels, so the swap is the careful part: LVGL keeps drawing from the buffer
// it was given, and freeing the old one while it is still the widget's source
// is a use-after-free at the next redraw. Off the screen first, then free.
static void promote_next(void) {
	if (!next_ready) {
		return;
	}
	lv_image_set_src(cover_img, NULL);
	lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
	cover_free(&image_pic);
	cover_free(&image_strip);
	image_pic = next_pic;
	image_strip = next_strip;
	snprintf(image_file, sizeof(image_file), "%s", next_file);

	memset(&next_pic, 0, sizeof(next_pic));
	memset(&next_strip, 0, sizeof(next_strip));
	next_ready = false;
	next_file[0] = '\0';
}

// Draws whatever is currently held. Nothing is decoded here.
static void draw_from_folder(void) {
	set_strip_height(DEFAULT_STRIP_H);

	if (image_pic.pixels) {
		lv_image_set_src(cover_img, &image_pic.dsc);
		// Already built at screen size by cover_load_screensaver_images: no
		// zoom to apply, and none to leave over from the album source.
		lv_image_set_scale(cover_img, 256);
		lv_obj_set_hidden(cover_img, false);
		lv_obj_set_hidden(cover_placeholder, true);
	} else {
		// An empty folder, or a card that is not there. The note in the middle
		// of a black screen, which is what the album source shows for a track
		// with no artwork -- the same screen, missing the same thing.
		lv_image_set_src(cover_img, NULL);
		lv_obj_set_hidden(cover_img, true);
		lv_obj_set_hidden(cover_placeholder, false);
	}

	if (image_strip.pixels) {
		lv_obj_set_style_bg_image_src(info_strip, &image_strip.dsc, 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_TRANSP, 0);
	} else {
		lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
		lv_obj_set_style_bg_color(info_strip, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_60, 0);
	}
}

// The artwork is the player's own, already decoded for this track: borrowed
// for as long as the screensaver is up and dropped the moment it goes down,
// so a track change while it is hidden can never leave a stale pointer here.
// A picture from the folder is the other way round -- owned here, decoded here
// and freed here.
static void refresh_content(void) {
	device_state_t state;
	device_state_get(&state);

	// Recorded before anything is drawn from it, so what the tick above
	// compares against is exactly what ended up on screen. A live stream has no
	// file, so the station name stands in; without it every radio would look
	// identical to the tick and a change of station would never redraw.
	snprintf(shown_file, sizeof(shown_file), "%s",
			 state.live ? state.metadata.title : state.current_file);

	// The placeholder behind a missing cover says what kind of thing is
	// playing: a note for a track, a radio for a stream.
	if (cover_placeholder) {
		lv_image_set_src(cover_placeholder, state.live ? &icon_radio_player : &icon_music_note);
	}

	if (state.live) {
		scrolltext_set(title_label, state.metadata.title);
		scrolltext_set(artist_label, state.metadata.artist);
	} else if (state.current_file[0]) {
		const char *slash = strrchr(state.current_file, '/');
		scrolltext_set(title_label, state.metadata.title[0] ? state.metadata.title
															: (slash ? slash + 1 : state.current_file));
		const char *artist = metadata_shown_artist(&state.metadata);
		scrolltext_set(artist_label, artist);
	} else {
		scrolltext_set(title_label, "");
		scrolltext_set(artist_label, "");
	}

	// Recorded whatever is being drawn: the tick above compares it to notice a
	// track change, and in Images mode a track change still has to redraw the
	// title -- it just must not change the picture.
	shown_cover = player_cover_image();

	// The Clock source shows the dial alone: no picture, no track.
	bool clock_only = source == SCREENSAVER_SOURCE_CLOCK;
	lv_obj_set_hidden(cover_box, clock_only);
	lv_obj_set_hidden(info_strip, clock_only);
	lv_obj_set_hidden(clock_group, !clock_only);
	if (clock_only) {
		clockface_refresh(clock_face);
		// Under the dial, what is playing: the same title and artist as the
		// strip, and the cover shrunk to a thumbnail, cropped square.
		bool any = state.live || state.current_file[0];
		// Faded out rather than hidden, so the dial stays where it is.
		lv_obj_set_style_opa(mini_row, any ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		if (any) {
			const char *slash = strrchr(state.current_file, (int)0x2f);
			scrolltext_set(mini_title, state.live || state.metadata.title[0] ? state.metadata.title
														: (slash ? slash + 1 : state.current_file));
			scrolltext_set(mini_artist, state.live ? state.metadata.artist : metadata_shown_artist(&state.metadata));
			const lv_image_dsc_t *art = shown_cover;
			lv_obj_set_hidden(mini_art_box, !art);
			if (art) {
				lv_image_set_src(mini_art, art);
				int32_t zw = 256 * MINI_ART / (int32_t)art->header.w, zh = 256 * MINI_ART / (int32_t)art->header.h;
				lv_image_set_scale(mini_art, zw > zh ? zw : zh);
			} else {
				lv_image_set_src(mini_art, NULL);
			}
		}
		return;
	}

	if (source == SCREENSAVER_SOURCE_IMAGES) {
		draw_from_folder();
		refresh_clock();
		return;
	}

	// The strip first: the artwork below is scaled to whatever is left above
	// it, so its height has to be settled before that sum is done.
	const lv_image_dsc_t *backdrop = player_backdrop_image();
	// The player's blurred copy is as tall as whatever shows it, and one
	// arrangement shows it behind the whole screen: a strip that tall is the
	// screen, so that one keeps the default height and takes the top of the
	// picture instead.
	int strip_from_backdrop = backdrop ? (int)backdrop->header.h : 0;
	set_strip_height(strip_from_backdrop > 0 && strip_from_backdrop < screen_h ? strip_from_backdrop
																			  : DEFAULT_STRIP_H);

	const lv_image_dsc_t *cover = shown_cover;
	if (cover) {
		lv_image_set_src(cover_img, cover);
		// Fill the whole area above the strip, cropping the overflow: a
		// letterboxed cover with black bars would look like a bug, not a
		// screensaver.
		int32_t zoom_w = (256 * screen_w) / (int32_t)cover->header.w;
		int32_t zoom_h = (256 * (screen_h - strip_h)) / (int32_t)cover->header.h;
		lv_image_set_scale(cover_img, zoom_w > zoom_h ? zoom_w : zoom_h);
		lv_obj_set_hidden(cover_img, false);
		lv_obj_set_hidden(cover_placeholder, true);
	} else {
		lv_image_set_src(cover_img, NULL);
		lv_obj_set_hidden(cover_img, true);
		lv_obj_set_hidden(cover_placeholder, false);
	}

	if (backdrop) {
		lv_obj_set_style_bg_image_src(info_strip, backdrop, 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_TRANSP, 0);
	} else {
		// No artwork at all: a plain dark strip carries the text instead.
		lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
		lv_obj_set_style_bg_color(info_strip, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(info_strip, LV_OPA_60, 0);
	}

	refresh_clock();
}

// ---------------------------------------------------------------------------
// raising and dismissing
// ---------------------------------------------------------------------------

static void anim_y_cb(void *obj, int32_t v);

static void borrow_bar(void) {
	bar_borrowed = true;
	bar_was_hidden = topbar_is_hidden();
	bar_was_over_art = topbar_is_over_artwork();
	topbar_set_hidden(false);
	topbar_set_over_artwork(true, false);
	topbar_bring_to_front();
}

static void return_bar(void) {
	if (!bar_borrowed) {
		return;
	}
	bar_borrowed = false;
	if (!bar_was_over_art) {
		topbar_set_over_artwork(false, false);
	}
	if (bar_was_hidden) {
		topbar_set_hidden(true);
	}
}

void screensaver_show(void) {
	if (!saver || !enabled || screensaver_is_visible()) {
		return;
	}

	collect_next();
	promote_next();
	refresh_content();
	lv_anim_delete(saver, anim_y_cb);
	lv_obj_set_y(saver, 0);
	lv_obj_set_hidden(saver, false);
	lv_obj_move_foreground(saver);
	if (source == SCREENSAVER_SOURCE_CLOCK) {
		borrow_bar();
	}
	cover_screen();
	lv_timer_resume(clock_timer);
}

// Everything that has to happen once it is really gone, whether it was
// dismissed by a tap, by a finished swipe, or by switching the option off.
static void finish_hide(void) {
	uncover_screen();
	lv_obj_set_hidden(saver, true);
	return_bar();
	lv_obj_set_y(saver, 0);
	lv_timer_pause(clock_timer);
	// Let go of the player's pixels: it frees them at the next track change,
	// and nothing here may still be pointing at them by then. The picture owned
	// here goes too -- about a megabyte, and the device is in use again, which
	// is exactly when that megabyte is worth having back.
	lv_image_set_src(cover_img, NULL);
	lv_image_set_src(mini_art, NULL);
	lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
	drop_image();
	// The one being prepared goes too. It is another megabyte, the device is in
	// use again, and the next screen-off asks for a fresh one anyway.
	drop_next();
	power_notify_activity(); // dismissing it counts as using the device
}

static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

// Where the slide currently running is headed. Held explicitly rather than read
// back from the final coordinate: LVGL can finish an animation a pixel or four
// short of its end value, which would answer "did it leave?" with a no and
// leave the screensaver technically visible off-screen forever.
static bool sliding_out;

static void slide_done_cb(lv_anim_t *a) {
	(void)a;
	if (sliding_out) {
		finish_hide();
	} else {
		lv_obj_set_y(saver, 0); // exactly home, whatever the last frame did
		cover_screen(); // fell back: covered again
	}
}

// Animates the last stretch after the finger lets go: up and away, or back
// down into place.
static void slide_to(int y) {
	lv_anim_delete(saver, anim_y_cb);
	sliding_out = (y != 0);
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, saver);
	lv_anim_set_exec_cb(&a, anim_y_cb);
	lv_anim_set_values(&a, lv_obj_get_y(saver), y);
	lv_anim_set_duration(&a, DISMISS_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, slide_done_cb);
	lv_anim_start(&a);
}

bool screensaver_prepare(void) {
	if (!saver || !enabled) {
		return false;
	}
	// Called once as the panel goes dark and again as it lights up, and the two
	// do different work. Going dark is when the next picture is asked for: the
	// screen is about to be off, the loader thread has it to itself, and nobody
	// is waiting. Lighting up is when it is collected and swapped in -- no card,
	// no decode, nothing between the button and the backlight.
	bool waking = power_screen_is_on();

	if (screensaver_is_visible()) {
		collect_next();
		if (waking) {
			promote_next();
		}
		refresh_content();
		cover_screen(); // the page may have changed under it
	} else {
		screensaver_show(); // collects and promotes on its way through
	}

	if (!waking) {
		request_next();
	}
	return screensaver_is_visible();
}

void screensaver_release_artwork(void) {
	if (!cover_img) {
		return;
	}
	if (source == SCREENSAVER_SOURCE_IMAGES) {
		// Nothing here is the player's: it can free its artwork without this
		// screen losing anything, and blanking the picture would put a black
		// screen up for no reason.
		return;
	}

	// Whatever was on screen is gone; the tick above has to see a difference
	// so it puts the new track's picture up rather than leaving black.
	shown_cover = NULL;
	shown_file[0] = '\0';
	// Whatever happens next, nothing here may still be pointing at the
	// player's buffers when it frees them.
	lv_image_set_src(cover_img, NULL);
	lv_image_set_src(mini_art, NULL);
	lv_obj_set_hidden(cover_img, true);
	lv_obj_set_hidden(cover_placeholder, false);
	lv_obj_set_style_bg_image_src(info_strip, NULL, 0);
	lv_obj_set_style_bg_color(info_strip, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(info_strip, LV_OPA_60, 0);
}

void screensaver_hide(void) {
	if (!saver || !screensaver_is_visible()) {
		return;
	}
	uncover_screen(); // it slides away over the page
	slide_to(-screen_h);
}

// The swipe up, following the finger the whole way: the screensaver slides
// with it, and the release decides whether it carries on off the top or falls
// back into place. Same feel as the control centre.
static void drag_cb(lv_event_t *e) {
	static lv_point_t start;
	static bool tracking;
	static bool engaged;

	lv_indev_t *indev = lv_indev_active();
	if (!indev) {
		return;
	}

	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED) {
		lv_indev_get_point(indev, &start);
		lv_anim_delete(saver, anim_y_cb);
		tracking = true;
		engaged = false;
		return;
	}
	if (!tracking) {
		return;
	}

	lv_point_t point;
	lv_indev_get_point(indev, &point);
	int dy = point.y - start.y;

	if (code == LV_EVENT_PRESSING) {
		if (!engaged) {
			if (dy > -DRAG_ENGAGE_PX) {
				return; // not (yet) an upward drag
			}
			engaged = true;
			uncover_screen(); // the page shows as the finger lifts the screensaver
		}
		// Only upward: dragging down does not pull it off the bottom.
		lv_obj_set_y(saver, dy < 0 ? dy : 0);
		return;
	}

	if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		tracking = false;
		if (!engaged) {
			return;
		}
		engaged = false;
		// A quarter of the screen of travel and it goes; short of that it
		// falls back.
		slide_to(dy < -screen_h / 4 ? -screen_h : 0);
	}
}

void screensaver_init(gui_config_t *cfg) {
	screen_w = cfg->screen_width;
	screen_h = cfg->screen_height;
	strip_h = DEFAULT_STRIP_H;

	screensaver_set_card_root(cfg->sd_root_path);
	enabled = config_get_bool("screen", "screensaver", false);
	source = (screensaver_source_t)config_get_int("screen", "screensaver_source", SCREENSAVER_SOURCE_ALBUM);
	if (source != SCREENSAVER_SOURCE_IMAGES && source != SCREENSAVER_SOURCE_CLOCK) {
		source = SCREENSAVER_SOURCE_ALBUM;
	}
	cover_box_h = screen_h - strip_h;

	saver = lv_obj_create(lv_layer_top());
	lv_obj_set_size(saver, screen_w, screen_h);
	lv_obj_set_pos(saver, 0, 0);
	lv_obj_set_style_bg_color(saver, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(saver, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(saver, 0, 0);
	lv_obj_set_style_radius(saver, 0, 0);
	lv_obj_set_style_pad_all(saver, 0, 0);
	lv_obj_set_scrollable(saver, false);
	lv_obj_set_clickable(saver, true);
	lv_obj_set_hidden(saver, true);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(saver, drag_cb, LV_EVENT_PRESS_LOST, NULL);

	// The artwork: everything above the strip, centre-cropped to fill it.
	cover_box = lv_obj_create(saver);
	lv_obj_set_size(cover_box, screen_w, screen_h - strip_h);
	lv_obj_set_pos(cover_box, 0, 0);
	lv_obj_set_style_bg_color(cover_box, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(cover_box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(cover_box, 0, 0);
	lv_obj_set_style_radius(cover_box, 0, 0);
	lv_obj_set_style_pad_all(cover_box, 0, 0);
	lv_obj_set_style_clip_corner(cover_box, true, 0);
	lv_obj_set_scrollable(cover_box, false);
	lv_obj_set_event_bubble(cover_box, true);
	lv_obj_set_clickable(cover_box, false);

	// Nothing to show for a track with no artwork: the same note the player
	// puts in the same place, so the screensaver is recognisably the player
	// with the controls taken away rather than a different screen that has
	// gone wrong.
	cover_placeholder = lv_image_create(cover_box);
	lv_image_set_src(cover_placeholder, &icon_music_note);
	lv_obj_set_style_image_recolor(cover_placeholder, theme()->text_secondary, 0);
	lv_obj_set_style_image_recolor_opa(cover_placeholder, LV_OPA_COVER, 0);
	lv_obj_center(cover_placeholder);

	cover_img = lv_image_create(cover_box);
	lv_obj_center(cover_img);
	lv_image_set_inner_align(cover_img, LV_IMAGE_ALIGN_CENTER);

	// The blurred block: the same picture the player draws its controls on.
	info_strip = lv_obj_create(saver);
	lv_obj_set_size(info_strip, screen_w, strip_h);
	lv_obj_align(info_strip, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_opa(info_strip, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(info_strip, 0, 0);
	lv_obj_set_style_radius(info_strip, 0, 0);
	lv_obj_set_style_pad_hor(info_strip, cfg->padding + 6, 0);
	lv_obj_set_style_pad_ver(info_strip, 18, 0);
	lv_obj_set_style_pad_gap(info_strip, 2, 0);
	lv_obj_set_scrollable(info_strip, false);
	lv_obj_set_event_bubble(info_strip, true);
	lv_obj_set_clickable(info_strip, false);
	lv_obj_set_flex_flow(info_strip, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(info_strip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	// The blurred backdrop is always the dark treatment, so the text on it is
	// light in both themes.
	title_label = lv_label_create(info_strip);
	lv_obj_set_width(title_label, lv_pct(100));
	lv_obj_set_style_text_font(title_label, &font_ui_28, 0);
	lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
	lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(title_label);

	artist_label = lv_label_create(info_strip);
	lv_obj_set_width(artist_label, lv_pct(100));
	lv_obj_set_style_text_font(artist_label, &font_ui_22, 0);
	lv_obj_set_style_text_color(artist_label, lv_color_make(200, 200, 200), 0);
	lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
	scrolltext_apply(artist_label);

	// The time, in the largest figures the screen has.
	clock_label = clocklabel_create(info_strip, &font_ui_72, 72, &font_ui_32, 32);
	lv_obj_set_style_text_color(clock_label, lv_color_white(), 0);
	lv_obj_set_style_pad_top(clock_label, 6, 0);

	// The way out, spelled out: swipe up. A sign, not a button -- tapping it
	// does nothing, since the screensaver is only dismissed by dragging -- so it
	// is not clickable and a finger landing on it talks to the saver underneath,
	// where the drag gesture lives.
	lv_obj_t *chevron_btn = lv_obj_create(saver);
	lv_obj_set_size(chevron_btn, 80, 52);
	lv_obj_align(chevron_btn, LV_ALIGN_BOTTOM_MID, 0, -4);
	lv_obj_set_style_bg_opa(chevron_btn, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(chevron_btn, 0, 0);
	lv_obj_set_style_shadow_width(chevron_btn, 0, 0);
	lv_obj_set_style_pad_all(chevron_btn, 0, 0);
	lv_obj_set_clickable(chevron_btn, false);
	lv_obj_set_scrollable(chevron_btn, false);

	lv_obj_t *chevron = lv_image_create(chevron_btn);
	lv_image_set_src(chevron, &icon_chevron_up);
	lv_obj_set_style_image_recolor(chevron, lv_color_white(), 0);
	lv_obj_set_style_image_recolor_opa(chevron, LV_OPA_COVER, 0);
	lv_obj_set_style_image_opa(chevron, LV_OPA_70, 0);
	lv_obj_center(chevron);

	// The Clock source: the Persona 3 dial alone, a little above the middle,
	// clear of the chevron.
	clock_group = lv_obj_create(saver);
	lv_obj_set_size(clock_group, screen_w, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(clock_group, 0, 0);
	lv_obj_set_style_border_width(clock_group, 0, 0);
	lv_obj_set_style_pad_all(clock_group, 0, 0);
	lv_obj_set_style_pad_row(clock_group, 24, 0);
	lv_obj_set_scrollable(clock_group, false);
	lv_obj_set_clickable(clock_group, false);
	lv_obj_set_event_bubble(clock_group, true);
	lv_obj_set_flex_flow(clock_group, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(clock_group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// The dial the Clocks page has, the same size, and the two a little above
	// the middle, clear of the chevron.
	clock_face = clockface_create(clock_group, screen_w * 3 / 4, false);

	date_line = lv_label_create(clock_group);
	lv_obj_set_style_text_font(date_line, &font_ui_28, 0);
	lv_obj_set_style_text_color(date_line, lv_color_white(), 0);
	lv_label_set_text(date_line, "");

	// Meloetta: the running timer, between the dial and what is playing.
	timer_line = lv_label_create(clock_group);
	lv_obj_set_style_text_font(timer_line, &font_ui_32, 0);
	lv_obj_set_style_text_color(timer_line, lv_color_white(), 0);
	lv_obj_set_style_opa(timer_line, LV_OPA_TRANSP, 0);
	lv_label_set_text(timer_line, "0:00");

	mini_row = lv_obj_create(clock_group);
	lv_obj_set_size(mini_row, screen_w - 2 * (cfg->padding + 18), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(mini_row, 0, 0);
	lv_obj_set_style_border_width(mini_row, 0, 0);
	lv_obj_set_style_pad_all(mini_row, 0, 0);
	lv_obj_set_style_pad_column(mini_row, 16, 0);
	lv_obj_set_scrollable(mini_row, false);
	lv_obj_set_clickable(mini_row, false);
	lv_obj_set_event_bubble(mini_row, true);
	lv_obj_set_flex_flow(mini_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(mini_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	mini_art_box = lv_obj_create(mini_row);
	lv_obj_set_size(mini_art_box, MINI_ART, MINI_ART);
	lv_obj_set_style_bg_color(mini_art_box, lv_color_make(28, 28, 32), 0);
	lv_obj_set_style_bg_opa(mini_art_box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(mini_art_box, 0, 0);
	lv_obj_set_style_radius(mini_art_box, 10, 0);
	lv_obj_set_style_clip_corner(mini_art_box, true, 0);
	lv_obj_set_style_pad_all(mini_art_box, 0, 0);
	lv_obj_set_scrollable(mini_art_box, false);
	lv_obj_set_clickable(mini_art_box, false);
	lv_obj_set_event_bubble(mini_art_box, true);
	mini_art = lv_image_create(mini_art_box);
	lv_obj_center(mini_art);
	lv_image_set_inner_align(mini_art, LV_IMAGE_ALIGN_CENTER);
	lv_obj_t *mini_text = lv_obj_create(mini_row);
	lv_obj_set_height(mini_text, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(mini_text, 1);
	lv_obj_set_style_bg_opa(mini_text, 0, 0);
	lv_obj_set_style_border_width(mini_text, 0, 0);
	lv_obj_set_style_pad_all(mini_text, 0, 0);
	lv_obj_set_style_pad_row(mini_text, 4, 0);
	lv_obj_set_scrollable(mini_text, false);
	lv_obj_set_clickable(mini_text, false);
	lv_obj_set_event_bubble(mini_text, true);
	lv_obj_set_flex_flow(mini_text, LV_FLEX_FLOW_COLUMN);
	mini_title = lv_label_create(mini_text);
	lv_obj_set_width(mini_title, lv_pct(100));
	lv_obj_set_style_text_font(mini_title, &font_ui_24, 0);
	lv_obj_set_style_text_color(mini_title, lv_color_white(), 0);
	scrolltext_apply(mini_title);
	mini_artist = lv_label_create(mini_text);
	lv_obj_set_width(mini_artist, lv_pct(100));
	lv_obj_set_style_text_font(mini_artist, &font_ui_20, 0);
	lv_obj_set_style_text_color(mini_artist, lv_color_make(170, 170, 170), 0);
	scrolltext_apply(mini_artist);

	lv_obj_align(clock_group, LV_ALIGN_CENTER, 0, -40);
	lv_obj_set_hidden(clock_group, true);

	// One second, not ten: the clock only needs a minute's resolution, but the
	// same tick is what notices a track change, and a screensaver that shows
	// the previous song for ten seconds is a screensaver that looks broken.
	clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
	lv_timer_pause(clock_timer);
	// It is drawn into the framebuffer before the blank so the next wake opens
	// on it -- but with the panel off nothing is scanned out, so ticking it
	// once a second all night buys nothing.
	power_pause_in_standby(clock_timer);
}
