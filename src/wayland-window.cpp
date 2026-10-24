#include "wayland-window.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include <sys/mman.h>
#include <unistd.h>

#include <libdecor.h>
#include <wayland-client.h>

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

namespace {
   constexpr uint32_t kBackgroundColor = 0x202020;
   constexpr const char *kBarText = "host area";

   // A 5x7 bitmap font, only the letters which we draw: one byte per row, the high bit on the left
   struct Glyph {
      char c;
      uint8_t rows[7];
   };
   constexpr Glyph kGlyphs[] = {
      {'a', {0x00, 0x00, 0x0e, 0x01, 0x0f, 0x11, 0x0f}},
      {'e', {0x00, 0x00, 0x0e, 0x11, 0x1f, 0x10, 0x0e}},
      {'h', {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11}},
      {'o', {0x00, 0x00, 0x0e, 0x11, 0x11, 0x11, 0x0e}},
      {'r', {0x00, 0x00, 0x16, 0x19, 0x10, 0x10, 0x10}},
      {'s', {0x00, 0x00, 0x0f, 0x10, 0x0e, 0x01, 0x1e}},
      {'t', {0x08, 0x08, 0x1c, 0x08, 0x08, 0x09, 0x06}},
   };

   // Draws text at (x, y), each font pixel as a scale x scale square; unknown characters are spaces
   void drawText(uint32_t *pixels, uint32_t width, uint32_t height, uint32_t x, uint32_t y, int scale,
                 const char *text, uint32_t color) {
      for (; *text; ++text, x += 6 * scale) {
         const Glyph *glyph = nullptr;
         for (auto &g : kGlyphs)
            if (g.c == *text)
               glyph = &g;
         if (!glyph)
            continue;

         for (uint32_t row = 0; row < 7; ++row)
            for (uint32_t col = 0; col < 5; ++col) {
               if (!(glyph->rows[row] & (0x10 >> col)))
                  continue;
               for (int dy = 0; dy < scale; ++dy)
                  for (int dx = 0; dx < scale; ++dx) {
                     const uint32_t px = x + col * scale + dx;
                     const uint32_t py = y + row * scale + dy;
                     if (px < width && py < height)
                        pixels[size_t(py) * width + px] = color;
                  }
            }
      }
   }
} // namespace

WaylandConnection::~WaylandConnection() {
   if (_decor)
      libdecor_unref(_decor);
   if (_cursorShapeDevice)
      wp_cursor_shape_device_v1_destroy(_cursorShapeDevice);
   if (_pointer)
      wl_pointer_release(_pointer);
   if (_seat)
      wl_seat_release(_seat);
   if (_cursorShapeManager)
      wp_cursor_shape_manager_v1_destroy(_cursorShapeManager);
   if (_fractionalScaleManager)
      wp_fractional_scale_manager_v1_destroy(_fractionalScaleManager);
   if (_viewporter)
      wp_viewporter_destroy(_viewporter);
   if (_wmBase)
      xdg_wm_base_destroy(_wmBase);
   if (_shm)
      wl_shm_destroy(_shm);
   if (_linuxDmabuf)
      zwp_linux_dmabuf_v1_destroy(_linuxDmabuf);
   if (_subcompositor)
      wl_subcompositor_destroy(_subcompositor);
   if (_compositor)
      wl_compositor_destroy(_compositor);
   if (_registry)
      wl_registry_destroy(_registry);
   if (_display)
      wl_display_disconnect(_display);
}

bool WaylandConnection::open(const char *appId) {
   _appId = appId;
   _display = wl_display_connect(nullptr);
   if (!_display) {
      std::cerr << "failed to connect to the Wayland compositor (is WAYLAND_DISPLAY set?)\n";
      return false;
   }
   _isConnected = true;

   static const wl_registry_listener registryListener = {
      .global = [](void *data, wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version) { self(data).onGlobal(registry, name, interface, version); },
      .global_remove = [](void *, wl_registry *, uint32_t) {},
   };
   _registry = wl_display_get_registry(_display);
   wl_registry_add_listener(_registry, &registryListener, this);
   wl_display_roundtrip(_display);

   if (!_compositor || !_subcompositor || !_shm) {
      std::cerr << "the compositor lacks wl_compositor, wl_subcompositor or wl_shm\n";
      return false;
   }
   std::cout << "system compositor: viewporter " << (_viewporter ? "yes" : "no") << ", fractional-scale "
             << (_fractionalScaleManager ? "yes" : "no") << "\n";

   static libdecor_interface decorInterface = {
      .error = [](libdecor *, libdecor_error error, const char *message) {
         std::cerr << "libdecor error " << error << ": " << message << "\n";
      },
   };
   _decor = libdecor_new(_display, &decorInterface);
   if (!_decor) {
      std::cerr << "libdecor failed to initialize\n";
      return false;
   }
   return true;
}

WaylandWindow::WaylandWindow(WaylandConnection &connection, const Colors &colors)
   : _connection(connection), _colors(colors) {
   _connection._windows.push_back(this);
}

WaylandWindow::~WaylandWindow() {
   std::erase(_connection._windows, this);
   if (_buffer)
      wl_buffer_destroy(_buffer);
   if (_frame)
      libdecor_frame_unref(_frame);
   if (_surface)
      wl_surface_destroy(_surface);
   wl_display_flush(_connection._display);
}

bool WaylandWindow::open(const char *title, uint32_t width, uint32_t height) {
   _width = width;
   _height = height + kBarHeight;
   _isOpen = true;

   static libdecor_frame_interface frameInterface = {
      .configure = [](libdecor_frame *frame, libdecor_configuration *configuration, void *data) {
         self(data).onConfigure(frame, configuration);
      },
      .close = [](libdecor_frame *, void *data) { self(data)._isOpen = false; },
      // Our surface's state, for example the plugin's subsurface position, goes with the frame's
      .commit = [](libdecor_frame *, void *data) { wl_surface_commit(self(data)._surface); },
      .dismiss_popup = [](libdecor_frame *, const char *, void *) {},
   };

   _surface = wl_compositor_create_surface(_connection._compositor);
   _frame = libdecor_decorate(_connection._decor, _surface, &frameInterface, this);
   libdecor_frame_set_title(_frame, title);
   libdecor_frame_set_app_id(_frame, _connection._appId);
   // The plugin decides the size, see resize(); maximizing would only add empty space
   libdecor_frame_unset_capabilities(
      _frame, static_cast<libdecor_capabilities>(LIBDECOR_ACTION_RESIZE | LIBDECOR_ACTION_FULLSCREEN));
   libdecor_frame_set_min_content_size(_frame, _width, _height);
   libdecor_frame_set_max_content_size(_frame, _width, _height);
   libdecor_frame_map(_frame);

   while (isOpen() && !_isConfigured)
      if (libdecor_dispatch(_connection._decor, -1) < 0)
         return false;

   return isOpen();
}

// The compositor's size doesn't matter: our content has the plugin's size
void WaylandWindow::onConfigure(libdecor_frame *, libdecor_configuration *configuration) {
   commitFrame(configuration);
   if (!_isConfigured) {
      _isConfigured = true;
      redraw();
   }
}

void WaylandWindow::commitFrame(libdecor_configuration *configuration) {
   libdecor_state *state = libdecor_state_new(_width, _height);
   libdecor_frame_commit(_frame, state, configuration);
   libdecor_state_free(state);
}

void WaylandWindow::resize(uint32_t width, uint32_t height) {
   height += kBarHeight;
   if (width == _width && height == _height)
      return;

   _width = width;
   _height = height;
   libdecor_frame_set_min_content_size(_frame, _width, _height);
   libdecor_frame_set_max_content_size(_frame, _width, _height);
   commitFrame(nullptr);
   redraw();
}

NestedCompositor::Upstream WaylandConnection::upstream() const {
   return {
      .display = _display,
      .compositor = _compositor,
      .subcompositor = _subcompositor,
      .shm = _shm,
      .linuxDmabuf = _linuxDmabuf,
      .linuxDmabufVersion = _linuxDmabufVersion,
      .seat = _seat,
      .viewporter = _viewporter,
      .fractionalScaleManager = _fractionalScaleManager,
      .cursorShapeManager = _cursorShapeManager,
      .wmBase = _wmBase,
      .wmBaseVersion = _wmBaseVersion,
   };
}

NestedCompositor::Window WaylandWindow::nested() const {
   return {
      .surface = _surface,
      .xdgSurface = libdecor_frame_get_xdg_surface(_frame),
      .toplevel = libdecor_frame_get_xdg_toplevel(_frame),
   };
}

void WaylandWindow::contentOrigin(int32_t &x, int32_t &y) const {
   int frameX = 0;
   int frameY = 0;
   libdecor_frame_translate_coordinate(_frame, 0, 0, &frameX, &frameY);
   x = frameX;
   y = frameY;
}

void WaylandWindow::commit() {
   wl_surface_commit(_surface);
   wl_display_flush(_connection._display);
}

void WaylandConnection::onGlobal(wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
   auto bind = [&](const wl_interface *iface, uint32_t v) { return wl_registry_bind(registry, name, iface, v); };

   if (!strcmp(interface, wl_compositor_interface.name) && version >= 4) {
      // v6 gives wl_surface.preferred_buffer_scale, which we forward to the plugins
      _compositor = static_cast<wl_compositor *>(bind(&wl_compositor_interface, std::min(version, 6u)));
   } else if (!strcmp(interface, wl_subcompositor_interface.name)) {
      _subcompositor = static_cast<wl_subcompositor *>(bind(&wl_subcompositor_interface, 1));
   } else if (!strcmp(interface, wl_shm_interface.name)) {
      _shm = static_cast<wl_shm *>(bind(&wl_shm_interface, 1));
   } else if (!strcmp(interface, zwp_linux_dmabuf_v1_interface.name) && version >= 4) {
      // Only for the plugins, through the nested compositor. Version 4 brings the feedback,
      // which tells the plugins' drivers which GPU to render on.
      _linuxDmabufVersion = std::min(version, 5u);
      _linuxDmabuf =
         static_cast<zwp_linux_dmabuf_v1 *>(bind(&zwp_linux_dmabuf_v1_interface, _linuxDmabufVersion));
   } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
      // For the plugins' own windows, through the nested compositor; libdecor has its own.
      // Version 6 at most: the nested compositor forwards it as is, and knows up to that one.
      static const xdg_wm_base_listener wmBaseListener = {
         .ping = [](void *, xdg_wm_base *wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); },
      };
      _wmBaseVersion = std::min(version, 6u);
      _wmBase = static_cast<xdg_wm_base *>(bind(&xdg_wm_base_interface, _wmBaseVersion));
      xdg_wm_base_add_listener(_wmBase, &wmBaseListener, this);
   } else if (!strcmp(interface, wp_viewporter_interface.name)) {
      _viewporter = static_cast<wp_viewporter *>(bind(&wp_viewporter_interface, 1));
   } else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
      _fractionalScaleManager = static_cast<wp_fractional_scale_manager_v1 *>(
         bind(&wp_fractional_scale_manager_v1_interface, 1));
   } else if (!strcmp(interface, wp_cursor_shape_manager_v1_interface.name)) {
      _cursorShapeManager =
         static_cast<wp_cursor_shape_manager_v1 *>(bind(&wp_cursor_shape_manager_v1_interface, 1));
   } else if (!strcmp(interface, wl_seat_interface.name) && !_seat && version >= 5) {
      static const wl_seat_listener seatListener = {
         .capabilities = [](void *data, wl_seat *, uint32_t caps) { self(data).onSeatCapabilities(caps); },
         .name = [](void *, wl_seat *, const char *) {},
      };
      _seat = static_cast<wl_seat *>(bind(&wl_seat_interface, 5));
      wl_seat_add_listener(_seat, &seatListener, this);
   }
}

bool WaylandConnection::isOurs(wl_surface *surface) const {
   for (auto w : _windows)
      if (w->_surface == surface)
         return true;
   return false;
}

// Our own pointer, for the cursor over the bars; the nested compositor has its own one for the
// plugins' areas
void WaylandConnection::onSeatCapabilities(uint32_t capabilities) {
   static const wl_pointer_listener pointerListener = {
      .enter =
         [](void *data, wl_pointer *, uint32_t serial, wl_surface *surface, wl_fixed_t, wl_fixed_t) {
            auto &c = self(data);
            if (c.isOurs(surface) && c._cursorShapeDevice)
               wp_cursor_shape_device_v1_set_shape(c._cursorShapeDevice, serial,
                                                   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
         },
      .leave = [](void *, wl_pointer *, uint32_t, wl_surface *) {},
      .motion = [](void *, wl_pointer *, uint32_t, wl_fixed_t, wl_fixed_t) {},
      .button = [](void *, wl_pointer *, uint32_t, uint32_t, uint32_t, uint32_t) {},
      .axis = [](void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {},
      .frame = [](void *, wl_pointer *) {},
      .axis_source = [](void *, wl_pointer *, uint32_t) {},
      .axis_stop = [](void *, wl_pointer *, uint32_t, uint32_t) {},
      .axis_discrete = [](void *, wl_pointer *, uint32_t, int32_t) {},
   };

   if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !_pointer) {
      _pointer = wl_seat_get_pointer(_seat);
      wl_pointer_add_listener(_pointer, &pointerListener, this);
      if (_cursorShapeManager)
         _cursorShapeDevice = wp_cursor_shape_manager_v1_get_pointer(_cursorShapeManager, _pointer);
   }
}

// A bar with its label and an accent line, and a dark background behind the plugin's area
void WaylandWindow::redraw() {
   const uint32_t stride = _width * 4;
   const size_t size = size_t(stride) * _height;

   int fd = memfd_create("clap-wayland-host", MFD_CLOEXEC);
   if (fd < 0)
      return;
   if (ftruncate(fd, size) < 0) {
      close(fd);
      return;
   }
   auto pixels = static_cast<uint32_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
   if (pixels == MAP_FAILED) {
      close(fd);
      return;
   }

   for (uint32_t y = 0; y < _height; ++y) {
      uint32_t color = kBackgroundColor;
      if (y < kBarHeight - 2)
         color = _colors.bar;
      else if (y < kBarHeight)
         color = _colors.accent;
      std::fill_n(pixels + size_t(y) * _width, _width, color);
   }
   // 14px tall, centered in the bar above the accent line
   constexpr int kTextScale = 2;
   drawText(pixels, _width, _height, 12, (kBarHeight - 2 - 7 * kTextScale) / 2, kTextScale, kBarText, _colors.text);
   munmap(pixels, size);

   auto pool = wl_shm_create_pool(_connection._shm, fd, size);
   auto buffer = wl_shm_pool_create_buffer(pool, 0, _width, _height, stride, WL_SHM_FORMAT_XRGB8888);
   wl_shm_pool_destroy(pool);
   close(fd);

   wl_surface_attach(_surface, buffer, 0, 0);
   wl_surface_damage_buffer(_surface, 0, 0, _width, _height);
   wl_surface_commit(_surface);
   // Destroying it while the compositor uses it is fine: it keeps its own reference
   if (_buffer)
      wl_buffer_destroy(_buffer);
   _buffer = buffer;
   wl_display_flush(_connection._display);
}

void WaylandConnection::dispatch(int timeoutMs, std::vector<pollfd> &extraFds) {
   while (wl_display_prepare_read(_display) != 0)
      wl_display_dispatch_pending(_display);
   wl_display_flush(_display);

   std::vector<pollfd> fds;
   fds.reserve(extraFds.size() + 1);
   fds.push_back({.fd = wl_display_get_fd(_display), .events = POLLIN, .revents = 0});
   fds.insert(fds.end(), extraFds.begin(), extraFds.end());

   const int ready = poll(fds.data(), fds.size(), timeoutMs);
   if (ready > 0 && (fds[0].revents & POLLIN)) {
      if (wl_display_read_events(_display) < 0)
         _isConnected = false;
   } else
      wl_display_cancel_read(_display);

   for (size_t i = 0; i < extraFds.size(); ++i)
      extraFds[i].revents = ready > 0 ? fds[i + 1].revents : 0;

   if (wl_display_dispatch_pending(_display) < 0)
      _isConnected = false;

   // libdecor's GTK plugin has its own main loop work, for example to redraw the title bars
   if (_isConnected && libdecor_dispatch(_decor, 0) < 0)
      _isConnected = false;
}
