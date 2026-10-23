// A CLAP which draws its GUI with nothing but libwayland-client and wl_shm, embedded in the host's
// window with clap.gui-wayland-nested-compositor and clap_surface_embedding_v1: the smallest
// complete use of the protocol.
//
// - gui.create(): the host gives us a connection to its nested compositor (open_connection), on
//   which we create a plain wl_surface. The connection's fd goes to the host's posix-fd-support,
//   which calls us back when the compositor has sent events.
// - set_parent(token): clap_surface_embedding_v1.get_embedded_surface gives the surface its role.
// - gui.show(): we attach a buffer, and redraw on every frame callback. The compositor paces the
//   animation, and stops it while the GUI isn't visible.
//
// The picture is a palette-cycling plasma, in software. Moving the pointer moves a lens, and
// clicking changes the palette. The audio is a quiet 330 Hz sine.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

#include <clap/clap.h>
#include <clap/ext/draft/wayland-experiment.h>

#include "xdg-shell-client-protocol.h"
// After xdg-shell's
#include "clap-surface-embedding-v1-client-protocol.h"

namespace {

   constexpr int32_t kWidth = 400;
   constexpr int32_t kHeight = 260;
   constexpr double kFrequency = 330;
   constexpr float kLevel = 0.1f;

   const char *const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                    CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};

   const clap_plugin_descriptor kDescriptor = {
      .clap_version = CLAP_VERSION_INIT,
      .id = "org.example.wayland-shm-demo",
      .name = "Wayland SHM Demo",
      .vendor = "wayland_clap_protocol_host",
      .url = "",
      .manual_url = "",
      .support_url = "",
      .version = "0.1.0",
      .description = "Plasma drawn into wl_shm buffers, embedded with the Wayland nested compositor API",
      .features = kFeatures,
   };

   // A table of sines in 0..255, over 256 steps
   const std::array<uint8_t, 256> kSine = [] {
      std::array<uint8_t, 256> table{};
      for (int i = 0; i < 256; ++i)
         table[i] = uint8_t(127.5 + 127.5 * std::sin(2 * M_PI * i / 256));
      return table;
   }();

   struct ShmBuffer {
      wl_buffer *buffer = nullptr;
      uint32_t *pixels = nullptr;
      size_t size = 0;
      int32_t width = 0; // in buffer pixels
      int32_t height = 0;
      bool isBusy = false; // until the compositor releases it
   };

   class Plugin {
   public:
      explicit Plugin(const clap_host *host) : _host(host) {
         _plugin.desc = &kDescriptor;
         _plugin.plugin_data = this;
         _plugin.init = [](const clap_plugin *p) { return self(p).init(); };
         _plugin.destroy = [](const clap_plugin *p) {
            self(p).guiDestroy();
            delete &self(p);
         };
         _plugin.activate = [](const clap_plugin *p, double sampleRate, uint32_t, uint32_t) {
            self(p)._sampleRate = sampleRate;
            return true;
         };
         _plugin.deactivate = [](const clap_plugin *) {};
         _plugin.start_processing = [](const clap_plugin *) { return true; };
         _plugin.stop_processing = [](const clap_plugin *) {};
         _plugin.reset = [](const clap_plugin *p) { self(p)._phase = 0; };
         _plugin.process = [](const clap_plugin *p, const clap_process *process) {
            return self(p).process(process);
         };
         _plugin.get_extension = [](const clap_plugin *p, const char *id) {
            return self(p).extension(id);
         };
         _plugin.on_main_thread = [](const clap_plugin *) {};
      }

      const clap_plugin *clapPlugin() const { return &_plugin; }

   private:
      static Plugin &self(const clap_plugin *p) { return *static_cast<Plugin *>(p->plugin_data); }

      void log(clap_log_severity severity, const char *format, ...) {
         char message[512];
         va_list args;
         va_start(args, format);
         vsnprintf(message, sizeof(message), format, args);
         va_end(args);
         if (_hostLog)
            _hostLog->log(_host, severity, message);
         else
            fprintf(stderr, "wayland-shm-demo: %s\n", message);
      }

      bool init() {
         // Queried here since some hosts only answer during init()
         _hostLog = static_cast<const clap_host_log *>(_host->get_extension(_host, CLAP_EXT_LOG));
         _hostPosixFd = static_cast<const clap_host_posix_fd_support *>(
            _host->get_extension(_host, CLAP_EXT_POSIX_FD_SUPPORT));
         _hostCompositor = static_cast<const clap_host_gui_wayland_nested_compositor *>(
            _host->get_extension(_host, CLAP_EXT_GUI_WAYLAND_NESTED_COMPOSITOR));
         return true;
      }

      // ---- audio ----

      clap_process_status process(const clap_process *process) {
         if (process->audio_outputs_count < 1)
            return CLAP_PROCESS_CONTINUE;

         const auto &out = process->audio_outputs[0];
         const double step = 2 * M_PI * kFrequency / _sampleRate;
         for (uint32_t i = 0; i < process->frames_count; ++i) {
            const float sample = kLevel * float(std::sin(_phase));
            _phase = std::fmod(_phase + step, 2 * M_PI);
            for (uint32_t c = 0; c < out.channel_count; ++c)
               out.data32[c][i] = sample;
         }
         return CLAP_PROCESS_CONTINUE;
      }

      static const clap_plugin_audio_ports kAudioPorts;

      // ---- Wayland ----

      bool canEmbed() const { return _hostCompositor && _hostPosixFd; }

      bool isApiSupported(const char *api, bool isFloating) const {
         return !isFloating && !strcmp(api, CLAP_WINDOW_API_WAYLAND_SURFACE_EMBEDDING_V1) &&
                canEmbed();
      }

      void onGlobal(wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
         auto bind = [&](const wl_interface *iface, uint32_t v) {
            return wl_registry_bind(registry, name, iface, v);
         };
         // wl_surface v4 for damage_buffer, v6 for preferred_buffer_scale
         if (!strcmp(interface, wl_compositor_interface.name) && version >= 4)
            _compositor = static_cast<wl_compositor *>(
               bind(&wl_compositor_interface, std::min(version, 6u)));
         else if (!strcmp(interface, wl_shm_interface.name))
            _shm = static_cast<wl_shm *>(bind(&wl_shm_interface, 1));
         else if (!strcmp(interface, wl_seat_interface.name) && !_seat && version >= 5) {
            _seat = static_cast<wl_seat *>(bind(&wl_seat_interface, 5));
            wl_seat_add_listener(_seat, &kSeatListener, this);
         } else if (!strcmp(interface, clap_surface_embedding_v1_interface.name))
            _embedding = static_cast<clap_surface_embedding_v1 *>(
               bind(&clap_surface_embedding_v1_interface, 1));
      }

      static const wl_registry_listener kRegistryListener;
      static const wl_seat_listener kSeatListener;
      static const wl_pointer_listener kPointerListener;
      static const wl_surface_listener kSurfaceListener;
      static const wl_buffer_listener kBufferListener;
      static const wl_callback_listener kFrameListener;
      static const clap_embedded_surface_v1_listener kEmbeddedSurfaceListener;

      // Sends our requests. If the socket is full, the rest goes with the next frame.
      void flush() {
         if (_display && wl_display_flush(_display) < 0 && errno != EAGAIN)
            onConnectionError();
      }

      // Reads and dispatches the compositor's events, when the host says that the fd is readable
      void onFd(int fd, clap_posix_fd_flags_t flags) {
         if (!_display || fd != _fd)
            return;
         if (flags & CLAP_POSIX_FD_ERROR) {
            onConnectionError();
            return;
         }
         while (wl_display_prepare_read(_display) != 0)
            wl_display_dispatch_pending(_display);
         if (wl_display_read_events(_display) < 0 || wl_display_dispatch_pending(_display) < 0) {
            onConnectionError();
            return;
         }
         flush();
      }

      void onConnectionError() {
         log(CLAP_LOG_ERROR, "lost the connection to the host's compositor (error %d)",
             _display ? wl_display_get_error(_display) : 0);
         // The fd stays registered until guiDestroy(), but the host mustn't spin on it
         if (_fd >= 0) {
            _hostPosixFd->unregister_fd(_host, _fd);
            _fd = -1;
         }
         _isVisible = false;
      }

      // ---- GUI ----

      bool guiCreate(const char *api, bool isFloating) {
         if (!isApiSupported(api, isFloating) || _display)
            return false;

         // wl_display_connect_to_fd() takes the fd, even on failure
         const int fd = _hostCompositor->open_connection(_host);
         if (fd < 0)
            return false;
         _display = wl_display_connect_to_fd(fd);
         if (!_display) {
            log(CLAP_LOG_ERROR, "can't connect to the host's compositor");
            return false;
         }

         _registry = wl_display_get_registry(_display);
         wl_registry_add_listener(_registry, &kRegistryListener, this);
         if (wl_display_roundtrip(_display) < 0 || !_compositor || !_shm || !_embedding) {
            log(CLAP_LOG_ERROR, "the host's compositor lacks wl_compositor v4, wl_shm or "
                                "clap_surface_embedding_v1");
            guiDestroy();
            return false;
         }

         _surface = wl_compositor_create_surface(_compositor);
         wl_surface_add_listener(_surface, &kSurfaceListener, this);

         _fd = wl_display_get_fd(_display);
         if (!_hostPosixFd->register_fd(_host, _fd, CLAP_POSIX_FD_READ | CLAP_POSIX_FD_ERROR)) {
            _fd = -1;
            guiDestroy();
            return false;
         }
         flush();
         return true;
      }

      void guiDestroy() {
         if (_fd >= 0) {
            _hostPosixFd->unregister_fd(_host, _fd);
            _fd = -1;
         }
         if (_frameCallback) {
            wl_callback_destroy(_frameCallback);
            _frameCallback = nullptr;
         }
         for (auto &b : _buffers)
            destroyBuffer(b);
         // The role goes before the surface
         if (_embeddedSurface) {
            clap_embedded_surface_v1_destroy(_embeddedSurface);
            _embeddedSurface = nullptr;
         }
         if (_surface) {
            wl_surface_destroy(_surface);
            _surface = nullptr;
         }
         if (_pointer) {
            wl_pointer_release(_pointer);
            _pointer = nullptr;
         }
         if (_seat) {
            wl_seat_release(_seat);
            _seat = nullptr;
         }
         if (_embedding) {
            clap_surface_embedding_v1_destroy(_embedding);
            _embedding = nullptr;
         }
         if (_shm) {
            wl_shm_destroy(_shm);
            _shm = nullptr;
         }
         if (_compositor) {
            wl_compositor_destroy(_compositor);
            _compositor = nullptr;
         }
         if (_registry) {
            wl_registry_destroy(_registry);
            _registry = nullptr;
         }
         if (_display) {
            wl_display_disconnect(_display);
            _display = nullptr;
         }
         _isVisible = false;
      }

      bool guiSetWaylandParent(const char *token) {
         if (!_surface || !_embedding || !token)
            return false;
         // Another token: the previous embedding ends, and the surface is unmapped
         if (_embeddedSurface)
            clap_embedded_surface_v1_destroy(_embeddedSurface);
         _embeddedSurface =
            clap_surface_embedding_v1_get_embedded_surface(_embedding, _surface, token);
         clap_embedded_surface_v1_add_listener(_embeddedSurface, &kEmbeddedSurfaceListener, this);
         if (_isVisible)
            draw();
         flush();
         return true;
      }

      bool guiShow() {
         if (!_embeddedSurface)
            return false;
         if (!_isVisible) {
            _isVisible = true;
            // A pending frame callback draws the next frame anyway
            if (!_frameCallback)
               draw();
            flush();
         }
         return true;
      }

      bool guiHide() {
         if (!_surface)
            return false;
         // No buffer unmaps the surface, and no frame callback stops the animation
         _isVisible = false;
         wl_surface_attach(_surface, nullptr, 0, 0);
         wl_surface_commit(_surface);
         flush();
         return true;
      }

      static const clap_plugin_gui kGui;
      static const clap_plugin_gui_wayland_nested_compositor kGuiWaylandNestedCompositor;
      static const clap_plugin_posix_fd_support kPosixFdSupport;

      // ---- drawing ----

      bool createBuffer(ShmBuffer &b, int32_t width, int32_t height) {
         const int32_t stride = width * 4;
         b.size = size_t(stride) * size_t(height);
         const int fd = memfd_create("wayland-shm-demo", MFD_CLOEXEC);
         if (fd < 0)
            return false;
         if (ftruncate(fd, off_t(b.size)) < 0) {
            close(fd);
            return false;
         }
         void *data = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
         if (data == MAP_FAILED) {
            close(fd);
            return false;
         }
         auto pool = wl_shm_create_pool(_shm, fd, int32_t(b.size));
         b.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
         wl_shm_pool_destroy(pool);
         close(fd);
         wl_buffer_add_listener(b.buffer, &kBufferListener, &b);
         b.pixels = static_cast<uint32_t *>(data);
         b.width = width;
         b.height = height;
         b.isBusy = false;
         return true;
      }

      void destroyBuffer(ShmBuffer &b) {
         if (b.buffer)
            wl_buffer_destroy(b.buffer);
         if (b.pixels)
            munmap(b.pixels, b.size);
         b = {};
      }

      // A free buffer at the current scale: the compositor may still read the other one
      ShmBuffer *nextBuffer() {
         const int32_t width = kWidth * _scale;
         const int32_t height = kHeight * _scale;
         for (auto &b : _buffers) {
            if (b.isBusy)
               continue;
            if (b.buffer && (b.width != width || b.height != height))
               destroyBuffer(b);
            if (!b.buffer && !createBuffer(b, width, height))
               return nullptr;
            return &b;
         }
         return nullptr;
      }

      void draw() {
         if (!_isVisible || !_embeddedSurface)
            return;

         ShmBuffer *b = nextBuffer();
         if (!b) {
            // Both are busy: the next frame callback tries again
            requestFrame();
            wl_surface_commit(_surface);
            return;
         }

         renderPlasma(*b);

         wl_surface_set_buffer_scale(_surface, _scale);
         wl_surface_attach(_surface, b->buffer, 0, 0);
         wl_surface_damage_buffer(_surface, 0, 0, b->width, b->height);
         b->isBusy = true;
         requestFrame();
         wl_surface_commit(_surface);
      }

      void requestFrame() {
         if (_frameCallback)
            return;
         _frameCallback = wl_surface_frame(_surface);
         wl_callback_add_listener(_frameCallback, &kFrameListener, this);
      }

      void onFrameDone(uint32_t timeMs) {
         _frameCallback = nullptr;
         _time = timeMs;
         draw();
      }

      void renderPlasma(ShmBuffer &b) {
         // In logical pixels, so that the picture is the same at every scale
         const uint32_t t = _time / 16;
         const float inverseScale = 1.f / float(_scale);
         const float lensRadius2 = 50.f * 50.f;

         for (int32_t py = 0; py < b.height; ++py) {
            uint32_t *row = b.pixels + size_t(py) * size_t(b.width);
            const float y = float(py) * inverseScale;
            for (int32_t px = 0; px < b.width; ++px) {
               float x = float(px) * inverseScale;
               float yy = y;

               // The lens magnifies around the pointer
               const float dx = x - _pointerX;
               const float dy = y - _pointerY;
               const float d2 = dx * dx + dy * dy;
               const bool isInLens = _isPointerInside && d2 < lensRadius2;
               if (isInLens) {
                  const float k = 0.4f + 0.6f * d2 / lensRadius2;
                  x = _pointerX + dx * k;
                  yy = _pointerY + dy * k;
               }

               const int ix = int(x);
               const int iy = int(yy);
               const unsigned v = kSine[(ix * 2 + t) & 255] + kSine[(iy * 3 - t * 2) & 255] +
                                  kSine[((ix + iy) + t * 3) & 255] +
                                  kSine[(kSine[(iy + t) & 255] / 2 + ix) & 255];
               uint32_t color = palette(uint8_t(v / 4 + t));
               if (isInLens && d2 > lensRadius2 * 0.9f)
                  color = 0xffffff;
               row[px] = color;
            }
         }
      }

      uint32_t palette(uint8_t i) const {
         // Three phase-shifted sines, which clicks rotate
         const uint8_t r = kSine[(i + _paletteShift) & 255];
         const uint8_t g = kSine[(i + 85 + _paletteShift * 2) & 255];
         const uint8_t b = kSine[(i + 170) & 255];
         return (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
      }

      void onPreferredBufferScale(int32_t factor) {
         if (factor < 1 || factor == _scale)
            return;
         _scale = factor;
         if (_isVisible && !_frameCallback)
            draw();
      }

      // ---- input ----

      void onSeatCapabilities(uint32_t capabilities) {
         const bool hasPointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
         if (hasPointer && !_pointer) {
            _pointer = wl_seat_get_pointer(_seat);
            wl_pointer_add_listener(_pointer, &kPointerListener, this);
         } else if (!hasPointer && _pointer) {
            wl_pointer_release(_pointer);
            _pointer = nullptr;
            _isPointerInside = false;
         }
      }

      // ---- extensions ----

      const void *extension(const char *id) {
         if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
            return &kAudioPorts;
         if (!strcmp(id, CLAP_EXT_GUI))
            return &kGui;
         if (!strcmp(id, CLAP_EXT_GUI_WAYLAND_NESTED_COMPOSITOR))
            return &kGuiWaylandNestedCompositor;
         if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
            return &kPosixFdSupport;
         return nullptr;
      }

      clap_plugin _plugin{};
      const clap_host *_host;
      const clap_host_log *_hostLog = nullptr;
      const clap_host_posix_fd_support *_hostPosixFd = nullptr;
      const clap_host_gui_wayland_nested_compositor *_hostCompositor = nullptr;

      double _sampleRate = 48000;
      double _phase = 0;

      wl_display *_display = nullptr;
      int _fd = -1; // registered with the host
      wl_registry *_registry = nullptr;
      wl_compositor *_compositor = nullptr;
      wl_shm *_shm = nullptr;
      wl_seat *_seat = nullptr;
      wl_pointer *_pointer = nullptr;
      clap_surface_embedding_v1 *_embedding = nullptr;
      wl_surface *_surface = nullptr;
      clap_embedded_surface_v1 *_embeddedSurface = nullptr;
      wl_callback *_frameCallback = nullptr;
      std::array<ShmBuffer, 2> _buffers;

      bool _isVisible = false;
      int32_t _scale = 1;
      uint32_t _time = 0; // ms, from the frame callbacks

      bool _isPointerInside = false;
      float _pointerX = 0;
      float _pointerY = 0;
      uint8_t _paletteShift = 0;
   };

   const wl_registry_listener Plugin::kRegistryListener = {
      .global = [](void *data, wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version) { static_cast<Plugin *>(data)->onGlobal(registry, name, interface, version); },
      .global_remove = [](void *, wl_registry *, uint32_t) {},
   };

   const wl_seat_listener Plugin::kSeatListener = {
      .capabilities = [](void *data, wl_seat *,
                         uint32_t capabilities) { static_cast<Plugin *>(data)->onSeatCapabilities(capabilities); },
      .name = [](void *, wl_seat *, const char *) {},
   };

   const wl_pointer_listener Plugin::kPointerListener = {
      .enter =
         [](void *data, wl_pointer *, uint32_t, wl_surface *, wl_fixed_t x, wl_fixed_t y) {
            auto &p = *static_cast<Plugin *>(data);
            p._isPointerInside = true;
            p._pointerX = float(wl_fixed_to_double(x));
            p._pointerY = float(wl_fixed_to_double(y));
         },
      .leave = [](void *data, wl_pointer *, uint32_t, wl_surface *) { static_cast<Plugin *>(data)->_isPointerInside = false; },
      .motion =
         [](void *data, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
            auto &p = *static_cast<Plugin *>(data);
            p._pointerX = float(wl_fixed_to_double(x));
            p._pointerY = float(wl_fixed_to_double(y));
         },
      .button =
         [](void *data, wl_pointer *, uint32_t, uint32_t, uint32_t, uint32_t state) {
            if (state == WL_POINTER_BUTTON_STATE_PRESSED)
               static_cast<Plugin *>(data)->_paletteShift += 37;
         },
      .axis = [](void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {},
      .frame = [](void *, wl_pointer *) {},
      .axis_source = [](void *, wl_pointer *, uint32_t) {},
      .axis_stop = [](void *, wl_pointer *, uint32_t, uint32_t) {},
      .axis_discrete = [](void *, wl_pointer *, uint32_t, int32_t) {},
      .axis_value120 = [](void *, wl_pointer *, uint32_t, int32_t) {},
      .axis_relative_direction = [](void *, wl_pointer *, uint32_t, uint32_t) {},
   };

   const wl_surface_listener Plugin::kSurfaceListener = {
      .enter = [](void *, wl_surface *, wl_output *) {},
      .leave = [](void *, wl_surface *, wl_output *) {},
      .preferred_buffer_scale = [](void *data, wl_surface *,
                                   int32_t factor) { static_cast<Plugin *>(data)->onPreferredBufferScale(factor); },
      .preferred_buffer_transform = [](void *, wl_surface *, uint32_t) {},
   };

   const wl_buffer_listener Plugin::kBufferListener = {
      .release = [](void *data, wl_buffer *) { static_cast<ShmBuffer *>(data)->isBusy = false; },
   };

   const wl_callback_listener Plugin::kFrameListener = {
      .done = [](void *data, wl_callback *callback, uint32_t time) {
         wl_callback_destroy(callback);
         static_cast<Plugin *>(data)->onFrameDone(time);
      },
   };

   const clap_embedded_surface_v1_listener Plugin::kEmbeddedSurfaceListener = {
      .position = [](void *, clap_embedded_surface_v1 *, int32_t, int32_t) {},
      .closed =
         [](void *data, clap_embedded_surface_v1 *) {
            // The host's place is gone: the surface is unmapped until another set_parent
            auto &p = *static_cast<Plugin *>(data);
            p.log(CLAP_LOG_WARNING, "the host closed the embedding");
            p._isVisible = false;
         },
   };

   const clap_plugin_audio_ports Plugin::kAudioPorts = {
      .count = [](const clap_plugin *, bool isInput) -> uint32_t { return isInput ? 0 : 1; },
      .get =
         [](const clap_plugin *, uint32_t index, bool isInput, clap_audio_port_info *info) {
            if (isInput || index != 0)
               return false;
            info->id = 0;
            snprintf(info->name, sizeof(info->name), "Output");
            info->flags = CLAP_AUDIO_PORT_IS_MAIN;
            info->channel_count = 2;
            info->port_type = CLAP_PORT_STEREO;
            info->in_place_pair = CLAP_INVALID_ID;
            return true;
         },
   };

   const clap_plugin_gui Plugin::kGui = {
      .is_api_supported = [](const clap_plugin *p, const char *api,
                             bool isFloating) { return self(p).isApiSupported(api, isFloating); },
      .get_preferred_api =
         [](const clap_plugin *p, const char **api, bool *isFloating) {
            if (!self(p).canEmbed())
               return false;
            *api = CLAP_WINDOW_API_WAYLAND_SURFACE_EMBEDDING_V1;
            *isFloating = false;
            return true;
         },
      .create = [](const clap_plugin *p, const char *api,
                   bool isFloating) { return self(p).guiCreate(api, isFloating); },
      .destroy = [](const clap_plugin *p) { self(p).guiDestroy(); },
      // The compositor tells the surface its scale
      .set_scale = [](const clap_plugin *, double) { return false; },
      .get_size =
         [](const clap_plugin *, uint32_t *width, uint32_t *height) {
            *width = kWidth;
            *height = kHeight;
            return true;
         },
      .can_resize = [](const clap_plugin *) { return false; },
      .get_resize_hints = [](const clap_plugin *, clap_gui_resize_hints *) { return false; },
      .adjust_size = [](const clap_plugin *, uint32_t *, uint32_t *) { return false; },
      .set_size = [](const clap_plugin *, uint32_t width,
                     uint32_t height) { return width == uint32_t(kWidth) && height == uint32_t(kHeight); },
      // Embedding goes through clap.gui-wayland-nested-compositor instead
      .set_parent = [](const clap_plugin *, const clap_window *) { return false; },
      .set_transient = [](const clap_plugin *, const clap_window *) { return false; },
      .suggest_title = [](const clap_plugin *, const char *) {},
      .show = [](const clap_plugin *p) { return self(p).guiShow(); },
      .hide = [](const clap_plugin *p) { return self(p).guiHide(); },
   };

   const clap_plugin_gui_wayland_nested_compositor Plugin::kGuiWaylandNestedCompositor = {
      .set_parent = [](const clap_plugin *p,
                       const char *token) { return self(p).guiSetWaylandParent(token); },
   };

   const clap_plugin_posix_fd_support Plugin::kPosixFdSupport = {
      .on_fd = [](const clap_plugin *p, int fd, clap_posix_fd_flags_t flags) { self(p).onFd(fd, flags); },
   };

   const clap_plugin_factory kFactory = {
      .get_plugin_count = [](const clap_plugin_factory *) -> uint32_t { return 1; },
      .get_plugin_descriptor = [](const clap_plugin_factory *,
                                  uint32_t index) { return index == 0 ? &kDescriptor : nullptr; },
      .create_plugin =
         [](const clap_plugin_factory *, const clap_host *host,
            const char *id) -> const clap_plugin * {
            if (strcmp(id, kDescriptor.id))
               return nullptr;
            return (new Plugin(host))->clapPlugin();
         },
   };

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry clap_entry = {
   .clap_version = CLAP_VERSION_INIT,
   .init = [](const char *) { return true; },
   .deinit = [] {},
   .get_factory = [](const char *id) -> const void * {
      return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr;
   },
};
