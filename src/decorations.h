#pragma once

// The title bars which the nested compositor draws around the plugins' windows, when they ask
// for server-side decorations (zxdg_decoration_manager_v1). The system compositor may not draw
// any, like GNOME's, and the plugin's windows go through us anyway.
//
// Everything is in logical pixels, except for the buffer which drawTitleBar() fills.

#include <cstdint>
#include <string>

namespace decorations {
   constexpr int kTitleBarHeight = 34;

   struct TitleBarState {
      std::string title;
      bool isActive = true;
      bool isCloseHovered = false;
      bool isClosePressed = false;
   };

   // Fills an ARGB8888 buffer of bufferWidth x bufferHeight pixels, which shows a title bar of
   // width x kTitleBarHeight logical pixels at the given scale
   void drawTitleBar(uint32_t *pixels, int bufferWidth, int bufferHeight, int width, double scale,
                     const TitleBarState &state);

   // Hit tests, in the title bar's logical coordinates
   bool isOnCloseButton(double x, double y, int width);
} // namespace decorations
