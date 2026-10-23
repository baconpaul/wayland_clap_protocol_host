#pragma once

// A minimal Wayland compositor which the host runs for its plugins, so that their GUIs can be
// embedded into the host's window (CLAP_WINDOW_API_WAYLAND_SURFACE_EMBEDDING_V1).
//
// It doesn't render anything itself: it forwards the plugins' surfaces to the system compositor.
// The host may have several windows, all on one connection to the system compositor, and one
// compositor serves them all: a plugin may use one connection for all its instances.
// - Each place where a plugin can embed its surface is a wl_subsurface of one of the host's windows
// - The plugins' other windows, toplevels and popups, are xdg-shell windows on the system
//   compositor, kept above the host's window where the plugin embeds. Their popups may also use
//   that window as their parent, through clap_embedded_surface_v1.get_parent_surface.
// The plugins' shm pools are shared with the system compositor as is, so the pixels are never
// copied. Input flows back from the system compositor to the plugins: the pointer goes to the
// surface under it; the keyboard goes to a plugin's own window when it has the focus, and within
// a host's window to the place which was clicked last.
//
// It offers wl_compositor, wl_shm, wl_seat (pointer and keyboard), clap_surface_embedding_v1,
// xdg_wm_base, and if the system compositor has them, wp_viewporter and
// wp_fractional_scale_manager_v1.
//
// It runs on its own thread: the plugin talks to it synchronously (wl_display_roundtrip) from
// calls which the host makes on the main thread, like clap_plugin_gui_wayland_nested_compositor
// ->set_parent().

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct wl_compositor;
struct wl_display;
struct wl_event_queue;
struct wl_seat;
struct wl_shm;
struct wl_subcompositor;
struct wl_surface;
struct wp_cursor_shape_manager_v1;
struct wp_fractional_scale_manager_v1;
struct wp_viewporter;
struct xdg_surface;
struct xdg_toplevel;
struct xdg_wm_base;
struct zwp_linux_dmabuf_v1;

class NestedCompositor {
public:
   // The host's objects on the system compositor. They must outlive the compositor.
   struct Upstream {
      wl_display *display = nullptr;
      wl_compositor *compositor = nullptr;
      wl_subcompositor *subcompositor = nullptr;
      wl_shm *shm = nullptr;
      // optional, version 4 or later: GPU buffers, forwarded as is like wl_shm's
      zwp_linux_dmabuf_v1 *linuxDmabuf = nullptr;
      uint32_t linuxDmabufVersion = 0;
      wl_seat *seat = nullptr;                                         // optional
      wp_viewporter *viewporter = nullptr;                             // optional
      wp_fractional_scale_manager_v1 *fractionalScaleManager = nullptr; // optional
      wp_cursor_shape_manager_v1 *cursorShapeManager = nullptr;       // optional

      // For the plugins' own windows (optional, without it they can only embed). The host
      // answers xdg_wm_base's pings.
      xdg_wm_base *wmBase = nullptr;
      uint32_t wmBaseVersion = 0;
   };

   // A window of the host, on the same connection as Upstream's objects
   struct Window {
      wl_surface *surface = nullptr;     // its toplevel surface, the places' parent
      xdg_surface *xdgSurface = nullptr; // as popups' parent
      xdg_toplevel *toplevel = nullptr;  // as dialogs' parent
   };
   using WindowId = uint32_t;

   NestedCompositor();
   ~NestedCompositor();

   NestedCompositor(const NestedCompositor &) = delete;
   NestedCompositor &operator=(const NestedCompositor &) = delete;

   [[nodiscard]] bool start(const Upstream &upstream);
   void stop();

   // For clap_host_gui_wayland_nested_compositor->open_connection(): a connected socket, which
   // the caller owns, or -1. [main thread]
   int openConnection();

   // Adds a window which the plugins can embed into. [main thread]
   WindowId addWindow(const Window &window);

   // Ends the embeddings into the window's places: their plugins get clap_embedded_surface_v1
   // .closed. The host calls it before it destroys the window. [main thread]
   void removeWindow(WindowId window);

   // Reserves a place at (x, y) in the window's surface, and returns the embedding token which
   // designates it. The caller commits the window's surface to apply the position. [main thread]
   std::string createPlace(WindowId window, int32_t x, int32_t y);

   // Where the window surface's (0, 0) is within the window's geometry, for example below a title
   // bar which the host draws itself. The plugins place their popups relative to the window's
   // geometry. [main thread]
   void setParentOrigin(WindowId window, int32_t x, int32_t y);

private:
   struct Impl;
   std::unique_ptr<Impl> _impl;
};
