# vst3_equiv: the nested compositor for VST3

The same embedding as the CLAP host, for VST3: the host runs a Wayland compositor, and the plugin
connects to it and embeds its surface into the host's window with the `clap_surface_embedding_v1`
protocol. The protocol is plain Wayland; only its name is CLAP's.

| Path | What |
|------|------|
| [`extension/`](extension/include/vst3-wayland-experiment/iwaylandnestedcompositor.h) | the interface: a platform type and `IWaylandNestedCompositor`, documented in the header |
| [`host/`](host/) | `vst3-wayland-host`, a VST3 host on the same nested compositor and windows as `clap-wayland-host` (`src/`) |
| [`plugin/`](plugin/wayland-shm-demo-vst3.cpp) | `wayland-shm-demo-vst3`, `plugins/wayland-shm-demo` as a VST3: libwayland-client and `wl_shm`, nothing else |

## Not VST 3.8's Wayland support

VST 3.8 has a Wayland platform of its own: `kPlatformTypeWaylandSurfaceID`, `IWaylandHost` and
`IWaylandFrame`. Their calls pass `wl_display*`, `wl_surface*`, `xdg_surface*` and `xdg_toplevel*`
between the host and the plugin, which ties both to one libwayland-client, in one process: the
plugin can't use another Wayland implementation, and can't run its GUI in another process.

This is an alternative, which mirrors the CLAP extension: only a socket and strings go across.

| | CLAP (clap-wayland-experiment) | VST3 (here) |
|-|------|------|
| the window API | `CLAP_WINDOW_API_WAYLAND_SURFACE_EMBEDDING_V1` | `kPlatformTypeWaylandSurfaceEmbeddingV1` (`"WaylandSurfaceEmbeddingV1"`) |
| the plugin gets a connection | `clap_host_gui_wayland_nested_compositor->open_connection()` | `IWaylandNestedCompositor::openConnection()`, on the `IHostApplication` context |
| the plugin gets its token | `clap_plugin_gui_wayland_nested_compositor->set_parent(token)` | `IPlugView::attached(token, kPlatformTypeWaylandSurfaceEmbeddingV1)`: `parent` is the `const char *` token |
| the connection's fd | `posix-fd-support` | `Linux::IRunLoop` on the `IPlugFrame` |
| sizes | logical, no `set_scale()` | logical, no `IPlugViewContentScaleSupport` |
| the end | `clap_plugin_gui->destroy()` | `IPlugView::removed()` |

The view opens as follows:

1. the plugin queries its `IHostApplication` context for `IWaylandNestedCompositor` in `initialize()`
2. `IEditController::createView(ViewType::kEditor)`
3. `IPlugView::isPlatformTypeSupported(kPlatformTypeWaylandSurfaceEmbeddingV1)`
4. `IPlugView::setFrame(frame)`
5. `IPlugView::getSize()`, and the host makes room for the view
6. `IPlugView::attached(token, kPlatformTypeWaylandSurfaceEmbeddingV1)`

The interface's IID is defined once per binary, as the SDK's own are: link
`vst3-wayland-experiment`, or compile `extension/source/iwaylandnestedcompositor.cpp`.

## Building and running

It is built with the rest, unless `-DBUILD_VST3_EQUIV=OFF`. CMake fetches the VST 3 SDK (3.8,
MIT), only its `base`, `pluginterfaces` and `public.sdk`, and builds only the sources which the
host and the plugin need (`cmake/Vst3Sdk.cmake`). `-DVST3_SDK_DIR=<a checkout>` uses a local one.

```bash
B=build/vst3_equiv/plugin/wayland-shm-demo-vst3.vst3
./build/vst3_equiv/host/vst3-wayland-host $B
./build/vst3_equiv/host/vst3-wayland-host $B $B   # two instances, in two windows
```

The arguments are `<plugin.vst3> [plugin-name]`, with the first audio effect class of the bundle by
default, and may be repeated. Clicking the plugin turns its palette, which is a parameter: the host
prints the edits (`beginEdit`, `performEdit`, `endEdit`). A plugin without the platform type is
refused, for example a VST3 from clap-wrapper:

```
the plugin doesn't support WaylandSurfaceEmbeddingV1
```

## The host

It is `src/host.*` for VST3, and shares everything else with the CLAP host: the nested compositor,
the windows, and the main loop's shape. It

- loads bundles with the SDK's `VST3::Hosting::Module`, and gives each factory the host context
- creates the component, and the controller: the component itself, or a class of its own which
  it connects with `IConnectionPoint` and gives the component's state
- implements `IComponentHandler` (prints the edits), `IPlugFrame` (the window follows
  `resizeView()`) and `Linux::IRunLoop` (fds and timers in the main loop's `poll()`)

Like the CLAP host, it never activates the plugin: there is no audio.
