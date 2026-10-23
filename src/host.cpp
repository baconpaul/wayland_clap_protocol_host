#include "host.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include <dlfcn.h>

PluginLibrary::~PluginLibrary() {
   if (_entry)
      _entry->deinit();
   if (_library)
      dlclose(_library);
}

bool PluginLibrary::load(const std::string &path) {
   _path = path;
   _library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
   if (!_library) {
      std::cerr << "failed to load " << path << ": " << dlerror() << "\n";
      return false;
   }

   auto entry = static_cast<const clap_plugin_entry *>(dlsym(_library, "clap_entry"));
   if (!entry || !clap_version_is_compatible(entry->clap_version) || !entry->init(path.c_str())) {
      std::cerr << path << " has no usable clap_entry\n";
      return false;
   }
   _entry = entry;

   _factory = static_cast<const clap_plugin_factory *>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
   if (!_factory || _factory->get_plugin_count(_factory) == 0) {
      std::cerr << "no plugin factory, or no plugins in " << path << "\n";
      return false;
   }
   return true;
}

const clap_plugin_descriptor *PluginLibrary::find(const std::string &pluginId) const {
   for (uint32_t i = 0; i < _factory->get_plugin_count(_factory); ++i) {
      auto d = _factory->get_plugin_descriptor(_factory, i);
      if (pluginId.empty() ? i == 0 : pluginId == d->id)
         return d;
   }
   std::cerr << "no plugin " << pluginId << " in " << _path << ", it has:\n";
   for (uint32_t i = 0; i < _factory->get_plugin_count(_factory); ++i) {
      auto d = _factory->get_plugin_descriptor(_factory, i);
      std::cerr << "  " << d->id << "  (" << d->name << ")\n";
   }
   return nullptr;
}

Host::Host(WaylandConnection &connection, NestedCompositor &compositor, const WaylandWindow::Colors &colors)
   : _connection(connection), _compositor(compositor), _colors(colors) {
   _host = {
      .clap_version = CLAP_VERSION,
      .host_data = this,
      .name = "CLAP Wayland Host",
      .vendor = "example",
      .url = nullptr,
      .version = "0.1",
      .get_extension = [](const clap_host *h, const char *id) { return self(h).getExtension(id); },
      .request_restart = [](const clap_host *) {},
      .request_process = [](const clap_host *) {},
      .request_callback = [](const clap_host *h) { self(h)._isCallbackRequested = true; },
   };

   _hostLog = {
      .log = [](const clap_host *, clap_log_severity severity, const char *msg) {
         std::cerr << "[plugin log " << severity << "] " << msg << "\n";
      },
   };

   _hostThreadCheck = {
      .is_main_thread = [](const clap_host *h) { return self(h).isMainThread(); },
      .is_audio_thread = [](const clap_host *) { return false; }, // never activated
   };

   _hostTimerSupport = {
      .register_timer = [](const clap_host *h, uint32_t periodMs, clap_id *timerId) {
         auto &host = self(h);
         const auto period = std::chrono::milliseconds(std::max(periodMs, 1u));
         *timerId = host._nextTimerId++;
         host._timers.push_back({*timerId, period, Clock::now() + period});
         return true;
      },
      .unregister_timer = [](const clap_host *h, clap_id timerId) {
         return std::erase_if(self(h)._timers, [&](auto &t) { return t.id == timerId; }) > 0;
      },
   };

   _hostPosixFdSupport = {
      .register_fd = [](const clap_host *h, int fd, clap_posix_fd_flags_t flags) {
         auto &fds = self(h)._posixFds;
         if (std::any_of(fds.begin(), fds.end(), [&](auto &f) { return f.fd == fd; }))
            return false;
         fds.push_back({fd, flags});
         return true;
      },
      .modify_fd = [](const clap_host *h, int fd, clap_posix_fd_flags_t flags) {
         for (auto &f : self(h)._posixFds)
            if (f.fd == fd) {
               f.flags = flags;
               return true;
            }
         return false;
      },
      .unregister_fd = [](const clap_host *h, int fd) {
         return std::erase_if(self(h)._posixFds, [&](auto &f) { return f.fd == fd; }) > 0;
      },
   };

   _hostParams = {
      .rescan = [](const clap_host *, clap_param_rescan_flags) {},
      .clear = [](const clap_host *, clap_id, clap_param_clear_flags) {},
      .request_flush = [](const clap_host *h) { self(h)._isFlushRequested = true; },
   };

   _hostGui = {
      .resize_hints_changed = [](const clap_host *) {},
      // Our window follows the plugin's size (logical, as always with Wayland)
      .request_resize = [](const clap_host *h, uint32_t width, uint32_t height) {
         auto &host = self(h);
         if (!host._window)
            return false;
         host._window->resize(width, height);
         return true;
      },
      .request_show = [](const clap_host *) { return false; },
      .request_hide = [](const clap_host *) { return false; },
      .closed = [](const clap_host *, bool) { std::cout << "the plugin closed its GUI\n"; },
   };

   _hostNestedCompositor = {
      .open_connection = [](const clap_host *h) { return self(h)._compositor.openConnection(); },
   };
}

Host::~Host() {
   if (_isGuiCreated)
      _gui->destroy(_plugin);
   if (_plugin)
      _plugin->destroy(_plugin);
   // The plugin may keep its connection for its other instances: the compositor ends what is
   // still embedded into our window before it goes
   if (_window)
      _compositor.removeWindow(_windowId);
}

const void *Host::getExtension(const char *id) const {
   if (!strcmp(id, CLAP_EXT_LOG))
      return &_hostLog;
   if (!strcmp(id, CLAP_EXT_THREAD_CHECK))
      return &_hostThreadCheck;
   if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT))
      return &_hostTimerSupport;
   if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
      return &_hostPosixFdSupport;
   if (!strcmp(id, CLAP_EXT_PARAMS))
      return &_hostParams;
   if (!strcmp(id, CLAP_EXT_GUI))
      return &_hostGui;
   // The plugins ask for it in init()
   if (!strcmp(id, CLAP_EXT_GUI_WAYLAND_NESTED_COMPOSITOR))
      return &_hostNestedCompositor;
   return nullptr;
}

bool Host::create(const PluginLibrary &library, const clap_plugin_descriptor *descriptor) {
   _pluginName = descriptor->name;

   auto factory = library.factory();
   _plugin = factory->create_plugin(factory, &_host, descriptor->id);
   if (!_plugin || !_plugin->init(_plugin)) {
      std::cerr << "failed to create " << descriptor->id << "\n";
      if (_plugin)
         _plugin->destroy(_plugin);
      _plugin = nullptr;
      return false;
   }
   std::cout << "loaded " << descriptor->name << " (" << descriptor->id << ")\n";

   auto ext = [&](const char *id) { return _plugin->get_extension(_plugin, id); };
   _gui = static_cast<const clap_plugin_gui *>(ext(CLAP_EXT_GUI));
   _guiNestedCompositor =
      static_cast<const clap_plugin_gui_wayland_nested_compositor *>(ext(CLAP_EXT_GUI_WAYLAND_NESTED_COMPOSITOR));
   _params = static_cast<const clap_plugin_params *>(ext(CLAP_EXT_PARAMS));
   _timerSupport = static_cast<const clap_plugin_timer_support *>(ext(CLAP_EXT_TIMER_SUPPORT));
   _posixFdSupport = static_cast<const clap_plugin_posix_fd_support *>(ext(CLAP_EXT_POSIX_FD_SUPPORT));

   if (!_gui || !_guiNestedCompositor) {
      std::cerr << "the plugin has no GUI, or can't embed it through a nested Wayland compositor ("
                << CLAP_EXT_GUI_WAYLAND_NESTED_COMPOSITOR << ")\n";
      return false;
   }
   return true;
}

// The nested compositor sequence from clap-wayland-experiment's wayland-experiment.h
bool Host::openGui() {
   constexpr const char *api = CLAP_WINDOW_API_WAYLAND_SURFACE_EMBEDDING_V1;

   // A placeholder size until the plugin tells its own
   _window = std::make_unique<WaylandWindow>(_connection, _colors);
   const std::string title = _pluginName + " - CLAP Wayland Host";
   if (!_window->open(title.c_str(), 400, 300))
      return false;
   _windowId = _compositor.addWindow(_window->nested());

   if (!_gui->is_api_supported(_plugin, api, false)) {
      std::cerr << "the plugin doesn't support " << api << "\n";
      return false;
   }
   if (!_gui->create(_plugin, api, false)) {
      std::cerr << "the plugin failed to create its GUI\n";
      return false;
   }
   _isGuiCreated = true;

   // No set_scale(): with Wayland, sizes are logical and the plugin gets the scale from the
   // compositor, which is us: we forward the system compositor's
   uint32_t width = 0;
   uint32_t height = 0;
   if (!_gui->get_size(_plugin, &width, &height)) {
      std::cerr << "the plugin didn't give its size\n";
      return false;
   }
   std::cout << "plugin GUI: " << width << "x" << height << (_gui->can_resize(_plugin) ? ", resizable" : "") << "\n";
   _window->resize(width, height);

   // The place below our bar, which the token designates
   const std::string token = _compositor.createPlace(_windowId, 0, WaylandWindow::kBarHeight);
   _window->commit();

   // The plugin places its popups relative to our window's geometry, which libdecor's title bar
   // is part of
   int32_t originX = 0;
   int32_t originY = 0;
   _window->contentOrigin(originX, originY);
   _compositor.setParentOrigin(_windowId, originX, originY);

   if (!_guiNestedCompositor->set_parent(_plugin, token.c_str())) {
      std::cerr << "the plugin failed to embed its GUI\n";
      return false;
   }
   if (!_gui->show(_plugin))
      std::cerr << "the plugin failed to show its GUI\n";
   return true;
}

void Host::idle() {
   if (_isCallbackRequested) {
      _isCallbackRequested = false;
      _plugin->on_main_thread(_plugin);
   }
   runTimers();
   flushParams();
}

void Host::addPollFds(std::vector<pollfd> &fds) const {
   for (auto &f : _posixFds) {
      short events = 0;
      if (f.flags & CLAP_POSIX_FD_READ)
         events |= POLLIN;
      if (f.flags & CLAP_POSIX_FD_WRITE)
         events |= POLLOUT;
      fds.push_back({.fd = f.fd, .events = events, .revents = 0});
   }
}

void Host::runTimers() {
   if (!_timerSupport)
      return;

   const auto now = Clock::now();
   // Index loop: the plugin may (un)register timers from on_timer()
   for (size_t i = 0; i < _timers.size(); ++i) {
      if (_timers[i].deadline > now)
         continue;
      _timers[i].deadline = now + _timers[i].period;
      _timerSupport->on_timer(_plugin, _timers[i].id);
   }
}

int Host::nextTimeoutMs() const {
   if (_isCallbackRequested || _isFlushRequested)
      return 0;
   if (_timers.empty() || !_timerSupport)
      return -1;

   auto deadline = _timers.front().deadline;
   for (auto &t : _timers)
      deadline = std::min(deadline, t.deadline);
   const auto delay = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
   return std::max<int>(0, delay.count());
}

void Host::runPosixFds(const pollfd *fds, size_t count) {
   if (!_posixFdSupport)
      return;

   for (size_t i = 0; i < count; ++i) {
      auto &p = fds[i];
      clap_posix_fd_flags_t flags = 0;
      if (p.revents & POLLIN)
         flags |= CLAP_POSIX_FD_READ;
      if (p.revents & POLLOUT)
         flags |= CLAP_POSIX_FD_WRITE;
      if (p.revents & (POLLERR | POLLHUP))
         flags |= CLAP_POSIX_FD_ERROR;
      if (flags)
         _posixFdSupport->on_fd(_plugin, p.fd, flags);
   }
}

void Host::flushParams() {
   if (!_isFlushRequested || !_params)
      return;
   _isFlushRequested = false;

   static const clap_input_events in = {
      .ctx = nullptr,
      .size = [](const clap_input_events *) -> uint32_t { return 0; },
      .get = [](const clap_input_events *, uint32_t) -> const clap_event_header * { return nullptr; },
   };

   static const clap_output_events out = {
      .ctx = nullptr,
      .try_push = [](const clap_output_events *, const clap_event_header *hdr) {
         if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return true;
         switch (hdr->type) {
         case CLAP_EVENT_PARAM_VALUE: {
            auto ev = reinterpret_cast<const clap_event_param_value *>(hdr);
            std::cout << "param " << ev->param_id << " = " << ev->value << "\n";
            break;
         }
         case CLAP_EVENT_PARAM_GESTURE_BEGIN:
         case CLAP_EVENT_PARAM_GESTURE_END: {
            auto ev = reinterpret_cast<const clap_event_param_gesture *>(hdr);
            std::cout << "param " << ev->param_id << ": gesture "
                      << (hdr->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? "begin" : "end") << "\n";
            break;
         }
         }
         return true;
      },
   };

   _params->flush(_plugin, &in, &out);
}
