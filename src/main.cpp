// clap-wayland-host: embeds a CLAP plugin's GUI into a Wayland window (no X11), through a
// nested compositor which the host runs.
//
// Usage: clap-wayland-host <plugin.clap> [plugin-id]
//
// Without a plugin id it loads the bundle's first plugin; with an unknown one it lists them.

#include <csignal>
#include <iostream>

#include "host.h"

volatile std::sig_atomic_t gQuitRequested = 0;

int main(int argc, char **argv) {
   if (argc < 2 || argc > 3) {
      std::cerr << "usage: " << argv[0] << " <plugin.clap> [plugin-id]\n";
      return 1;
   }

   // Ctrl+C interrupts the main loop's poll(), which then exits so that everything shuts down
   struct sigaction action = {};
   action.sa_handler = [](int) { gQuitRequested = 1; };
   sigaction(SIGINT, &action, nullptr);
   sigaction(SIGTERM, &action, nullptr);

   Host host;
   if (!host.load(argv[1], argc > 2 ? argv[2] : "") || !host.openGui())
      return 1;

   host.run();
   return 0;
}
