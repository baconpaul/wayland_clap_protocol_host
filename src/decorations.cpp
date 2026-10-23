#include "decorations.h"

#include <cmath>

#include <cairo.h>
#include <pango/pangocairo.h>

namespace decorations {
   namespace {
      // Close to Adwaita's header bars
      constexpr double kActiveBackground[] = {0.92, 0.92, 0.92};
      constexpr double kInactiveBackground[] = {0.98, 0.98, 0.98};
      constexpr double kActiveText[] = {0.18, 0.20, 0.21};
      constexpr double kInactiveText[] = {0.55, 0.55, 0.55};
      constexpr double kSeparator[] = {0.82, 0.82, 0.82};
      constexpr double kCloseHover[] = {0.85, 0.85, 0.85};
      constexpr double kClosePressed[] = {0.76, 0.76, 0.76};

      constexpr double kCornerRadius = 8;
      constexpr double kCloseSize = 24;
      constexpr double kCloseMargin = 6;

      void setColor(cairo_t *cr, const double (&rgb)[3]) { cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]); }

      // The close button's center
      double closeCenterX(int width) { return width - kCloseMargin - kCloseSize / 2; }
      double closeCenterY() { return kTitleBarHeight / 2.0; }
   } // namespace

   bool isOnCloseButton(double x, double y, int width) {
      const double dx = x - closeCenterX(width);
      const double dy = y - closeCenterY();
      return std::abs(dx) <= kCloseSize / 2 && std::abs(dy) <= kCloseSize / 2;
   }

   void drawTitleBar(uint32_t *pixels, int bufferWidth, int bufferHeight, int width, double scale,
                     const TitleBarState &state) {
      auto surface = cairo_image_surface_create_for_data(reinterpret_cast<unsigned char *>(pixels),
                                                         CAIRO_FORMAT_ARGB32, bufferWidth, bufferHeight,
                                                         bufferWidth * 4);
      auto cr = cairo_create(surface);

      // Transparent around the rounded corners
      cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
      cairo_paint(cr);
      cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
      cairo_scale(cr, scale, scale);

      const double w = width;
      const double h = kTitleBarHeight;

      // The background, with the top corners rounded like GNOME's windows
      cairo_new_path(cr);
      cairo_arc(cr, kCornerRadius, kCornerRadius, kCornerRadius, M_PI, 1.5 * M_PI);
      cairo_arc(cr, w - kCornerRadius, kCornerRadius, kCornerRadius, 1.5 * M_PI, 2 * M_PI);
      cairo_line_to(cr, w, h);
      cairo_line_to(cr, 0, h);
      cairo_close_path(cr);
      setColor(cr, state.isActive ? kActiveBackground : kInactiveBackground);
      cairo_fill(cr);

      // A separator from the content
      cairo_rectangle(cr, 0, h - 1, w, 1);
      setColor(cr, kSeparator);
      cairo_fill(cr);

      // The close button, as GTK draws it: a cross, in a circle when hovered
      const double cx = closeCenterX(width);
      const double cy = closeCenterY();
      if (state.isCloseHovered || state.isClosePressed) {
         cairo_arc(cr, cx, cy, kCloseSize / 2, 0, 2 * M_PI);
         setColor(cr, state.isClosePressed ? kClosePressed : kCloseHover);
         cairo_fill(cr);
      }
      constexpr double arm = 4.5;
      cairo_move_to(cr, cx - arm, cy - arm);
      cairo_line_to(cr, cx + arm, cy + arm);
      cairo_move_to(cr, cx + arm, cy - arm);
      cairo_line_to(cr, cx - arm, cy + arm);
      cairo_set_line_width(cr, 1.5);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      setColor(cr, state.isActive ? kActiveText : kInactiveText);
      cairo_stroke(cr);

      // The title, centered in the room which the button leaves
      const double reserved = 2 * (kCloseSize + 2 * kCloseMargin);
      const double textWidth = std::max(0.0, w - reserved);
      if (!state.title.empty() && textWidth > 0) {
         auto layout = pango_cairo_create_layout(cr);
         auto font = pango_font_description_from_string("Sans Bold 10.5");
         pango_layout_set_font_description(layout, font);
         pango_font_description_free(font);
         pango_layout_set_text(layout, state.title.c_str(), -1);
         pango_layout_set_width(layout, int(textWidth * PANGO_SCALE));
         pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
         pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
         pango_layout_set_single_paragraph_mode(layout, true);

         int textHeight = 0;
         pango_layout_get_pixel_size(layout, nullptr, &textHeight);
         cairo_move_to(cr, reserved / 2, (h - textHeight) / 2);
         setColor(cr, state.isActive ? kActiveText : kInactiveText);
         pango_cairo_show_layout(cr, layout);
         g_object_unref(layout);
      }

      cairo_destroy(cr);
      cairo_surface_flush(surface);
      cairo_surface_destroy(surface);
   }
} // namespace decorations
