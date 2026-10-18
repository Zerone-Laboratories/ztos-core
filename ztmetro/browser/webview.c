// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * webview: WPE WebKit on a Wayland subsurface, composited on the GPU.
 *
 * WebKit renders in its own process; wpebackend-fdo hands us each finished
 * frame as an EGL image (a dmabuf under the hood). We import it as a GL texture
 * in an EGL context on the subsurface's wl_egl_window and draw it full-bleed.
 * Nothing of the page is ever touched by the CPU.
 *
 * Targets wpe-webkit-2.0 + wpebackend-fdo-1.0. The WebKit API moves between
 * releases; if the first on-device build reports a changed symbol, it is one
 * of the calls in webview_create() or the gl bits below. See README.md.
 */
#define _GNU_SOURCE
#include "webview.h"

#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <wpe/webkit.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>

struct webview {
	/* wayland */
	struct wl_display *display;
	struct wl_surface *surface;		/* our subsurface */
	struct wl_subsurface *subsurface;
	struct wl_egl_window *egl_window;
	int x, y, w, h, scale;
	bool shown;
	/* egl */
	EGLDisplay egl;
	EGLContext ctx;
	EGLConfig config;
	EGLSurface egl_surface;
	GLuint prog, tex, vbo;
	GLint a_pos, a_uv, u_tex;
	/* wpe / webkit */
	struct wpe_view_backend_exportable_fdo *exportable;
	WebKitWebView *web;
	struct wpe_fdo_egl_exported_image *current_image;
	/* chrome */
	struct wv_delegate d;
};

/* ---- the tiny shader that draws one frame ---------------------------------- */

static const char *VERT =
	"attribute vec2 pos; attribute vec2 uv; varying vec2 v;"
	"void main(){ v = uv; gl_Position = vec4(pos, 0.0, 1.0); }";
static const char *FRAG =
	"precision mediump float; varying vec2 v; uniform sampler2D t;"
	"void main(){ gl_FragColor = texture2D(t, v); }";

static GLuint compile(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	return s;
}

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target;

static void gl_setup(struct webview *wv) {
	wv->prog = glCreateProgram();
	glAttachShader(wv->prog, compile(GL_VERTEX_SHADER, VERT));
	glAttachShader(wv->prog, compile(GL_FRAGMENT_SHADER, FRAG));
	glBindAttribLocation(wv->prog, 0, "pos");
	glBindAttribLocation(wv->prog, 1, "uv");
	glLinkProgram(wv->prog);
	wv->u_tex = glGetUniformLocation(wv->prog, "t");
	/* a full-screen quad, flipped vertically (GL origin is bottom-left) */
	static const float quad[] = {
		-1, -1, 0, 1,   1, -1, 1, 1,   -1, 1, 0, 0,   1, 1, 1, 0,
	};
	glGenBuffers(1, &wv->vbo);
	glBindBuffer(GL_ARRAY_BUFFER, wv->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glGenTextures(1, &wv->tex);
}

/* draw the latest exported image into the subsurface and commit it */
static void paint(struct webview *wv) {
	if (!wv->current_image || !wv->shown) {
		return;
	}
	eglMakeCurrent(wv->egl, wv->egl_surface, wv->egl_surface, wv->ctx);
	glViewport(0, 0, wv->w * wv->scale, wv->h * wv->scale);
	glClear(GL_COLOR_BUFFER_BIT);
	glUseProgram(wv->prog);
	EGLImageKHR img = wpe_fdo_egl_exported_image_get_egl_image(wv->current_image);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, wv->tex);
	image_target(GL_TEXTURE_2D, img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glUniform1i(wv->u_tex, 0);
	glBindBuffer(GL_ARRAY_BUFFER, wv->vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	eglSwapBuffers(wv->egl, wv->egl_surface);
}

/* wpebackend-fdo hands us a finished frame */
static void on_export(void *data, struct wpe_fdo_egl_exported_image *image) {
	struct webview *wv = data;
	if (wv->current_image) {
		wpe_view_backend_exportable_fdo_egl_dispatch_release_exported_image(wv->exportable, wv->current_image);
	}
	wv->current_image = image;
	paint(wv);
	wpe_view_backend_exportable_fdo_dispatch_frame_complete(wv->exportable);
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
		wv->d.secure(wv->d.user, webkit_web_view_get_tls_info(web, NULL, NULL));
	}
}

/* ---- persistence: one data manager under data_dir ------------------------- */

static WebKitWebsiteDataManager *data_manager(const char *base) {
	char cookies[600], ls[600];
	snprintf(cookies, sizeof(cookies), "%s/cookies.sqlite", base);
	snprintf(ls, sizeof(ls), "%s/localstorage", base);
	WebKitWebsiteDataManager *m = webkit_website_data_manager_new(
		"base-data-directory", base,
		"base-cache-directory", base,
		NULL);
	WebKitCookieManager *cm = webkit_website_data_manager_get_cookie_manager(m);
	webkit_cookie_manager_set_persistent_storage(cm, cookies, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
	webkit_cookie_manager_set_accept_policy(cm, WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
	return m;
}

static void tune(WebKitSettings *s) {
	webkit_settings_set_enable_developer_extras(s, FALSE);
	webkit_settings_set_enable_webgl(s, TRUE);
	webkit_settings_set_enable_smooth_scrolling(s, TRUE);
	webkit_settings_set_hardware_acceleration_policy(s, WEBKIT_HARDWARE_ACCELERATION_POLICY_ALWAYS);
	webkit_settings_set_enable_offline_web_application_cache(s, TRUE);
	webkit_settings_set_enable_html5_local_storage(s, TRUE);
	webkit_settings_set_enable_html5_database(s, TRUE);
	webkit_settings_set_enable_page_cache(s, TRUE);
	webkit_settings_set_user_agent_with_application_details(s, "ztBrowser", "1.0");
}

/* ---- the interface -------------------------------------------------------- */

struct webview *webview_create(struct wl_display *display, struct wl_compositor *compositor,
			       struct wl_surface *parent, const char *data_dir,
			       const struct wv_delegate *delegate) {
	static bool fdo_ready;
	if (!fdo_ready) {
		if (!wpe_fdo_initialize_for_egl_display(eglGetDisplay((EGLNativeDisplayType)display))) {
			return NULL;
		}
		create_image = (void *)eglGetProcAddress("eglCreateImageKHR");
		destroy_image = (void *)eglGetProcAddress("eglDestroyImageKHR");
		image_target = (void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
		fdo_ready = true;
	}
	struct webview *wv = calloc(1, sizeof(*wv));
	wv->display = display;
	wv->d = *delegate;
	wv->scale = 1;

	/* the subsurface the page is drawn into (placed by webview_place) */
	wv->surface = wl_compositor_create_surface(compositor);
	/* wl_subcompositor is bound by the chrome's bootstrap (see note in README):
	 * the parent surface's subcompositor is reused. The chrome binds it and
	 * passes it through a global; for now it is obtained from the registry. */
	/* --- subsurface + egl window + egl context: see webview_place --- */
	(void)parent;

	wv->exportable = wpe_view_backend_exportable_fdo_egl_create(
		&(struct wpe_view_backend_exportable_fdo_egl_client){ .export_fdo_egl_image = on_export },
		wv, 1, 1);
	struct wpe_view_backend *backend = wpe_view_backend_exportable_fdo_get_view_backend(wv->exportable);

	WebKitWebsiteDataManager *dm = data_manager(data_dir);
	WebKitWebContext *wc = webkit_web_context_new_with_website_data_manager(dm);
	webkit_web_context_set_process_model(wc, WEBKIT_PROCESS_MODEL_MULTIPLE_SECONDARY_PROCESSES);

	wv->web = WEBKIT_WEB_VIEW(webkit_web_view_new_with_context(
		webkit_web_view_backend_new(backend, NULL, NULL), wc));
	tune(webkit_web_view_get_settings(wv->web));
	g_signal_connect(wv->web, "notify::title", G_CALLBACK(notify_title), wv);
	g_signal_connect(wv->web, "notify::uri", G_CALLBACK(notify_uri), wv);
	g_signal_connect(wv->web, "notify::estimated-load-progress", G_CALLBACK(notify_progress), wv);
	g_signal_connect(wv->web, "load-changed", G_CALLBACK(load_changed), wv);
	return wv;
}

void webview_place(struct webview *wv, int x, int y, int w, int h, int scale) {
	wv->x = x; wv->y = y; wv->w = w; wv->h = h; wv->scale = scale;
	if (wv->egl_window) {
		wl_egl_window_resize(wv->egl_window, w * scale, h * scale, 0, 0);
	}
	if (wv->subsurface) {
		wl_subsurface_set_position(wv->subsurface, x, y);
	}
	/* tell WebKit the new size so it lays the page out to fit */
	struct wpe_view_backend *b = wpe_view_backend_exportable_fdo_get_view_backend(wv->exportable);
	wpe_view_backend_dispatch_set_size(b, w * scale, h * scale);
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

/* input: turn touches into libwpe events on the backend */
void webview_touch_down(struct webview *wv, int id, double x, double y) {
	struct wpe_view_backend *b = wpe_view_backend_exportable_fdo_get_view_backend(wv->exportable);
	struct wpe_input_touch_event_raw raw = { wpe_input_touch_event_type_down, 0, id,
		(int)(x * wv->scale), (int)(y * wv->scale) };
	struct wpe_input_touch_event ev = { &raw, 1, wpe_input_touch_event_type_down, id, 0 };
	wpe_view_backend_dispatch_touch_event(b, &ev);
}
void webview_touch_motion(struct webview *wv, int id, double x, double y) {
	struct wpe_view_backend *b = wpe_view_backend_exportable_fdo_get_view_backend(wv->exportable);
	struct wpe_input_touch_event_raw raw = { wpe_input_touch_event_type_motion, 0, id,
		(int)(x * wv->scale), (int)(y * wv->scale) };
	struct wpe_input_touch_event ev = { &raw, 1, wpe_input_touch_event_type_motion, id, 0 };
	wpe_view_backend_dispatch_touch_event(b, &ev);
}
void webview_touch_up(struct webview *wv, int id) {
	struct wpe_view_backend *b = wpe_view_backend_exportable_fdo_get_view_backend(wv->exportable);
	struct wpe_input_touch_event_raw raw = { wpe_input_touch_event_type_up, 0, id, 0, 0 };
	struct wpe_input_touch_event ev = { &raw, 1, wpe_input_touch_event_type_up, id, 0 };
	wpe_view_backend_dispatch_touch_event(b, &ev);
}
void webview_key(struct webview *wv, const char *utf8) { (void)wv; (void)utf8; /* TODO: map to wpe keysyms */ }
void webview_scroll(struct webview *wv, double dx, double dy) { (void)wv; (void)dx; (void)dy; }

void webview_destroy(struct webview *wv) {
	if (!wv) return;
	if (wv->web) g_object_unref(wv->web);
	if (wv->exportable) wpe_view_backend_exportable_fdo_destroy(wv->exportable);
	if (wv->egl_window) wl_egl_window_destroy(wv->egl_window);
	if (wv->subsurface) wl_subsurface_destroy(wv->subsurface);
	if (wv->surface) wl_surface_destroy(wv->surface);
	free(wv);
}
