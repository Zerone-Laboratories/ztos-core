// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * webview: a WPE WebKit page on a Wayland subsurface under the app's main
 * surface. The chrome (superscan.c) talks only to this interface, so the
 * engine stays in one place.
 */
#ifndef ZTBROWSER_WEBVIEW_H
#define ZTBROWSER_WEBVIEW_H

#include <stdbool.h>

enum wv_load {
	WV_STARTED,	/* a new page began to load */
	WV_COMMITTED,	/* the first bytes arrived (the URL is now the real one) */
	WV_FINISHED,	/* done */
	WV_FAILED,
};

/* What the view tells the chrome. Every callback runs on the main thread. */
struct wv_delegate {
	void (*title)(void *user, const char *title);
	void (*uri)(void *user, const char *uri);
	void (*progress)(void *user, double fraction);	/* 0..1 */
	void (*load)(void *user, enum wv_load state);
	void (*secure)(void *user, bool https_ok);
	/* a text field in the page took or lost focus; caret_bottom: where its
	 * caret ends, in points from the top of the view (-1: not known) */
	void (*ime)(void *user, bool active, int caret_bottom);
	/* the page started or stopped playing sound */
	void (*playing)(void *user, bool playing);
	void *user;
};

struct webview;

/* Create the view: a subsurface of `parent` (zmc.surface), placed at x,y,w,h
 * in points on a `scale`-times buffer, persisting its data under `data_dir`.
 * Returns NULL if WPE or the backend could not start. */
struct webview *webview_create(struct wl_display *display, struct wl_compositor *compositor,
			       struct wl_surface *parent, const char *data_dir,
			       const struct wv_delegate *delegate);

void webview_place(struct webview *wv, int x, int y, int w, int h, int scale);
/* the `user` the delegate's callbacks get from now on (a tab that moved) */
void webview_set_user(struct webview *wv, void *user);
void webview_show(struct webview *wv, bool on);

void webview_load(struct webview *wv, const char *url);
void webview_reload(struct webview *wv, bool bypass_cache);
void webview_stop(struct webview *wv);
bool webview_can_go_back(struct webview *wv);
bool webview_can_go_forward(struct webview *wv);
void webview_back(struct webview *wv);
void webview_forward(struct webview *wv);

/* input in the content area, in points relative to the view's top-left */
/* a finger on the page: a drag scrolls it, a tap clicks; time in ms */
void webview_touch_down(struct webview *wv, int id, double x, double y, unsigned time);
void webview_touch_motion(struct webview *wv, int id, double x, double y, unsigned time);
void webview_touch_up(struct webview *wv, int id, unsigned time);
void webview_touch_cancel(struct webview *wv);
/* typing into the page's focused text field: a char, "\b" or "\n" */
void webview_key(struct webview *wv, const char *utf8);
void webview_scroll(struct webview *wv, double dx, double dy);

/* Draw the latest page frame as a GPU quad over (x,y,w,h) in points. Call from
 * the app's .overlay hook. No-op until a frame has arrived. */
void webview_composite(struct webview *wv, int x, int y, int w, int h);

/* Run WebKit's (GLib) work: call often from a zmc_every tick, since zmc's loop
 * is Wayland-only and would otherwise never service the engine. */
void webview_pump(void);

void webview_destroy(struct webview *wv);

#endif
