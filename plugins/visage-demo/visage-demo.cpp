// A CLAP whose GUI is drawn by Visage (https://github.com/VitalAudio/visage), on the GPU with
// bgfx and Vulkan, embedded in the host's window with clap.gui-wayland-nested-compositor and
// clap_surface_embedding_v1.
//
// - Visage needs its Wayland backend (VISAGE_WAYLAND), see createWaylandEmbeddedWindow().
// - Visage's renderer is global to the process and draws on one wl_display, so all the instances
//   share one connection to the host's compositor. It stays open until the plugin is unloaded,
//   since the renderer keeps using it.
// - Each window gives a pollable fd (its connection's and its frame timer's), which its instance
//   registers with the host's posix-fd-support.
//
// The audio is a quiet 330 Hz sine.

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include <wayland-client.h>

#include <clap/clap.h>
#include <clap/ext/draft/wayland-experiment.h>

#include <visage/app.h>

namespace {

   constexpr int kWidth = 480;
   constexpr int kHeight = 320;
   constexpr double kFrequency = 330;
   constexpr float kLevel = 0.1f;

   const char *const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                    CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};

   const clap_plugin_descriptor kDescriptor = {
      .clap_version = CLAP_VERSION_INIT,
      .id = "org.example.visage-demo",
      .name = "Visage Wayland Demo",
      .vendor = "clap-wayland-host",
      .url = "",
      .manual_url = "",
      .support_url = "",
      .version = "0.1.0",
      .description = "Visage GUI embedded with the Wayland nested compositor API",
      .features = kFeatures,
   };

   // The connection which all the instances use, until the plugin is unloaded
   struct SharedConnection {
      // Of the host which gave it: another host could run another compositor
      int(CLAP_ABI *openConnection)(const clap_host *) = nullptr;
      wl_display *display = nullptr;
   } gConnection;

   struct Ripple {
      visage::Point position;
      double startTime;
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
            fprintf(stderr, "visage-demo: %s\n", message);
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

      // ---- GUI ----

      bool canEmbed() const { return _hostCompositor && _hostPosixFd; }

      bool isApiSupported(const char *api, bool isFloating) const {
         return !isFloating && !strcmp(api, CLAP_WINDOW_API_WAYLAND_SURFACE_EMBEDDING_V1) &&
                canEmbed();
      }

      // The first instance connects, the others use the same connection
      bool connect() {
         if (gConnection.display)
            return gConnection.openConnection == _hostCompositor->open_connection;

         // wl_display_connect_to_fd() takes the fd, even on failure
         const int fd = _hostCompositor->open_connection(_host);
         if (fd < 0)
            return false;
         gConnection.display = wl_display_connect_to_fd(fd);
         if (!gConnection.display)
            return false;
         gConnection.openConnection = _hostCompositor->open_connection;
         return true;
      }

      bool guiCreate(const char *api, bool isFloating) {
         if (!isApiSupported(api, isFloating) || _app)
            return false;
         if (!connect()) {
            log(CLAP_LOG_ERROR, "can't connect to the host's compositor, or Visage already uses "
                                "another host's compositor");
            return false;
         }

         _app = std::make_unique<visage::ApplicationWindow>();
         _app->onDraw() = [this](visage::Canvas &canvas) { draw(canvas); };
         _app->onMouseMove() = [this](const visage::MouseEvent &e) { _mouse = e.position; };
         _app->onMouseDrag() = [this](const visage::MouseEvent &e) { _mouse = e.position; };
         _app->onMouseExit() = [this](const visage::MouseEvent &) { _mouse = {-1, -1}; };
         _app->onMouseDown() = [this](const visage::MouseEvent &e) {
            ++_clicks;
            _ripples.push_back({e.position, _time});
         };
         return true;
      }

      void guiDestroy() {
         unregisterFd();
         // The window goes after the editor has left it
         _app.reset();
         _ripples.clear();
      }

      void unregisterFd() {
         if (_fd >= 0) {
            _hostPosixFd->unregister_fd(_host, _fd);
            _fd = -1;
         }
      }

      bool guiSetWaylandParent(const char *token) {
         if (!_app || !token)
            return false;

         auto window = visage::createWaylandEmbeddedWindow(kWidth, kHeight, gConnection.display, token);
         if (!window) {
            log(CLAP_LOG_ERROR, "the host's compositor refused the embedding");
            return false;
         }
         unregisterFd();
         _app->show(std::move(window));

         _fd = _app->window()->posixFd();
         if (!_hostPosixFd->register_fd(_host, _fd, CLAP_POSIX_FD_READ)) {
            log(CLAP_LOG_ERROR, "the host didn't take our fd");
            _fd = -1;
            return false;
         }
         return true;
      }

      bool guiShow() {
         if (!_app || !_app->window())
            return false;
         _app->window()->show();
         return true;
      }

      bool guiHide() {
         if (_app && _app->window())
            _app->window()->hide();
         return true;
      }

      void onFd(int fd) {
         if (_app && _app->window() && fd == _fd)
            _app->window()->processPluginFdEvents();
      }

      static const clap_plugin_gui kGui;
      static const clap_plugin_gui_wayland_nested_compositor kGuiWaylandNestedCompositor;
      static const clap_plugin_posix_fd_support kPosixFdSupport;

      // ---- drawing ----

      static visage::Color hue(float h, float alpha = 1.0f, float brightness = 1.0f) {
         auto channel = [h, brightness](float offset) {
            return brightness * (0.5f + 0.5f * std::cos(2.0f * float(M_PI) * (h + offset)));
         };
         return visage::Color(alpha, channel(0.0f), channel(2.0f / 3.0f), channel(1.0f / 3.0f));
      }

      void draw(visage::Canvas &canvas) {
         _time = canvas.time();
         const float w = _app->width();
         const float h = _app->height();
         const float t = float(_time);

         // A slowly turning gradient behind everything
         canvas.setColor(visage::Brush::linear(hue(t * 0.03f, 1.0f, 0.25f),
                                               hue(t * 0.03f + 0.5f, 1.0f, 0.12f), {0, 0}, {w, h}));
         canvas.fill(0, 0, w, h);

         // Rings breathing in the middle
         for (int i = 0; i < 6; ++i) {
            const float radius = 30.0f + i * 22.0f + 8.0f * std::sin(t * 1.3f + i * 0.7f);
            canvas.setColor(hue(t * 0.05f + i * 0.12f, 0.6f));
            canvas.ring(w * 0.5f - radius, h * 0.5f - radius, 2.0f * radius, 3.0f);
         }

         // Orbiting balls
         for (int ball = 0; ball < 7; ++ball) {
            const float a = t * (0.6f + ball * 0.15f) + ball * 0.9f;
            const float x = w * 0.5f + std::cos(a) * (w * 0.38f - ball * 10.0f);
            const float y = h * 0.5f + std::sin(a * 1.4f) * (h * 0.32f - ball * 6.0f);
            const float r = 10.0f - ball * 0.8f;
            canvas.setColor(hue(ball * 0.14f + t * 0.1f));
            canvas.circle(x - r, y - r, 2.0f * r);
         }

         // Ripples where the pointer clicked
         std::erase_if(_ripples, [this](const Ripple &r) { return _time - r.startTime > 1.5; });
         for (const auto &r : _ripples) {
            const float age = float(_time - r.startTime) / 1.5f;
            const float radius = 6.0f + age * 90.0f;
            canvas.setColor(hue(age + _clicks * 0.13f, 1.0f - age));
            canvas.ring(r.position.x - radius, r.position.y - radius, 2.0f * radius, 3.0f);
         }

         // A dot per click, along the bottom
         for (int i = 0; i < std::min(_clicks, 40); ++i) {
            canvas.setColor(hue(i * 0.13f));
            canvas.circle(10.0f + i * 11.0f, h - 18.0f, 8.0f);
         }

         // The pointer
         if (_mouse.x >= 0) {
            canvas.setColor(0xccffffff);
            canvas.ring(_mouse.x - 12.0f, _mouse.y - 12.0f, 24.0f, 2.0f);
         }

         _app->redraw(); // animated: always the next frame
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

      std::unique_ptr<visage::ApplicationWindow> _app;
      int _fd = -1;

      double _time = 0;
      visage::Point _mouse = {-1, -1};
      int _clicks = 0;
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

   const clap_plugin_posix_fd_support Plugin::kPosixFdSupport = {
      .on_fd = [](const clap_plugin *p, int fd, clap_posix_fd_flags_t) { self(p).onFd(fd); },
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
   .deinit =
      [] {
         if (gConnection.display) {
            wl_display_disconnect(gConnection.display);
            gConnection = {};
         }
      },
   .get_factory = [](const char *id) -> const void * {
      return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr;
   },
};
