// A CLAP whose GUI is drawn by Pugl (https://github.com/lv2/pugl), with OpenGL on EGL, embedded
// in the host's window with clap.gui-wayland-nested-compositor and clap_surface_embedding_v1.
//
// - Pugl needs its Wayland backend with the embedding: puglNewWaylandWorld() and
//   puglSetWaylandEmbeddingToken(), see include/pugl/wayland.h there.
// - All the instances share one Pugl world on one connection to the host's compositor, which
//   goes with the last GUI. The views draw on their surfaces' frame callbacks.
// - Each instance registers the connection's fd with its own host's posix-fd-support, and any of
//   them updates the whole world.
//
// The audio is a quiet 220 Hz sine.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include <wayland-client.h>

#include <clap/clap.h>
#include <clap/ext/draft/wayland-experiment.h>

#include <pugl/gl.h>
#include <pugl/pugl.h>
#include <pugl/wayland.h>

namespace {

   constexpr int kWidth = 480;
   constexpr int kHeight = 320;
   constexpr double kFrequency = 220;
   constexpr float kLevel = 0.1f;

   const char *const kFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT,
                                    CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};

   const clap_plugin_descriptor kDescriptor = {
      .clap_version = CLAP_VERSION_INIT,
      .id = "org.example.pugl-demo",
      .name = "Pugl Wayland Demo",
      .vendor = "clap-wayland-host",
      .url = "",
      .manual_url = "",
      .support_url = "",
      .version = "0.1.0",
      .description = "Pugl GUI embedded with the Wayland nested compositor API",
      .features = kFeatures,
   };

   // The world which all the GUIs use, until the last one goes
   struct SharedWorld {
      // Of the host which gave it: another host could run another compositor
      int(CLAP_ABI *openConnection)(const clap_host *) = nullptr;
      wl_display *display = nullptr;
      PuglWorld *world = nullptr;
      int users = 0;
   } gWorld;

   struct Ripple {
      double x, y;
      double startTime;
   };

   struct Color {
      float r, g, b, a;
   };

   Color hue(double h, float alpha = 1.0f, float brightness = 1.0f) {
      auto channel = [h, brightness](double offset) {
         return brightness * float(0.5 + 0.5 * std::cos(2 * M_PI * (h + offset)));
      };
      return {channel(0.0), channel(2.0 / 3.0), channel(1.0 / 3.0), alpha};
   }

   void setColor(const Color &c) { glColor4f(c.r, c.g, c.b, c.a); }

   void disc(double x, double y, double r) {
      glBegin(GL_TRIANGLE_FAN);
      glVertex2d(x, y);
      for (int i = 0; i <= 32; ++i) {
         const double a = 2 * M_PI * i / 32;
         glVertex2d(x + r * std::cos(a), y + r * std::sin(a));
      }
      glEnd();
   }

   void ring(double x, double y, double r, double thickness) {
      glBegin(GL_TRIANGLE_STRIP);
      for (int i = 0; i <= 64; ++i) {
         const double a = 2 * M_PI * i / 64;
         const double c = std::cos(a), s = std::sin(a);
         glVertex2d(x + (r - thickness / 2) * c, y + (r - thickness / 2) * s);
         glVertex2d(x + (r + thickness / 2) * c, y + (r + thickness / 2) * s);
      }
      glEnd();
   }

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
            fprintf(stderr, "pugl-demo: %s\n", message);
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

      // The first GUI connects and makes the world, the others use them
      bool acquireWorld() {
         if (gWorld.world) {
            if (gWorld.openConnection != _hostCompositor->open_connection)
               return false;
            ++gWorld.users;
            return true;
         }

         // wl_display_connect_to_fd() takes the fd, even on failure
         const int fd = _hostCompositor->open_connection(_host);
         if (fd < 0)
            return false;
         gWorld.display = wl_display_connect_to_fd(fd);
         if (!gWorld.display)
            return false;
         gWorld.world = puglNewWaylandWorld(PUGL_MODULE, 0, gWorld.display);
         if (!gWorld.world) {
            wl_display_disconnect(gWorld.display);
            gWorld = {};
            return false;
         }
         puglSetWorldString(gWorld.world, PUGL_CLASS_NAME, "PuglWaylandDemo");
         gWorld.openConnection = _hostCompositor->open_connection;
         gWorld.users = 1;
         return true;
      }

      static void releaseWorld() {
         if (--gWorld.users > 0)
            return;
         puglFreeWorld(gWorld.world);
         wl_display_disconnect(gWorld.display);
         gWorld = {};
      }

      bool guiCreate(const char *api, bool isFloating) {
         if (!isApiSupported(api, isFloating) || _view)
            return false;
         if (!acquireWorld()) {
            log(CLAP_LOG_ERROR, "can't connect to the host's compositor, or the plugin already "
                                "uses another host's compositor");
            return false;
         }

         _view = puglNewView(gWorld.world);
         puglSetHandle(_view, this);
         puglSetEventFunc(_view, [](PuglView *view, const PuglEvent *event) {
            return static_cast<Plugin *>(puglGetHandle(view))->onEvent(*event);
         });
         puglSetBackend(_view, puglGlBackend());
         puglSetViewHint(_view, PUGL_CONTEXT_API, PUGL_OPENGL_API);
         puglSetViewHint(_view, PUGL_CONTEXT_VERSION_MAJOR, 2);
         puglSetViewHint(_view, PUGL_CONTEXT_VERSION_MINOR, 1);
         puglSetViewHint(_view, PUGL_CONTEXT_PROFILE, PUGL_OPENGL_COMPATIBILITY_PROFILE);
         puglSetViewHint(_view, PUGL_CONTINUOUS, PUGL_TRUE); // animated
         puglSetSizeHint(_view, PUGL_DEFAULT_SIZE, kWidth, kHeight);
         return true;
      }

      void guiDestroy() {
         if (!_view)
            return;
         unregisterFd();
         puglFreeView(_view);
         _view = nullptr;
         _ripples.clear();
         releaseWorld();
      }

      void unregisterFd() {
         if (_fd >= 0) {
            _hostPosixFd->unregister_fd(_host, _fd);
            _fd = -1;
         }
      }

      // The view is realized here, since its embedding needs the token
      bool guiSetWaylandParent(const char *token) {
         if (!_view || !token)
            return false;

         PuglStatus st = puglSetWaylandEmbeddingToken(_view, token);
         if (!st)
            st = puglRealize(_view);
         if (st) {
            log(CLAP_LOG_ERROR, "can't embed the view: %s", puglStrerror(st));
            return false;
         }

         _fd = wl_display_get_fd(gWorld.display);
         if (!_hostPosixFd->register_fd(_host, _fd, CLAP_POSIX_FD_READ)) {
            log(CLAP_LOG_ERROR, "the host didn't take our fd");
            _fd = -1;
            return false;
         }
         return true;
      }

      bool guiShow() { return _view && puglShow(_view, PUGL_SHOW_PASSIVE) == PUGL_SUCCESS; }

      bool guiHide() {
         if (_view)
            puglHide(_view);
         return true;
      }

      void onFd(int fd) {
         if (gWorld.world && fd == _fd)
            puglUpdate(gWorld.world, 0.0);
      }

      static const clap_plugin_gui kGui;
      static const clap_plugin_gui_wayland_nested_compositor kGuiWaylandNestedCompositor;
      static const clap_plugin_posix_fd_support kPosixFdSupport;

      // ---- events and drawing ----

      PuglStatus onEvent(const PuglEvent &event) {
         switch (event.type) {
         case PUGL_CONFIGURE:
            _width = event.configure.width;
            _height = event.configure.height;
            break;
         case PUGL_EXPOSE:
            draw();
            break;
         case PUGL_MOTION:
            _mouseX = event.motion.x;
            _mouseY = event.motion.y;
            break;
         case PUGL_POINTER_OUT:
            _mouseX = -1;
            break;
         case PUGL_BUTTON_PRESS:
            ++_clicks;
            _ripples.push_back({event.button.x, event.button.y, _time});
            break;
         case PUGL_KEY_PRESS:
            // A ripple in the middle, and the colors turn
            ++_keys;
            _ripples.push_back({_width / 2.0, _height / 2.0, _time});
            break;
         case PUGL_CLOSE:
            log(CLAP_LOG_INFO, "the host ended the embedding");
            break;
         default:
            break;
         }
         return PUGL_SUCCESS;
      }

      void draw() {
         _time = puglGetTime(gWorld.world);
         const double t = _time;
         const double w = _width, h = _height;
         const double s = puglGetScaleFactor(_view); // our coordinates are buffer pixels
         const double shift = _keys * 0.11;

         glViewport(0, 0, int(w), int(h));
         glMatrixMode(GL_PROJECTION);
         glLoadIdentity();
         glOrtho(0, w, h, 0, -1, 1);
         glMatrixMode(GL_MODELVIEW);
         glLoadIdentity();
         glEnable(GL_BLEND);
         glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

         // A dark background, its corners slowly changing colors
         glBegin(GL_QUADS);
         setColor(hue(t * 0.04 + shift, 1, 0.22f));
         glVertex2d(0, 0);
         setColor(hue(t * 0.04 + shift + 0.25, 1, 0.12f));
         glVertex2d(w, 0);
         setColor(hue(t * 0.04 + shift + 0.5, 1, 0.22f));
         glVertex2d(w, h);
         setColor(hue(t * 0.04 + shift + 0.75, 1, 0.12f));
         glVertex2d(0, h);
         glEnd();

         // A Lissajous figure, drawn as a trail of dots
         for (int i = 0; i < 240; ++i) {
            const double u = t * 0.8 - i * 0.012;
            const double x = w * 0.5 + w * 0.36 * std::sin(3 * u + t * 0.1);
            const double y = h * 0.5 + h * 0.34 * std::sin(4 * u);
            setColor(hue(shift + i / 240.0 + t * 0.05, 1.0f - i / 240.0f));
            disc(x, y, s * (4.5 - i * 0.015));
         }

         // Squares turning around the middle
         for (int i = 0; i < 5; ++i) {
            glPushMatrix();
            glTranslated(w * 0.5, h * 0.5, 0);
            glRotated(t * (20 + i * 12) * (i % 2 ? -1 : 1), 0, 0, 1);
            const double half = s * (18 + i * 16);
            setColor(hue(shift + t * 0.07 + i * 0.15, 0.35f));
            glBegin(GL_LINE_LOOP);
            glVertex2d(-half, -half);
            glVertex2d(half, -half);
            glVertex2d(half, half);
            glVertex2d(-half, half);
            glEnd();
            glPopMatrix();
         }

         // Ripples where the pointer clicked, or in the middle for keys
         std::erase_if(_ripples, [this](const Ripple &r) { return _time - r.startTime > 1.5; });
         for (const auto &r : _ripples) {
            const double age = (_time - r.startTime) / 1.5;
            setColor(hue(age + _clicks * 0.13 + shift, float(1 - age)));
            ring(r.x, r.y, s * (6 + age * 90), s * 3);
         }

         // A dot per click along the bottom, a square per key along the top
         for (int i = 0; i < std::min(_clicks, 40); ++i) {
            setColor(hue(i * 0.13));
            disc(s * (14 + i * 11), h - s * 14, s * 4);
         }
         for (int i = 0; i < std::min(_keys, 40); ++i) {
            setColor(hue(0.5 + i * 0.13));
            const double x = s * (10 + i * 11), y = s * 10;
            glRectd(x, y, x + s * 8, y + s * 8);
         }

         // The pointer
         if (_mouseX >= 0) {
            setColor({1, 1, 1, 0.8f});
            ring(_mouseX, _mouseY, s * 12, s * 2);
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

      PuglView *_view = nullptr;
      int _fd = -1;

      unsigned _width = kWidth;
      unsigned _height = kHeight;
      double _time = 0;
      double _mouseX = -1, _mouseY = -1;
      int _clicks = 0;
      int _keys = 0;
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
   .deinit = [] {},
   .get_factory = [](const char *id) -> const void * {
      return !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &kFactory : nullptr;
   },
};
