// A CLAP whose GUI is drawn by SDL3, embedded in the host's window with
// clap.gui-wayland-nested-compositor and clap_surface_embedding_v1.
//
// - The host gives us a connection to its nested compositor (open_connection).
// - SDL uses that connection instead of opening its own
//   (SDL_PROP_GLOBAL_VIDEO_WAYLAND_WL_DISPLAY_POINTER).
// - SDL creates the window's wl_surface without a role (WAYLAND_SURFACE_ROLE_CUSTOM), and we give
//   it the embedded role with the host's token.
// - A host timer pumps SDL's events and draws a frame.
//
// SDL's video subsystem is global, so only one instance at a time can show its GUI.
//
// The audio is a quiet 220 Hz sine.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <wayland-client.h>

#include <clap/clap.h>
#include <clap/ext/draft/wayland-experiment.h>

#include "xdg-shell-client-protocol.h"
// After xdg-shell's
#include "clap-surface-embedding-v1-client-protocol.h"

namespace {

   constexpr uint32_t kWidth = 480;
   constexpr uint32_t kHeight = 320;
   constexpr uint32_t kFramePeriodMs = 16;
   constexpr double kFrequency = 220;
   constexpr float kLevel = 0.1f;

   const char *const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                    CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};

   const clap_plugin_descriptor kDescriptor = {
      .clap_version = CLAP_VERSION_INIT,
      .id = "org.example.sdl3-demo",
      .name = "SDL3 Wayland Demo",
      .vendor = "clap-wayland-host",
      .url = "",
      .manual_url = "",
      .support_url = "",
      .version = "0.1.0",
      .description = "Animated SDL3 GUI embedded with the Wayland nested compositor API",
      .features = kFeatures,
   };

   // Whether an instance has initialized SDL's video subsystem
   bool gIsSdlInUse = false;

   struct Ripple {
      float x, y;
      double startTime;
      SDL_FColor color;
   };

   class Plugin {
   public:
      explicit Plugin(const clap_host *host) : _host(host) {
         _plugin.desc = &kDescriptor;
         _plugin.plugin_data = this;
         _plugin.init = [](const clap_plugin *p) { return self(p).init(); };
         _plugin.destroy = [](const clap_plugin *p) { delete &self(p); };
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
            fprintf(stderr, "sdl3-demo: %s\n", message);
      }

      bool init() {
         // Queried here since some hosts only answer during init()
         _hostLog = static_cast<const clap_host_log *>(_host->get_extension(_host, CLAP_EXT_LOG));
         _hostTimer = static_cast<const clap_host_timer_support *>(
            _host->get_extension(_host, CLAP_EXT_TIMER_SUPPORT));
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

      // ---- GUI ----

      bool canEmbed() const { return _hostCompositor && _hostTimer; }

      bool isApiSupported(const char *api, bool isFloating) const {
         return !isFloating && !strcmp(api, CLAP_WINDOW_API_WAYLAND_SURFACE_EMBEDDING_V1) &&
                canEmbed();
      }

      bool guiCreate(const char *api, bool isFloating) {
         if (!isApiSupported(api, isFloating) || _window)
            return false;
         if (gIsSdlInUse) {
            log(CLAP_LOG_ERROR, "SDL is already used by another instance");
            return false;
         }

         // wl_display_connect_to_fd() takes the fd, even on failure
         const int fd = _hostCompositor->open_connection(_host);
         if (fd < 0)
            return false;
         _display = wl_display_connect_to_fd(fd);
         if (!_display) {
            log(CLAP_LOG_ERROR, "can't connect to the host's compositor");
            return false;
         }

         // Our own registry for clap_surface_embedding_v1, SDL makes its own for everything else
         static const wl_registry_listener registryListener = {
            .global =
               [](void *data, wl_registry *registry, uint32_t name, const char *interface,
                  uint32_t version) {
                  auto &plugin = *static_cast<Plugin *>(data);
                  if (!strcmp(interface, clap_surface_embedding_v1_interface.name))
                     plugin._embedding = static_cast<clap_surface_embedding_v1 *>(
                        wl_registry_bind(registry, name, &clap_surface_embedding_v1_interface, 1));
               },
            .global_remove = [](void *, wl_registry *, uint32_t) {},
         };
         _registry = wl_display_get_registry(_display);
         wl_registry_add_listener(_registry, &registryListener, this);
         if (wl_display_roundtrip(_display) < 0 || !_embedding) {
            log(CLAP_LOG_ERROR, "the host's compositor has no clap_surface_embedding_v1");
            destroyConnection();
            return false;
         }

         SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland");
         // The host owns the process
         SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
         SDL_SetPointerProperty(SDL_GetGlobalProperties(),
                                SDL_PROP_GLOBAL_VIDEO_WAYLAND_WL_DISPLAY_POINTER, _display);
         if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            log(CLAP_LOG_ERROR, "SDL_InitSubSystem: %s", SDL_GetError());
            SDL_SetPointerProperty(SDL_GetGlobalProperties(),
                                   SDL_PROP_GLOBAL_VIDEO_WAYLAND_WL_DISPLAY_POINTER, nullptr);
            destroyConnection();
            return false;
         }
         gIsSdlInUse = true;

         const SDL_PropertiesID props = SDL_CreateProperties();
         SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, kDescriptor.name);
         SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, kWidth);
         SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, kHeight);
         SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, true);
         SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
         SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN, true);
         SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_WAYLAND_SURFACE_ROLE_CUSTOM_BOOLEAN,
                                true);
         SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_WAYLAND_CREATE_EGL_WINDOW_BOOLEAN,
                                true);
         _window = SDL_CreateWindowWithProperties(props);
         SDL_DestroyProperties(props);
         if (!_window) {
            log(CLAP_LOG_ERROR, "SDL_CreateWindow: %s", SDL_GetError());
            guiDestroy();
            return false;
         }

         _renderer = SDL_CreateRenderer(_window, nullptr);
         if (!_renderer) {
            log(CLAP_LOG_ERROR, "SDL_CreateRenderer: %s", SDL_GetError());
            guiDestroy();
            return false;
         }
         // Never wait for the compositor on the main thread, the timer paces the frames
         SDL_SetRenderVSync(_renderer, SDL_RENDERER_VSYNC_DISABLED);
         SDL_SetRenderLogicalPresentation(_renderer, kWidth, kHeight,
                                          SDL_LOGICAL_PRESENTATION_STRETCH);
         SDL_SetRenderDrawBlendMode(_renderer, SDL_BLENDMODE_BLEND);

         int major = SDL_VERSIONNUM_MAJOR(SDL_GetVersion());
         int minor = SDL_VERSIONNUM_MINOR(SDL_GetVersion());
         int micro = SDL_VERSIONNUM_MICRO(SDL_GetVersion());
         snprintf(_info, sizeof(_info), "SDL %d.%d.%d  %s  renderer: %s", major, minor, micro,
                  SDL_GetCurrentVideoDriver(), SDL_GetRendererName(_renderer));
         log(CLAP_LOG_INFO, "%s", _info);

         _startTime = now();
         return true;
      }

      void guiDestroy() {
         guiHide();

         // The role goes before the surface
         if (_embeddedSurface) {
            clap_embedded_surface_v1_destroy(_embeddedSurface);
            _embeddedSurface = nullptr;
         }
         if (_renderer) {
            SDL_DestroyRenderer(_renderer);
            _renderer = nullptr;
         }
         if (_window) {
            SDL_DestroyWindow(_window);
            _window = nullptr;
         }
         if (gIsSdlInUse && _display) {
            // SDL doesn't disconnect a display it was given
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            SDL_SetPointerProperty(SDL_GetGlobalProperties(),
                                   SDL_PROP_GLOBAL_VIDEO_WAYLAND_WL_DISPLAY_POINTER, nullptr);
            gIsSdlInUse = false;
         }
         destroyConnection();
         _ripples.clear();
      }

      void destroyConnection() {
         if (_embedding) {
            clap_surface_embedding_v1_destroy(_embedding);
            _embedding = nullptr;
         }
         if (_registry) {
            wl_registry_destroy(_registry);
            _registry = nullptr;
         }
         if (_display) {
            wl_display_disconnect(_display);
            _display = nullptr;
         }
      }

      bool guiSetWaylandParent(const char *token) {
         if (!_window || !_embedding || !token)
            return false;

         auto *surface = static_cast<wl_surface *>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(_window), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr));
         if (!surface) {
            log(CLAP_LOG_ERROR, "SDL's window has no wl_surface");
            return false;
         }

         static const clap_embedded_surface_v1_listener embeddedListener = {
            .position = [](void *, clap_embedded_surface_v1 *, int32_t, int32_t) {},
            .closed =
               [](void *data, clap_embedded_surface_v1 *) {
                  auto &plugin = *static_cast<Plugin *>(data);
                  plugin.log(CLAP_LOG_WARNING, "the host closed the embedding");
                  plugin.guiHide();
               },
         };
         if (_embeddedSurface)
            clap_embedded_surface_v1_destroy(_embeddedSurface);
         _embeddedSurface =
            clap_surface_embedding_v1_get_embedded_surface(_embedding, surface, token);
         clap_embedded_surface_v1_add_listener(_embeddedSurface, &embeddedListener, this);
         wl_display_flush(_display);
         return true;
      }

      bool guiShow() {
         if (!_window || !_embeddedSurface)
            return false;
         if (_timer == CLAP_INVALID_ID && !_hostTimer->register_timer(_host, kFramePeriodMs, &_timer))
            return false;
         SDL_ShowWindow(_window);
         onTimer();
         return true;
      }

      bool guiHide() {
         if (_timer != CLAP_INVALID_ID) {
            _hostTimer->unregister_timer(_host, _timer);
            _timer = CLAP_INVALID_ID;
         }
         if (_window)
            SDL_HideWindow(_window);
         return true;
      }

      static const clap_plugin_gui kGui;
      static const clap_plugin_gui_wayland_nested_compositor kGuiWaylandNestedCompositor;
      static const clap_plugin_timer_support kTimerSupport;

      // ---- animation ----

      static double now() { return double(SDL_GetTicksNS()) * 1e-9; }

      void onTimer() {
         if (!_window)
            return;

         SDL_PumpEvents();
         SDL_Event event;
         while (SDL_PollEvent(&event)) {
            SDL_ConvertEventToRenderCoordinates(_renderer, &event);
            switch (event.type) {
            case SDL_EVENT_MOUSE_MOTION:
               _mouseX = event.motion.x;
               _mouseY = event.motion.y;
               break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
               _mouseX = _mouseY = -1;
               break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
               ++_clicks;
               _ripples.push_back({event.button.x, event.button.y, now(),
                                   hue(float(_clicks) * 0.13f)});
               break;
            case SDL_EVENT_KEY_DOWN:
               _lastKey = SDL_GetKeyName(event.key.key);
               break;
            default:
               break;
            }
         }

         draw(now() - _startTime);
      }

      static SDL_FColor hue(float h, float alpha = 1) {
         auto channel = [h](float offset) {
            return 0.5f + 0.5f * std::cos(2 * float(M_PI) * (h + offset));
         };
         return {channel(0), channel(2.f / 3), channel(1.f / 3), alpha};
      }

      void fillCircle(float cx, float cy, float radius, SDL_FColor color) {
         constexpr int kSegments = 40;
         std::vector<SDL_Vertex> vertices;
         std::vector<int> indices;
         vertices.push_back({{cx, cy}, color, {}});
         for (int i = 0; i <= kSegments; ++i) {
            const float a = 2 * float(M_PI) * float(i) / kSegments;
            vertices.push_back({{cx + radius * std::cos(a), cy + radius * std::sin(a)}, color, {}});
            if (i > 0)
               indices.insert(indices.end(), {0, i, i + 1});
         }
         SDL_RenderGeometry(_renderer, nullptr, vertices.data(), int(vertices.size()),
                            indices.data(), int(indices.size()));
      }

      void strokeCircle(float cx, float cy, float radius) {
         constexpr int kSegments = 64;
         SDL_FPoint points[kSegments + 1];
         for (int i = 0; i <= kSegments; ++i) {
            const float a = 2 * float(M_PI) * float(i) / kSegments;
            points[i] = {cx + radius * std::cos(a), cy + radius * std::sin(a)};
         }
         SDL_RenderLines(_renderer, points, kSegments + 1);
      }

      void draw(double t) {
         const float w = kWidth, h = kHeight;

         // Background: slowly drifting vertical gradient
         for (int y = 0; y < int(h); y += 4) {
            const float k = float(y) / h;
            const float pulse = 0.5f + 0.5f * float(std::sin(t * 0.4));
            SDL_SetRenderDrawColorFloat(_renderer, 0.05f + 0.05f * pulse * k, 0.06f + 0.08f * k,
                                        0.12f + 0.10f * (1 - k), 1);
            SDL_FRect band = {0, float(y), w, 4};
            SDL_RenderFillRect(_renderer, &band);
         }

         // Grid
         SDL_SetRenderDrawColorFloat(_renderer, 1, 1, 1, 0.05f);
         for (float x = 0; x < w; x += 32)
            SDL_RenderLine(_renderer, x, 0, x, h);
         for (float y = 0; y < h; y += 32)
            SDL_RenderLine(_renderer, 0, y, w, y);

         // A few scrolling sines, like an oscilloscope
         for (int wave = 0; wave < 3; ++wave) {
            constexpr int kPoints = 240;
            SDL_FPoint points[kPoints];
            const double speed = 1.5 + wave * 0.7;
            const double cycles = 2 + wave;
            const float amplitude = 40.f - float(wave) * 10.f;
            for (int i = 0; i < kPoints; ++i) {
               const float x = w * float(i) / (kPoints - 1);
               const double phase = 2 * M_PI * (cycles * x / w - speed * t * 0.25);
               points[i] = {x, h * 0.62f + amplitude * float(std::sin(phase)) *
                                              float(std::sin(t * 0.7 + wave))};
            }
            const SDL_FColor c = hue(float(t * 0.05) + float(wave) * 0.25f, 0.8f);
            SDL_SetRenderDrawColorFloat(_renderer, c.r, c.g, c.b, c.a);
            SDL_RenderLines(_renderer, points, kPoints);
         }

         // Orbiting balls with trails
         for (int ball = 0; ball < 5; ++ball) {
            for (int trail = 12; trail >= 0; --trail) {
               const double tt = t - trail * 0.03;
               const double a = tt * (0.8 + ball * 0.25) + ball * 1.3;
               const float x = w * 0.5f + float(std::cos(a) * (150 - ball * 18));
               const float y = h * 0.4f + float(std::sin(a * 1.6) * (70 - ball * 6));
               SDL_FColor c = hue(float(ball) * 0.2f + float(t * 0.1),
                                  trail == 0 ? 1.f : 0.35f * (1 - float(trail) / 13));
               fillCircle(x, y, (trail == 0 ? 9.f : 7.f) - float(ball) * 0.8f, c);
            }
         }

         // Ripples where the pointer clicked
         const double time = now();
         std::erase_if(_ripples, [time](const Ripple &r) { return time - r.startTime > 1.5; });
         for (const auto &r : _ripples) {
            const float age = float(time - r.startTime) / 1.5f;
            SDL_SetRenderDrawColorFloat(_renderer, r.color.r, r.color.g, r.color.b, 1 - age);
            strokeCircle(r.x, r.y, 6 + age * 80);
            strokeCircle(r.x, r.y, 4 + age * 60);
         }

         // Pointer crosshair
         if (_mouseX >= 0) {
            SDL_SetRenderDrawColorFloat(_renderer, 1, 1, 1, 0.6f);
            SDL_RenderLine(_renderer, _mouseX - 10, _mouseY, _mouseX + 10, _mouseY);
            SDL_RenderLine(_renderer, _mouseX, _mouseY - 10, _mouseX, _mouseY + 10);
         }

         // Text, with SDL's built-in 8x8 font
         ++_frames;
         if (time - _fpsTime >= 1) {
            _fps = double(_frames) / (time - _fpsTime);
            _frames = 0;
            _fpsTime = time;
         }
         SDL_SetRenderDrawColorFloat(_renderer, 0.1f, 0.1f, 0.15f, 0.7f);
         SDL_FRect panel = {0, 0, w, 44};
         SDL_RenderFillRect(_renderer, &panel);
         SDL_SetRenderDrawColor(_renderer, 0xbf, 0xe6, 0xdd, 0xff);
         SDL_RenderDebugText(_renderer, 10, 8, "SDL3 in a CLAP, embedded over Wayland");
         SDL_SetRenderDrawColor(_renderer, 0x8a, 0xa8, 0xb0, 0xff);
         SDL_RenderDebugText(_renderer, 10, 26, _info);
         char status[160];
         snprintf(status, sizeof(status), "%.0f fps  clicks: %d  last key: %s", _fps, _clicks,
                  _lastKey.c_str());
         SDL_RenderDebugText(_renderer, 10, h - 16, status);

         SDL_RenderPresent(_renderer);
      }

      // ---- extensions ----

      const void *extension(const char *id) {
         if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
            return &kAudioPorts;
         if (!strcmp(id, CLAP_EXT_GUI))
            return &kGui;
         if (!strcmp(id, CLAP_EXT_GUI_WAYLAND_NESTED_COMPOSITOR))
            return &kGuiWaylandNestedCompositor;
         if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT))
            return &kTimerSupport;
         return nullptr;
      }

      clap_plugin _plugin{};
      const clap_host *_host;
      const clap_host_log *_hostLog = nullptr;
      const clap_host_timer_support *_hostTimer = nullptr;
      const clap_host_gui_wayland_nested_compositor *_hostCompositor = nullptr;

      double _sampleRate = 48000;
      double _phase = 0;

      wl_display *_display = nullptr;
      wl_registry *_registry = nullptr;
      clap_surface_embedding_v1 *_embedding = nullptr;
      clap_embedded_surface_v1 *_embeddedSurface = nullptr;
      SDL_Window *_window = nullptr;
      SDL_Renderer *_renderer = nullptr;
      clap_id _timer = CLAP_INVALID_ID;

      char _info[160] = {};
      double _startTime = 0;
      double _fpsTime = 0;
      int _frames = 0;
      double _fps = 0;
      float _mouseX = -1;
      float _mouseY = -1;
      int _clicks = 0;
      std::string _lastKey = "-";
      std::vector<Ripple> _ripples;
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
                     uint32_t height) { return width == kWidth && height == kHeight; },
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

   const clap_plugin_timer_support Plugin::kTimerSupport = {
      .on_timer =
         [](const clap_plugin *p, clap_id id) {
            if (id == self(p)._timer)
               self(p).onTimer();
         },
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
