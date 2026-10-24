#include "vst3-host.h"

#include <algorithm>
#include <iostream>

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

using namespace Steinberg;
using WaylandExperiment::kPlatformTypeWaylandSurfaceEmbeddingV1;

// ---- HostContext ----

tresult PLUGIN_API HostContext::getName(Vst::String128 name) {
   return Vst::StringConvert::convert("VST3 Wayland Host", name) ? kResultTrue : kInternalError;
}

// The messages which the component and the controller exchange
tresult PLUGIN_API HostContext::createInstance(TUID cid, TUID iid, void **obj) {
   if (FUnknownPrivate::iidEqual(cid, Vst::IMessage::iid) && FUnknownPrivate::iidEqual(iid, Vst::IMessage::iid)) {
      *obj = new Vst::HostMessage;
      return kResultTrue;
   }
   if (FUnknownPrivate::iidEqual(cid, Vst::IAttributeList::iid) &&
       FUnknownPrivate::iidEqual(iid, Vst::IAttributeList::iid)) {
      if (auto list = Vst::HostAttributeList::make()) {
         *obj = list.take();
         return kResultTrue;
      }
   }
   *obj = nullptr;
   return kResultFalse;
}

// ---- Vst3Library ----

bool Vst3Library::load(const std::string &path, HostContext &context) {
   _path = path;
   std::string error;
   _module = VST3::Hosting::Module::create(path, error);
   if (!_module) {
      std::cerr << "failed to load " << path << ": " << error << "\n";
      return false;
   }
   // So that plugins can query IWaylandNestedCompositor before any instance exists
   _module->getFactory().setHostContext(context.unknown());

   for (auto &c : _module->getFactory().classInfos())
      if (c.category() == kVstAudioEffectClass)
         _classes.push_back(c);
   if (_classes.empty()) {
      std::cerr << "no audio effect classes in " << path << "\n";
      return false;
   }
   return true;
}

const VST3::Hosting::ClassInfo *Vst3Library::find(const std::string &name) const {
   for (auto &c : _classes)
      if (name.empty() || name == c.name())
         return &c;
   std::cerr << "no plugin " << name << " in " << _path << ", it has:\n";
   for (auto &c : _classes)
      std::cerr << "  " << c.name() << "  (" << c.ID().toString() << ")\n";
   return nullptr;
}

// ---- Vst3Host ----

Vst3Host::Vst3Host(WaylandConnection &connection, NestedCompositor &compositor, HostContext &context,
                   const WaylandWindow::Colors &colors)
   : _connection(connection), _compositor(compositor), _context(context), _colors(colors) {}

Vst3Host::~Vst3Host() {
   if (_view) {
      if (_isAttached)
         _view->removed();
      _view->setFrame(nullptr);
      _view = nullptr;
   }
   if (_controller) {
      _controller->setComponentHandler(nullptr);
      disconnectController();
      if (_isControllerSeparate)
         _controller->terminate();
      _controller = nullptr;
   }
   if (_component) {
      _component->terminate();
      _component = nullptr;
   }
   // What the plugin left registered
   _eventHandlers.clear();
   _timers.clear();
   // The plugin may keep its connection for its other instances: the compositor ends what is
   // still embedded into our window before it goes
   if (_window)
      _compositor.removeWindow(_windowId);
}

bool Vst3Host::create(const Vst3Library &library, const VST3::Hosting::ClassInfo &info) {
   _pluginName = info.name();

   _component = library.factory().createInstance<Vst::IComponent>(info.ID());
   if (!_component || _component->initialize(_context.unknown()) != kResultOk) {
      std::cerr << "failed to create " << info.name() << "\n";
      _component = nullptr;
      return false;
   }
   std::cout << "loaded " << info.name() << " (" << info.ID().toString() << ")\n";

   if (!connectController(library)) {
      std::cerr << "the plugin has no edit controller\n";
      return false;
   }
   _controller->setComponentHandler(this);
   return true;
}

// The controller is the component itself (SingleComponentEffect), or a class of its own which
// gets the component's state and talks to it through IConnectionPoint
bool Vst3Host::connectController(const Vst3Library &library) {
   _controller = U::cast<Vst::IEditController>(_component);
   if (_controller)
      return true;

   TUID controllerId;
   if (_component->getControllerClassId(controllerId) != kResultOk)
      return false;
   _controller = library.factory().createInstance<Vst::IEditController>(VST3::UID::fromTUID(controllerId));
   if (!_controller || _controller->initialize(_context.unknown()) != kResultOk) {
      _controller = nullptr;
      return false;
   }
   _isControllerSeparate = true;

   auto componentPoint = U::cast<Vst::IConnectionPoint>(_component);
   auto controllerPoint = U::cast<Vst::IConnectionPoint>(_controller);
   if (componentPoint && controllerPoint) {
      componentPoint->connect(controllerPoint);
      controllerPoint->connect(componentPoint);
      _areConnected = true;
   }

   MemoryStream state;
   if (_component->getState(&state) == kResultOk) {
      state.seek(0, IBStream::kIBSeekSet, nullptr);
      _controller->setComponentState(&state);
   }
   return true;
}

void Vst3Host::disconnectController() {
   if (!_areConnected)
      return;
   auto componentPoint = U::cast<Vst::IConnectionPoint>(_component);
   auto controllerPoint = U::cast<Vst::IConnectionPoint>(_controller);
   componentPoint->disconnect(controllerPoint);
   controllerPoint->disconnect(componentPoint);
   _areConnected = false;
}

// The sequence from vst3_equiv/extension's iwaylandnestedcompositor.h
bool Vst3Host::openGui() {
   // A placeholder size until the plugin tells its own
   _window = std::make_unique<WaylandWindow>(_connection, _colors);
   const std::string title = _pluginName + " - VST3 Wayland Host";
   if (!_window->open(title.c_str(), 400, 300))
      return false;
   _windowId = _compositor.addWindow(_window->nested());

   _view = owned(_controller->createView(Vst::ViewType::kEditor));
   if (!_view) {
      std::cerr << "the plugin has no editor view\n";
      return false;
   }
   if (_view->isPlatformTypeSupported(kPlatformTypeWaylandSurfaceEmbeddingV1) != kResultTrue) {
      std::cerr << "the plugin doesn't support " << kPlatformTypeWaylandSurfaceEmbeddingV1 << "\n";
      return false;
   }
   // Before attached(): the plugin finds our run loop on the frame
   _view->setFrame(this);

   // No IPlugViewContentScaleSupport: with Wayland, sizes are logical and the plugin gets the
   // scale from the compositor, which is us: we forward the system compositor's
   ViewRect rect;
   if (_view->getSize(&rect) != kResultOk) {
      std::cerr << "the plugin didn't give its size\n";
      return false;
   }
   std::cout << "plugin view: " << rect.getWidth() << "x" << rect.getHeight()
             << (_view->canResize() == kResultTrue ? ", resizable" : "") << "\n";
   _window->resize(rect.getWidth(), rect.getHeight());

   // The place below our bar, which the token designates
   const std::string token = _compositor.createPlace(_windowId, 0, WaylandWindow::kBarHeight);
   _window->commit();

   // The plugin places its popups relative to our window's geometry, which libdecor's title bar
   // is part of
   int32_t originX = 0;
   int32_t originY = 0;
   _window->contentOrigin(originX, originY);
   _compositor.setParentOrigin(_windowId, originX, originY);

   if (_view->attached(const_cast<char *>(token.c_str()), kPlatformTypeWaylandSurfaceEmbeddingV1) != kResultOk) {
      std::cerr << "the plugin failed to embed its view\n";
      return false;
   }
   _isAttached = true;
   return true;
}

// ---- IComponentHandler ----

tresult PLUGIN_API Vst3Host::beginEdit(Vst::ParamID id) {
   std::cout << "param " << id << ": gesture begin\n";
   return kResultOk;
}

tresult PLUGIN_API Vst3Host::performEdit(Vst::ParamID id, Vst::ParamValue valueNormalized) {
   std::cout << "param " << id << " = " << valueNormalized << "\n";
   return kResultOk;
}

tresult PLUGIN_API Vst3Host::endEdit(Vst::ParamID id) {
   std::cout << "param " << id << ": gesture end\n";
   return kResultOk;
}

tresult PLUGIN_API Vst3Host::restartComponent(int32 flags) { return kResultOk; }

// ---- IPlugFrame ----

// Our window follows the plugin's size (logical, as always with Wayland)
tresult PLUGIN_API Vst3Host::resizeView(IPlugView *view, ViewRect *newSize) {
   if (!_window || !newSize || view != _view.get())
      return kInvalidArgument;
   _window->resize(newSize->getWidth(), newSize->getHeight());
   view->onSize(newSize);
   return kResultTrue;
}

// ---- Linux::IRunLoop ----

tresult PLUGIN_API Vst3Host::registerEventHandler(Linux::IEventHandler *handler, Linux::FileDescriptor fd) {
   if (!handler || fd < 0 || std::any_of(_eventHandlers.begin(), _eventHandlers.end(), [&](auto &e) { return e.fd == fd; }))
      return kInvalidArgument;
   _eventHandlers.push_back({handler, fd});
   return kResultTrue;
}

tresult PLUGIN_API Vst3Host::unregisterEventHandler(Linux::IEventHandler *handler) {
   return std::erase_if(_eventHandlers, [&](auto &e) { return e.handler == handler; }) > 0 ? kResultTrue
                                                                                          : kInvalidArgument;
}

tresult PLUGIN_API Vst3Host::registerTimer(Linux::ITimerHandler *handler, Linux::TimerInterval milliseconds) {
   if (!handler)
      return kInvalidArgument;
   const auto period = std::chrono::milliseconds(std::max<Linux::TimerInterval>(milliseconds, 1));
   _timers.push_back({handler, period, Clock::now() + period});
   return kResultTrue;
}

tresult PLUGIN_API Vst3Host::unregisterTimer(Linux::ITimerHandler *handler) {
   return std::erase_if(_timers, [&](auto &t) { return t.handler == handler; }) > 0 ? kResultTrue
                                                                                   : kInvalidArgument;
}

// ---- the main loop ----

void Vst3Host::idle() {
   const auto now = Clock::now();
   // Index loop: the plugin may (un)register timers from onTimer()
   for (size_t i = 0; i < _timers.size(); ++i) {
      if (_timers[i].deadline > now)
         continue;
      _timers[i].deadline = now + _timers[i].period;
      auto handler = _timers[i].handler;
      handler->onTimer();
   }
}

int Vst3Host::nextTimeoutMs() const {
   if (_timers.empty())
      return -1;

   auto deadline = _timers.front().deadline;
   for (auto &t : _timers)
      deadline = std::min(deadline, t.deadline);
   const auto delay = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
   return std::max<int>(0, delay.count());
}

void Vst3Host::addPollFds(std::vector<pollfd> &fds) const {
   for (auto &e : _eventHandlers)
      fds.push_back({.fd = e.fd, .events = POLLIN, .revents = 0});
}

void Vst3Host::runPosixFds(const pollfd *fds, size_t count) {
   for (size_t i = 0; i < count; ++i) {
      if (!(fds[i].revents & (POLLIN | POLLERR | POLLHUP)))
         continue;
      // The handler may have gone with an earlier one's call
      auto it = std::find_if(_eventHandlers.begin(), _eventHandlers.end(),
                             [&](auto &e) { return e.fd == fds[i].fd; });
      if (it == _eventHandlers.end())
         continue;
      auto handler = it->handler;
      handler->onFDIsSet(fds[i].fd);
   }
}
