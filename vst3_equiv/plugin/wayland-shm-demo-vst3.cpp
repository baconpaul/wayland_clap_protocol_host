// A VST3 which draws its view with nothing but libwayland-client and wl_shm, embedded in the
// host's window with WaylandExperiment::IWaylandNestedCompositor and clap_surface_embedding_v1:
// plugins/wayland-shm-demo, as a VST3.
//
// - initialize(): the host context gives us IWaylandNestedCompositor.
// - IPlugView::attached(token): we open a connection to the host's compositor, create a plain
//   wl_surface, and clap_surface_embedding_v1.get_embedded_surface gives it its role. The
//   connection's fd goes to the IPlugFrame's Linux::IRunLoop, which calls us back when the
//   compositor has sent events.
// - Then we draw on every frame callback. The compositor paces the animation, and stops it while
//   the view isn't visible. IPlugView::removed() ends it all.
//
// The picture is a palette-cycling plasma, in software. Moving the pointer moves a lens, and
// clicking changes the palette, which is a parameter: the host sees the edits. The audio is a
// quiet 330 Hz sine.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>

// First: it renames IEditController's setState() and getState() for the headers it includes
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "public.sdk/source/common/pluginview.h"
#include "public.sdk/source/main/pluginfactory.h"

#include "vst3-wayland-experiment/iwaylandnestedcompositor.h"

#include "xdg-shell-client-protocol.h"
// After xdg-shell's
#include "clap-surface-embedding-v1-client-protocol.h"

using namespace Steinberg;
using WaylandExperiment::IWaylandNestedCompositor;
using WaylandExperiment::kPlatformTypeWaylandSurfaceEmbeddingV1;

namespace {

   constexpr int32_t kWidth = 400;
   constexpr int32_t kHeight = 260;
   constexpr double kFrequency = 330;
   constexpr float kLevel = 0.1f;

   constexpr Vst::ParamID kPaletteParam = 0;
   // A click turns the palette by this much
   constexpr double kPaletteStep = 37. / 256.;

   const FUID kEffectUid(0x6FEEFB2A, 0x5BFF4C9C, 0xB836059A, 0xB5097C28);

   // A table of sines in 0..255, over 256 steps
   const std::array<uint8_t, 256> kSine = [] {
      std::array<uint8_t, 256> table{};
      for (int i = 0; i < 256; ++i)
         table[i] = uint8_t(127.5 + 127.5 * std::sin(2 * M_PI * i / 256));
      return table;
   }();

   struct ShmBuffer {
      wl_buffer *buffer = nullptr;
      uint32_t *pixels = nullptr;
      size_t size = 0;
      int32_t width = 0; // in buffer pixels
      int32_t height = 0;
      bool isBusy = false; // until the compositor releases it
   };

   class Effect;

   // The view: one connection to the host's compositor while it is attached
   class View final : public CPluginView, public Linux::IEventHandler {
   public:
      explicit View(Effect &effect);
      ~View() override { detach(); }

      // ---- IPlugView ----

      tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
         return !strcmp(type, kPlatformTypeWaylandSurfaceEmbeddingV1) ? kResultTrue : kResultFalse;
      }

      tresult PLUGIN_API attached(void *parent, FIDString type) override {
         if (isAttached() || isPlatformTypeSupported(type) != kResultTrue || !parent)
            return kResultFalse;
         if (!attach(static_cast<const char *>(parent))) {
            detach();
            return kResultFalse;
         }
         systemWindow = this; // the token is only valid during the call
         return kResultTrue;
      }

      tresult PLUGIN_API removed() override {
         detach();
         systemWindow = nullptr;
         return kResultTrue;
      }

      tresult PLUGIN_API onSize(ViewRect *newSize) override {
         // Our size is fixed
         return newSize && newSize->getWidth() == kWidth && newSize->getHeight() == kHeight
                   ? kResultTrue
                   : kResultFalse;
      }

      // ---- Linux::IEventHandler ----

      // Reads and dispatches the compositor's events, when the host says that the fd is readable
      void PLUGIN_API onFDIsSet(Linux::FileDescriptor fd) override {
         if (!_display || fd != _fd)
            return;
         while (wl_display_prepare_read(_display) != 0)
            wl_display_dispatch_pending(_display);
         if (wl_display_read_events(_display) < 0 || wl_display_dispatch_pending(_display) < 0) {
            onConnectionError();
            return;
         }
         flush();
      }

      OBJ_METHODS(View, CPluginView)
      DEFINE_INTERFACES
         DEF_INTERFACE(Linux::IEventHandler)
      END_DEFINE_INTERFACES(CPluginView)
      REFCOUNT_METHODS(CPluginView)

   private:
      void log(const char *message) const { fprintf(stderr, "wayland-shm-demo-vst3: %s\n", message); }

      bool attach(const char *token);
      void detach();

      void onGlobal(wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
         auto bind = [&](const wl_interface *iface, uint32_t v) {
            return wl_registry_bind(registry, name, iface, v);
         };
         // wl_surface v4 for damage_buffer, v6 for preferred_buffer_scale
         if (!strcmp(interface, wl_compositor_interface.name) && version >= 4)
            _compositor = static_cast<wl_compositor *>(
               bind(&wl_compositor_interface, std::min(version, 6u)));
         else if (!strcmp(interface, wl_shm_interface.name))
            _shm = static_cast<wl_shm *>(bind(&wl_shm_interface, 1));
         else if (!strcmp(interface, wl_seat_interface.name) && !_seat && version >= 5) {
            _seat = static_cast<wl_seat *>(bind(&wl_seat_interface, 5));
            wl_seat_add_listener(_seat, &kSeatListener, this);
         } else if (!strcmp(interface, clap_surface_embedding_v1_interface.name))
            _embedding = static_cast<clap_surface_embedding_v1 *>(
               bind(&clap_surface_embedding_v1_interface, 1));
      }

      static const wl_registry_listener kRegistryListener;
      static const wl_seat_listener kSeatListener;
      static const wl_pointer_listener kPointerListener;
      static const wl_surface_listener kSurfaceListener;
      static const wl_buffer_listener kBufferListener;
      static const wl_callback_listener kFrameListener;
      static const clap_embedded_surface_v1_listener kEmbeddedSurfaceListener;

      // Sends our requests. If the socket is full, the rest goes with the next frame.
      void flush() {
         if (_display && wl_display_flush(_display) < 0 && errno != EAGAIN)
            onConnectionError();
      }

      void onConnectionError() {
         char message[128];
         snprintf(message, sizeof(message), "lost the connection to the host's compositor (error %d)",
                  _display ? wl_display_get_error(_display) : 0);
         log(message);
         // The host mustn't spin on the fd until removed()
         unregisterFd();
         _isVisible = false;
      }

      void unregisterFd() {
         if (_runLoop && _fd >= 0)
            _runLoop->unregisterEventHandler(this);
         _fd = -1;
      }

      // ---- drawing ----

      bool createBuffer(ShmBuffer &b, int32_t width, int32_t height);
      void destroyBuffer(ShmBuffer &b);
      ShmBuffer *nextBuffer();
      void draw();
      void requestFrame();
      void renderPlasma(ShmBuffer &b);
      uint32_t palette(uint8_t i) const;

      void onFrameDone(uint32_t timeMs) {
         _frameCallback = nullptr;
         _time = timeMs;
         draw();
      }

      void onPreferredBufferScale(int32_t factor) {
         if (factor < 1 || factor == _scale)
            return;
         _scale = factor;
         if (_isVisible && !_frameCallback)
            draw();
      }

      // ---- input ----

      void onSeatCapabilities(uint32_t capabilities) {
         const bool hasPointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
         if (hasPointer && !_pointer) {
            _pointer = wl_seat_get_pointer(_seat);
            wl_pointer_add_listener(_pointer, &kPointerListener, this);
         } else if (!hasPointer && _pointer) {
            wl_pointer_release(_pointer);
            _pointer = nullptr;
            _isPointerInside = false;
         }
      }

      void onClick();

      Effect &_effect;
      IPtr<Linux::IRunLoop> _runLoop;

      wl_display *_display = nullptr;
      int _fd = -1; // registered with the run loop
      wl_registry *_registry = nullptr;
      wl_compositor *_compositor = nullptr;
      wl_shm *_shm = nullptr;
      wl_seat *_seat = nullptr;
      wl_pointer *_pointer = nullptr;
      clap_surface_embedding_v1 *_embedding = nullptr;
      wl_surface *_surface = nullptr;
      clap_embedded_surface_v1 *_embeddedSurface = nullptr;
      wl_callback *_frameCallback = nullptr;
      std::array<ShmBuffer, 2> _buffers;

      bool _isVisible = false;
      int32_t _scale = 1;
      uint32_t _time = 0; // ms, from the frame callbacks

      bool _isPointerInside = false;
      float _pointerX = 0;
      float _pointerY = 0;
   };

   // The processor and the controller in one: the view only needs the palette parameter
   class Effect final : public Vst::SingleComponentEffect {
   public:
      static FUnknown *createInstance(void *) { return static_cast<Vst::IAudioProcessor *>(new Effect); }

      tresult PLUGIN_API initialize(FUnknown *context) override {
         const tresult result = SingleComponentEffect::initialize(context);
         if (result != kResultOk)
            return result;

         addAudioOutput(STR16("Output"), Vst::SpeakerArr::kStereo);
         parameters.addParameter(STR16("Palette"), nullptr, 0, 0, Vst::ParameterInfo::kCanAutomate,
                                 kPaletteParam);

         // Queried here, as a CLAP plugin asks for the host's extensions in init()
         _compositor = U::cast<IWaylandNestedCompositor>(context);
         return kResultOk;
      }

      tresult PLUGIN_API terminate() override {
         _compositor = nullptr;
         return SingleComponentEffect::terminate();
      }

      // ---- audio ----

      tresult PLUGIN_API setupProcessing(Vst::ProcessSetup &setup) override {
         _sampleRate = setup.sampleRate;
         return SingleComponentEffect::setupProcessing(setup);
      }

      tresult PLUGIN_API setActive(TBool state) override {
         _phase = 0;
         return SingleComponentEffect::setActive(state);
      }

      tresult PLUGIN_API process(Vst::ProcessData &data) override {
         if (data.numOutputs < 1 || data.outputs[0].numChannels < 1 ||
             data.symbolicSampleSize != Vst::kSample32)
            return kResultOk;

         auto &out = data.outputs[0];
         const double step = 2 * M_PI * kFrequency / _sampleRate;
         for (int32 i = 0; i < data.numSamples; ++i) {
            const float sample = kLevel * float(std::sin(_phase));
            _phase = std::fmod(_phase + step, 2 * M_PI);
            for (int32 c = 0; c < out.numChannels; ++c)
               out.channelBuffers32[c][i] = sample;
         }
         out.silenceFlags = 0;
         return kResultOk;
      }

      // ---- state: the palette ----

      tresult PLUGIN_API setState(IBStream *state) override {
         IBStreamer streamer(state, kLittleEndian);
         double palette = 0;
         if (!streamer.readDouble(palette))
            return kResultFalse;
         setParamNormalized(kPaletteParam, palette);
         return kResultOk;
      }

      tresult PLUGIN_API getState(IBStream *state) override {
         IBStreamer streamer(state, kLittleEndian);
         return streamer.writeDouble(getParamNormalized(kPaletteParam)) ? kResultOk : kResultFalse;
      }

      // ---- the view ----

      IPlugView *PLUGIN_API createView(FIDString name) override {
         if (!name || strcmp(name, Vst::ViewType::kEditor) || !_compositor)
            return nullptr;
         return new View(*this);
      }

      IWaylandNestedCompositor *compositor() const { return _compositor; }

      // In a gesture, so that the host records one edit
      void turnPalette() {
         const double value = std::fmod(getParamNormalized(kPaletteParam) + kPaletteStep, 1.);
         beginEdit(kPaletteParam);
         setParamNormalized(kPaletteParam, value);
         performEdit(kPaletteParam, value);
         endEdit(kPaletteParam);
      }

      uint8_t paletteShift() { return uint8_t(std::lround(getParamNormalized(kPaletteParam) * 256) & 255); }

   private:
      IPtr<IWaylandNestedCompositor> _compositor;
      double _sampleRate = 48000;
      double _phase = 0;
   };

   // ---- View ----

   View::View(Effect &effect) : _effect(effect) {
      const ViewRect r(0, 0, kWidth, kHeight);
      setRect(r);
   }

   bool View::attach(const char *token) {
      // setFrame() comes before attached(): the frame is the host's run loop too
      _runLoop = U::cast<Linux::IRunLoop>(plugFrame);
      if (!_runLoop || !_effect.compositor()) {
         log("the host has no run loop, or no IWaylandNestedCompositor");
         return false;
      }

      // wl_display_connect_to_fd() takes the fd, even on failure
      const int fd = _effect.compositor()->openConnection();
      if (fd < 0)
         return false;
      _display = wl_display_connect_to_fd(fd);
      if (!_display) {
         log("can't connect to the host's compositor");
         return false;
      }

      _registry = wl_display_get_registry(_display);
      wl_registry_add_listener(_registry, &kRegistryListener, this);
      if (wl_display_roundtrip(_display) < 0 || !_compositor || !_shm || !_embedding) {
         log("the host's compositor lacks wl_compositor v4, wl_shm or clap_surface_embedding_v1");
         return false;
      }

      _surface = wl_compositor_create_surface(_compositor);
      wl_surface_add_listener(_surface, &kSurfaceListener, this);
      _embeddedSurface = clap_surface_embedding_v1_get_embedded_surface(_embedding, _surface, token);
      clap_embedded_surface_v1_add_listener(_embeddedSurface, &kEmbeddedSurfaceListener, this);

      _fd = wl_display_get_fd(_display);
      if (_runLoop->registerEventHandler(this, _fd) != kResultOk) {
         _fd = -1;
         log("the host's run loop refused our fd");
         return false;
      }

      // No show() in VST3: attached is visible
      _isVisible = true;
      draw();
      flush();
      return true;
   }

   void View::detach() {
      unregisterFd();
      _runLoop = nullptr;
      if (_frameCallback) {
         wl_callback_destroy(_frameCallback);
         _frameCallback = nullptr;
      }
      for (auto &b : _buffers)
         destroyBuffer(b);
      // The role goes before the surface
      if (_embeddedSurface) {
         clap_embedded_surface_v1_destroy(_embeddedSurface);
         _embeddedSurface = nullptr;
      }
      if (_surface) {
         wl_surface_destroy(_surface);
         _surface = nullptr;
      }
      if (_pointer) {
         wl_pointer_release(_pointer);
         _pointer = nullptr;
      }
      if (_seat) {
         wl_seat_release(_seat);
         _seat = nullptr;
      }
      if (_embedding) {
         clap_surface_embedding_v1_destroy(_embedding);
         _embedding = nullptr;
      }
      if (_shm) {
         wl_shm_destroy(_shm);
         _shm = nullptr;
      }
      if (_compositor) {
         wl_compositor_destroy(_compositor);
         _compositor = nullptr;
      }
      if (_registry) {
         wl_registry_destroy(_registry);
         _registry = nullptr;
      }
      if (_display) {
         wl_display_disconnect(_display);
         _display = nullptr;
      }
      _isVisible = false;
      _isPointerInside = false;
   }

   bool View::createBuffer(ShmBuffer &b, int32_t width, int32_t height) {
      const int32_t stride = width * 4;
      b.size = size_t(stride) * size_t(height);
      const int fd = memfd_create("wayland-shm-demo-vst3", MFD_CLOEXEC);
      if (fd < 0)
         return false;
      if (ftruncate(fd, off_t(b.size)) < 0) {
         close(fd);
         return false;
      }
      void *data = mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (data == MAP_FAILED) {
         close(fd);
         return false;
      }
      auto pool = wl_shm_create_pool(_shm, fd, int32_t(b.size));
      b.buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
      wl_shm_pool_destroy(pool);
      close(fd);
      wl_buffer_add_listener(b.buffer, &kBufferListener, &b);
      b.pixels = static_cast<uint32_t *>(data);
      b.width = width;
      b.height = height;
      b.isBusy = false;
      return true;
   }

   void View::destroyBuffer(ShmBuffer &b) {
      if (b.buffer)
         wl_buffer_destroy(b.buffer);
      if (b.pixels)
         munmap(b.pixels, b.size);
      b = {};
   }

   // A free buffer at the current scale: the compositor may still read the other one
   ShmBuffer *View::nextBuffer() {
      const int32_t width = kWidth * _scale;
      const int32_t height = kHeight * _scale;
      for (auto &b : _buffers) {
         if (b.isBusy)
            continue;
         if (b.buffer && (b.width != width || b.height != height))
            destroyBuffer(b);
         if (!b.buffer && !createBuffer(b, width, height))
            return nullptr;
         return &b;
      }
      return nullptr;
   }

   void View::draw() {
      if (!_isVisible || !_embeddedSurface)
         return;

      ShmBuffer *b = nextBuffer();
      if (!b) {
         // Both are busy: the next frame callback tries again
         requestFrame();
         wl_surface_commit(_surface);
         return;
      }

      renderPlasma(*b);

      wl_surface_set_buffer_scale(_surface, _scale);
      wl_surface_attach(_surface, b->buffer, 0, 0);
      wl_surface_damage_buffer(_surface, 0, 0, b->width, b->height);
      b->isBusy = true;
      requestFrame();
      wl_surface_commit(_surface);
   }

   void View::requestFrame() {
      if (_frameCallback)
         return;
      _frameCallback = wl_surface_frame(_surface);
      wl_callback_add_listener(_frameCallback, &kFrameListener, this);
   }

   void View::renderPlasma(ShmBuffer &b) {
      // In logical pixels, so that the picture is the same at every scale
      const uint32_t t = _time / 16;
      const float inverseScale = 1.f / float(_scale);
      const float lensRadius2 = 50.f * 50.f;

      for (int32_t py = 0; py < b.height; ++py) {
         uint32_t *row = b.pixels + size_t(py) * size_t(b.width);
         const float y = float(py) * inverseScale;
         for (int32_t px = 0; px < b.width; ++px) {
            float x = float(px) * inverseScale;
            float yy = y;

            // The lens magnifies around the pointer
            const float dx = x - _pointerX;
            const float dy = y - _pointerY;
            const float d2 = dx * dx + dy * dy;
            const bool isInLens = _isPointerInside && d2 < lensRadius2;
            if (isInLens) {
               const float k = 0.4f + 0.6f * d2 / lensRadius2;
               x = _pointerX + dx * k;
               yy = _pointerY + dy * k;
            }

            const int ix = int(x);
            const int iy = int(yy);
            const unsigned v = kSine[(ix * 2 + t) & 255] + kSine[(iy * 3 - t * 2) & 255] +
                               kSine[((ix + iy) + t * 3) & 255] +
                               kSine[(kSine[(iy + t) & 255] / 2 + ix) & 255];
            uint32_t color = palette(uint8_t(v / 4 + t));
            if (isInLens && d2 > lensRadius2 * 0.9f)
               color = 0xffffff;
            row[px] = color;
         }
      }
   }

   uint32_t View::palette(uint8_t i) const {
      // Three phase-shifted sines, which the palette parameter rotates
      const uint8_t shift = _effect.paletteShift();
      const uint8_t r = kSine[(i + shift) & 255];
      const uint8_t g = kSine[(i + 85 + shift * 2) & 255];
      const uint8_t b = kSine[(i + 170) & 255];
      return (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
   }

   void View::onClick() { _effect.turnPalette(); }

   const wl_registry_listener View::kRegistryListener = {
      .global = [](void *data, wl_registry *registry, uint32_t name, const char *interface,
                   uint32_t version) { static_cast<View *>(data)->onGlobal(registry, name, interface, version); },
      .global_remove = [](void *, wl_registry *, uint32_t) {},
   };

   const wl_seat_listener View::kSeatListener = {
      .capabilities = [](void *data, wl_seat *,
                         uint32_t capabilities) { static_cast<View *>(data)->onSeatCapabilities(capabilities); },
      .name = [](void *, wl_seat *, const char *) {},
   };

   const wl_pointer_listener View::kPointerListener = {
      .enter =
         [](void *data, wl_pointer *, uint32_t, wl_surface *, wl_fixed_t x, wl_fixed_t y) {
            auto &v = *static_cast<View *>(data);
            v._isPointerInside = true;
            v._pointerX = float(wl_fixed_to_double(x));
            v._pointerY = float(wl_fixed_to_double(y));
         },
      .leave = [](void *data, wl_pointer *, uint32_t, wl_surface *) { static_cast<View *>(data)->_isPointerInside = false; },
      .motion =
         [](void *data, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y) {
            auto &v = *static_cast<View *>(data);
            v._pointerX = float(wl_fixed_to_double(x));
            v._pointerY = float(wl_fixed_to_double(y));
         },
      .button =
         [](void *data, wl_pointer *, uint32_t, uint32_t, uint32_t, uint32_t state) {
            if (state == WL_POINTER_BUTTON_STATE_PRESSED)
               static_cast<View *>(data)->onClick();
         },
      .axis = [](void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {},
      .frame = [](void *, wl_pointer *) {},
      .axis_source = [](void *, wl_pointer *, uint32_t) {},
      .axis_stop = [](void *, wl_pointer *, uint32_t, uint32_t) {},
      .axis_discrete = [](void *, wl_pointer *, uint32_t, int32_t) {},
      .axis_value120 = [](void *, wl_pointer *, uint32_t, int32_t) {},
      .axis_relative_direction = [](void *, wl_pointer *, uint32_t, uint32_t) {},
   };

   const wl_surface_listener View::kSurfaceListener = {
      .enter = [](void *, wl_surface *, wl_output *) {},
      .leave = [](void *, wl_surface *, wl_output *) {},
      .preferred_buffer_scale = [](void *data, wl_surface *,
                                   int32_t factor) { static_cast<View *>(data)->onPreferredBufferScale(factor); },
      .preferred_buffer_transform = [](void *, wl_surface *, uint32_t) {},
   };

   const wl_buffer_listener View::kBufferListener = {
      .release = [](void *data, wl_buffer *) { static_cast<ShmBuffer *>(data)->isBusy = false; },
   };

   const wl_callback_listener View::kFrameListener = {
      .done = [](void *data, wl_callback *callback, uint32_t time) {
         wl_callback_destroy(callback);
         static_cast<View *>(data)->onFrameDone(time);
      },
   };

   const clap_embedded_surface_v1_listener View::kEmbeddedSurfaceListener = {
      .position = [](void *, clap_embedded_surface_v1 *, int32_t, int32_t) {},
      .closed =
         [](void *data, clap_embedded_surface_v1 *) {
            // The host's place is gone: the surface is unmapped until removed()
            auto &v = *static_cast<View *>(data);
            v.log("the host closed the embedding");
            v._isVisible = false;
         },
   };

} // namespace

BEGIN_FACTORY_DEF("wayland_clap_protocol_host", "https://github.com/baconpaul/wayland_clap_protocol_host", "")

DEF_CLASS2(INLINE_UID_FROM_FUID(kEffectUid), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "Wayland SHM Demo VST3", 0, Vst::PlugType::kInstrumentSynth, "0.1.0", kVstVersionString,
           Effect::createInstance)

END_FACTORY
