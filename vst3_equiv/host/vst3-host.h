#pragma once

// A minimal VST3 host which embeds a plugin's view into a Wayland window of its own, through the
// nested compositor which src/ shares with the CLAP host
// (WaylandExperiment::kPlatformTypeWaylandSurfaceEmbeddingV1, see vst3_equiv/extension).
// There is one Vst3Host per plugin instance; they share the connection to the system compositor,
// the nested compositor and the host context, as the plugins in a DAW would.
//
// The plugin isn't activated: there is no audio. The parameter edits made in the view come
// through IComponentHandler, and are printed.

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "public.sdk/source/vst/hosting/module.h"

#include "vst3-wayland-experiment/iwaylandnestedcompositor.h"

#include "nested-compositor.h"
#include "wayland-window.h"

// The IHostApplication which every plugin instance gets in initialize(), with the host's
// compositor. The plugins' factories may keep it until their library is unloaded.
class HostContext final
   : public Steinberg::U::ImplementsNonDestroyable<
        Steinberg::U::Directly<Steinberg::Vst::IHostApplication, WaylandExperiment::IWaylandNestedCompositor>> {
public:
   // While the compositor runs
   void setCompositor(NestedCompositor *compositor) { _compositor = compositor; }

   Steinberg::FUnknown *unknown() { return unknownCast(); }

   // IHostApplication
   Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override;
   Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID cid, Steinberg::TUID iid, void **obj) override;

   // IWaylandNestedCompositor
   Steinberg::int32 PLUGIN_API openConnection() override {
      return _compositor ? _compositor->openConnection() : -1;
   }

private:
   NestedCompositor *_compositor = nullptr;
};

// A .vst3 bundle, loaded once for all its instances
class Vst3Library final {
public:
   bool load(const std::string &path, HostContext &context);

   // An audio effect class: with an empty name, the first; with an unknown one, prints those it has
   const VST3::Hosting::ClassInfo *find(const std::string &name) const;

   const VST3::Hosting::PluginFactory &factory() const { return _module->getFactory(); }

private:
   std::string _path;
   VST3::Hosting::Module::Ptr _module;
   std::vector<VST3::Hosting::ClassInfo> _classes;
};

class Vst3Host final
   : public Steinberg::U::ImplementsNonDestroyable<Steinberg::U::Directly<
        Steinberg::Vst::IComponentHandler, Steinberg::IPlugFrame, Steinberg::Linux::IRunLoop>> {
public:
   using Clock = std::chrono::steady_clock;

   // The connection, the compositor and the context are shared by all the hosts, and outlive them
   Vst3Host(WaylandConnection &connection, NestedCompositor &compositor, HostContext &context,
            const WaylandWindow::Colors &colors);
   ~Vst3Host() override;

   Vst3Host(const Vst3Host &) = delete;
   Vst3Host &operator=(const Vst3Host &) = delete;

   bool create(const Vst3Library &library, const VST3::Hosting::ClassInfo &info);
   bool openGui();

   bool isOpen() const { return _window && _window->isOpen(); }

   // The main loop's work, which main() runs for all the hosts
   void idle(); // the run loop's timers
   int nextTimeoutMs() const;
   void addPollFds(std::vector<pollfd> &fds) const;
   void runPosixFds(const pollfd *fds, size_t count); // those which addPollFds() added

   // IComponentHandler
   Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override;
   Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
                                             Steinberg::Vst::ParamValue valueNormalized) override;
   Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override;
   Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;

   // IPlugFrame
   Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView *view, Steinberg::ViewRect *newSize) override;

   // Linux::IRunLoop
   Steinberg::tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler *handler,
                                                      Steinberg::Linux::FileDescriptor fd) override;
   Steinberg::tresult PLUGIN_API unregisterEventHandler(Steinberg::Linux::IEventHandler *handler) override;
   Steinberg::tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler *handler,
                                               Steinberg::Linux::TimerInterval milliseconds) override;
   Steinberg::tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler *handler) override;

private:
   bool connectController(const Vst3Library &library);
   void disconnectController();

   struct EventHandler {
      Steinberg::IPtr<Steinberg::Linux::IEventHandler> handler;
      int fd;
   };

   struct Timer {
      Steinberg::IPtr<Steinberg::Linux::ITimerHandler> handler;
      std::chrono::milliseconds period;
      Clock::time_point deadline;
   };

   WaylandConnection &_connection;
   NestedCompositor &_compositor;
   HostContext &_context;
   const WaylandWindow::Colors _colors;

   // What the plugin gives us
   Steinberg::IPtr<Steinberg::Vst::IComponent> _component;
   Steinberg::IPtr<Steinberg::Vst::IEditController> _controller;
   bool _isControllerSeparate = false; // else the component is the controller too
   bool _areConnected = false;         // the component and the controller's IConnectionPoints
   Steinberg::IPtr<Steinberg::IPlugView> _view;
   bool _isAttached = false;
   std::string _pluginName;

   std::vector<EventHandler> _eventHandlers;
   std::vector<Timer> _timers;

   std::unique_ptr<WaylandWindow> _window;
   NestedCompositor::WindowId _windowId = UINT32_MAX; // none, until openGui()
};
