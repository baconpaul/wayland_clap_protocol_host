// vst3-wayland-host: embeds VST3 plugins' views into Wayland windows (no X11), through the nested
// compositor which it shares with clap-wayland-host, and vst3_equiv/extension's
// IWaylandNestedCompositor.
//
// Usage: vst3-wayland-host <plugin.vst3> [plugin-name] [<plugin.vst3> [plugin-name]]...
//
// Each plugin gets a window of its own, all in this process. Without a plugin name it loads the
// bundle's first audio effect class; with an unknown one it lists them. A bundle given twice is
// loaded once.

#include <csignal>
#include <iostream>
#include <map>

#include "vst3-host.h"

volatile std::sig_atomic_t gQuitRequested = 0;

namespace {
   // The windows' bars, in the order of the plugins on the command line
   constexpr WaylandWindow::Colors kColors[] = {
      {.bar = 0x1e6b32, .accent = 0x5fd37a, .text = 0xd8f5df}, // green
      {.bar = 0x8a1f24, .accent = 0xf06a6a, .text = 0xfadcdc}, // red
      {.bar = 0x1f4a8a, .accent = 0x6aa0f0, .text = 0xdce8fa}, // blue
      {.bar = 0x7a5a12, .accent = 0xf0c050, .text = 0xfaf0d8}, // amber
   };

   struct PluginArg {
      std::string path;
      std::string name;
   };

   std::string bundlePath(std::string arg) {
      while (arg.size() > 1 && arg.ends_with('/'))
         arg.pop_back();
      return arg.size() > 5 && arg.ends_with(".vst3") ? arg : std::string();
   }
} // namespace

int main(int argc, char **argv) {
   // A .vst3 starts a plugin, and the argument after it may be its name
   std::vector<PluginArg> args;
   for (int i = 1; i < argc; ++i) {
      if (auto path = bundlePath(argv[i]); !path.empty())
         args.push_back({path, {}});
      else if (!args.empty() && args.back().name.empty())
         args.back().name = argv[i];
      else
         args.clear(), i = argc; // an error
   }
   if (args.empty()) {
      std::cerr << "usage: " << argv[0] << " <plugin.vst3> [plugin-name] [<plugin.vst3> [plugin-name]]...\n";
      return 1;
   }

   // Ctrl+C interrupts the main loop's poll(), which then exits so that everything shuts down
   struct sigaction action = {};
   action.sa_handler = [](int) { gQuitRequested = 1; };
   sigaction(SIGINT, &action, nullptr);
   sigaction(SIGTERM, &action, nullptr);

   // In this order, so that each one outlives those which use it: the libraries go after the
   // compositor, which the plugins may still be connected to until it stops, and the context goes
   // after the libraries, whose factories may keep it
   WaylandConnection connection;
   if (!connection.open("vst3-wayland-host"))
      return 1;
   HostContext context;
   std::map<std::string, std::unique_ptr<Vst3Library>> libraries;
   NestedCompositor compositor;
   if (!compositor.start(connection.upstream()))
      return 1;
   context.setCompositor(&compositor);
   std::vector<std::unique_ptr<Vst3Host>> hosts;

   for (auto &arg : args) {
      auto &library = libraries[arg.path];
      if (!library) {
         library = std::make_unique<Vst3Library>();
         if (!library->load(arg.path, context))
            return 1;
      }
      auto info = library->find(arg.name);
      if (!info)
         return 1;

      const auto &colors = kColors[hosts.size() % std::size(kColors)];
      auto &host = hosts.emplace_back(std::make_unique<Vst3Host>(connection, compositor, context, colors));
      if (!host->create(*library, *info) || !host->openGui())
         return 1;
   }
   std::cout << "close the windows (or Ctrl+C) to quit\n";

   std::vector<pollfd> fds;
   std::vector<size_t> fdCounts;
   while (connection.isConnected() && !gQuitRequested) {
      // A closed window ends its plugin; the others go on
      std::erase_if(hosts, [](auto &h) { return !h->isOpen(); });
      if (hosts.empty())
         break;

      int timeoutMs = -1;
      for (auto &h : hosts) {
         h->idle();
         const int t = h->nextTimeoutMs();
         if (t >= 0 && (timeoutMs < 0 || t < timeoutMs))
            timeoutMs = t;
      }

      fds.clear();
      fdCounts.clear();
      for (auto &h : hosts) {
         const size_t before = fds.size();
         h->addPollFds(fds);
         fdCounts.push_back(fds.size() - before);
      }

      connection.dispatch(timeoutMs, fds);

      const pollfd *next = fds.data();
      for (size_t i = 0; i < hosts.size(); ++i) {
         hosts[i]->runPosixFds(next, fdCounts[i]);
         next += fdCounts[i];
      }
   }

   hosts.clear();
   context.setCompositor(nullptr);
   compositor.stop();
   return 0;
}
