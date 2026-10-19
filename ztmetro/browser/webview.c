// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * webview: WPE WebKit (the unified WPE Platform API, wpewebkit 2.54).
 *
 * The packaged WebKit is the new WPE Platform: no wpebackend-fdo. The engine
 * runs on a WPEDisplay of our own (below): a headless one inside for the GPU
 * and buffer formats, with frames acked only once they are in one of our
 * frames, and WebKit's clock on the panel's vblank. Each frame is a DMA-BUF
 * that zmc draws as a GPU texture, without a copy. ZTB_HEADLESS=1 runs the
 * plain headless platform instead; ZTB_TRACE=1 logs when frames reach the
 * screen.
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
#include <fcntl.h>
#include <xf86drm.h>

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
	WPEBuffer *pending;	/* rendered by WebKit, not yet in one of our frames */
	WPEBuffer *on_screen;	/* in our last frame: WebKit must not draw into it */
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
			fprintf(stderr, "superscan: %d frames/s, fence wait %.2f ms avg (worst %.2f), fence=%s\n",
				stats.frames, stats.fence_ms / stats.frames, stats.worst_ms, fd >= 0 ? "yes" : "none");
			stats.frames = 0; stats.fence_ms = stats.worst_ms = 0; stats.since = t0;
		}
	}
}

/* WebKit rendered a frame. Zero copy: a DMA-BUF frame is handed to the GPU as
 * a texture where it already lies; only if that fails is it copied out.
 * Runs on the main thread (the GLib pump below), same as everything else.
 * Returns true if the frame is drawn straight from the buffer, which then
 * stays WebKit's to reuse only once a newer one has replaced it on screen. */
static bool take_frame(struct webview *wv, WPEBuffer *buffer) {
	if (!wv->logged) {
		wv->logged = true;
		fprintf(stderr, "superscan: frames %dx%d, %s (view %dx%d points at scale %d)\n",
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
			return true;	/* the caller puts it up (zmc_present) */
		}
	}
	GError *err = NULL;
	GBytes *px = wpe_buffer_import_to_pixels(buffer, &err);
	if (!px) {
		if (err) g_error_free(err);
		return false;
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
	return false;
}

/* ---- our own WPE platform: frames paced by the screen ----------------------
 * The headless platform acks each frame on a timer of its own, out of step
 * with the display: during a scroll frames came 6 ms apart and then 25, so
 * about one in five was never seen and the page judders. This display and
 * view are the headless ones (the GPU, DRM device and buffer formats are
 * asked of a headless display inside) with one change: a frame is acked
 * (wpe_view_buffer_rendered) only once it has gone into one of our frames,
 * so WebKit renders one frame per frame shown, on our clock, and never one
 * that would be thrown away. */

typedef struct { WPEDisplay parent; WPEDisplay *inner; bool connected; } ZtDisplay;
typedef struct { WPEDisplayClass parent; } ZtDisplayClass;
typedef struct { WPEView parent; struct webview *wv; } ZtView;
typedef struct { WPEViewClass parent; } ZtViewClass;
typedef struct { WPEToplevel parent; } ZtToplevel;
typedef struct { WPEToplevelClass parent; } ZtToplevelClass;
typedef struct { WPEScreen parent; } ZtScreen;
typedef struct { WPEScreenClass parent; } ZtScreenClass;

typedef struct { WPEScreenSyncObserver parent; int fd; GThread *thread; gint running; } ZtSync;
typedef struct { WPEScreenSyncObserverClass parent; } ZtSyncClass;

G_DEFINE_TYPE(ZtSync, zt_sync, WPE_TYPE_SCREEN_SYNC_OBSERVER)
G_DEFINE_TYPE(ZtScreen, zt_screen, WPE_TYPE_SCREEN)
G_DEFINE_TYPE(ZtToplevel, zt_toplevel, WPE_TYPE_TOPLEVEL)
G_DEFINE_TYPE(ZtView, zt_view, WPE_TYPE_VIEW)
G_DEFINE_TYPE(ZtDisplay, zt_display, WPE_TYPE_DISPLAY)

static WPEScreen *the_screen;	/* one phone, one panel */

/* WebKit's clock for animation frames, scrolling and requestAnimationFrame.
 * Left alone it is a free-running 60 Hz timer that drifts against the panel,
 * so its frames beat with ours. This one ticks on the panel's own vblank,
 * read from the display controller (exynos; panfrost has no vblank), which
 * is the clock the shell flips on. */
static gpointer sync_loop(gpointer p) {
	ZtSync *s = p;
	WPEScreenSyncObserverClass *c = WPE_SCREEN_SYNC_OBSERVER_GET_CLASS(s);
	while (g_atomic_int_get(&s->running)) {
		drmVBlank vb = { .request = { .type = DRM_VBLANK_RELATIVE, .sequence = 1 } };
		if (drmWaitVBlank(s->fd, &vb) != 0) {
			g_usleep(16667);	/* panel off: keep a slow beat */
		}
		if (g_atomic_int_get(&s->running)) {
			c->sync(WPE_SCREEN_SYNC_OBSERVER(s));
		}
	}
	return NULL;
}
static void sync_start(WPEScreenSyncObserver *o) {
	ZtSync *s = (ZtSync *)o;
	if (s->thread) return;
	g_atomic_int_set(&s->running, 1);
	s->thread = g_thread_new("ztb-vblank", sync_loop, s);
}
static void sync_stop(WPEScreenSyncObserver *o) {
	ZtSync *s = (ZtSync *)o;
	if (!s->thread) return;
	g_atomic_int_set(&s->running, 0);
	if (g_thread_self() == s->thread) {
		g_thread_unref(s->thread);	/* stopped from its own tick: it ends itself */
	} else {
		g_thread_join(s->thread);
	}
	s->thread = NULL;
}
static void zt_sync_class_init(ZtSyncClass *k) {
	WPEScreenSyncObserverClass *c = WPE_SCREEN_SYNC_OBSERVER_CLASS(k);
	c->start = sync_start;
	c->stop = sync_stop;
}
static void zt_sync_init(ZtSync *s) { s->fd = -1; }

/* the display controller: the DRM card that has vblanks */
static ZtSync *sync_new(void) {
	for (int i = 0; i < 4; i++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/dri/card%d", i);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		drmVBlank vb = { .request = { .type = DRM_VBLANK_RELATIVE, .sequence = 0 } };
		if (drmWaitVBlank(fd, &vb) == 0) {
			ZtSync *s = g_object_new(zt_sync_get_type(), NULL);
			s->fd = fd;
			fprintf(stderr, "superscan: WebKit's clock is %s vblank\n", path);
			return s;
		}
		close(fd);
	}
	return NULL;
}

static ZtSync *the_sync;
static WPEScreenSyncObserver *screen_sync(WPEScreen *screen) {
	(void)screen;
	return the_sync ? g_object_ref(WPE_SCREEN_SYNC_OBSERVER(the_sync)) : NULL;
}
static void zt_screen_class_init(ZtScreenClass *k) {
	WPE_SCREEN_CLASS(k)->get_sync_observer = screen_sync;
}
static void zt_screen_init(ZtScreen *s) { (void)s; }

static WPEScreen *top_get_screen(WPEToplevel *t) { (void)t; return the_screen; }
static gboolean top_resize(WPEToplevel *t, int w, int h) {
	wpe_toplevel_resized(t, w, h);
	return TRUE;
}
static void zt_toplevel_class_init(ZtToplevelClass *k) {
	WPEToplevelClass *c = WPE_TOPLEVEL_CLASS(k);
	c->get_screen = top_get_screen;
	c->resize = top_resize;
}
static void zt_toplevel_init(ZtToplevel *t) { (void)t; }

static void glib_tick(void);

/* the frame went into one of ours: the one before it is free again, and
 * WebKit may start on the next */
static void frame_shown(struct webview *wv) {
	WPEBuffer *b = wv->pending;
	wv->pending = NULL;
	if (getenv("ZTB_TRACE")) {
		static gint64 last;
		gint64 now = g_get_monotonic_time();
		fprintf(stderr, "ztb-shown %.1f\n", last ? (now - last) / 1000.0 : 0.0);
		last = now;
	}
	if (wv->on_screen && wv->on_screen != b) {
		wpe_view_buffer_released(wv->view, wv->on_screen);
	}
	wv->on_screen = b;
	wpe_view_buffer_rendered(wv->view, b);
	/* the ack is GLib work (a message to the web process) that nothing would
	 * wake the pump for: run it straight after this frame, not inside it */
	zmc_every(1, glib_tick);
}

static gboolean view_render_buffer(WPEView *view, WPEBuffer *buffer, const WPERectangle *damage,
				   guint n_damage, GError **error) {
	(void)damage; (void)n_damage; (void)error;
	struct webview *wv = ((ZtView *)view)->wv;
	if (!wv || !take_frame(wv, buffer)) {
		/* no one to show it to, or copied out already: done with it now */
		wpe_view_buffer_rendered(view, buffer);
		wpe_view_buffer_released(view, buffer);
		return TRUE;
	}
	if (wv->pending && wv->pending != buffer) {
		/* overtaken before it was ever shown */
		WPEBuffer *old = wv->pending;
		wv->pending = NULL;
		wpe_view_buffer_rendered(view, old);
		if (old != wv->on_screen) wpe_view_buffer_released(view, old);
	}
	wv->pending = buffer;
	zmc_present();	/* after pending is set: an idle zmc draws (and acks) at once */
	return TRUE;
}

/* WebKit swapped its set of buffers (a resize): forget any that went */
static void view_buffers_changed(WPEView *view, WPEBuffer **buffers, guint n) {
	struct webview *wv = ((ZtView *)view)->wv;
	if (!wv) return;
	bool pending = false, on_screen = false;
	for (guint i = 0; i < n; i++) {
		pending |= buffers[i] == wv->pending;
		on_screen |= buffers[i] == wv->on_screen;
	}
	if (!pending) wv->pending = NULL;
	if (!on_screen) wv->on_screen = NULL;
}

static void zt_view_class_init(ZtViewClass *k) {
	WPEViewClass *c = WPE_VIEW_CLASS(k);
	c->render_buffer = view_render_buffer;
	c->buffers_changed = view_buffers_changed;
}
static void zt_view_init(ZtView *v) { (void)v; }

static gboolean dpy_connect(WPEDisplay *d, GError **error) {
	ZtDisplay *z = (ZtDisplay *)d;
	if (!z->connected) z->connected = wpe_display_connect(z->inner, error);
	return z->connected;
}
static WPEToplevel *dpy_create_toplevel(WPEDisplay *d, guint max_views) {
	return g_object_new(zt_toplevel_get_type(), "display", d, "max-views", max_views, NULL);
}
static WPEView *dpy_create_view(WPEDisplay *d) {
	WPEView *view = g_object_new(zt_view_get_type(), "display", d, NULL);
	if (!wpe_view_get_toplevel(view)) {
		WPEToplevel *top = dpy_create_toplevel(d, 1);
		wpe_view_set_toplevel(view, top);
		g_object_unref(top);
	}
	return view;
}
static gpointer dpy_egl(WPEDisplay *d, GError **error) {
	return wpe_display_get_egl_display(((ZtDisplay *)d)->inner, error);
}
static WPEKeymap *dpy_keymap(WPEDisplay *d) { return wpe_display_get_keymap(((ZtDisplay *)d)->inner); }
static WPEBufferFormats *dpy_formats(WPEDisplay *d) {
	return wpe_display_get_preferred_buffer_formats(((ZtDisplay *)d)->inner);
}
static WPEDRMDevice *dpy_drm(WPEDisplay *d) { return wpe_display_get_drm_device(((ZtDisplay *)d)->inner); }
static guint dpy_n_screens(WPEDisplay *d) { (void)d; return 1; }
static WPEScreen *dpy_screen(WPEDisplay *d, guint i) { (void)d; return i == 0 ? the_screen : NULL; }

static void dpy_finalize(GObject *o) {
	g_clear_object(&((ZtDisplay *)o)->inner);
	G_OBJECT_CLASS(zt_display_parent_class)->finalize(o);
}
static void zt_display_class_init(ZtDisplayClass *k) {
	WPEDisplayClass *c = WPE_DISPLAY_CLASS(k);
	G_OBJECT_CLASS(k)->finalize = dpy_finalize;
	c->connect = dpy_connect;
	c->create_view = dpy_create_view;
	c->create_toplevel = dpy_create_toplevel;
	c->get_egl_display = dpy_egl;
	c->get_keymap = dpy_keymap;
	c->get_preferred_buffer_formats = dpy_formats;
	c->get_drm_device = dpy_drm;
	c->get_n_screens = dpy_n_screens;
	c->get_screen = dpy_screen;
}
static void zt_display_init(ZtDisplay *z) { z->inner = wpe_display_headless_new(); }

static WPEDisplay *paced_display_new(void) {
	if (!the_screen) {
		GObjectClass *sc = g_type_class_ref(zt_screen_get_type());
		the_screen = g_object_class_find_property(sc, "id")
			? g_object_new(zt_screen_get_type(), "id", 1, NULL)
			: g_object_new(zt_screen_get_type(), NULL);
		g_type_class_unref(sc);
		wpe_screen_set_refresh_rate(the_screen, 60000);	/* mHz */
		the_sync = sync_new();
	}
	ZtDisplay *z = g_object_new(zt_display_get_type(), NULL);
	GError *err = NULL;
	if (!z->inner || !wpe_display_connect(WPE_DISPLAY(z), &err)) {
		fprintf(stderr, "superscan: paced display failed (%s), using headless\n",
			err ? err->message : "no display");
		g_clear_error(&err);
		g_object_unref(z);
		return NULL;
	}
	return WPE_DISPLAY(z);
}

/* the headless platform's own view (fallback): it acks frames itself */
static void on_buffer(WPEView *view, WPEBuffer *buffer, gpointer u) {
	(void)view;
	if (take_frame(u, buffer)) zmc_present();
}

/* headless only: when its frames reach one of ours (for ZTB_TRACE A/B) */
static void trace_shown(void) {
	static gint64 last;
	gint64 now = g_get_monotonic_time();
	fprintf(stderr, "ztb-shown %.1f\n", last ? (now - last) / 1000.0 : 0.0);
	last = now;
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
	webkit_settings_set_enable_html5_local_storage(s, TRUE);
	webkit_settings_set_enable_html5_database(s, TRUE);
	/* a phone user agent, so sites send their mobile pages */
	webkit_settings_set_user_agent(s,
		"Mozilla/5.0 (Linux; Android 14; ztOS) AppleWebKit/605.1.15 "
		"(KHTML, like Gecko) Version/18.0 Mobile Safari/605.1.15 Superscan/1.0");
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

	wv->wpe = getenv("ZTB_HEADLESS") ? NULL : paced_display_new();	/* (A/B: the old path) */
	if (wv->wpe) {
		/* WebKit finds screens (and so the vblank clock) on the primary display */
		wpe_display_set_primary(wv->wpe);
	} else {
		wv->wpe = wpe_display_headless_new();
	}
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
		if (G_TYPE_CHECK_INSTANCE_TYPE(wv->view, zt_view_get_type())) {
			((ZtView *)wv->view)->wv = wv;
		} else {
			g_signal_connect(wv->view, "buffer-rendered", G_CALLBACK(on_buffer), wv);
		}
		fprintf(stderr, "superscan: %s\n", G_TYPE_CHECK_INSTANCE_TYPE(wv->view, zt_view_get_type())
			? "frames paced by our display" : "headless frames (timer paced)");
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
		fprintf(stderr, "superscan: view %dx%d at scale %.1f (toplevel %s)\n", w, h,
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

/* Input. A finger goes to WebKit as touch events only. WebKit gives them to
 * the page first; whatever the page does not take (no listener, or one that
 * did not preventDefault) goes to the view's gesture controller (WPE's own,
 * made on first use), which says tap or drag, and WebKit turns that into one
 * click or a scroll (with begin/end phases, so it coasts). Sending a click of our own as well
 * made every tap land twice on pages that listen to touches (YouTube).
 * Coordinates are CSS pixels (points). */
static void send(struct webview *wv, WPEEvent *ev) {
	if (ev) {
		wpe_view_event(wv->view, ev);
		wpe_event_unref(ev);
	}
}

static void touch(struct webview *wv, WPEEventType type, int id, double x, double y, unsigned time) {
	if (!wv || !wv->view) return;
	if (getenv("ZTB_DEBUG")) fprintf(stderr, "superscan: touch %d id %d at %.0f,%.0f\n", type, id, x, y);
	send(wv, wpe_event_touch_new(type, wv->view, WPE_INPUT_SOURCE_TOUCHSCREEN, time, 0, (guint32)id, x, y));
}

static struct { int id; double x, y; bool active; } finger;

void webview_touch_down(struct webview *wv, int id, double x, double y, unsigned time) {
	finger.id = id;
	finger.x = x;
	finger.y = y;
	finger.active = true;
	touch(wv, WPE_EVENT_TOUCH_DOWN, id, x, y, time);
}

void webview_touch_motion(struct webview *wv, int id, double x, double y, unsigned time) {
	if (!finger.active || id != finger.id) return;
	finger.x = x;
	finger.y = y;
	touch(wv, WPE_EVENT_TOUCH_MOVE, id, x, y, time);
}

void webview_touch_up(struct webview *wv, int id, unsigned time) {
	if (!finger.active || id != finger.id) return;
	finger.active = false;
	touch(wv, WPE_EVENT_TOUCH_UP, id, finger.x, finger.y, time);	/* (zmc gives no position on lift) */
}

void webview_touch_cancel(struct webview *wv) {
	if (!finger.active) return;
	finger.active = false;
	touch(wv, WPE_EVENT_TOUCH_CANCEL, finger.id, finger.x, finger.y,
	      (unsigned)(g_get_monotonic_time() / 1000));
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
	if (wv->pending) {
		frame_shown(wv);	/* on screen with this frame: WebKit may draw the next */
	}
	static void *last_image;
	if (getenv("ZTB_TRACE") && getenv("ZTB_HEADLESS") && wv->image != last_image) {
		last_image = wv->image;
		trace_shown();
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
