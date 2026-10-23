#include "nested-compositor.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <iostream>
#include <random>
#include <unordered_map>
#include <utility>

#include <cmath>

#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-server.h>

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "fractional-scale-v1-server-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-dmabuf-v1-server-protocol.h"
#include "viewporter-client-protocol.h"
#include "viewporter-server-protocol.h"
#include "xdg-decoration-unstable-v1-server-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-shell-server-protocol.h"
#include "clap-surface-embedding-v1-server-protocol.h"

#include "decorations.h"

#include <sys/mman.h>

// Naming: "up" objects are proxies on the system compositor, "res" ones are resources of our
// own compositor, held by the plugins.

namespace {
   struct Impl;
   struct Surface;
   struct XdgSurface;
   struct Popup;

   template <class T>
   T *dataOf(wl_resource *resource) {
      return static_cast<T *>(wl_resource_get_user_data(resource));
   }

   void destroyResource(wl_client *, wl_resource *resource) { wl_resource_destroy(resource); }

   // wp_viewport.set_source's "unset" value: -1 as wl_fixed, not the raw -1
   constexpr wl_fixed_t kUnset = -256;
   static_assert(kUnset == -1 * 256);

   struct SurfaceState {
      int32_t bufferScale = 1;
      int32_t destWidth = -1;
      int32_t destHeight = -1;
      wl_fixed_t source[4] = {kUnset, kUnset, kUnset, kUnset};
   };

   // A surface on the system compositor, which shows a plugin's surface
   struct Mirror {
      Impl *impl = nullptr;
      wl_surface *up = nullptr;
      wp_viewport *viewport = nullptr;
      wp_fractional_scale_v1 *fractionalScale = nullptr;

      // From the system compositor, forwarded to the plugin's surface
      uint32_t scale120 = 0;
      int32_t bufferScale = 0;

      // The state which we last applied to `up`, to forward only the changes
      SurfaceState applied;

      Surface *shown = nullptr;

      // A wl_surface keeps its role: a mirror which needs another one is replaced
      enum class Role { none, subsurface, xdgSurface } role = Role::none;

      // A fresh start for a new surface
      void reset() {
         applied = {};
         if (viewport) {
            wp_viewport_set_destination(viewport, -1, -1);
            wp_viewport_set_source(viewport, kUnset, kUnset, kUnset, kUnset);
         }
         wl_surface_set_buffer_scale(up, 1);
      }
   };

   // A place in the host's window, where one plugin surface can be embedded
   struct Place {
      std::string token;
      int32_t x = 0;
      int32_t y = 0;
      Mirror mirror;
      wl_subsurface *subsurface = nullptr;
   };

   struct Buffer {
      wl_resource *res = nullptr;
      wl_buffer *up = nullptr;
      int32_t width = 0;
      int32_t height = 0;
   };

   // A frame callback: pending until the surface's commit, then forwarded
   struct FrameCallback {
      wl_resource *res = nullptr;
      wl_callback *up = nullptr;
      Surface *pendingOn = nullptr;
   };

   // Tracks the buffer attached to a surface until its commit. The listener comes first, so that
   // we find the struct back from it.
   struct BufferRef {
      wl_listener listener;
      Surface *owner;
      wl_resource *buffer;
   };

   struct Surface {
      Impl *impl = nullptr;
      wl_resource *res = nullptr;
      wl_resource *viewportRes = nullptr;
      wl_resource *fractionalScaleRes = nullptr;

      // Its role: embedded into a place, or an xdg_surface with a mirror of its own
      wl_resource *embeddedRes = nullptr;
      Place *place = nullptr;
      XdgSurface *xdg = nullptr;
      std::unique_ptr<Mirror> ownMirror;

      Mirror *mirror() const { return place ? &place->mirror : ownMirror.get(); }

      // Its size in logical pixels, from its last buffer and viewport
      int32_t width = 0;
      int32_t height = 0;

      bool isBufferAttached = false;
      BufferRef pendingBuffer = {};
      std::vector<FrameCallback *> pendingFrames;
      SurfaceState pending;
      SurfaceState current;
   };

   ///////////////
   // xdg-shell //
   ///////////////

   // The resources which can be an xdg_surface or an xdg_toplevel start with their kind, so that
   // we tell ours from the ones which stand for the host's window
   enum class Kind { xdgSurface, toplevel, hostWindowSurface, hostWindowToplevel };

   struct XdgSurface {
      Kind kind = Kind::xdgSurface;
      Impl *impl = nullptr;
      wl_resource *res = nullptr;
      xdg_surface *up = nullptr;
      Surface *surface = nullptr;
      struct Toplevel *toplevel = nullptr;
      Popup *popup = nullptr;

      // The window geometry, which goes upstream with the role
      bool hasGeometry = false;
      int32_t geometry[4] = {};

      // Its title bar, which outlives the toplevel object as the role surface upstream
      std::unique_ptr<struct Frame> frame;
   };

   // The title bar around a plugin's toplevel, when it wants server-side decorations: the
   // toplevel's surface upstream, with the plugin's content as a subsurface below the bar
   struct Frame {
      Mirror mirror; // its own surface: shown is the plugin's content, for the input
      wl_subsurface *content = nullptr;
      wl_buffer *buffer = nullptr;
      decorations::TitleBarState state;
      decorations::TitleBarState drawnState;
      int32_t drawnWidth = 0;
      double drawnScale = 0;
      bool isPointerInside = false;
      double pointerX = 0;
      double pointerY = 0;
      uint32_t lastClickTime = 0;
   };

   struct Toplevel {
      Kind kind = Kind::toplevel;
      Impl *impl = nullptr;
      wl_resource *res = nullptr;
      xdg_toplevel *up = nullptr; // from the surface's first commit, when we know its decorations
      XdgSurface *xs = nullptr;

      wl_resource *decorationRes = nullptr;
      bool isDecorated = false; // by us, as the plugin asked

      // Until up exists
      std::string title;
      std::string appId;
      int32_t minSize[2] = {};
      int32_t maxSize[2] = {};
      xdg_toplevel *parent = nullptr;
      bool hasParent = false;
      bool isMaximized = false;
      bool isFullscreen = false;
      bool isMinimized = false;

      int32_t titleBarHeight() const { return isDecorated ? decorations::kTitleBarHeight : 0; }
   };

   struct Popup {
      Impl *impl = nullptr;
      wl_resource *res = nullptr;
      xdg_popup *up = nullptr;
      XdgSurface *xs = nullptr;
      Popup *parent = nullptr;
      std::vector<Popup *> children;
      int32_t parentOffsetY = 0; // the parent's title bar, which the plugin doesn't know of
   };

   // From get_parent_surface and get_parent_toplevel: stands for the host's window
   struct HostWindowRef {
      Kind kind;
   };
   HostWindowRef hostWindowSurfaceRef{Kind::hostWindowSurface};
   HostWindowRef hostWindowToplevelRef{Kind::hostWindowToplevel};

   Kind kindOf(wl_resource *res) { return *static_cast<Kind *>(wl_resource_get_user_data(res)); }

   struct Positioner {
      wl_resource *res = nullptr;
      xdg_positioner *up = nullptr;
      // The plugin's, which we shift below a parent's title bar
      bool hasAnchorRect = false;
      int32_t anchorRect[4] = {};
   };

   struct OwnedFd {
      int fd;
      explicit OwnedFd(int fd) : fd(fd) {}
      ~OwnedFd() { close(fd); }
      OwnedFd(const OwnedFd &) = delete;
      OwnedFd &operator=(const OwnedFd &) = delete;
   };

   // An event of zwp_linux_dmabuf_feedback_v1, but done
   struct DmabufFeedbackEvent {
      enum Type { FormatTable, MainDevice, TrancheTargetDevice, TrancheFormats, TrancheFlags, TrancheDone };

      explicit DmabufFeedbackEvent(Type type, const wl_array *a = nullptr) : type(type) {
         wl_array_init(&array);
         if (a)
            wl_array_copy(&array, const_cast<wl_array *>(a));
      }
      DmabufFeedbackEvent(DmabufFeedbackEvent &&other) noexcept
         : type(other.type), array(other.array), value(other.value), fd(std::move(other.fd)) {
         wl_array_init(&other.array);
      }
      DmabufFeedbackEvent(const DmabufFeedbackEvent &) = delete;
      DmabufFeedbackEvent &operator=(const DmabufFeedbackEvent &) = delete;
      ~DmabufFeedbackEvent() { wl_array_release(&array); }

      Type type;
      wl_array array;
      uint32_t value = 0;
      std::shared_ptr<OwnedFd> fd;
   };

   struct DmabufFeedbackCache {
      zwp_linux_dmabuf_feedback_v1 *up = nullptr;
      std::vector<DmabufFeedbackEvent> pending; // until done
      std::vector<DmabufFeedbackEvent> current;
      std::vector<wl_resource *> resources;
   };

   struct Impl {
      NestedCompositor::Upstream upstream;
      wl_event_queue *queue = nullptr;

      // Wrappers of the upstream globals, so that the objects which we create from them deliver
      // their events to our thread's queue
      wl_compositor *compositor = nullptr;
      wl_subcompositor *subcompositor = nullptr;
      wl_shm *shm = nullptr;
      wl_seat *seat = nullptr;
      wp_viewporter *viewporter = nullptr;
      wp_fractional_scale_manager_v1 *fractionalScaleManager = nullptr;
      wp_cursor_shape_manager_v1 *cursorShapeManager = nullptr;
      xdg_wm_base *wmBase = nullptr;
      zwp_linux_dmabuf_v1 *linuxDmabuf = nullptr;
      DmabufFeedbackCache dmabufFeedback;

      // Every mirror by its upstream surface, to route the input
      std::unordered_map<wl_surface *, Mirror *> mirrors;
      std::unordered_map<wl_surface *, XdgSurface *> frames;
      XdgSurface *pointerFrame = nullptr; // the title bar under the pointer

      // The input
      wl_pointer *pointer = nullptr;
      wp_cursor_shape_device_v1 *cursorShapeDevice = nullptr;
      Mirror *pointerMirror = nullptr;
      Surface *pointerFocus = nullptr; // where we sent enter

      wl_keyboard *keyboard = nullptr;
      wl_surface *keyboardUp = nullptr;   // where the system compositor's keyboard focus is
      Place *keyboardPlace = nullptr;     // within the host's window, the place clicked last
      Surface *keyboardFocus = nullptr;   // where we sent enter
      int keymapFd = -1;
      uint32_t keymapFormat = 0;
      uint32_t keymapSize = 0;
      int32_t repeatRate = 25;
      int32_t repeatDelay = 600;
      uint32_t modifiers[4] = {}; // depressed, latched, locked, group

      // The plugins give our serials back, for example to move a window: they map to the
      // system compositor's serials of the same input events
      std::deque<std::pair<uint32_t, uint32_t>> serials; // ours, upstream
      uint32_t lastUpstreamSerial = 0;

      wl_display *server = nullptr;
      std::vector<wl_resource *> pointerResources;
      std::vector<wl_resource *> keyboardResources;

      std::thread thread;
      int wakeFd = -1;
      std::atomic<bool> isStopping = false;
      std::mutex tasksMutex;
      std::vector<std::function<void()>> tasks;

      std::vector<std::unique_ptr<Place>> places; // only touched by our thread
      int32_t parentOriginX = 0;
      int32_t parentOriginY = 0;

      void post(std::function<void()> task) {
         {
            std::lock_guard lock(tasksMutex);
            tasks.push_back(std::move(task));
         }
         const uint64_t one = 1;
         [[maybe_unused]] auto n = write(wakeFd, &one, sizeof(one));
      }

      void run();
      void runTasks();
      void teardown();

      void createGlobals();
      Place *createPlace(int32_t x, int32_t y);
      void setParentOrigin(int32_t x, int32_t y);
      Place *findPlace(const char *token) {
         for (auto &p : places)
            if (p->token == token)
               return p.get();
         return nullptr;
      }

      void initMirror(Mirror &m, bool routesInput = true);
      void destroyMirror(Mirror &m);
      Mirror &mirrorForRole(Surface &s, Mirror::Role role);

      // Toplevels
      void createToplevelUpstream(XdgSurface &xs);
      void updateFrame(XdgSurface &xs);
      void destroyFrame(XdgSurface &xs);
      void onFramePointer(XdgSurface &xs, double x, double y);
      void onFrameButton(XdgSurface &xs, uint32_t upSerial, uint32_t time, uint32_t button, uint32_t state);

      // Plugin side
      void commit(Surface &s);
      void embed(Surface &s, Place &place);
      void unembed(Surface &s);
      void sendPosition(Surface &s);
      void sendScale(Surface &s);
      void finishPendingFrames(Surface &s);
      void setPendingBuffer(Surface &s, wl_resource *buffer);
      void surfaceGone(Surface &s);

      uint32_t mapSerial(uint32_t upstreamSerial);
      uint32_t upstreamSerial(uint32_t ourSerial) const;

      // System compositor side
      void onPointerEnter(uint32_t serial, wl_surface *surface, wl_fixed_t x, wl_fixed_t y);
      void onPointerLeave();
      void sendPointerLeave();
      template <class Fn>
      void forEachFocusedPointer(Fn fn) {
         forEachResourceOf(pointerFocus, pointerResources, fn);
      }

      void updateKeyboardFocus();
      void sendKeymap(wl_resource *keyboardRes);
      void sendKeyboardEnter(wl_resource *keyboardRes);
      template <class Fn>
      void forEachFocusedKeyboard(Fn fn) {
         forEachResourceOf(keyboardFocus, keyboardResources, fn);
      }

      // The resources which belong to the client of a surface
      template <class Fn>
      void forEachResourceOf(Surface *s, const std::vector<wl_resource *> &resources, Fn fn) {
         if (!s)
            return;
         wl_client *client = wl_resource_get_client(s->res);
         for (auto r : std::vector(resources)) // fn may destroy resources
            if (wl_resource_get_client(r) == client)
               fn(r);
      }
   };

   std::string randomToken() {
      std::random_device rd;
      static const char hex[] = "0123456789abcdef";
      std::string token;
      for (int i = 0; i < 32; ++i)
         token += hex[rd() % 16];
      return token;
   }

   template <class T>
   T *wrap(T *proxy, wl_event_queue *queue) {
      if (!proxy)
         return nullptr;
      auto wrapper = static_cast<T *>(wl_proxy_create_wrapper(proxy));
      wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(wrapper), queue);
      return wrapper;
   }

   template <class T>
   void unwrap(T *&wrapper) {
      if (wrapper)
         wl_proxy_wrapper_destroy(wrapper);
      wrapper = nullptr;
   }

   /////////////
   // Serials //
   /////////////

   uint32_t Impl::mapSerial(uint32_t upstream) {
      const uint32_t ours = wl_display_next_serial(server);
      serials.emplace_back(ours, upstream);
      if (serials.size() > 256)
         serials.pop_front();
      lastUpstreamSerial = upstream;
      return ours;
   }

   uint32_t Impl::upstreamSerial(uint32_t ours) const {
      for (auto it = serials.rbegin(); it != serials.rend(); ++it)
         if (it->first == ours)
            return it->second;
      // Too old, or not one of ours: the compositor will judge the latest input event
      return lastUpstreamSerial;
   }

   /////////////
   // Mirrors //
   /////////////

   const wl_surface_listener mirrorSurfaceListener = {
      .enter = [](void *, wl_surface *, wl_output *) {},
      .leave = [](void *, wl_surface *, wl_output *) {},
      .preferred_buffer_scale =
         [](void *data, wl_surface *, int32_t factor) {
            auto m = static_cast<Mirror *>(data);
            m->bufferScale = factor;
            if (m->shown)
               m->impl->sendScale(*m->shown);
         },
      .preferred_buffer_transform = [](void *, wl_surface *, uint32_t) {},
   };

   const wp_fractional_scale_v1_listener mirrorFractionalScaleListener = {
      .preferred_scale =
         [](void *data, wp_fractional_scale_v1 *, uint32_t scale120) {
            auto m = static_cast<Mirror *>(data);
            m->scale120 = scale120;
            auto &impl = *m->impl;
            if (auto it = impl.frames.find(m->up); it != impl.frames.end())
               impl.updateFrame(*it->second); // a title bar
            else if (m->shown)
               impl.sendScale(*m->shown);
         },
   };

   void Impl::initMirror(Mirror &m, bool routesInput) {
      m.impl = this;
      m.up = wl_compositor_create_surface(compositor);
      wl_surface_add_listener(m.up, &mirrorSurfaceListener, &m);
      if (viewporter)
         m.viewport = wp_viewporter_get_viewport(viewporter, m.up);
      if (fractionalScaleManager) {
         m.fractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(fractionalScaleManager, m.up);
         wp_fractional_scale_v1_add_listener(m.fractionalScale, &mirrorFractionalScaleListener, &m);
      }
      if (routesInput)
         mirrors[m.up] = &m;
   }

   Mirror &Impl::mirrorForRole(Surface &s, Mirror::Role role) {
      if (s.ownMirror && s.ownMirror->role != Mirror::Role::none && s.ownMirror->role != role) {
         if (pointerFocus == &s)
            sendPointerLeave();
         destroyMirror(*s.ownMirror);
         s.ownMirror.reset();
      }
      if (!s.ownMirror) {
         s.ownMirror = std::make_unique<Mirror>();
         initMirror(*s.ownMirror);
         s.ownMirror->shown = &s;
      }
      s.ownMirror->role = role;
      return *s.ownMirror;
   }

   void Impl::destroyMirror(Mirror &m) {
      if (pointerMirror == &m)
         pointerMirror = nullptr;
      if (keyboardUp == m.up)
         keyboardUp = nullptr;
      mirrors.erase(m.up);
      if (m.fractionalScale)
         wp_fractional_scale_v1_destroy(m.fractionalScale);
      if (m.viewport)
         wp_viewport_destroy(m.viewport);
      wl_surface_destroy(m.up);
      m = {};
   }

   ////////////////////////////////////////////
   // Upstream (system compositor) listeners //
   ////////////////////////////////////////////

   extern const xdg_surface_listener upXdgSurfaceListener;
   extern const xdg_toplevel_listener upToplevelListener;

   const wl_buffer_listener upBufferListener = {
      .release = [](void *data, wl_buffer *) { wl_buffer_send_release(static_cast<Buffer *>(data)->res); },
   };

   const wl_callback_listener upFrameListener = {
      .done =
         [](void *data, wl_callback *, uint32_t time) {
            auto cb = static_cast<FrameCallback *>(data);
            wl_callback_send_done(cb->res, time);
            wl_resource_destroy(cb->res); // which destroys cb
         },
   };

   Impl &implOf(void *data) { return *static_cast<Impl *>(data); }

   const wl_pointer_listener upPointerListener = {
      .enter =
         [](void *data, wl_pointer *, uint32_t serial, wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
            implOf(data).onPointerEnter(serial, surface, x, y);
         },
      .leave = [](void *data, wl_pointer *, uint32_t, wl_surface *) { implOf(data).onPointerLeave(); },
      .motion =
         [](void *data, wl_pointer *, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
            auto &impl = implOf(data);
            if (impl.pointerFrame)
               impl.onFramePointer(*impl.pointerFrame, wl_fixed_to_double(x), wl_fixed_to_double(y));
            impl.forEachFocusedPointer([&](wl_resource *r) { wl_pointer_send_motion(r, time, x, y); });
         },
      .button =
         [](void *data, wl_pointer *, uint32_t upSerial, uint32_t time, uint32_t button, uint32_t state) {
            auto &impl = implOf(data);
            if (impl.pointerFrame) {
               impl.onFrameButton(*impl.pointerFrame, upSerial, time, button, state);
               return;
            }
            // Within the host's window, a click into a place gives it the keys, and a click
            // elsewhere takes them. The plugins' own windows have the focus as the system decides.
            if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
               auto m = impl.pointerMirror;
               const bool isOwnWindow = m && m->shown && m->shown->ownMirror.get() == m;
               if (!isOwnWindow) {
                  impl.keyboardPlace = nullptr;
                  for (auto &p : impl.places)
                     if (&p->mirror == m)
                        impl.keyboardPlace = p.get();
                  impl.updateKeyboardFocus();
               }
            }
            const uint32_t serial = impl.mapSerial(upSerial);
            impl.forEachFocusedPointer(
               [&](wl_resource *r) { wl_pointer_send_button(r, serial, time, button, state); });
         },
      .axis =
         [](void *data, wl_pointer *, uint32_t time, uint32_t axis, wl_fixed_t value) {
            implOf(data).forEachFocusedPointer([&](wl_resource *r) { wl_pointer_send_axis(r, time, axis, value); });
         },
      .frame =
         [](void *data, wl_pointer *) {
            implOf(data).forEachFocusedPointer([&](wl_resource *r) {
               if (wl_resource_get_version(r) >= WL_POINTER_FRAME_SINCE_VERSION)
                  wl_pointer_send_frame(r);
            });
         },
      .axis_source =
         [](void *data, wl_pointer *, uint32_t source) {
            implOf(data).forEachFocusedPointer([&](wl_resource *r) {
               if (wl_resource_get_version(r) >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
                  wl_pointer_send_axis_source(r, source);
            });
         },
      .axis_stop =
         [](void *data, wl_pointer *, uint32_t time, uint32_t axis) {
            implOf(data).forEachFocusedPointer([&](wl_resource *r) {
               if (wl_resource_get_version(r) >= WL_POINTER_AXIS_STOP_SINCE_VERSION)
                  wl_pointer_send_axis_stop(r, time, axis);
            });
         },
      .axis_discrete =
         [](void *data, wl_pointer *, uint32_t axis, int32_t discrete) {
            implOf(data).forEachFocusedPointer([&](wl_resource *r) {
               if (wl_resource_get_version(r) >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
                  wl_pointer_send_axis_discrete(r, axis, discrete);
            });
         },
   };

   void Impl::onPointerEnter(uint32_t serial, wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
      sendPointerLeave();
      if (auto f = frames.find(surface); f != frames.end()) {
         // One of our title bars: we handle it ourselves
         pointerMirror = nullptr;
         pointerFrame = f->second;
         lastUpstreamSerial = serial;
         if (cursorShapeDevice)
            wp_cursor_shape_device_v1_set_shape(cursorShapeDevice, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
         onFramePointer(*pointerFrame, wl_fixed_to_double(x), wl_fixed_to_double(y));
         return;
      }
      auto it = mirrors.find(surface);
      pointerMirror = it != mirrors.end() ? it->second : nullptr;
      if (!pointerMirror || !pointerMirror->shown)
         return;

      // The plugin may set its own cursor through us later; start with the default one
      if (cursorShapeDevice)
         wp_cursor_shape_device_v1_set_shape(cursorShapeDevice, serial, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);

      pointerFocus = pointerMirror->shown;
      const uint32_t ourSerial = mapSerial(serial);
      forEachFocusedPointer([&](wl_resource *r) { wl_pointer_send_enter(r, ourSerial, pointerFocus->res, x, y); });
   }

   void Impl::sendPointerLeave() {
      const uint32_t serial = wl_display_next_serial(server);
      forEachFocusedPointer([&](wl_resource *r) { wl_pointer_send_leave(r, serial, pointerFocus->res); });
      pointerFocus = nullptr;
   }

   void Impl::onPointerLeave() {
      sendPointerLeave();
      pointerMirror = nullptr;
      if (auto xs = std::exchange(pointerFrame, nullptr); xs && xs->frame) {
         xs->frame->isPointerInside = false;
         xs->frame->state.isCloseHovered = false;
         xs->frame->state.isClosePressed = false;
         updateFrame(*xs);
      }
   }

   //////////////
   // Keyboard //
   //////////////

   void Impl::sendKeymap(wl_resource *r) {
      if (keymapFd >= 0)
         wl_keyboard_send_keymap(r, keymapFormat, keymapFd, keymapSize); // dups the fd
      if (wl_resource_get_version(r) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
         wl_keyboard_send_repeat_info(r, repeatRate, repeatDelay);
   }

   void Impl::sendKeyboardEnter(wl_resource *r) {
      wl_array keys;
      wl_array_init(&keys); // we don't track the held keys: the plugin starts with none
      wl_keyboard_send_enter(r, mapSerial(lastUpstreamSerial), keyboardFocus->res, &keys);
      wl_array_release(&keys);
      wl_keyboard_send_modifiers(
         r, wl_display_next_serial(server), modifiers[0], modifiers[1], modifiers[2], modifiers[3]);
   }

   // Sends leave and enter as the focus changes, see keyboardUp and keyboardPlace
   void Impl::updateKeyboardFocus() {
      Surface *target = nullptr;
      if (keyboardUp) {
         auto it = mirrors.find(keyboardUp);
         if (it != mirrors.end() && it->second->shown && it->second->shown->ownMirror.get() == it->second)
            target = it->second->shown; // one of the plugins' own windows
         else if (auto f = frames.find(keyboardUp); f != frames.end())
            target = f->second->surface; // one of them, with our title bar
         else if (keyboardUp == upstream.parent && keyboardPlace)
            target = keyboardPlace->mirror.shown;
      }

      if (target == keyboardFocus)
         return;

      const uint32_t serial = wl_display_next_serial(server);
      forEachFocusedKeyboard([&](wl_resource *r) { wl_keyboard_send_leave(r, serial, keyboardFocus->res); });
      keyboardFocus = target;
      forEachFocusedKeyboard([&](wl_resource *r) { sendKeyboardEnter(r); });
   }

   const wl_keyboard_listener upKeyboardListener = {
      .keymap =
         [](void *data, wl_keyboard *, uint32_t format, int32_t fd, uint32_t size) {
            auto &impl = implOf(data);
            if (impl.keymapFd >= 0)
               close(impl.keymapFd);
            impl.keymapFd = fd;
            impl.keymapFormat = format;
            impl.keymapSize = size;
            for (auto r : impl.keyboardResources)
               wl_keyboard_send_keymap(r, format, fd, size);
         },
      .enter =
         [](void *data, wl_keyboard *, uint32_t serial, wl_surface *surface, wl_array *) {
            auto &impl = implOf(data);
            impl.lastUpstreamSerial = serial;
            // The host's window is all one surface for this: its decorations are subsurfaces
            impl.keyboardUp =
               impl.mirrors.count(surface) || impl.frames.count(surface) ? surface : impl.upstream.parent;
            impl.updateKeyboardFocus();
         },
      .leave =
         [](void *data, wl_keyboard *, uint32_t, wl_surface *) {
            auto &impl = implOf(data);
            impl.keyboardUp = nullptr;
            impl.updateKeyboardFocus();
         },
      .key =
         [](void *data, wl_keyboard *, uint32_t upSerial, uint32_t time, uint32_t key, uint32_t state) {
            auto &impl = implOf(data);
            const uint32_t serial = impl.mapSerial(upSerial);
            impl.forEachFocusedKeyboard([&](wl_resource *r) { wl_keyboard_send_key(r, serial, time, key, state); });
         },
      .modifiers =
         [](void *data, wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched, uint32_t locked,
            uint32_t group) {
            auto &impl = implOf(data);
            impl.modifiers[0] = depressed;
            impl.modifiers[1] = latched;
            impl.modifiers[2] = locked;
            impl.modifiers[3] = group;
            const uint32_t serial = wl_display_next_serial(impl.server);
            impl.forEachFocusedKeyboard([&](wl_resource *r) {
               wl_keyboard_send_modifiers(r, serial, depressed, latched, locked, group);
            });
         },
      .repeat_info =
         [](void *data, wl_keyboard *, int32_t rate, int32_t delay) {
            auto &impl = implOf(data);
            impl.repeatRate = rate;
            impl.repeatDelay = delay;
            for (auto r : impl.keyboardResources)
               if (wl_resource_get_version(r) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
                  wl_keyboard_send_repeat_info(r, rate, delay);
         },
   };

   ////////////////////////////////
   // Our compositor: wl_surface //
   ////////////////////////////////

   void Impl::setPendingBuffer(Surface &s, wl_resource *buffer) {
      if (s.pendingBuffer.buffer)
         wl_list_remove(&s.pendingBuffer.listener.link);
      s.pendingBuffer.buffer = buffer;
      if (buffer) {
         s.pendingBuffer.listener.notify = [](wl_listener *listener, void *) {
            // The plugin destroyed the buffer before committing it: it attaches nothing then
            auto ref = reinterpret_cast<BufferRef *>(listener);
            wl_list_remove(&ref->listener.link);
            ref->buffer = nullptr;
         };
         wl_resource_add_destroy_listener(buffer, &s.pendingBuffer.listener);
      }
   }

   void Impl::finishPendingFrames(Surface &s) {
      // Nothing will show this frame: don't keep the plugin waiting
      auto frames = std::move(s.pendingFrames);
      for (auto cb : frames) {
         cb->pendingOn = nullptr;
         wl_callback_send_done(cb->res, 0);
         wl_resource_destroy(cb->res);
      }
   }

   void Impl::commit(Surface &s) {
      s.current = s.pending;

      // A toplevel's objects go upstream with its first commit, now that we know its decorations
      XdgSurface *xs = s.xdg;
      Toplevel *toplevel = xs ? xs->toplevel : nullptr;
      if (toplevel && !xs->up)
         createToplevelUpstream(*xs);

      Mirror *m = s.mirror();

      if (!m) {
         // No role (yet): there is nowhere to show it. For example a cursor surface.
         s.isBufferAttached = false;
         setPendingBuffer(s, nullptr);
         finishPendingFrames(s);
         return;
      }

      auto &cur = s.current;
      if (s.isBufferAttached) {
         Buffer *b = s.pendingBuffer.buffer ? dataOf<Buffer>(s.pendingBuffer.buffer) : nullptr;
         wl_surface_attach(m->up, b ? b->up : nullptr, 0, 0);
         if (b)
            wl_surface_damage_buffer(m->up, 0, 0, INT32_MAX, INT32_MAX);
         s.isBufferAttached = false;
         setPendingBuffer(s, nullptr);

         // Its logical size, for our title bar
         s.width = b ? b->width / cur.bufferScale : 0;
         s.height = b ? b->height / cur.bufferScale : 0;
      }
      if (cur.destWidth > 0 && s.width > 0) {
         s.width = cur.destWidth;
         s.height = cur.destHeight;
      }

      if (cur.bufferScale != m->applied.bufferScale) {
         wl_surface_set_buffer_scale(m->up, cur.bufferScale);
         m->applied.bufferScale = cur.bufferScale;
      }

      if (m->viewport) {
         if (cur.destWidth != m->applied.destWidth || cur.destHeight != m->applied.destHeight) {
            wp_viewport_set_destination(m->viewport, cur.destWidth, cur.destHeight);
            m->applied.destWidth = cur.destWidth;
            m->applied.destHeight = cur.destHeight;
         }
         if (memcmp(cur.source, m->applied.source, sizeof(cur.source))) {
            wp_viewport_set_source(m->viewport, cur.source[0], cur.source[1], cur.source[2], cur.source[3]);
            memcpy(m->applied.source, cur.source, sizeof(cur.source));
         }
      }

      for (auto cb : s.pendingFrames) {
         cb->pendingOn = nullptr;
         cb->up = wl_surface_frame(m->up);
         wl_callback_add_listener(cb->up, &upFrameListener, cb);
      }
      s.pendingFrames.clear();

      wl_surface_commit(m->up);

      // The title bar follows the content
      if (xs && xs->frame)
         updateFrame(*xs);
   }

   ////////////
   // Frames //
   ////////////

   void Impl::createToplevelUpstream(XdgSurface &xs) {
      auto &s = *xs.surface;
      auto &t = *xs.toplevel;

      wl_surface *roleSurface = nullptr;
      if (t.isDecorated) {
         // Our title bar is the window, and the plugin's content a subsurface below the bar
         xs.frame = std::make_unique<Frame>();
         auto &f = *xs.frame;
         initMirror(f.mirror, false);
         f.mirror.shown = &s;
         f.state.title = t.title;
         frames[f.mirror.up] = &xs;

         auto &content = mirrorForRole(s, Mirror::Role::subsurface);
         f.content = wl_subcompositor_get_subsurface(subcompositor, content.up, f.mirror.up);
         wl_subsurface_set_position(f.content, 0, decorations::kTitleBarHeight);
         wl_subsurface_set_desync(f.content);
         roleSurface = f.mirror.up;
      } else
         roleSurface = mirrorForRole(s, Mirror::Role::xdgSurface).up;

      xs.up = xdg_wm_base_get_xdg_surface(wmBase, roleSurface);
      xdg_surface_add_listener(xs.up, &upXdgSurfaceListener, &xs);
      t.up = xdg_surface_get_toplevel(xs.up);
      xdg_toplevel_add_listener(t.up, &upToplevelListener, &t);

      // What the plugin asked for until now. Its windows stay above the host's window, unless
      // it puts them above one of its own.
      const int32_t bar = t.titleBarHeight();
      if (auto parent = t.hasParent && t.parent ? t.parent : upstream.parentToplevel)
         xdg_toplevel_set_parent(t.up, parent);
      if (!t.title.empty())
         xdg_toplevel_set_title(t.up, t.title.c_str());
      if (!t.appId.empty())
         xdg_toplevel_set_app_id(t.up, t.appId.c_str());
      if (t.minSize[0] || t.minSize[1])
         xdg_toplevel_set_min_size(t.up, t.minSize[0], t.minSize[1] ? t.minSize[1] + bar : 0);
      if (t.maxSize[0] || t.maxSize[1])
         xdg_toplevel_set_max_size(t.up, t.maxSize[0], t.maxSize[1] ? t.maxSize[1] + bar : 0);
      if (t.isMaximized)
         xdg_toplevel_set_maximized(t.up);
      if (t.isFullscreen)
         xdg_toplevel_set_fullscreen(t.up, nullptr);
      if (t.isMinimized)
         xdg_toplevel_set_minimized(t.up);
      if (!t.isDecorated && xs.hasGeometry)
         xdg_surface_set_window_geometry(xs.up, xs.geometry[0], xs.geometry[1], xs.geometry[2], xs.geometry[3]);
   }

   // Redraws the title bar if needed, and commits it with the window's geometry
   void Impl::updateFrame(XdgSurface &xs) {
      auto *f = xs.frame.get();
      if (!f || !xs.up || !xs.surface)
         return;

      auto &s = *xs.surface;
      const int32_t width = xs.hasGeometry ? xs.geometry[2] : s.width;
      const int32_t height = xs.hasGeometry ? xs.geometry[3] : s.height;
      const double scale = f->mirror.scale120 > 0 ? f->mirror.scale120 / 120.0
                           : f->mirror.bufferScale > 0 ? f->mirror.bufferScale : 1.0;

      const auto &drawn = f->drawnState;
      if (width > 0 && (width != f->drawnWidth || scale != f->drawnScale || !f->buffer ||
                        f->state.title != drawn.title || f->state.isActive != drawn.isActive ||
                        f->state.isCloseHovered != drawn.isCloseHovered ||
                        f->state.isClosePressed != drawn.isClosePressed)) {
         const int bw = std::max(1, int(std::ceil(width * scale)));
         const int bh = std::max(1, int(std::ceil(decorations::kTitleBarHeight * scale)));
         const size_t size = size_t(bw) * bh * 4;

         int fd = memfd_create("clap-wayland-host-frame", MFD_CLOEXEC);
         if (fd >= 0 && ftruncate(fd, size) == 0) {
            auto pixels = static_cast<uint32_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
            if (pixels != MAP_FAILED) {
               decorations::drawTitleBar(pixels, bw, bh, width, scale, f->state);
               munmap(pixels, size);

               auto pool = wl_shm_create_pool(shm, fd, size);
               auto buffer = wl_shm_pool_create_buffer(pool, 0, bw, bh, bw * 4, WL_SHM_FORMAT_ARGB8888);
               wl_shm_pool_destroy(pool);

               wl_surface_attach(f->mirror.up, buffer, 0, 0);
               wl_surface_damage_buffer(f->mirror.up, 0, 0, bw, bh);
               if (f->mirror.viewport)
                  wp_viewport_set_destination(f->mirror.viewport, width, decorations::kTitleBarHeight);
               else
                  wl_surface_set_buffer_scale(f->mirror.up, std::max(1, int(scale)));
               // The compositor keeps what it needs of the old one
               if (f->buffer)
                  wl_buffer_destroy(f->buffer);
               f->buffer = buffer;
               f->drawnWidth = width;
               f->drawnScale = scale;
               f->drawnState = f->state;
            }
         }
         if (fd >= 0)
            close(fd);
      }

      // The window is the bar and the content below it
      if (width > 0 && height > 0)
         xdg_surface_set_window_geometry(xs.up, 0, 0, width, decorations::kTitleBarHeight + height);
      wl_surface_commit(f->mirror.up);
   }

   void Impl::destroyFrame(XdgSurface &xs) {
      if (!xs.frame)
         return;
      auto &f = *xs.frame;
      frames.erase(f.mirror.up);
      if (pointerFrame == &xs)
         pointerFrame = nullptr;
      if (keyboardUp == f.mirror.up)
         keyboardUp = nullptr;
      if (f.content)
         wl_subsurface_destroy(f.content);
      if (f.buffer)
         wl_buffer_destroy(f.buffer);
      destroyMirror(f.mirror);
      xs.frame.reset();
   }

   void Impl::onFramePointer(XdgSurface &xs, double x, double y) {
      auto &f = *xs.frame;
      f.isPointerInside = true;
      f.pointerX = x;
      f.pointerY = y;
      const bool onClose = decorations::isOnCloseButton(x, y, f.drawnWidth);
      if (onClose != f.state.isCloseHovered) {
         f.state.isCloseHovered = onClose;
         updateFrame(xs);
      }
   }

   // The title bar works like GNOME's: drag to move, double click to maximize, right click for
   // the window menu, and the close button
   void Impl::onFrameButton(XdgSurface &xs, uint32_t upSerial, uint32_t time, uint32_t button, uint32_t state) {
      auto &f = *xs.frame;
      auto *t = xs.toplevel;
      if (!t || !t->up)
         return;
      const bool isPressed = state == WL_POINTER_BUTTON_STATE_PRESSED;
      const bool onClose = decorations::isOnCloseButton(f.pointerX, f.pointerY, f.drawnWidth);

      if (button == BTN_LEFT && isPressed) {
         if (onClose) {
            f.state.isClosePressed = true;
            updateFrame(xs);
         } else if (time - f.lastClickTime < 400) {
            f.lastClickTime = 0;
            if (t->isMaximized)
               xdg_toplevel_unset_maximized(t->up);
            else
               xdg_toplevel_set_maximized(t->up);
         } else {
            f.lastClickTime = time;
            xdg_toplevel_move(t->up, seat, upSerial);
         }
      } else if (button == BTN_LEFT && f.state.isClosePressed) {
         f.state.isClosePressed = false;
         updateFrame(xs);
         if (onClose)
            xdg_toplevel_send_close(t->res);
      } else if (button == BTN_RIGHT && isPressed)
         xdg_toplevel_show_window_menu(t->up, seat, upSerial, int32_t(f.pointerX), int32_t(f.pointerY));
   }

   void Impl::embed(Surface &s, Place &place) {
      s.place = &place;
      place.mirror.shown = &s;
      place.mirror.reset();

      sendPosition(s);
      sendScale(s);
      updateKeyboardFocus();
   }

   void Impl::unembed(Surface &s) {
      Place *p = s.place;
      if (!p)
         return;

      if (pointerFocus == &s)
         sendPointerLeave(); // the pointer stays in the place, with nothing to send the events to
      wl_surface_attach(p->mirror.up, nullptr, 0, 0);
      wl_surface_commit(p->mirror.up);
      p->mirror.shown = nullptr;
      s.place = nullptr;
      updateKeyboardFocus(); // and the place keeps the click, for its next surface
   }

   // Relative to the host window's geometry, for the plugin's popups
   void Impl::sendPosition(Surface &s) {
      if (s.place && s.embeddedRes)
         clap_embedded_surface_v1_send_position(s.embeddedRes, parentOriginX + s.place->x,
                                                parentOriginY + s.place->y);
   }

   void Impl::sendScale(Surface &s) {
      Mirror *m = s.mirror();
      if (!m)
         return;
      if (s.fractionalScaleRes && m->scale120 > 0)
         wp_fractional_scale_v1_send_preferred_scale(s.fractionalScaleRes, m->scale120);
      if (m->bufferScale > 0 && wl_resource_get_version(s.res) >= WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
         wl_surface_send_preferred_buffer_scale(s.res, m->bufferScale);
   }

   const struct wl_surface_interface surfaceImpl = {
      .destroy = destroyResource,
      .attach =
         [](wl_client *, wl_resource *res, wl_resource *buffer, int32_t, int32_t) {
            auto &s = *dataOf<Surface>(res);
            s.isBufferAttached = true;
            s.impl->setPendingBuffer(s, buffer);
         },
      .damage = [](wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {},
      .frame =
         [](wl_client *client, wl_resource *res, uint32_t id) {
            auto &s = *dataOf<Surface>(res);
            auto cb = new FrameCallback;
            cb->pendingOn = &s;
            cb->res = wl_resource_create(client, &wl_callback_interface, 1, id);
            wl_resource_set_implementation(cb->res, nullptr, cb, [](wl_resource *r) {
               auto cb = dataOf<FrameCallback>(r);
               if (cb->up)
                  wl_callback_destroy(cb->up);
               if (cb->pendingOn)
                  std::erase(cb->pendingOn->pendingFrames, cb);
               delete cb;
            });
            s.pendingFrames.push_back(cb);
         },
      .set_opaque_region = [](wl_client *, wl_resource *, wl_resource *) {},
      .set_input_region = [](wl_client *, wl_resource *, wl_resource *) {},
      .commit = [](wl_client *, wl_resource *res) { dataOf<Surface>(res)->impl->commit(*dataOf<Surface>(res)); },
      .set_buffer_transform = [](wl_client *, wl_resource *, int32_t) {},
      .set_buffer_scale =
         [](wl_client *, wl_resource *res, int32_t scale) {
            if (scale < 1) {
               wl_resource_post_error(res, WL_SURFACE_ERROR_INVALID_SCALE, "invalid scale %d", scale);
               return;
            }
            dataOf<Surface>(res)->pending.bufferScale = scale;
         },
      .damage_buffer = [](wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {},
      .offset = [](wl_client *, wl_resource *, int32_t, int32_t) {},
   };

   void destroyXdgSurfaceUpstream(XdgSurface &xs);

   void Impl::surfaceGone(Surface &s) {
      if (pointerFocus == &s)
         pointerFocus = nullptr; // no leave: its resource is gone
      if (keyboardFocus == &s)
         keyboardFocus = nullptr;
      unembed(s);
      // The system compositor wants the role objects gone before the surface
      if (s.xdg) {
         destroyXdgSurfaceUpstream(*s.xdg);
         s.xdg->surface = nullptr;
      }
      if (s.ownMirror)
         destroyMirror(*s.ownMirror);
   }

   void destroySurface(wl_resource *res) {
      auto s = dataOf<Surface>(res);
      auto &impl = *s->impl;
      impl.surfaceGone(*s);
      impl.setPendingBuffer(*s, nullptr);
      impl.finishPendingFrames(*s);
      // These become inert
      for (auto r : {s->embeddedRes, s->viewportRes, s->fractionalScaleRes})
         if (r)
            wl_resource_set_user_data(r, nullptr);
      delete s;
   }

   const struct wl_region_interface regionImpl = {
      .destroy = destroyResource,
      .add = [](wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {},
      .subtract = [](wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {},
   };

   const struct wl_compositor_interface compositorImpl = {
      .create_surface =
         [](wl_client *client, wl_resource *res, uint32_t id) {
            auto s = new Surface;
            s->impl = dataOf<Impl>(res);
            s->pendingBuffer.owner = s;
            s->res = wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(res), id);
            wl_resource_set_implementation(s->res, &surfaceImpl, s, destroySurface);
         },
      .create_region =
         [](wl_client *client, wl_resource *res, uint32_t id) {
            auto r = wl_resource_create(client, &wl_region_interface, 1, id);
            wl_resource_set_implementation(r, &regionImpl, nullptr, nullptr);
         },
   };

   ////////////
   // wl_shm //
   ////////////

   const struct wl_buffer_interface bufferImpl = {.destroy = destroyResource};

   const struct wl_shm_pool_interface shmPoolImpl = {
      .create_buffer =
         [](wl_client *client, wl_resource *res, uint32_t id, int32_t offset, int32_t width, int32_t height,
            int32_t stride, uint32_t format) {
            // The system compositor validates the parameters too, but would kill the host's
            // connection rather than the plugin's: check them first
            if (width <= 0 || height <= 0 || stride < width * 4 || offset < 0) {
               wl_resource_post_error(res, WL_SHM_ERROR_INVALID_STRIDE, "invalid buffer parameters");
               return;
            }
            auto pool = dataOf<wl_shm_pool>(res);
            auto b = new Buffer;
            b->res = wl_resource_create(client, &wl_buffer_interface, 1, id);
            b->width = width;
            b->height = height;
            b->up = wl_shm_pool_create_buffer(pool, offset, width, height, stride, format);
            wl_buffer_add_listener(b->up, &upBufferListener, b);
            wl_resource_set_implementation(b->res, &bufferImpl, b, [](wl_resource *r) {
               auto b = dataOf<Buffer>(r);
               wl_buffer_destroy(b->up);
               delete b;
            });
         },
      .destroy = destroyResource,
      .resize = [](wl_client *, wl_resource *res, int32_t size) { wl_shm_pool_resize(dataOf<wl_shm_pool>(res), size); },
   };

   const struct wl_shm_interface shmImpl = {
      .create_pool =
         [](wl_client *client, wl_resource *res, uint32_t id, int32_t fd, int32_t size) {
            auto &impl = *dataOf<Impl>(res);
            // Shared as is with the system compositor: wl_shm_create_pool() dups the fd
            auto pool = wl_shm_create_pool(impl.shm, fd, size);
            close(fd);
            auto r = wl_resource_create(client, &wl_shm_pool_interface, 1, id);
            wl_resource_set_implementation(r, &shmPoolImpl, pool,
                                           [](wl_resource *r) { wl_shm_pool_destroy(dataOf<wl_shm_pool>(r)); });
         },
      .release = destroyResource,
   };

   /////////////////////////
   // zwp_linux_dmabuf_v1 //
   /////////////////////////

   // GPU buffers are fds too, so they are forwarded like wl_shm's pools: the system compositor
   // reads the plugin's memory, and we never touch it. The feedback tells the plugin's driver
   // which GPU, formats and modifiers to use: we forward the system compositor's as is.
   //
   // The system compositor validates the buffers too, but its protocol errors would end the
   // host's connection: what we can check, we check first. It can't tell us the formats it
   // didn't advertise, though, so a plugin which ignores the feedback can still bring the host
   // down.

   constexpr uint32_t kMaxDmabufPlanes = 4;

   struct DmabufParams {
      wl_resource *res = nullptr; // null once the plugin destroyed it
      zwp_linux_buffer_params_v1 *up = nullptr;
      uint32_t planes = 0; // bit mask of the added planes
      bool isUsed = false;
      bool isWaitingForUpstream = false; // create(): until created or failed
      int32_t width = 0;
      int32_t height = 0;
   };

   void destroyDmabufParams(DmabufParams *p) {
      zwp_linux_buffer_params_v1_destroy(p->up);
      delete p;
   }

   wl_resource *createBufferResource(wl_client *client, uint32_t id, wl_buffer *up, int32_t width, int32_t height) {
      auto b = new Buffer;
      b->res = wl_resource_create(client, &wl_buffer_interface, 1, id);
      b->up = up;
      b->width = width;
      b->height = height;
      wl_buffer_add_listener(up, &upBufferListener, b);
      wl_resource_set_implementation(b->res, &bufferImpl, b, [](wl_resource *r) {
         auto b = dataOf<Buffer>(r);
         wl_buffer_destroy(b->up);
         delete b;
      });
      return b->res;
   }

   const zwp_linux_buffer_params_v1_listener upDmabufParamsListener = {
      .created =
         [](void *data, zwp_linux_buffer_params_v1 *, wl_buffer *up) {
            auto p = static_cast<DmabufParams *>(data);
            p->isWaitingForUpstream = false;
            if (!p->res) {
               wl_buffer_destroy(up);
               destroyDmabufParams(p);
               return;
            }
            // A buffer which the server creates: id 0
            auto r = createBufferResource(wl_resource_get_client(p->res), 0, up, p->width, p->height);
            zwp_linux_buffer_params_v1_send_created(p->res, r);
         },
      .failed =
         [](void *data, zwp_linux_buffer_params_v1 *) {
            auto p = static_cast<DmabufParams *>(data);
            p->isWaitingForUpstream = false;
            if (!p->res) {
               destroyDmabufParams(p);
               return;
            }
            zwp_linux_buffer_params_v1_send_failed(p->res);
         },
   };

   // Checks what the system compositor would refuse with a protocol error
   bool checkDmabufCreate(wl_resource *res, DmabufParams &p, int32_t width, int32_t height) {
      if (p.isUsed) {
         wl_resource_post_error(res, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "the params were already used");
         return false;
      }
      p.isUsed = true;
      // The planes must be 0..n-1
      if (!p.planes || (p.planes & (p.planes + 1))) {
         wl_resource_post_error(res, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "missing planes");
         return false;
      }
      if (width <= 0 || height <= 0) {
         wl_resource_post_error(res, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS, "invalid size");
         return false;
      }
      p.width = width;
      p.height = height;
      return true;
   }

   const struct zwp_linux_buffer_params_v1_interface dmabufParamsImpl = {
      .destroy = destroyResource,
      .add =
         [](wl_client *, wl_resource *res, int32_t fd, uint32_t planeIdx, uint32_t offset, uint32_t stride,
            uint32_t modifierHi, uint32_t modifierLo) {
            auto &p = *dataOf<DmabufParams>(res);
            if (p.isUsed)
               wl_resource_post_error(res, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "the params were already used");
            else if (planeIdx >= kMaxDmabufPlanes)
               wl_resource_post_error(res, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX, "plane index too large");
            else if (p.planes & (1u << planeIdx))
               wl_resource_post_error(res, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET, "plane already set");
            else {
               p.planes |= 1u << planeIdx;
               // Marshalling dups the fd
               zwp_linux_buffer_params_v1_add(p.up, fd, planeIdx, offset, stride, modifierHi, modifierLo);
            }
            close(fd);
         },
      .create =
         [](wl_client *, wl_resource *res, int32_t width, int32_t height, uint32_t format, uint32_t flags) {
            auto &p = *dataOf<DmabufParams>(res);
            if (!checkDmabufCreate(res, p, width, height))
               return;
            p.isWaitingForUpstream = true;
            zwp_linux_buffer_params_v1_create(p.up, width, height, format, flags);
         },
      .create_immed =
         [](wl_client *client, wl_resource *res, uint32_t id, int32_t width, int32_t height, uint32_t format,
            uint32_t flags) {
            auto &p = *dataOf<DmabufParams>(res);
            if (!checkDmabufCreate(res, p, width, height))
               return;
            // If the system compositor refuses it, it sends failed... to our params, while the
            // plugin's buffer lives on: the plugin's later use of it is then the problem of the
            // system compositor, which only sees a dead buffer
            auto up = zwp_linux_buffer_params_v1_create_immed(p.up, width, height, format, flags);
            createBufferResource(client, id, up, width, height);
         },
   };

   const struct zwp_linux_dmabuf_feedback_v1_interface dmabufFeedbackImpl = {.destroy = destroyResource};

   void sendDmabufFeedback(Impl &impl, wl_resource *r) {
      for (auto &e : impl.dmabufFeedback.current) {
         switch (e.type) {
         case DmabufFeedbackEvent::FormatTable:
            // Marshalling dups the fd, which the cache keeps
            zwp_linux_dmabuf_feedback_v1_send_format_table(r, e.fd->fd, e.value);
            break;
         case DmabufFeedbackEvent::MainDevice:
            zwp_linux_dmabuf_feedback_v1_send_main_device(r, &e.array);
            break;
         case DmabufFeedbackEvent::TrancheTargetDevice:
            zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(r, &e.array);
            break;
         case DmabufFeedbackEvent::TrancheFormats:
            zwp_linux_dmabuf_feedback_v1_send_tranche_formats(r, &e.array);
            break;
         case DmabufFeedbackEvent::TrancheFlags:
            zwp_linux_dmabuf_feedback_v1_send_tranche_flags(r, e.value);
            break;
         case DmabufFeedbackEvent::TrancheDone:
            zwp_linux_dmabuf_feedback_v1_send_tranche_done(r);
            break;
         }
      }
      zwp_linux_dmabuf_feedback_v1_send_done(r);
   }

   // The system compositor's default feedback, which we fetch once and cache: the plugins' drivers
   // expect it within a roundtrip, which we would answer before the system compositor does
   const zwp_linux_dmabuf_feedback_v1_listener upDmabufFeedbackListener = {
      .done =
         [](void *data, zwp_linux_dmabuf_feedback_v1 *) {
            auto &impl = *static_cast<Impl *>(data);
            auto &cache = impl.dmabufFeedback;
            cache.current = std::move(cache.pending);
            cache.pending.clear();
            for (auto r : cache.resources)
               sendDmabufFeedback(impl, r);
         },
      .format_table =
         [](void *data, zwp_linux_dmabuf_feedback_v1 *, int32_t fd, uint32_t size) {
            auto &e = static_cast<Impl *>(data)->dmabufFeedback.pending.emplace_back(DmabufFeedbackEvent::FormatTable);
            e.fd = std::make_shared<OwnedFd>(fd);
            e.value = size;
         },
      .main_device =
         [](void *data, zwp_linux_dmabuf_feedback_v1 *, wl_array *device) {
            static_cast<Impl *>(data)->dmabufFeedback.pending.emplace_back(DmabufFeedbackEvent::MainDevice, device);
         },
      .tranche_done =
         [](void *data, zwp_linux_dmabuf_feedback_v1 *) {
            static_cast<Impl *>(data)->dmabufFeedback.pending.emplace_back(DmabufFeedbackEvent::TrancheDone);
         },
      .tranche_target_device =
         [](void *data, zwp_linux_dmabuf_feedback_v1 *, wl_array *device) {
            static_cast<Impl *>(data)->dmabufFeedback.pending.emplace_back(DmabufFeedbackEvent::TrancheTargetDevice,
                                                                           device);
         },
      .tranche_formats =
         [](void *data, zwp_linux_dmabuf_feedback_v1 *, wl_array *indices) {
            static_cast<Impl *>(data)->dmabufFeedback.pending.emplace_back(DmabufFeedbackEvent::TrancheFormats,
                                                                           indices);
         },
      .tranche_flags =
         [](void *data, zwp_linux_dmabuf_feedback_v1 *, uint32_t flags) {
            auto &e = static_cast<Impl *>(data)->dmabufFeedback.pending.emplace_back(DmabufFeedbackEvent::TrancheFlags);
            e.value = flags;
         },
   };

   // Surface feedback would only add the tranches for direct scanout, which matter little for a
   // plugin's GUI: every feedback is the default one
   void createDmabufFeedback(wl_client *client, wl_resource *res, uint32_t id) {
      auto &impl = *dataOf<Impl>(res);
      auto r = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(res), id);
      wl_resource_set_implementation(r, &dmabufFeedbackImpl, &impl, [](wl_resource *r) {
         auto &resources = dataOf<Impl>(r)->dmabufFeedback.resources;
         std::erase(resources, r);
      });
      impl.dmabufFeedback.resources.push_back(r);
      // Otherwise it comes with the system compositor's
      if (!impl.dmabufFeedback.current.empty())
         sendDmabufFeedback(impl, r);
   }

   const struct zwp_linux_dmabuf_v1_interface linuxDmabufImpl = {
      .destroy = destroyResource,
      .create_params =
         [](wl_client *client, wl_resource *res, uint32_t id) {
            auto &impl = *dataOf<Impl>(res);
            auto p = new DmabufParams;
            p->res = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(res), id);
            p->up = zwp_linux_dmabuf_v1_create_params(impl.linuxDmabuf);
            zwp_linux_buffer_params_v1_add_listener(p->up, &upDmabufParamsListener, p);
            wl_resource_set_implementation(p->res, &dmabufParamsImpl, p, [](wl_resource *r) {
               auto p = dataOf<DmabufParams>(r);
               // Until the system compositor answers create(), its answer refers to the params
               p->res = nullptr;
               if (!p->isWaitingForUpstream)
                  destroyDmabufParams(p);
            });
         },
      .get_default_feedback = createDmabufFeedback,
      .get_surface_feedback = [](wl_client *client, wl_resource *res, uint32_t id,
                                 wl_resource *) { createDmabufFeedback(client, res, id); },
   };

   /////////////
   // wl_seat //
   /////////////

   const struct wl_pointer_interface pointerImpl = {
      .set_cursor = [](wl_client *, wl_resource *, uint32_t, wl_resource *, int32_t, int32_t) {},
      .release = destroyResource,
   };

   const struct wl_seat_interface seatImpl = {
      .get_pointer =
         [](wl_client *client, wl_resource *res, uint32_t id) {
            auto &impl = *dataOf<Impl>(res);
            auto r = wl_resource_create(client, &wl_pointer_interface, wl_resource_get_version(res), id);
            wl_resource_set_implementation(r, &pointerImpl, &impl, [](wl_resource *r) {
               std::erase(dataOf<Impl>(r)->pointerResources, r);
            });
            impl.pointerResources.push_back(r);
         },
      .get_keyboard =
         [](wl_client *client, wl_resource *res, uint32_t id) {
            static const struct wl_keyboard_interface keyboardImpl = {.release = destroyResource};
            auto &impl = *dataOf<Impl>(res);
            auto r = wl_resource_create(client, &wl_keyboard_interface, wl_resource_get_version(res), id);
            wl_resource_set_implementation(r, &keyboardImpl, &impl, [](wl_resource *r) {
               std::erase(dataOf<Impl>(r)->keyboardResources, r);
            });
            impl.keyboardResources.push_back(r);
            impl.sendKeymap(r);
            // Its client may have the focus already
            if (impl.keyboardFocus && wl_resource_get_client(impl.keyboardFocus->res) == client)
               impl.sendKeyboardEnter(r);
         },
      // We don't advertise it, but a client may still ask: give an inert object
      .get_touch =
         [](wl_client *client, wl_resource *res, uint32_t id) {
            static const struct wl_touch_interface touchImpl = {.release = destroyResource};
            auto r = wl_resource_create(client, &wl_touch_interface, wl_resource_get_version(res), id);
            wl_resource_set_implementation(r, &touchImpl, nullptr, nullptr);
         },
      .release = destroyResource,
   };

   ///////////////////
   // wp_viewporter //
   ///////////////////

   const struct wp_viewport_interface viewportImpl = {
      .destroy = destroyResource,
      .set_source =
         [](wl_client *, wl_resource *res, wl_fixed_t x, wl_fixed_t y, wl_fixed_t w, wl_fixed_t h) {
            if (auto s = dataOf<Surface>(res)) {
               wl_fixed_t src[4] = {x, y, w, h};
               memcpy(s->pending.source, src, sizeof(src));
            }
         },
      .set_destination =
         [](wl_client *, wl_resource *res, int32_t width, int32_t height) {
            if (auto s = dataOf<Surface>(res)) {
               s->pending.destWidth = width;
               s->pending.destHeight = height;
            }
         },
   };

   const struct wp_viewporter_interface viewporterImpl = {
      .destroy = destroyResource,
      .get_viewport =
         [](wl_client *client, wl_resource *res, uint32_t id, wl_resource *surface) {
            auto s = dataOf<Surface>(surface);
            if (s->viewportRes) {
               wl_resource_post_error(res, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS, "the surface has a viewport");
               return;
            }
            auto r = wl_resource_create(client, &wp_viewport_interface, 1, id);
            wl_resource_set_implementation(r, &viewportImpl, s, [](wl_resource *r) {
               if (auto s = dataOf<Surface>(r)) {
                  s->viewportRes = nullptr;
                  s->pending.destWidth = s->pending.destHeight = -1;
                  std::fill(std::begin(s->pending.source), std::end(s->pending.source), kUnset);
               }
            });
            s->viewportRes = r;
         },
   };

   ////////////////////////////////////
   // wp_fractional_scale_manager_v1 //
   ////////////////////////////////////

   const struct wp_fractional_scale_v1_interface fractionalScaleImpl = {.destroy = destroyResource};

   const struct wp_fractional_scale_manager_v1_interface fractionalScaleManagerImpl = {
      .destroy = destroyResource,
      .get_fractional_scale =
         [](wl_client *client, wl_resource *res, uint32_t id, wl_resource *surface) {
            auto s = dataOf<Surface>(surface);
            if (s->fractionalScaleRes) {
               wl_resource_post_error(res, WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
                                      "the surface has a fractional scale");
               return;
            }
            auto r = wl_resource_create(client, &wp_fractional_scale_v1_interface, 1, id);
            wl_resource_set_implementation(r, &fractionalScaleImpl, s, [](wl_resource *r) {
               if (auto s = dataOf<Surface>(r))
                  s->fractionalScaleRes = nullptr;
            });
            s->fractionalScaleRes = r;
            s->impl->sendScale(*s);
         },
   };

   ///////////////////////////////
   // xdg-shell, toward upstream //
   ///////////////////////////////

   // The objects are forwarded one to one: the system compositor does the window management,
   // and we only translate the parents, the seats and the serials.

   const struct xdg_positioner_interface positionerImpl = {
      .destroy = destroyResource,
      .set_size = [](wl_client *, wl_resource *r, int32_t w, int32_t h) { xdg_positioner_set_size(dataOf<Positioner>(r)->up, w, h); },
      .set_anchor_rect =
         [](wl_client *, wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
            auto p = dataOf<Positioner>(r);
            p->hasAnchorRect = true;
            p->anchorRect[0] = x;
            p->anchorRect[1] = y;
            p->anchorRect[2] = w;
            p->anchorRect[3] = h;
            xdg_positioner_set_anchor_rect(p->up, x, y, w, h);
         },
      .set_anchor = [](wl_client *, wl_resource *r, uint32_t a) { xdg_positioner_set_anchor(dataOf<Positioner>(r)->up, a); },
      .set_gravity = [](wl_client *, wl_resource *r, uint32_t g) { xdg_positioner_set_gravity(dataOf<Positioner>(r)->up, g); },
      .set_constraint_adjustment =
         [](wl_client *, wl_resource *r, uint32_t c) {
            xdg_positioner_set_constraint_adjustment(dataOf<Positioner>(r)->up, c);
         },
      .set_offset = [](wl_client *, wl_resource *r, int32_t x, int32_t y) { xdg_positioner_set_offset(dataOf<Positioner>(r)->up, x, y); },
      .set_reactive = [](wl_client *, wl_resource *r) { xdg_positioner_set_reactive(dataOf<Positioner>(r)->up); },
      .set_parent_size =
         [](wl_client *, wl_resource *r, int32_t w, int32_t h) { xdg_positioner_set_parent_size(dataOf<Positioner>(r)->up, w, h); },
      .set_parent_configure =
         [](wl_client *, wl_resource *r, uint32_t serial) {
            // A configure serial of the parent, which we pass through as is
            xdg_positioner_set_parent_configure(dataOf<Positioner>(r)->up, serial);
         },
   };

   // Runs fn with the positioner's anchor below the parent's title bar, which the plugin doesn't
   // know of: the parent's geometry starts with it
   template <class Fn>
   void withAnchorBelowTitleBar(Positioner &p, int32_t offsetY, Fn fn) {
      const bool shifts = offsetY != 0 && p.hasAnchorRect;
      if (shifts)
         xdg_positioner_set_anchor_rect(p.up, p.anchorRect[0], p.anchorRect[1] + offsetY, p.anchorRect[2],
                                        p.anchorRect[3]);
      fn();
      if (shifts)
         xdg_positioner_set_anchor_rect(p.up, p.anchorRect[0], p.anchorRect[1], p.anchorRect[2], p.anchorRect[3]);
   }

   // Toward the system compositor: the role objects go before their xdg_surface, and the
   // popups from the topmost down. The plugin should destroy them in that order, but when it
   // disconnects, its resources go in any order.
   void destroyPopupUpstream(Popup &p) {
      for (auto child : p.children)
         destroyPopupUpstream(*child);
      if (p.up) {
         xdg_popup_destroy(p.up);
         p.up = nullptr;
      }
   }

   void destroyXdgSurfaceUpstream(XdgSurface &xs) {
      if (xs.toplevel && xs.toplevel->up) {
         xdg_toplevel_destroy(xs.toplevel->up);
         xs.toplevel->up = nullptr;
      }
      if (xs.popup)
         destroyPopupUpstream(*xs.popup);
      if (xs.up) {
         xdg_surface_destroy(xs.up);
         xs.up = nullptr;
      }
      // Our title bar was its surface
      xs.impl->destroyFrame(xs);
   }

   const xdg_surface_listener upXdgSurfaceListener = {
      .configure = [](void *data, xdg_surface *, uint32_t serial) {
         // The serial goes back with ack_configure on the same object, so it passes as is
         xdg_surface_send_configure(static_cast<XdgSurface *>(data)->res, serial);
      },
   };

   const xdg_toplevel_listener upToplevelListener = {
      .configure =
         [](void *data, xdg_toplevel *, int32_t w, int32_t h, wl_array *states) {
            auto &t = *static_cast<Toplevel *>(data);
            bool isActive = false;
            t.isMaximized = false;
            for (auto state = static_cast<uint32_t *>(states->data);
                 (char *)state < (char *)states->data + states->size; ++state) {
               if (*state == XDG_TOPLEVEL_STATE_ACTIVATED)
                  isActive = true;
               if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED)
                  t.isMaximized = true;
            }

            // The size is the window's, our title bar included
            const int32_t bar = t.titleBarHeight();
            xdg_toplevel_send_configure(t.res, w, h > bar ? h - bar : h, states);

            if (t.xs && t.xs->frame && t.xs->frame->state.isActive != isActive) {
               t.xs->frame->state.isActive = isActive;
               t.impl->updateFrame(*t.xs);
            }
         },
      .close = [](void *data, xdg_toplevel *) { xdg_toplevel_send_close(static_cast<Toplevel *>(data)->res); },
      .configure_bounds =
         [](void *data, xdg_toplevel *, int32_t w, int32_t h) {
            auto &t = *static_cast<Toplevel *>(data);
            if (wl_resource_get_version(t.res) >= XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION)
               xdg_toplevel_send_configure_bounds(t.res, w, h > t.titleBarHeight() ? h - t.titleBarHeight() : h);
         },
      .wm_capabilities =
         [](void *data, xdg_toplevel *, wl_array *capabilities) {
            auto res = static_cast<Toplevel *>(data)->res;
            if (wl_resource_get_version(res) >= XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
               xdg_toplevel_send_wm_capabilities(res, capabilities);
         },
   };

   const xdg_popup_listener upPopupListener = {
      .configure =
         [](void *data, xdg_popup *, int32_t x, int32_t y, int32_t w, int32_t h) {
            auto p = static_cast<Popup *>(data);
            xdg_popup_send_configure(p->res, x, y - p->parentOffsetY, w, h);
         },
      .popup_done = [](void *data, xdg_popup *) { xdg_popup_send_popup_done(static_cast<Popup *>(data)->res); },
      .repositioned =
         [](void *data, xdg_popup *, uint32_t token) {
            auto res = static_cast<Popup *>(data)->res;
            if (wl_resource_get_version(res) >= XDG_POPUP_REPOSITIONED_SINCE_VERSION)
               xdg_popup_send_repositioned(res, token);
         },
   };

   Toplevel &toplevelOf(wl_resource *r) { return *dataOf<Toplevel>(r); }

   const struct xdg_toplevel_interface toplevelImpl = {
      .destroy = destroyResource,
      .set_parent =
         [](wl_client *, wl_resource *r, wl_resource *parent) {
            // One of the plugin's windows, or the host's window
            auto &t = toplevelOf(r);
            t.hasParent = true;
            t.parent = parent && kindOf(parent) == Kind::toplevel ? dataOf<Toplevel>(parent)->up : nullptr;
            if (t.up)
               xdg_toplevel_set_parent(t.up, t.parent ? t.parent : t.impl->upstream.parentToplevel);
         },
      .set_title =
         [](wl_client *, wl_resource *r, const char *title) {
            auto &t = toplevelOf(r);
            t.title = title;
            if (t.up)
               xdg_toplevel_set_title(t.up, title);
            if (t.xs && t.xs->frame) {
               t.xs->frame->state.title = title;
               t.impl->updateFrame(*t.xs);
            }
         },
      .set_app_id =
         [](wl_client *, wl_resource *r, const char *appId) {
            auto &t = toplevelOf(r);
            t.appId = appId;
            if (t.up)
               xdg_toplevel_set_app_id(t.up, appId);
         },
      .show_window_menu =
         [](wl_client *, wl_resource *r, wl_resource *, uint32_t serial, int32_t x, int32_t y) {
            auto &t = toplevelOf(r);
            if (t.up)
               xdg_toplevel_show_window_menu(t.up, t.impl->seat, t.impl->upstreamSerial(serial), x,
                                             y + t.titleBarHeight());
         },
      .move =
         [](wl_client *, wl_resource *r, wl_resource *, uint32_t serial) {
            auto &t = toplevelOf(r);
            if (t.up)
               xdg_toplevel_move(t.up, t.impl->seat, t.impl->upstreamSerial(serial));
         },
      .resize =
         [](wl_client *, wl_resource *r, wl_resource *, uint32_t serial, uint32_t edges) {
            auto &t = toplevelOf(r);
            if (t.up)
               xdg_toplevel_resize(t.up, t.impl->seat, t.impl->upstreamSerial(serial), edges);
         },
      .set_max_size =
         [](wl_client *, wl_resource *r, int32_t w, int32_t h) {
            auto &t = toplevelOf(r);
            t.maxSize[0] = w;
            t.maxSize[1] = h;
            if (t.up)
               xdg_toplevel_set_max_size(t.up, w, h ? h + t.titleBarHeight() : 0);
         },
      .set_min_size =
         [](wl_client *, wl_resource *r, int32_t w, int32_t h) {
            auto &t = toplevelOf(r);
            t.minSize[0] = w;
            t.minSize[1] = h;
            if (t.up)
               xdg_toplevel_set_min_size(t.up, w, h ? h + t.titleBarHeight() : 0);
         },
      .set_maximized =
         [](wl_client *, wl_resource *r) {
            auto &t = toplevelOf(r);
            t.isMaximized = true;
            if (t.up)
               xdg_toplevel_set_maximized(t.up);
         },
      .unset_maximized =
         [](wl_client *, wl_resource *r) {
            auto &t = toplevelOf(r);
            t.isMaximized = false;
            if (t.up)
               xdg_toplevel_unset_maximized(t.up);
         },
      // We offer no outputs, so the compositor picks one
      .set_fullscreen =
         [](wl_client *, wl_resource *r, wl_resource *) {
            auto &t = toplevelOf(r);
            t.isFullscreen = true;
            if (t.up)
               xdg_toplevel_set_fullscreen(t.up, nullptr);
         },
      .unset_fullscreen =
         [](wl_client *, wl_resource *r) {
            auto &t = toplevelOf(r);
            t.isFullscreen = false;
            if (t.up)
               xdg_toplevel_unset_fullscreen(t.up);
         },
      .set_minimized =
         [](wl_client *, wl_resource *r) {
            auto &t = toplevelOf(r);
            t.isMinimized = !t.up; // only matters before it maps
            if (t.up)
               xdg_toplevel_set_minimized(t.up);
         },
   };

   // The upstream parent's title bar, which shifts its geometry
   int32_t titleBarOffsetOf(XdgSurface *parent) { return parent && parent->frame ? decorations::kTitleBarHeight : 0; }

   const struct xdg_popup_interface popupImpl = {
      .destroy = destroyResource,
      .grab =
         [](wl_client *, wl_resource *r, wl_resource *, uint32_t serial) {
            auto p = dataOf<Popup>(r);
            if (p->up)
               xdg_popup_grab(p->up, p->impl->seat, p->impl->upstreamSerial(serial));
         },
      .reposition =
         [](wl_client *, wl_resource *r, wl_resource *positioner, uint32_t token) {
            auto p = dataOf<Popup>(r);
            if (p->up)
               withAnchorBelowTitleBar(*dataOf<Positioner>(positioner), p->parentOffsetY,
                                       [&] { xdg_popup_reposition(p->up, dataOf<Positioner>(positioner)->up, token); });
         },
   };

   const struct xdg_surface_interface xdgSurfaceImpl = {
      .destroy = destroyResource,
      .get_toplevel =
         [](wl_client *client, wl_resource *r, uint32_t id) {
            auto xs = dataOf<XdgSurface>(r);
            auto t = new Toplevel;
            t->impl = xs->impl;
            t->xs = xs;
            t->res = wl_resource_create(client, &xdg_toplevel_interface, wl_resource_get_version(r), id);
            wl_resource_set_implementation(t->res, &toplevelImpl, t, [](wl_resource *r) {
               auto t = dataOf<Toplevel>(r);
               if (t->up)
                  xdg_toplevel_destroy(t->up);
               if (t->decorationRes)
                  wl_resource_set_user_data(t->decorationRes, nullptr);
               if (t->xs)
                  t->xs->toplevel = nullptr;
               delete t;
            });
            // Its objects go upstream with the surface's first commit
            xs->toplevel = t;
         },
      .get_popup =
         [](wl_client *client, wl_resource *r, uint32_t id, wl_resource *parent, wl_resource *positioner) {
            auto xs = dataOf<XdgSurface>(r);
            auto &impl = *xs->impl;
            auto p = new Popup;
            p->impl = xs->impl;
            p->xs = xs;
            p->res = wl_resource_create(client, &xdg_popup_interface, wl_resource_get_version(r), id);
            wl_resource_set_implementation(p->res, &popupImpl, p, [](wl_resource *r) {
               auto p = dataOf<Popup>(r);
               destroyPopupUpstream(*p);
               if (p->parent)
                  std::erase(p->parent->children, p);
               for (auto child : p->children)
                  child->parent = nullptr;
               if (p->xs)
                  p->xs->popup = nullptr;
               delete p;
            });
            xs->popup = p;

            // The parent is one of the plugin's windows, or the host's window around the place
            xdg_surface *upParent = nullptr;
            if (parent && kindOf(parent) == Kind::xdgSurface) {
               auto parentXs = dataOf<XdgSurface>(parent);
               upParent = parentXs->up;
               p->parentOffsetY = titleBarOffsetOf(parentXs);
               if (parentXs->popup) {
                  p->parent = parentXs->popup;
                  parentXs->popup->children.push_back(p);
               }
            } else if (parent && kindOf(parent) == Kind::hostWindowSurface)
               upParent = impl.upstream.parentXdgSurface;

            if (!xs->surface || !upParent) {
               // Without a parent, the compositor can't place it: dismiss it right away
               xdg_popup_send_popup_done(p->res);
               return;
            }

            // A popup goes upstream right away: it has no decorations
            auto &m = impl.mirrorForRole(*xs->surface, Mirror::Role::xdgSurface);
            xs->up = xdg_wm_base_get_xdg_surface(impl.wmBase, m.up);
            xdg_surface_add_listener(xs->up, &upXdgSurfaceListener, xs);
            if (xs->hasGeometry)
               xdg_surface_set_window_geometry(xs->up, xs->geometry[0], xs->geometry[1], xs->geometry[2],
                                               xs->geometry[3]);

            auto &pos = *dataOf<Positioner>(positioner);
            withAnchorBelowTitleBar(pos, p->parentOffsetY,
                                    [&] { p->up = xdg_surface_get_popup(xs->up, upParent, pos.up); });
            xdg_popup_add_listener(p->up, &upPopupListener, p);
         },
      .set_window_geometry =
         [](wl_client *, wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h) {
            auto xs = dataOf<XdgSurface>(r);
            xs->hasGeometry = true;
            xs->geometry[0] = x;
            xs->geometry[1] = y;
            xs->geometry[2] = w;
            xs->geometry[3] = h;
            // With our title bar, the window's geometry is ours, see updateFrame()
            if (xs->up && !xs->frame)
               xdg_surface_set_window_geometry(xs->up, x, y, w, h);
         },
      .ack_configure =
         [](wl_client *, wl_resource *r, uint32_t serial) {
            if (auto up = dataOf<XdgSurface>(r)->up)
               xdg_surface_ack_configure(up, serial);
         },
   };

   const struct xdg_wm_base_interface wmBaseImpl = {
      .destroy = destroyResource,
      .create_positioner =
         [](wl_client *client, wl_resource *r, uint32_t id) {
            auto &impl = *dataOf<Impl>(r);
            auto p = new Positioner;
            p->res = wl_resource_create(client, &xdg_positioner_interface, wl_resource_get_version(r), id);
            p->up = xdg_wm_base_create_positioner(impl.wmBase);
            wl_resource_set_implementation(p->res, &positionerImpl, p, [](wl_resource *r) {
               auto p = dataOf<Positioner>(r);
               xdg_positioner_destroy(p->up);
               delete p;
            });
         },
      .get_xdg_surface =
         [](wl_client *client, wl_resource *r, uint32_t id, wl_resource *surface) {
            auto &impl = *dataOf<Impl>(r);
            auto &s = *dataOf<Surface>(surface);
            if (s.embeddedRes || s.xdg) {
               wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE, "the surface has another role");
               return;
            }

            // Its objects go upstream with its role: then we know what its window looks like
            auto xs = new XdgSurface;
            xs->impl = &impl;
            xs->surface = &s;
            xs->res = wl_resource_create(client, &xdg_surface_interface, wl_resource_get_version(r), id);
            wl_resource_set_implementation(xs->res, &xdgSurfaceImpl, xs, [](wl_resource *r) {
               auto xs = dataOf<XdgSurface>(r);
               destroyXdgSurfaceUpstream(*xs);
               if (xs->toplevel)
                  xs->toplevel->xs = nullptr;
               if (xs->popup)
                  xs->popup->xs = nullptr;
               if (xs->surface)
                  xs->surface->xdg = nullptr;
               delete xs;
            });
            s.xdg = xs;
         },
      .pong = [](wl_client *, wl_resource *, uint32_t) {}, // we never ping
   };

   //////////////////////////////////
   // zxdg_decoration_manager_v1 //
   //////////////////////////////////

   // The plugins' toplevels may ask for server-side decorations: then we draw their title bars,
   // since the system compositor may draw none
   const struct zxdg_toplevel_decoration_v1_interface toplevelDecorationImpl = {
      .destroy = destroyResource,
      .set_mode =
         [](wl_client *, wl_resource *r, uint32_t mode) {
            auto t = dataOf<Toplevel>(r);
            if (!t)
               return;
            // The mode only applies before the window maps: we don't rebuild its window
            if (!t->up)
               t->isDecorated = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
            zxdg_toplevel_decoration_v1_send_configure(r, t->isDecorated ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                                                         : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
         },
      .unset_mode =
         [](wl_client *, wl_resource *r) {
            // Our preference: we draw them
            auto t = dataOf<Toplevel>(r);
            if (!t)
               return;
            if (!t->up)
               t->isDecorated = true;
            zxdg_toplevel_decoration_v1_send_configure(r, t->isDecorated ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                                                         : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
         },
   };

   const struct zxdg_decoration_manager_v1_interface decorationManagerImpl = {
      .destroy = destroyResource,
      .get_toplevel_decoration =
         [](wl_client *client, wl_resource *r, uint32_t id, wl_resource *toplevel) {
            auto t = dataOf<Toplevel>(toplevel);
            if (t->decorationRes) {
               wl_resource_post_error(r, ZXDG_TOPLEVEL_DECORATION_V1_ERROR_ALREADY_CONSTRUCTED,
                                      "the toplevel has a decoration object");
               return;
            }
            auto d = wl_resource_create(client, &zxdg_toplevel_decoration_v1_interface, 1, id);
            wl_resource_set_implementation(d, &toplevelDecorationImpl, t, [](wl_resource *d) {
               if (auto t = dataOf<Toplevel>(d))
                  t->decorationRes = nullptr;
            });
            t->decorationRes = d;
         },
   };

   ///////////////////////////////
   // clap_surface_embedding_v1 //
   ///////////////////////////////

   // get_parent_surface and get_parent_toplevel: objects which only stand for the host's window,
   // as parents. The compositor ignores their requests but destroy, which is opcode 0 on both.
   void createHostWindowRef(wl_client *client, const wl_interface *interface, uint32_t version, uint32_t id,
                            HostWindowRef &ref) {
      auto r = wl_resource_create(client, interface, version, id);
      wl_resource_set_dispatcher(
         r,
         [](const void *, void *target, uint32_t opcode, const wl_message *, wl_argument *) {
            if (opcode == 0)
               wl_resource_destroy(static_cast<wl_resource *>(target));
            return 0;
         },
         nullptr, &ref, nullptr);
   }

   // As the plugin's xdg_wm_base, which it uses with these objects
   uint32_t xdgVersionFor(Impl &impl) { return std::max(1u, impl.upstream.wmBaseVersion); }

   const struct clap_embedded_surface_v1_interface embeddedSurfaceImpl = {
      .destroy = destroyResource,
      .get_parent_surface =
         [](wl_client *client, wl_resource *r, uint32_t id) {
            auto s = dataOf<Surface>(r);
            createHostWindowRef(client, &xdg_surface_interface, s ? xdgVersionFor(*s->impl) : 1, id,
                                hostWindowSurfaceRef);
         },
      .get_parent_toplevel =
         [](wl_client *client, wl_resource *r, uint32_t id) {
            auto s = dataOf<Surface>(r);
            createHostWindowRef(client, &xdg_toplevel_interface, s ? xdgVersionFor(*s->impl) : 1, id,
                                hostWindowToplevelRef);
         },
   };

   const struct clap_surface_embedding_v1_interface embeddingImpl = {
      .destroy = destroyResource,
      .get_embedded_surface =
         [](wl_client *client, wl_resource *res, uint32_t id, wl_resource *surface, const char *token) {
            auto &impl = *dataOf<Impl>(res);
            auto &s = *dataOf<Surface>(surface);
            if (s.xdg || s.ownMirror) {
               wl_resource_post_error(res, CLAP_SURFACE_EMBEDDING_V1_ERROR_ROLE, "the surface has another role");
               return;
            }
            if (s.embeddedRes) {
               wl_resource_post_error(res, CLAP_SURFACE_EMBEDDING_V1_ERROR_ALREADY_EMBEDDED,
                                      "the surface is already embedded");
               return;
            }

            auto r = wl_resource_create(client, &clap_embedded_surface_v1_interface, 1, id);
            Place *place = impl.findPlace(token);
            if (!place || place->mirror.shown) {
               std::cerr << "nested compositor: refused embedding token " << token << "\n";
               wl_resource_set_implementation(r, &embeddedSurfaceImpl, nullptr, nullptr);
               clap_embedded_surface_v1_send_closed(r);
               return;
            }

            wl_resource_set_implementation(r, &embeddedSurfaceImpl, &s, [](wl_resource *r) {
               if (auto s = dataOf<Surface>(r)) {
                  s->impl->unembed(*s);
                  s->embeddedRes = nullptr;
               }
            });
            s.embeddedRes = r;
            impl.embed(s, *place);
         },
   };

   /////////////
   // Globals //
   /////////////

   template <class ImplT>
   void bindGlobal(wl_client *client, void *data, uint32_t version, uint32_t id, const wl_interface *interface,
                   const ImplT *implementation) {
      auto r = wl_resource_create(client, interface, version, id);
      wl_resource_set_implementation(r, implementation, data, nullptr);
   }

   void Impl::createGlobals() {
      wl_global_create(server, &wl_compositor_interface, 6, this,
                       [](wl_client *c, void *d, uint32_t v, uint32_t id) {
                          bindGlobal(c, d, v, id, &wl_compositor_interface, &compositorImpl);
                       });

      wl_global_create(server, &wl_shm_interface, 1, this, [](wl_client *c, void *d, uint32_t v, uint32_t id) {
         auto r = wl_resource_create(c, &wl_shm_interface, v, id);
         wl_resource_set_implementation(r, &shmImpl, d, nullptr);
         // Every compositor supports these two
         wl_shm_send_format(r, WL_SHM_FORMAT_ARGB8888);
         wl_shm_send_format(r, WL_SHM_FORMAT_XRGB8888);
      });

      if (seat)
         wl_global_create(server, &wl_seat_interface, 5, this, [](wl_client *c, void *d, uint32_t v, uint32_t id) {
            auto r = wl_resource_create(c, &wl_seat_interface, v, id);
            wl_resource_set_implementation(r, &seatImpl, d, nullptr);
            auto &impl = *static_cast<Impl *>(d);
            wl_seat_send_capabilities(
               r, WL_SEAT_CAPABILITY_POINTER | (impl.keyboard ? WL_SEAT_CAPABILITY_KEYBOARD : 0));
            if (v >= WL_SEAT_NAME_SINCE_VERSION)
               wl_seat_send_name(r, "seat0");
         });

      if (viewporter)
         wl_global_create(server, &wp_viewporter_interface, 1, this,
                          [](wl_client *c, void *d, uint32_t v, uint32_t id) {
                             bindGlobal(c, d, v, id, &wp_viewporter_interface, &viewporterImpl);
                          });

      // Only with a viewport can the plugin draw at a fractional scale
      if (fractionalScaleManager && viewporter)
         wl_global_create(server, &wp_fractional_scale_manager_v1_interface, 1, this,
                          [](wl_client *c, void *d, uint32_t v, uint32_t id) {
                             bindGlobal(c, d, v, id, &wp_fractional_scale_manager_v1_interface,
                                        &fractionalScaleManagerImpl);
                          });

      // The same version as upstream, since we forward everything to it
      if (wmBase)
         wl_global_create(server, &xdg_wm_base_interface, upstream.wmBaseVersion, this,
                          [](wl_client *c, void *d, uint32_t v, uint32_t id) {
                             bindGlobal(c, d, v, id, &xdg_wm_base_interface, &wmBaseImpl);
                          });

      if (wmBase)
         wl_global_create(server, &zxdg_decoration_manager_v1_interface, 1, this,
                          [](wl_client *c, void *d, uint32_t v, uint32_t id) {
                             bindGlobal(c, d, v, id, &zxdg_decoration_manager_v1_interface, &decorationManagerImpl);
                          });

      wl_global_create(server, &clap_surface_embedding_v1_interface, 1, this,
                       [](wl_client *c, void *d, uint32_t v, uint32_t id) {
                          bindGlobal(c, d, v, id, &clap_surface_embedding_v1_interface, &embeddingImpl);
                       });

      // The same version as upstream, since we forward everything to it
      if (linuxDmabuf)
         wl_global_create(server, &zwp_linux_dmabuf_v1_interface, upstream.linuxDmabufVersion, this,
                          [](wl_client *c, void *d, uint32_t v, uint32_t id) {
                             bindGlobal(c, d, v, id, &zwp_linux_dmabuf_v1_interface, &linuxDmabufImpl);
                          });

      // A placeholder output, since some toolkits (SDL3) refuse to start without any. The
      // surfaces never enter it: their scale comes from wp_fractional_scale_v1 instead.
      wl_global_create(server, &wl_output_interface, 4, this,
                       [](wl_client *c, void *d, uint32_t v, uint32_t id) {
                          static const struct wl_output_interface outputImpl = {
                             .release = [](wl_client *, wl_resource *r) { wl_resource_destroy(r); },
                          };
                          auto r = wl_resource_create(c, &wl_output_interface, v, id);
                          wl_resource_set_implementation(r, &outputImpl, d, nullptr);
                          wl_output_send_geometry(r, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                                                  "clap-wayland-host", "nested",
                                                  WL_OUTPUT_TRANSFORM_NORMAL);
                          wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT, 1920, 1080, 60000);
                          if (v >= WL_OUTPUT_SCALE_SINCE_VERSION)
                             wl_output_send_scale(r, 1);
                          if (v >= WL_OUTPUT_NAME_SINCE_VERSION) {
                             wl_output_send_name(r, "CLAP-HOST-1");
                             wl_output_send_description(r, "clap-wayland-host nested output");
                          }
                          if (v >= WL_OUTPUT_DONE_SINCE_VERSION)
                             wl_output_send_done(r);
                       });
   }

   ///////////////////
   // Thread & loop //
   ///////////////////

   Place *Impl::createPlace(int32_t x, int32_t y) {
      auto p = std::make_unique<Place>();
      p->token = randomToken();
      p->x = x;
      p->y = y;

      initMirror(p->mirror);
      p->subsurface = wl_subcompositor_get_subsurface(subcompositor, p->mirror.up, upstream.parent);
      wl_subsurface_set_position(p->subsurface, x, y);
      // The plugin's commits show right away, without waiting for the host's window
      wl_subsurface_set_desync(p->subsurface);

      places.push_back(std::move(p));
      return places.back().get();
   }

   void Impl::setParentOrigin(int32_t x, int32_t y) {
      if (x == parentOriginX && y == parentOriginY)
         return;
      parentOriginX = x;
      parentOriginY = y;
      for (auto &p : places)
         if (p->mirror.shown)
            sendPosition(*p->mirror.shown);
   }

   void Impl::runTasks() {
      uint64_t count;
      [[maybe_unused]] auto n = read(wakeFd, &count, sizeof(count));

      std::vector<std::function<void()>> todo;
      {
         std::lock_guard lock(tasksMutex);
         todo.swap(tasks);
      }
      for (auto &task : todo)
         task();
   }

   void Impl::run() {
      wl_event_loop *loop = wl_display_get_event_loop(server);
      pollfd fds[3] = {
         {.fd = wl_display_get_fd(upstream.display), .events = POLLIN, .revents = 0},
         {.fd = wl_event_loop_get_fd(loop), .events = POLLIN, .revents = 0},
         {.fd = wakeFd, .events = POLLIN, .revents = 0},
      };

      while (!isStopping) {
         // Several threads read the same connection, each with its own queue
         while (wl_display_prepare_read_queue(upstream.display, queue) != 0)
            wl_display_dispatch_queue_pending(upstream.display, queue);
         wl_display_flush(upstream.display);
         wl_display_flush_clients(server);

         for (auto &f : fds)
            f.revents = 0;
         if (poll(fds, 3, -1) > 0 && (fds[0].revents & POLLIN)) {
            if (wl_display_read_events(upstream.display) < 0) {
               std::cerr << "nested compositor: lost the system compositor\n";
               break;
            }
         } else
            wl_display_cancel_read(upstream.display);

         wl_display_dispatch_queue_pending(upstream.display, queue);
         if (fds[2].revents & POLLIN)
            runTasks();
         wl_event_loop_dispatch(loop, 0);
      }

      teardown();
   }

   void Impl::teardown() {
      // The resources' destructors destroy their upstream objects, so this comes first
      wl_display_destroy_clients(server);
      wl_display_destroy(server);
      server = nullptr;

      for (auto &p : places) {
         wl_subsurface_destroy(p->subsurface);
         destroyMirror(p->mirror);
      }
      places.clear();

      if (cursorShapeDevice)
         wp_cursor_shape_device_v1_destroy(cursorShapeDevice);
      if (pointer)
         wl_pointer_release(pointer);
      if (keyboard)
         wl_keyboard_release(keyboard);
      if (keymapFd >= 0)
         close(keymapFd);

      unwrap(compositor);
      unwrap(subcompositor);
      unwrap(shm);
      unwrap(seat);
      unwrap(viewporter);
      unwrap(fractionalScaleManager);
      unwrap(cursorShapeManager);
      unwrap(wmBase);
      if (dmabufFeedback.up)
         zwp_linux_dmabuf_feedback_v1_destroy(dmabufFeedback.up);
      dmabufFeedback = {};
      unwrap(linuxDmabuf);
      wl_display_flush(upstream.display);
   }
} // namespace

struct NestedCompositor::Impl : ::Impl {};

NestedCompositor::NestedCompositor() : _impl(std::make_unique<Impl>()) {}

NestedCompositor::~NestedCompositor() { stop(); }

bool NestedCompositor::start(const Upstream &upstream) {
   auto &impl = *_impl;
   if (!upstream.display || !upstream.parent || !upstream.compositor || !upstream.subcompositor || !upstream.shm) {
      std::cerr << "nested compositor: the system compositor lacks wl_subcompositor or wl_shm\n";
      return false;
   }

   impl.upstream = upstream;
   impl.queue = wl_display_create_queue(upstream.display);
   impl.compositor = wrap(upstream.compositor, impl.queue);
   impl.subcompositor = wrap(upstream.subcompositor, impl.queue);
   impl.shm = wrap(upstream.shm, impl.queue);
   impl.seat = wrap(upstream.seat, impl.queue);
   impl.viewporter = wrap(upstream.viewporter, impl.queue);
   impl.fractionalScaleManager = wrap(upstream.fractionalScaleManager, impl.queue);
   impl.cursorShapeManager = wrap(upstream.cursorShapeManager, impl.queue);
   impl.wmBase = wrap(upstream.wmBase, impl.queue);
   impl.linuxDmabuf = wrap(upstream.linuxDmabuf, impl.queue);

   // The GPU buffers' feedback, which the plugins' drivers want at once. Our thread doesn't run
   // yet, so we can wait for it here.
   if (impl.linuxDmabuf) {
      impl.dmabufFeedback.up = zwp_linux_dmabuf_v1_get_default_feedback(impl.linuxDmabuf);
      zwp_linux_dmabuf_feedback_v1_add_listener(impl.dmabufFeedback.up, &upDmabufFeedbackListener, &impl);
      wl_display_roundtrip_queue(upstream.display, impl.queue);
      if (impl.dmabufFeedback.current.empty())
         std::cerr << "nested compositor: no linux-dmabuf feedback, the plugins can't render on the GPU\n";
   }

   // Our own pointer: it gets the events of the plugins' surfaces, the host's one those of its
   // window
   if (impl.seat) {
      impl.pointer = wl_seat_get_pointer(impl.seat);
      wl_pointer_add_listener(impl.pointer, &upPointerListener, &impl);
      if (impl.cursorShapeManager)
         impl.cursorShapeDevice = wp_cursor_shape_manager_v1_get_pointer(impl.cursorShapeManager, impl.pointer);

      // The keys for the plugins: the host has no use for them itself
      impl.keyboard = wl_seat_get_keyboard(impl.seat);
      wl_keyboard_add_listener(impl.keyboard, &upKeyboardListener, &impl);
   }

   impl.server = wl_display_create();
   impl.wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
   if (!impl.server || impl.wakeFd < 0)
      return false;
   impl.createGlobals();

   impl.thread = std::thread([&impl] { impl.run(); });
   return true;
}

void NestedCompositor::stop() {
   auto &impl = *_impl;
   if (!impl.thread.joinable())
      return;

   impl.isStopping = true;
   impl.post([] {});
   impl.thread.join();

   // All its proxies are gone with the thread's teardown
   wl_event_queue_destroy(impl.queue);
   impl.queue = nullptr;
   close(impl.wakeFd);
   impl.wakeFd = -1;
}

int NestedCompositor::openConnection() {
   auto &impl = *_impl;
   if (!impl.thread.joinable())
      return -1;

   int fds[2];
   if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0)
      return -1;

   // The client's first requests wait in the socket until our thread creates the client
   const int serverFd = fds[0];
   impl.post([&impl, serverFd] {
      if (!wl_client_create(impl.server, serverFd)) {
         std::cerr << "nested compositor: failed to create the client\n";
         close(serverFd);
      }
   });
   return fds[1];
}

std::string NestedCompositor::createPlace(int32_t x, int32_t y) {
   auto &impl = *_impl;
   if (!impl.thread.joinable())
      return {};

   // On our thread, so that no event of the new objects is dispatched before it has a listener
   std::promise<std::string> token;
   impl.post([&] { token.set_value(impl.createPlace(x, y)->token); });
   return token.get_future().get();
}

void NestedCompositor::setParentOrigin(int32_t x, int32_t y) {
   auto &impl = *_impl;
   if (impl.thread.joinable())
      impl.post([&impl, x, y] { impl.setParentOrigin(x, y); });
}
