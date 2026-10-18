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
	cairo_surface_t *frame;	/* the latest page frame, as pixels */
	unsigned frame_rev;	/* bumped each frame so the GPU re-uploads */
	struct wv_delegate d;
};

/* WebKit rendered a frame: copy it into our cairo surface and ask for a draw.
 * Runs on the main thread (the GLib pump below), same as everything else. */
static void on_buffer(WPEView *view, WPEBuffer *buffer, gpointer u) {
	(void)view;
	struct webview *wv = u;
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
	}

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
		wpe_view_resized(wv->view, w, h);
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

/* input: WPEView events. Wired once the content area composites (next step). */
void webview_touch_down(struct webview *wv, int id, double x, double y) { (void)wv; (void)id; (void)x; (void)y; }
void webview_touch_motion(struct webview *wv, int id, double x, double y) { (void)wv; (void)id; (void)x; (void)y; }
void webview_touch_up(struct webview *wv, int id) { (void)wv; (void)id; }
void webview_key(struct webview *wv, const char *utf8) { (void)wv; (void)utf8; }
void webview_scroll(struct webview *wv, double dx, double dy) { (void)wv; (void)dx; (void)dy; }

void webview_composite(struct webview *wv, int x, int y, int w, int h) {
	if (!wv || !wv->frame) {
		return;
	}
	double corners[4][3] = {
		{ x, y, 0 }, { x + w, y, 0 }, { x, y + h, 0 }, { x + w, y + h, 0 },
	};
	zmc_gpu_quad(wv->frame, wv->frame_rev, corners, 1.0);
}

void webview_pump(void) {
	while (g_main_context_iteration(NULL, FALSE)) {
	}
}

void webview_destroy(struct webview *wv) {
	if (!wv) return;
	if (wv->web) g_object_unref(wv->web);
	if (wv->session) g_object_unref(wv->session);
	if (wv->wpe) g_object_unref(wv->wpe);
	if (wv->frame) cairo_surface_destroy(wv->frame);
	free(wv);
}
