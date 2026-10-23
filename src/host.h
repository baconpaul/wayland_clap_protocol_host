#pragma once

// A minimal CLAP host which embeds one plugin's GUI into its Wayland window, through a nested
// compositor (CLAP_WINDOW_API_WAYLAND_SURFACE_EMBEDDING_V1, see clap-wayland-experiment).
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

class Host final {
public:
   Host();
   ~Host();

   Host(const Host &) = delete;
   Host &operator=(const Host &) = delete;

   // Loads the bundle; prints the plugins it contains. With an empty id, picks the first one.
   bool load(const std::string &path, std::string pluginId);
   bool openGui();
   void run();

private:
   using Clock = std::chrono::steady_clock;

   static Host &self(const clap_host *host) { return *static_cast<Host *>(host->host_data); }

   const void *getExtension(const char *id) const;
   bool isMainThread() const { return std::this_thread::get_id() == _mainThread; }

   void runTimers();
   int nextTimeoutMs() const;
   void runPosixFds(const std::vector<pollfd> &fds);
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

   // What the plugin gives us
   void *_library = nullptr;
   const clap_plugin_entry *_entry = nullptr;
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

   // The window first: the compositor forwards the plugin's surface into it
   std::unique_ptr<WaylandWindow> _window;
   std::unique_ptr<NestedCompositor> _compositor;
};
