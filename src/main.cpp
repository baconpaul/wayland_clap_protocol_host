// clap-wayland-host: embeds CLAP plugins' GUIs into Wayland windows (no X11), through a nested
// compositor which the host runs.
//
// Usage: clap-wayland-host <plugin.clap> [plugin-id] [<plugin.clap> [plugin-id]]...
//
// Each plugin gets a window of its own, all in this process. Without a plugin id it loads the
// bundle's first plugin; with an unknown one it lists them. A bundle given twice is loaded once.

#include <csignal>
#include <iostream>
#include <map>

#include "host.h"

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
      std::string id;
   };

   bool isBundle(const std::string &arg) { return arg.size() > 5 && arg.ends_with(".clap"); }
} // namespace

int main(int argc, char **argv) {
   // A .clap starts a plugin, and the argument after it may be its id
   std::vector<PluginArg> args;
   for (int i = 1; i < argc; ++i) {
      if (isBundle(argv[i]))
         args.push_back({argv[i], {}});
      else if (!args.empty() && args.back().id.empty())
         args.back().id = argv[i];
      else
         args.clear(), i = argc; // an error
   }
   if (args.empty()) {
      std::cerr << "usage: " << argv[0] << " <plugin.clap> [plugin-id] [<plugin.clap> [plugin-id]]...\n";
      return 1;
   }

   // Ctrl+C interrupts the main loop's poll(), which then exits so that everything shuts down
   struct sigaction action = {};
   action.sa_handler = [](int) { gQuitRequested = 1; };
   sigaction(SIGINT, &action, nullptr);
   sigaction(SIGTERM, &action, nullptr);

   // In this order, so that each one outlives those which use it: the libraries go after the
   // compositor, which the plugins may still be connected to until it stops
   WaylandConnection connection;
   if (!connection.open())
      return 1;
   std::map<std::string, std::unique_ptr<PluginLibrary>> libraries;
   NestedCompositor compositor;
   if (!compositor.start(connection.upstream()))
      return 1;
   std::vector<std::unique_ptr<Host>> hosts;

   for (auto &arg : args) {
      auto &library = libraries[arg.path];
      if (!library) {
         library = std::make_unique<PluginLibrary>();
         if (!library->load(arg.path))
            return 1;
      }
      auto descriptor = library->find(arg.id);
      if (!descriptor)
         return 1;

      const auto &colors = kColors[hosts.size() % std::size(kColors)];
      auto &host = hosts.emplace_back(std::make_unique<Host>(connection, compositor, colors));
      if (!host->create(*library, descriptor) || !host->openGui())
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
   compositor.stop();
   return 0;
}
