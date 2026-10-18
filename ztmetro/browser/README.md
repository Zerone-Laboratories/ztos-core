# ztBrowser

A web browser for ztOS, in the look and motion of the Windows Phone 8.1
Internet Explorer. Built on the **C** ZrnMetroCore SDK (not the Python one),
with **WPE WebKit** for the web engine, composited on the GPU.

## Why WPE WebKit

WPE (Web Platform for Embedded) is the WebKit port made for embedded and
mobile: no GTK, no X11, Wayland/EGL native, the smallest footprint of any
WebKit, and hardware-accelerated compositing out of the box. It is the right
engine for "very efficient, low resource, HW-accelerated on mobile". WebKitGTK
would drag in GTK and a second toolkit's event loop; we do not use it.

Packages (Arch / Arch Linux ARM): `wpewebkit`, `wpebackend-fdo`, `libwpe`.
pkg-config: `wpe-webkit-2.0` (or `-1.1`), `wpebackend-fdo-1.0`, `wpe-1.0`.

## How it is put together

- **The chrome is zmc, the web view is a Wayland subsurface.** The app's main
  surface draws the Windows Phone chrome with the C SDK (address bar at the
  bottom, progress line, tabs, menu). The web page is a `wl_subsurface` the app
  creates (from `zmc.compositor` + `wl_subcompositor`) over the content area, so
  the web content is composited by the server, never rastered on the CPU.
- **WPE renders into that subsurface.** `wpebackend-fdo` exports each rendered
  frame as an EGL image (`wpe_fdo_egl_exported_image`); the app imports it as a
  GL texture in an EGL context on the subsurface's `wl_egl_window` and draws it.
  Frame-driven by WPE, so an idle page costs nothing.
- **Input is forwarded.** Touches zmc reports in the content area are turned
  into libwpe pointer/touch events and sent to the view; the chrome keeps the
  rest (the keyboard uses zmc's text hook into WPE key events).

## The Windows Phone 8.1 IE look

- The **address bar is at the bottom** (IE11 for WP8.1), with the URL, a
  refresh/stop glyph, and a "..." that opens tabs, favourites and settings.
- A thin **progress line** under the page while loading.
- **Tabs** as a grid of page thumbnails (the WP tab switcher), new/close.
- Flat, accent-coloured, Segoe/Selawik, the same tilt on every control as the
  rest of ztMETRO.

## Animations from the Lumia firmware

Reuse the SDK's Windows Phone transitions (added in `sdk` / `zmc_transition`):
Turnstile for page-to-page, the tab switcher's tilt/continuum, the address bar
sliding up from the bottom edge. Measured from the Lumia 530 firmware, as the
shell's own transitions were.

## Persistence (local storage, cookies, …)

One `WebKitWebsiteDataManager` rooted at `~/.local/share/ztbrowser`:

- cookies: `WebKitCookieManager`, persistent SQLite, accept-policy no-third-party
- localStorage, IndexedDB, the disk cache, service workers, offline app cache —
  all given directories under that root so they survive restarts.

## Optimisation

- Single web process, GPU compositing always on, WebGL on, developer extras off.
- A cap on the page cache and the memory cache; respond to memory pressure
  (`WebKitMemoryPressureSettings`) so a background tab is trimmed.
- `zrnperfd` policy `eco` while idle, `normal` while loading or scrolling.
- No speculative tabs; tab thumbnails are small GPU snapshots, not live views.

## Build and run

    make browser/ztbrowser     # on the phone, with the rest of ztmetro

Needs the WPE packages above on the phone. See the Makefile for the exact
pkg-config modules; the WebKit API is version-sensitive, so the first build on
the device may need small deltas against the installed `wpe-webkit` version.

## State

Iteration 1: the chrome, navigation state, and the WPE view wiring. The
engine glue targets `wpe-webkit-2.0`; confirm the installed version on the
phone and adjust if the build reports a changed symbol.
