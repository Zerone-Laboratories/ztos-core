// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * webview: WPE WebKit (the unified WPE Platform API, wpewebkit 2.54).
 *
 * The packaged WebKit is the new WPE Platform: no wpebackend-fdo. The web
 * engine runs on a headless WPEDisplay and loads pages fully (title, URL and
 * progress all work), which is what the chrome needs. Putting the page's
 * pixels on screen is a WPEView subclass that overrides render_buffer() to
 * import each frame's WPEBuffer as an EGL image
 * (wpe_buffer_import_to_egl_image) and draw it into a Wayland subsurface --
 * that compositing is the next iteration (see README.md). Until then the
 * content area is blank and the chrome is live.
 */
#define _GNU_SOURCE
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>

#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/headless/wpe-headless.h>

/* after the platform headers, so this sees the same wl_* declarations they do */
#include "webview.h"
#include "zrnmetrocore.h"	/* zmc_gpu_quad, zmc_present, cairo */

struct webview {
	struct wl_display *display;
	int x, y, w, h, scale;
	bool shown;
	WPEDisplay *wpe;
	WebKitNetworkSession *session;
	WebKitWebView *web;
	WPEView *view;		/* webkit_web_view_get_wpe_view */
	cairo_surface_t *frame;	/* fallback only: the latest frame as pixels */
	unsigned frame_rev;	/* bumped each frame so the GPU re-uploads */
	void *image;		/* the latest frame as an imported GPU image (no copy) */
	void *im;		/* ZtIm: the page's text fields */
	bool logged;
	struct wv_delegate d;
};

/* An imported image lives as long as WebKit's buffer: WebKit keeps a few
 * buffers and renders into them in turn, so each is imported once and then
 * only drawn. Freed when WebKit lets the buffer go. */
struct imported {
	void *image;
	struct webview *wv;
};

static void imported_free(gpointer p) {
	struct imported *im = p;
	if (im->wv && im->wv->image == im->image) {
		im->wv->image = NULL;
	}
	zmc_gpu_dmabuf_free(im->image);
	free(im);
}

/* the frame is ready on the GPU: wait for its render fence, if it has one */
static struct { int frames; double fence_ms, worst_ms; gint64 since; } stats;

static void wait_rendered(WPEBuffer *buffer) {
	int fd = wpe_buffer_take_rendering_fence(buffer);
	gint64 t0 = g_get_monotonic_time();
	if (fd >= 0) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		poll(&p, 1, 50);
		close(fd);
	}
	if (getenv("ZTB_DEBUG")) {
		double ms = (g_get_monotonic_time() - t0) / 1000.0;
		stats.frames++;
		stats.fence_ms += ms;
		if (ms > stats.worst_ms) stats.worst_ms = ms;
		if (!stats.since) stats.since = t0;
		if (t0 - stats.since > 1000000) {
			fprintf(stderr, "ztbrowser: %d frames/s, fence wait %.2f ms avg (worst %.2f), fence=%s\n",
				stats.frames, stats.fence_ms / stats.frames, stats.worst_ms, fd >= 0 ? "yes" : "none");
			stats.frames = 0; stats.fence_ms = stats.worst_ms = 0; stats.since = t0;
		}
	}
}

/* WebKit rendered a frame. Zero copy: a DMA-BUF frame is handed to the GPU as
 * a texture where it already lies; only if that fails is it copied out.
 * Runs on the main thread (the GLib pump below), same as everything else. */
static void on_buffer(WPEView *view, WPEBuffer *buffer, gpointer u) {
	(void)view;
	struct webview *wv = u;
	if (!wv->logged) {
		wv->logged = true;
		fprintf(stderr, "ztbrowser: frames %dx%d, %s (view %dx%d points at scale %d)\n",
			wpe_buffer_get_width(buffer), wpe_buffer_get_height(buffer),
			WPE_IS_BUFFER_DMA_BUF(buffer) ? "DMA-BUF, imported" : "not DMA-BUF, copied",
			wv->w, wv->h, wv->scale);
	}
	if (WPE_IS_BUFFER_DMA_BUF(buffer)) {
		struct imported *im = wpe_buffer_get_user_data(buffer);
		if (!im) {
			WPEBufferDMABuf *d = WPE_BUFFER_DMA_BUF(buffer);
			int n = (int)wpe_buffer_dma_buf_get_n_planes(d);
			int fds[4];
			uint32_t offsets[4], strides[4];
			for (int i = 0; i < n && i < 4; i++) {
				fds[i] = wpe_buffer_dma_buf_get_fd(d, i);
				offsets[i] = wpe_buffer_dma_buf_get_offset(d, i);
				strides[i] = wpe_buffer_dma_buf_get_stride(d, i);
			}
			void *image = zmc_gpu_dmabuf_import(wpe_buffer_get_width(buffer),
				wpe_buffer_get_height(buffer), wpe_buffer_dma_buf_get_format(d),
				wpe_buffer_dma_buf_get_modifier(d), n, fds, offsets, strides);
			if (image) {
				im = calloc(1, sizeof(*im));
				im->image = image;
				im->wv = wv;
				wpe_buffer_set_user_data(buffer, im, imported_free);
			}
		}
		if (im) {
			wait_rendered(buffer);
			wv->image = im->image;
			zmc_present();		/* a new frame to put up */
			return;
		}
	}
	GError *err = NULL;
	GBytes *px = wpe_buffer_import_to_pixels(buffer, &err);
	if (!px) {
		if (err) g_error_free(err);
		return;
	}
	int w = wpe_buffer_get_width(buffer), h = wpe_buffer_get_height(buffer);
	gsize n = 0;
	const unsigned char *src = g_bytes_get_data(px, &n);
	if (w > 0 && h > 0 && n >= (gsize)w * h * 4) {
		if (!wv->frame || cairo_image_surface_get_width(wv->frame) != w ||
		    cairo_image_surface_get_height(wv->frame) != h) {
			if (wv->frame) cairo_surface_destroy(wv->frame);
			wv->frame = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
		}
		int dst_stride = cairo_image_surface_get_stride(wv->frame);
		unsigned char *dst = cairo_image_surface_get_data(wv->frame);
		for (int y = 0; y < h; y++) {
			memcpy(dst + y * dst_stride, src + (size_t)y * w * 4, (size_t)w * 4);
		}
		cairo_surface_mark_dirty(wv->frame);
		wv->frame_rev++;
		zmc_present();		/* a new frame to put up */
	}
	/* no g_bytes_unref: import_to_pixels is transfer-none, the buffer owns them */
}

/* ---- WebKit signals -> the chrome ----------------------------------------- */

static void notify_title(WebKitWebView *web, GParamSpec *p, gpointer u) {
	struct webview *wv = u;
	if (wv->d.title) wv->d.title(wv->d.user, webkit_web_view_get_title(web));
}
static void notify_uri(WebKitWebView *web, GParamSpec *p, gpointer u) {
	struct webview *wv = u;
	if (wv->d.uri) wv->d.uri(wv->d.user, webkit_web_view_get_uri(web));
}
static void notify_progress(WebKitWebView *web, GParamSpec *p, gpointer u) {
	struct webview *wv = u;
	if (wv->d.progress) wv->d.progress(wv->d.user, webkit_web_view_get_estimated_load_progress(web));
}
static void load_changed(WebKitWebView *web, WebKitLoadEvent ev, gpointer u) {
	struct webview *wv = u;
	enum wv_load s = WV_STARTED;
	switch (ev) {
	case WEBKIT_LOAD_STARTED: s = WV_STARTED; break;
	case WEBKIT_LOAD_COMMITTED: s = WV_COMMITTED; break;
	case WEBKIT_LOAD_FINISHED: s = WV_FINISHED; break;
	default: s = WV_STARTED; break;
	}
	if (wv->d.load) wv->d.load(wv->d.user, s);
	if (ev == WEBKIT_LOAD_COMMITTED && wv->d.secure) {
		gboolean https = g_str_has_prefix(webkit_web_view_get_uri(web) ?: "", "https:");
		wv->d.secure(wv->d.user, https);
	}
}

/* ---- persistence ---------------------------------------------------------- */

static WebKitNetworkSession *make_session(const char *base) {
	char cookies[600];
	snprintf(cookies, sizeof(cookies), "%s/cookies.sqlite", base);
	/* persistent data + cache under one directory: cookies, localStorage,
	 * IndexedDB and the disk cache all live here and survive restarts */
	WebKitNetworkSession *s = webkit_network_session_new(base, base);
	WebKitCookieManager *cm = webkit_network_session_get_cookie_manager(s);
	webkit_cookie_manager_set_persistent_storage(cm, cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
	webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
	return s;
}

static void tune(WebKitSettings *s) {
	webkit_settings_set_enable_developer_extras(s, FALSE);
	webkit_settings_set_enable_webgl(s, TRUE);
	webkit_settings_set_enable_smooth_scrolling(s, TRUE);
	webkit_settings_set_enable_offline_web_application_cache(s, TRUE);
	webkit_settings_set_enable_html5_local_storage(s, TRUE);
	webkit_settings_set_enable_html5_database(s, TRUE);
	/* a phone user agent, so sites send their mobile pages */
	webkit_settings_set_user_agent(s,
		"Mozilla/5.0 (Linux; Android 14; ztOS) AppleWebKit/605.1.15 "
		"(KHTML, like Gecko) Version/18.0 Mobile Safari/605.1.15 ztBrowser/1.0");
}

/* ---- the interface -------------------------------------------------------- */

/* ---- the page's text fields: WebKit's input method context ----------------
 * WebKit tells this context when a text field takes focus, where its caret
 * is, and when it lets go; the shell's keyboard then comes up over it, and
 * what is typed goes back in through the "committed" signal. */
typedef struct {
	WebKitInputMethodContext parent;
	struct webview *wv;
	int caret_bottom;
	bool focused;
} ZtIm;
typedef struct {
	WebKitInputMethodContextClass parent;
} ZtImClass;
G_DEFINE_TYPE(ZtIm, zt_im, WEBKIT_TYPE_INPUT_METHOD_CONTEXT)

static void im_tell(ZtIm *im) {
	struct webview *wv = im->wv;
	if (wv && wv->d.ime) {
		wv->d.ime(wv->d.user, im->focused, im->caret_bottom);
	}
}
static void im_focus_in(WebKitInputMethodContext *c) {
	ZtIm *im = (ZtIm *)c;
	im->focused = true;
	im_tell(im);
}
static void im_focus_out(WebKitInputMethodContext *c) {
	ZtIm *im = (ZtIm *)c;
	im->focused = false;
	im_tell(im);
}
static void im_cursor_area(WebKitInputMethodContext *c, int x, int y, int w, int h) {
	(void)x; (void)w;
	ZtIm *im = (ZtIm *)c;
	im->caret_bottom = y + h;
	if (im->focused) {
		im_tell(im);	/* the field moved, or the caret went to another line */
	}
}
static void im_get_preedit(WebKitInputMethodContext *c, gchar **text, GList **underlines, guint *cursor) {
	(void)c;
	if (text) *text = g_strdup("");
	if (underlines) *underlines = NULL;
	if (cursor) *cursor = 0;
}
static gboolean im_filter_key(WebKitInputMethodContext *c, gpointer ev) { (void)c; (void)ev; return FALSE; }
static void im_reset(WebKitInputMethodContext *c) { (void)c; }
static void im_set_preedit(WebKitInputMethodContext *c, gboolean on) { (void)c; (void)on; }

static void zt_im_class_init(ZtImClass *k) {
	WebKitInputMethodContextClass *c = WEBKIT_INPUT_METHOD_CONTEXT_CLASS(k);
	c->notify_focus_in = im_focus_in;
	c->notify_focus_out = im_focus_out;
	c->notify_cursor_area = im_cursor_area;
	c->get_preedit = im_get_preedit;
	c->filter_key_event = im_filter_key;
	c->reset = im_reset;
	c->set_enable_preedit = im_set_preedit;
}
static void zt_im_init(ZtIm *im) { im->caret_bottom = -1; }

struct webview *webview_create(struct wl_display *display, struct wl_compositor *compositor,
			       struct wl_surface *parent, const char *data_dir,
			       const struct wv_delegate *delegate) {
	(void)compositor; (void)parent;
	struct webview *wv = calloc(1, sizeof(*wv));
	wv->display = display;
	wv->d = *delegate;
	wv->scale = 1;

	wv->wpe = wpe_display_headless_new();
	if (!wv->wpe) {
		free(wv);
		return NULL;
	}
	wv->session = make_session(data_dir);
	WebKitSettings *settings = webkit_settings_new();
	tune(settings);
	wv->web = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
		"display", wv->wpe,
		"network-session", wv->session,
		"settings", settings,
		NULL));
	g_object_unref(settings);
	wv->view = webkit_web_view_get_wpe_view(wv->web);
	if (wv->view) {
		g_signal_connect(wv->view, "buffer-rendered", G_CALLBACK(on_buffer), wv);
		wpe_view_map(wv->view);
		wpe_view_set_visible(wv->view, TRUE);
		wpe_view_focus_in(wv->view);	/* input (clicks, keys) only reaches a focused view */
	}

	ZtIm *im = g_object_new(zt_im_get_type(), NULL);
	im->wv = wv;
	wv->im = im;
	webkit_web_view_set_input_method_context(wv->web, WEBKIT_INPUT_METHOD_CONTEXT(im));
	g_object_unref(im);	/* the view holds it */

	g_signal_connect(wv->web, "notify::title", G_CALLBACK(notify_title), wv);
	g_signal_connect(wv->web, "notify::uri", G_CALLBACK(notify_uri), wv);
	g_signal_connect(wv->web, "notify::estimated-load-progress", G_CALLBACK(notify_progress), wv);
	g_signal_connect(wv->web, "load-changed", G_CALLBACK(load_changed), wv);
	return wv;
}

void webview_place(struct webview *wv, int x, int y, int w, int h, int scale) {
	wv->x = x; wv->y = y; wv->w = w; wv->h = h; wv->scale = scale;
	if (wv->view) {
		/* logical size in CSS pixels plus the device scale: the page lays out
		 * at phone width (~360 px) and renders sharp at the panel's density,
		 * instead of a 1440 px desktop layout with tiny text */
		WPEScreen *screen = wpe_view_get_screen(wv->view);
		if (screen) {
			wpe_screen_set_scale(screen, scale);
			wpe_screen_set_size(screen, w, h);
		}
		/* the view's device scale is its toplevel's: set there, or WebKit
		 * renders 360 px frames that the GPU stretches 4x (the blur) */
		WPEToplevel *top = wpe_view_get_toplevel(wv->view);
		if (top) {
			wpe_toplevel_resized(top, w, h);
			wpe_toplevel_scale_changed(top, scale);
		}
		wpe_view_resized(wv->view, w, h);
		fprintf(stderr, "ztbrowser: view %dx%d at scale %.1f (toplevel %s)\n", w, h,
			wpe_view_get_scale(wv->view), top ? "yes" : "none");
	}
}

void webview_show(struct webview *wv, bool on) { wv->shown = on; }
void webview_load(struct webview *wv, const char *url) { webkit_web_view_load_uri(wv->web, url); }
void webview_reload(struct webview *wv, bool bypass) {
	if (bypass) webkit_web_view_reload_bypass_cache(wv->web); else webkit_web_view_reload(wv->web);
}
void webview_stop(struct webview *wv) { webkit_web_view_stop_loading(wv->web); }
bool webview_can_go_back(struct webview *wv) { return webkit_web_view_can_go_back(wv->web); }
bool webview_can_go_forward(struct webview *wv) { return webkit_web_view_can_go_forward(wv->web); }
void webview_back(struct webview *wv) { webkit_web_view_go_back(wv->web); }
void webview_forward(struct webview *wv) { webkit_web_view_go_forward(wv->web); }

/* Input. WebKit only pans from raw touch when the view has a gesture
 * controller, so the gesture is decided here: once the finger has moved past
 * a few points it is a drag and turns into precise scroll deltas (lifted with
 * an is_stop, from which WebKit coasts on); a lift that never moved is a tap
 * and becomes a click at that point. Coordinates are CSS pixels (points). */
#define TAP_SLOP 8.0

static struct {
	double down_x, down_y, last_x, last_y;
	bool moved, active;
} finger;

static void send(struct webview *wv, WPEEvent *ev) {
	if (ev) {
		wpe_view_event(wv->view, ev);
		wpe_event_unref(ev);
	}
}

void webview_touch_down(struct webview *wv, int id, double x, double y, unsigned time) {
	(void)id;
	if (!wv || !wv->view) return;
	finger.down_x = finger.last_x = x;
	finger.down_y = finger.last_y = y;
	finger.moved = false;
	finger.active = true;
	if (getenv("ZTB_DEBUG")) fprintf(stderr, "ztbrowser: touch down %.0f,%.0f\n", x, y);
	/* the pointer to where the finger is, so :hover and the click land there */
	send(wv, wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, wv->view,
		WPE_INPUT_SOURCE_TOUCHSCREEN, time, 0, x, y, 0, 0));
}

void webview_touch_motion(struct webview *wv, int id, double x, double y, unsigned time) {
	(void)id;
	if (!wv || !wv->view || !finger.active) return;
	if (!finger.moved && hypot(x - finger.down_x, y - finger.down_y) > TAP_SLOP) {
		finger.moved = true;
		finger.last_x = x;	/* scroll from here: no jump at the start */
		finger.last_y = y;
		return;
	}
	if (finger.moved) {
		double dx = x - finger.last_x, dy = y - finger.last_y;
		send(wv, wpe_event_scroll_new(wv->view, WPE_INPUT_SOURCE_TOUCHSCREEN, time, 0,
			dx, dy, TRUE, FALSE, x, y));
		finger.last_x = x;
		finger.last_y = y;
	}
}

void webview_touch_up(struct webview *wv, int id, unsigned time) {
	(void)id;
	if (!wv || !wv->view || !finger.active) return;
	finger.active = false;
	double x = finger.last_x, y = finger.last_y;
	if (getenv("ZTB_DEBUG")) fprintf(stderr, "ztbrowser: touch up %.0f,%.0f moved=%d focus=%d\n", x, y,
		finger.moved, wpe_view_get_has_focus(wv->view));
	if (finger.moved) {
		send(wv, wpe_event_scroll_new(wv->view, WPE_INPUT_SOURCE_TOUCHSCREEN, time, 0,
			0, 0, TRUE, TRUE, x, y));
		return;
	}
	/* a tap: a real touch (touchstart/touchend, so touch-driven pages like
	 * YouTube react) and a mouse click at the same point (WebKit only turns
	 * pointer presses into clicks when they come from a mouse; from the
	 * touchscreen source it ignored them, which is why nothing clicked) */
	if (!wpe_view_get_has_focus(wv->view)) {
		wpe_view_focus_in(wv->view);
	}
	send(wv, wpe_event_touch_new(WPE_EVENT_TOUCH_DOWN, wv->view, WPE_INPUT_SOURCE_TOUCHSCREEN,
		time, 0, 0, x, y));
	send(wv, wpe_event_touch_new(WPE_EVENT_TOUCH_UP, wv->view, WPE_INPUT_SOURCE_TOUCHSCREEN,
		time, 0, 0, x, y));
	send(wv, wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, wv->view,
		WPE_INPUT_SOURCE_MOUSE, time, 0, x, y, 0, 0));
	send(wv, wpe_event_pointer_button_new(WPE_EVENT_POINTER_DOWN, wv->view,
		WPE_INPUT_SOURCE_MOUSE, time, 0, WPE_BUTTON_PRIMARY, x, y, 1));
	send(wv, wpe_event_pointer_button_new(WPE_EVENT_POINTER_UP, wv->view,
		WPE_INPUT_SOURCE_MOUSE, time, WPE_MODIFIER_POINTER_BUTTON1, WPE_BUTTON_PRIMARY, x, y, 0));
	/* (a release carries press count 0: WPE refuses to build one with 1, so
	 * the release used to be dropped and no click ever completed) */
}

void webview_touch_cancel(struct webview *wv) {
	(void)wv;
	finger.active = false;
}
/* one key press and release; keycodes are evdev + 8, as XKB counts them */
static void key_tap(struct webview *wv, guint keycode, guint keyval) {
	guint32 t = (guint32)(g_get_monotonic_time() / 1000);
	send(wv, wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_DOWN, wv->view, WPE_INPUT_SOURCE_KEYBOARD,
		t, 0, keycode, keyval));
	send(wv, wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_UP, wv->view, WPE_INPUT_SOURCE_KEYBOARD,
		t, 0, keycode, keyval));
}

void webview_key(struct webview *wv, const char *utf8) {
	if (!wv || !wv->view || !wv->im) {
		return;
	}
	if (strcmp(utf8, "\b") == 0) {
		key_tap(wv, 14 + 8, WPE_KEY_BackSpace);	/* KEY_BACKSPACE */
	} else if (strcmp(utf8, "\n") == 0) {
		key_tap(wv, 28 + 8, WPE_KEY_Return);	/* KEY_ENTER: submits a search */
	} else {
		g_signal_emit_by_name(wv->im, "committed", utf8);
	}
}
void webview_scroll(struct webview *wv, double dx, double dy) { (void)wv; (void)dx; (void)dy; }

void webview_composite(struct webview *wv, int x, int y, int w, int h) {
	if (!wv) {
		return;
	}
	double corners[4][3] = {
		{ x, y, 0 }, { x + w, y, 0 }, { x, y + h, 0 }, { x + w, y + h, 0 },
	};
	if (wv->image) {
		zmc_gpu_dmabuf(wv->image, corners, 1.0);	/* straight from WebKit's buffer */
	} else if (wv->frame) {
		zmc_gpu_quad(wv->frame, wv->frame_rev, corners, 1.0);
	}
}

/* WebKit's GLib work, event-driven: after each run, the file descriptors GLib
 * waits on (the web process's IPC, the network, timers' eventfds) go into
 * zmc's own poll and GLib's next timeout becomes zmc's timer, so WebKit is
 * served the moment something happens instead of on a fixed 16 ms tick that
 * added up to a frame of latency to every event, and woke the CPU for nothing
 * when idle. */
#define GLIB_FDS 32
static int watched[GLIB_FDS], n_watched;

static void glib_ready(int fd, void *data) { (void)fd; (void)data; webview_pump(); }
static void glib_tick(void) { webview_pump(); }

void webview_pump(void) {
	GMainContext *c = g_main_context_default();
	while (g_main_context_iteration(c, FALSE)) {
	}
	if (!g_main_context_acquire(c)) {
		return;
	}
	gint prio = 0, timeout = -1;
	GPollFD fds[GLIB_FDS];
	g_main_context_prepare(c, &prio);
	int n = g_main_context_query(c, prio, &timeout, fds, GLIB_FDS);
	g_main_context_release(c);
	if (n > GLIB_FDS) n = GLIB_FDS;
	/* stop watching what GLib no longer waits on, start on what is new */
	for (int i = 0; i < n_watched; i++) {
		bool keep = false;
		for (int j = 0; j < n && !keep; j++) keep = fds[j].fd == watched[i];
		if (!keep) zmc_watch_fd(watched[i], NULL, NULL);
	}
	for (int j = 0; j < n; j++) {
		bool known = false;
		for (int i = 0; i < n_watched && !known; i++) known = watched[i] == fds[j].fd;
		if (!known && fds[j].fd >= 0) zmc_watch_fd(fds[j].fd, glib_ready, NULL);
	}
	n_watched = 0;
	for (int j = 0; j < n; j++) {
		if (fds[j].fd >= 0) watched[n_watched++] = fds[j].fd;
	}
	/* GLib's next timer, or a slow heartbeat when it has none */
	zmc_every(timeout < 0 ? 1000 : timeout < 1 ? 1 : timeout, glib_tick);
}

void webview_destroy(struct webview *wv) {
	if (!wv) return;
	if (wv->web) g_object_unref(wv->web);
	if (wv->session) g_object_unref(wv->session);
	if (wv->wpe) g_object_unref(wv->wpe);
	if (wv->frame) cairo_surface_destroy(wv->frame);
	free(wv);
}
