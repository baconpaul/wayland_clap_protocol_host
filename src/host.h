#pragma once

// A minimal CLAP host which embeds a plugin's GUI into a Wayland window of its own, through a
// nested compositor (CLAP_WINDOW_API_WAYLAND_SURFACE_EMBEDDING_V1, see clap-wayland-experiment).
// There is one Host per plugin instance; they share the connection to the system compositor and
// the nested compositor, as the plugins in a DAW would.
//
// The plugin isn't activated: there is no audio. The parameter changes made in the GUI are
// collected with clap_plugin_params.flush() and printed.

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <clap/clap.h>
#include <clap/ext/draft/wayland-experiment.h>

#include "nested-compositor.h"
#include "wayland-window.h"

// A .clap bundle, loaded once for all its instances
class PluginLibrary final {
public:
   PluginLibrary() = default;
   ~PluginLibrary();

   PluginLibrary(const PluginLibrary &) = delete;
   PluginLibrary &operator=(const PluginLibrary &) = delete;

   bool load(const std::string &path);

   // With an empty id, the first plugin; with an unknown one, prints those it has
   const clap_plugin_descriptor *find(const std::string &pluginId) const;

   const clap_plugin_factory *factory() const { return _factory; }

private:
   std::string _path;
   void *_library = nullptr;
   const clap_plugin_entry *_entry = nullptr;
   const clap_plugin_factory *_factory = nullptr;
};

class Host final {
public:
   using Clock = std::chrono::steady_clock;

   // The connection and the compositor are shared by all the hosts, and outlive them
   Host(WaylandConnection &connection, NestedCompositor &compositor, const WaylandWindow::Colors &colors);
   ~Host();

   Host(const Host &) = delete;
   Host &operator=(const Host &) = delete;

   bool create(const PluginLibrary &library, const clap_plugin_descriptor *descriptor);
   bool openGui();

   bool isOpen() const { return _window && _window->isOpen(); }

   // The main loop's work, which main() runs for all the hosts
   void idle(); // callbacks, timers, parameter flushes
   int nextTimeoutMs() const;
   void addPollFds(std::vector<pollfd> &fds) const;
   void runPosixFds(const pollfd *fds, size_t count); // those which addPollFds() added

private:

   static Host &self(const clap_host *host) { return *static_cast<Host *>(host->host_data); }

   const void *getExtension(const char *id) const;
   bool isMainThread() const { return std::this_thread::get_id() == _mainThread; }

   void runTimers();
   void flushParams();

   struct Timer {
      clap_id id;
      std::chrono::milliseconds period;
      Clock::time_point deadline;
   };

   struct PosixFd {
      int fd;
      clap_posix_fd_flags_t flags;
   };

   const std::thread::id _mainThread = std::this_thread::get_id();

   // What we give the plugin
   clap_host _host;
   clap_host_log _hostLog;
   clap_host_thread_check _hostThreadCheck;
   clap_host_timer_support _hostTimerSupport;
   clap_host_posix_fd_support _hostPosixFdSupport;
   clap_host_params _hostParams;
   clap_host_gui _hostGui;
   clap_host_gui_wayland_nested_compositor _hostNestedCompositor;

   WaylandConnection &_connection;
   NestedCompositor &_compositor;
   const WaylandWindow::Colors _colors;

   // What the plugin gives us
   const clap_plugin *_plugin = nullptr;
   const clap_plugin_gui *_gui = nullptr;
   const clap_plugin_gui_wayland_nested_compositor *_guiNestedCompositor = nullptr;
   const clap_plugin_params *_params = nullptr;
   const clap_plugin_timer_support *_timerSupport = nullptr;
   const clap_plugin_posix_fd_support *_posixFdSupport = nullptr;
   std::string _pluginName;

   bool _isGuiCreated = false;

   std::vector<Timer> _timers;
   clap_id _nextTimerId = 0;
   std::vector<PosixFd> _posixFds;
   bool _isCallbackRequested = false;
   bool _isFlushRequested = false;

   std::unique_ptr<WaylandWindow> _window;
   NestedCompositor::WindowId _windowId = UINT32_MAX; // none, until openGui()
};
