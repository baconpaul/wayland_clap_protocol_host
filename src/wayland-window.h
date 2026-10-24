#pragma once

// The host's own toplevel windows on the system Wayland compositor: a strip of "host UI" at the
// top, and the area below it where a plugin's GUI is embedded through the nested compositor.
//
// All the windows share one connection, which the nested compositor forwards the plugins to.
// libdecor decorates them: with the compositor's title bar where there is xdg-decoration, and
// otherwise (GNOME) with one it draws itself.

#include <cstdint>
#include <vector>

#include <poll.h>

#include "nested-compositor.h"

struct libdecor;
struct libdecor_configuration;
struct libdecor_frame;
struct wl_buffer;
struct wl_pointer;
struct wl_registry;
struct wp_cursor_shape_device_v1;
struct xdg_wm_base;

class WaylandWindow;

// The connection to the system compositor, and its globals
class WaylandConnection {
public:
   WaylandConnection() = default;
   ~WaylandConnection();

   WaylandConnection(const WaylandConnection &) = delete;
   WaylandConnection &operator=(const WaylandConnection &) = delete;

   // appId is the windows' xdg_toplevel app id
   [[nodiscard]] bool open(const char *appId = "clap-wayland-host");

   // What the nested compositor needs to forward the plugins' surfaces
   NestedCompositor::Upstream upstream() const;

   // Reads and dispatches our connection's events, waiting up to timeoutMs (-1: forever).
   // extraFds are polled too, so that the plugins' posix fds wake us up; their revents are set.
   void dispatch(int timeoutMs, std::vector<pollfd> &extraFds);

   bool isConnected() const { return _isConnected; }

private:
   friend class WaylandWindow;

   static WaylandConnection &self(void *data) { return *static_cast<WaylandConnection *>(data); }

   void onGlobal(wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
   void onSeatCapabilities(uint32_t capabilities);
   bool isOurs(wl_surface *surface) const;

   bool _isConnected = false;
   const char *_appId = nullptr;

   wl_display *_display = nullptr;
   wl_registry *_registry = nullptr;
   wl_compositor *_compositor = nullptr;
   wl_subcompositor *_subcompositor = nullptr;
   wl_shm *_shm = nullptr;
   zwp_linux_dmabuf_v1 *_linuxDmabuf = nullptr;
   uint32_t _linuxDmabufVersion = 0;
   xdg_wm_base *_wmBase = nullptr;
   uint32_t _wmBaseVersion = 0;
   wp_viewporter *_viewporter = nullptr;
   wp_fractional_scale_manager_v1 *_fractionalScaleManager = nullptr;
   wp_cursor_shape_manager_v1 *_cursorShapeManager = nullptr;
   wl_seat *_seat = nullptr;
   wl_pointer *_pointer = nullptr;
   wp_cursor_shape_device_v1 *_cursorShapeDevice = nullptr;

   libdecor *_decor = nullptr;
   std::vector<WaylandWindow *> _windows;
};

class WaylandWindow {
public:
   static constexpr uint32_t kBarHeight = 32;

   struct Colors {
      uint32_t bar;
      uint32_t accent; // the line below the bar
      uint32_t text;
   };

   WaylandWindow(WaylandConnection &connection, const Colors &colors);
   ~WaylandWindow();

   WaylandWindow(const WaylandWindow &) = delete;
   WaylandWindow &operator=(const WaylandWindow &) = delete;

   // The size is the embedded area's, the window adds the bar on top
   [[nodiscard]] bool open(const char *title, uint32_t width, uint32_t height);
   void resize(uint32_t width, uint32_t height);

   // What the nested compositor needs to put a plugin's surface into this window
   NestedCompositor::Window nested() const;

   // Where our content's (0, 0) is in the window's geometry: below libdecor's title bar
   void contentOrigin(int32_t &x, int32_t &y) const;

   // Applies the pending state of our surface, for example a subsurface's position
   void commit();

   bool isOpen() const { return _isOpen && _connection.isConnected(); }

private:
   friend class WaylandConnection;

   static WaylandWindow &self(void *data) { return *static_cast<WaylandWindow *>(data); }

   void onConfigure(libdecor_frame *frame, libdecor_configuration *configuration);
   void commitFrame(libdecor_configuration *configuration);
   void redraw();

   WaylandConnection &_connection;
   const Colors _colors;

   bool _isOpen = false;
   bool _isConfigured = false;
   uint32_t _width = 0;
   uint32_t _height = 0;

   wl_surface *_surface = nullptr;
   libdecor_frame *_frame = nullptr;
   wl_buffer *_buffer = nullptr;
};
