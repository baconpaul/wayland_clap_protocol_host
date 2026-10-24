# wayland_clap_protocol_host

> [!WARNING]
> **This is AI-generated code.** It was written with Claude (Anthropic) during an exploratory
> session to validate a proposed CLAP API. It is a test bed for that proposal, not a production
> host. Read it as a reference, and expect rough edges.
>
> The write-up of the session, including how JUCE, Six Sines and Surge were made to embed (not
> part of this repository), is in this gist:
> **<https://gist.github.com/baconpaul/f7ba36dbc9b2436af9b1b1cfdc9c4b02>**

A small CLAP host which **embeds** plugin GUIs in its own window on Wayland, with no X11 at all.
It implements the "nested compositor" approach from
[free-audio/clap-wayland-experiment](https://github.com/free-audio/clap-wayland-experiment):

- the window API `CLAP_WINDOW_API_WAYLAND_SURFACE_EMBEDDING_V1`
- the extensions `clap.gui-wayland-nested-compositor/1`, on the host (`open_connection`) and on
  the plugin (`set_parent(token)`)
- the Wayland protocol `clap_surface_embedding_v1`, which gives a plugin's surface its place in
  the host's window

It comes with two small example plugins of its own, and a target which builds the ImGui examples
from [free-audio/clap-plugins](https://github.com/free-audio/clap-plugins).

## Building on Ubuntu 26.04

```bash
sudo apt-get install build-essential cmake ninja-build pkgconf git \
    libwayland-dev wayland-protocols libdecor-0-dev \
    libcairo2-dev libpango1.0-dev libsdl3-dev libxcb1-dev
```

| Package | For |
|---------|-----|
| `libwayland-dev`, `wayland-protocols` | everything: libwayland client and server, `wayland-scanner`, the protocol XML |
| `libdecor-0-dev` | the host window's title bar |
| `libcairo2-dev`, `libpango1.0-dev` | the title bars which the host draws around the plugins' own windows |
| `libsdl3-dev` | `plugins/sdl3-demo` (optional, skipped when missing) |
| `libxcb1-dev` | the `example-plugins` target only: clap-plugins also has an X11 backend |

```bash
git clone --recursive <this repository>
cd wayland_clap_protocol_host

cmake -S . -B build -G Ninja
cmake --build build

# Optional: fetch and build free-audio/clap-plugins (takes a few minutes)
cmake --build build --target example-plugins
```

The submodules are `libs/clap` and `libs/clap-wayland-experiment`. If you cloned without
`--recursive`, run `git submodule update --init`.

[`.github/workflows/build.yml`](.github/workflows/build.yml) builds all of it on Ubuntu 26.04
(x86_64 and arm64) on every push, every pull request, and on demand.

## Running

The host needs a Wayland session (`WAYLAND_DISPLAY`).

```bash
# The plugin which draws with libwayland and wl_shm only
./build/src/clap-wayland-host build/plugins/wayland-shm-demo/wayland-shm-demo.clap

# The SDL3 plugin
./build/src/clap-wayland-host build/plugins/sdl3-demo/sdl3-demo.clap

# The ImGui examples from free-audio/clap-plugins
P=build/example-plugins/clap-plugins.clap
./build/src/clap-wayland-host $P com.github.free-audio.clap.synth
./build/src/clap-wayland-host $P com.github.free-audio.clap.check-wayland-embed
./build/src/clap-wayland-host $P bogus   # lists the plugin ids

# Two plugins in one process, each in its own window
./build/src/clap-wayland-host build/plugins/wayland-shm-demo/wayland-shm-demo.clap \
    build/plugins/sdl3-demo/sdl3-demo.clap
```

The arguments are `<plugin.clap> [plugin-id]`, with the first plugin of the file by default, and
may be repeated to load several plugins. A bundle given twice is loaded once, with two instances.
Closing a window ends its plugin; close them all or press Ctrl+C to quit. `WAYLAND_DEBUG=1`
traces both sides: the plugins' requests to the nested compositor, and their forwarding (`->`) to
the system compositor.

The host loads each plugin and opens its GUI in a window of its own. The window has a bar
labelled "host area", which the host draws, with the plugin's surface embedded below it. The bars
are green, red, blue and amber, in the order of the command line. All the windows share one
connection to the system compositor and one nested compositor, as the plugins in a DAW would, so
a plugin may use one connection for all its instances.

## The example plugins

Each one is a single file, implements the whole plugin side of the API, and outputs a quiet sine
wave. The host never starts audio, though.

| Plugin | Draws with | Paced by | Shows |
|--------|-----------|----------|-------|
| [`wayland-shm-demo`](plugins/wayland-shm-demo/wayland-shm-demo.cpp) | libwayland-client and `wl_shm`, nothing else | frame callbacks, with the connection's fd on the host's `posix-fd-support` | a plasma; the pointer moves a lens and clicks change the palette |
| [`sdl3-demo`](plugins/sdl3-demo/sdl3-demo.cpp) | SDL3's renderer (OpenGL through EGL) | a 16 ms host timer which pumps SDL's events | sine waves, orbiting balls, click ripples, the renderer's name and fps |
| [`visage-demo`](plugins/visage-demo/visage-demo.cpp) (patched Visage, off by default) | [Visage](https://github.com/VitalAudio/visage): bgfx and Vulkan | a timer fd of Visage's, on the host's `posix-fd-support` | rings, orbiting balls, click ripples |
| [`pugl-demo`](plugins/pugl-demo/pugl-demo.cpp) (patched Pugl, off by default) | [Pugl](https://github.com/lv2/pugl): OpenGL through EGL | frame callbacks, with the connection's fd on the host's `posix-fd-support` | a Lissajous trail, turning squares, ripples for clicks and keys |
| `example-plugins` target | Dear ImGui, from free-audio/clap-plugins | the plugins' own loop | synth, check-wayland-embed, and the other clap-plugins |

`wayland-shm-demo` is the smallest complete example of the API:
1. In `gui.create()`, it calls `open_connection()`, `wl_display_connect_to_fd()`, and creates a
   plain `wl_surface`.
2. In `set_parent(token)`, it calls `clap_surface_embedding_v1.get_embedded_surface(surface, token)`.
3. In `gui.show()`, it attaches a buffer and keeps drawing on frame callbacks.

`sdl3-demo` shows that a toolkit can be pointed at the host's connection:
- `SDL_PROP_GLOBAL_VIDEO_WAYLAND_WL_DISPLAY_POINTER` makes SDL use our `wl_display`.
- `SDL_PROP_WINDOW_CREATE_WAYLAND_SURFACE_ROLE_CUSTOM_BOOLEAN` gives a role-less surface, which
  the plugin embeds itself.

SDL's video subsystem is global to the process, so `sdl3-demo` also shows how a plugin shares one
connection between its instances:
- The first instance to create its GUI calls `open_connection()` and initializes SDL. The others
  reuse that connection, and only embed their own surface with their own token. The last one shuts
  SDL down and disconnects.
- SDL's event queue is global too: every instance's timer pumps it, and each event goes to the
  instance whose window it is for (`SDL_GetWindowFromEvent`).
- The connection is only reused with the same `open_connection` function: another host in the
  same process could run another compositor.

### Visage and Pugl: patched upstream, not in the default build

> **`visage-demo` and `pugl-demo` need patched copies of Visage and Pugl.** Neither upstream
> project can embed a surface through `clap-surface-embedding-v1`: upstream Visage has no Wayland
> backend at all, and Pugl's Wayland backend is an unmerged work in progress which only makes
> toplevels. The patches which add this live in [`patches/`](patches/), have not been sent
> upstream, and are experiments. Both demos are off by default, and a plain `cmake -B build` builds
> neither.

| Patch | Applies to |
|-------|------------|
| [`patches/visage-wayland.patch`](patches/visage-wayland.patch) | [VitalAudio/visage](https://github.com/VitalAudio/visage) `main` at `7654e32` |
| [`patches/pugl-wayland-embed.patch`](patches/pugl-wayland-embed.patch) | [lv2/pugl](https://github.com/lv2/pugl) `wayland` branch at `d28d56b` |

With `-DBUILD_VISAGE_DEMO=ON` or `-DBUILD_PUGL_DEMO=ON`, CMake fetches that commit and applies the
patch. Or point it at a checkout which already has the patch applied
(`git -C ../visage apply /path/to/patches/visage-wayland.patch`). Pugl's `wayland` branch is
rebased now and then, so its commit may disappear, and the patch may need porting.

`visage-demo` needs the Wayland backend from the patch, and is only built on request since Visage
fetches bgfx and FreeType:

```bash
sudo apt-get install libxkbcommon-dev libx11-dev libxrandr-dev   # Visage's X11 backend still builds
cmake -B build -DBUILD_VISAGE_DEMO=ON                           # fetches Visage and patches it
cmake -B build -DBUILD_VISAGE_DEMO=ON -DVISAGE_SOURCE_DIR=../visage  # or a patched checkout
cmake --build build --target visage-demo
```

The backend (`visage_windowing/linux/windowing_wayland.cpp`) only embeds, through
`createWaylandEmbeddedWindow(width, height, display, token)`:
- **One connection:** Visage's renderer is global, and bgfx draws on one `wl_display`, so all the
  instances share one connection. The plugin keeps it until it is unloaded, since the renderer
  keeps using it.
- **One fd per window:** an epoll fd holding the connection's fd and a frame timer. Each instance
  registers its own window's fd with its own host, so no instance depends on another one's
  registration.
- **No vsync:** FIFO would block the host's main thread in the present whenever the window isn't
  painted, for example while the screen is off. bgfx is patched to take MAILBOX instead, which
  Mesa offers on Wayland in place of IMMEDIATE.
- **Scale:** the fractional scale if the compositor sends it, else the integer one, with a viewport.
- **bgfx's model window:** bgfx needs a window to initialize, which is a surface without a role
  that is never shown.

`pugl-demo` needs Pugl's Wayland backend plus the embedding from the patch. Pugl builds with meson,
so `cmake/Pugl.cmake` compiles its Wayland and OpenGL sources itself:

```bash
sudo apt-get install libxkbcommon-dev libwayland-dev libegl-dev libgl-dev
cmake -B build -DBUILD_PUGL_DEMO=ON                          # fetches Pugl and patches it
cmake -B build -DBUILD_PUGL_DEMO=ON -DPUGL_SOURCE_DIR=../pugl  # or a patched checkout
cmake --build build --target pugl-demo
```

The embedding (`include/pugl/wayland.h` there) is two functions:
- **`puglNewWaylandWorld(type, flags, display)`** makes a world on the host's connection, which
  the world doesn't own. A world without a seat or a shell is fine: only toplevels need them.
- **`puglSetWaylandEmbeddingToken(view, token)`** in place of `puglSetParent()`. The view is
  embedded when it is realized, which fails if the compositor refuses the token.

And how it behaves:
- **One world:** all the instances share one world on one connection, which goes with the last
  GUI. Each instance registers the connection's fd with its own host, and any of them runs
  `puglUpdate(world, 0)` for all the views.
- **Frame callbacks:** a view draws once when shown, then on its frame callbacks while it is
  continuous or asked to redraw. EGL's swap interval is 0, so that a swap never waits for the
  compositor.
- **Scale:** the default size is in logical pixels, and the view is configured at that size times
  the fractional (else integer) scale, with a viewport. Pointer coordinates are scaled to match.

## Layout

| Path | What |
|------|------|
| `src/main.cpp` | arguments, signals, and the main loop over the plugins |
| `src/host.*` | the CLAP host, one per plugin instance: loading, the host extensions (log, thread-check, timer-support, posix-fd-support, params, gui, the nested compositor), and the GUI sequence |
| `src/wayland-window.*` | the connection to the system compositor, and the host's toplevels, decorated by libdecor: the "host area" bar on top, the plugin's place below |
| `src/nested-compositor.*` | the Wayland compositor which the plugins connect to |
| `src/decorations.*` | the title bars which the compositor draws around the plugins' own windows |
| `cmake/WaylandProtocols.cmake` | `wayland_target_protocols(target [SERVER] xml...)` around wayland-scanner |
| `cmake/ExampleClapPlugins.cmake` | the `example-plugins` target: free-audio/clap-plugins, pinned with `CLAP_PLUGINS_TAG` |
| `cmake/Visage.cmake`, `cmake/Pugl.cmake` | the optional `visage-demo` and `pugl-demo`: fetch, patch, build |
| `patches/` | the patches to Visage and Pugl which those two demos need |
| `plugins/` | the example plugins |
| `libs/` | `clap` and `clap-wayland-experiment`, as submodules |

## How the embedding works

The host calls the plugin in this order:
1. `gui.is_api_supported(WAYLAND_SURFACE_EMBEDDING_V1, false)`
2. `gui.create()`, during which the plugin calls `open_connection()`
3. `gui.get_size()`
4. `set_parent(token)`, with a token for a place below the bar
5. `gui.show()`

The plugin is never activated.

**The nested compositor:**
- **Thread:** it is a minimal Wayland server on its own thread. It can't share the main thread,
  because a plugin may do a blocking `wl_display_roundtrip()` inside `set_parent()`, which the
  host calls on the main thread.
- **Connections:** `open_connection()` gives the plugin one end of a `socketpair()`.
- **No rendering:** every plugin surface is forwarded to a "mirror" surface on the system
  compositor:
  - **Embedded surface:** its mirror is a desync `wl_subsurface` of one of the host's windows.
    Its place is designated by a random token, which any connection may present.
  - **The plugin's own windows** (`xdg_wm_base`): toplevels and popups are forwarded one to one.
    Toplevels are parented to the host's window where the plugin embeds, and popups may use it as
    their parent through `get_parent_surface`.
  - **Server-side decorations** (`zxdg_decoration_manager_v1`): GNOME draws none, so the
    compositor draws the title bars itself. The plugin's content is a subsurface below the bar.
- **Buffers:** nothing is ever copied.
  - **`wl_shm`:** pools are re-created upstream with the same fd.
  - **`zwp_linux_dmabuf_v1`:** GPU buffers are forwarded the same way. The system compositor's
    default feedback is fetched once at startup and replayed to each plugin, since drivers expect
    it within a roundtrip.
  - **Back to the plugin:** `wl_buffer.release` and frame callbacks.
- **Input:**
  - **Pointer:** goes to the surface under it.
  - **Keyboard:** goes to a plugin window which the system compositor focuses. Within the host's
    window it is click-to-focus.
  - **Serials:** in `move`, `resize` and `grab` requests, they are mapped back to the system
    compositor's.
- **Scale:** fractional and preferred buffer scale are forwarded.
- **Offered globals:**
  - `wl_compositor` v6, `wl_shm`, and `wl_seat` (pointer and keyboard)
  - `clap_surface_embedding_v1`
  - `xdg_wm_base` and `zxdg_decoration_manager_v1`
  - `wp_viewporter` and `wp_fractional_scale_manager_v1`
  - `zwp_linux_dmabuf_v1`, when the system compositor has v4 or later
  - a placeholder `wl_output`, since SDL3 refuses to start without one

## Not done

- **No audio:** the plugin is never activated. Parameter changes are only printed.
- **Cursor shapes** aren't forwarded: you always get the default arrow.
- **No clipboard or drag and drop.** The `wl_output` is a placeholder (1920x1080, scale 1) which
  no surface enters.
- **Closing and reopening a GUI is untested.**
- **GPU buffers are tested on AMD only.** On an AMD Renoir (radeonsi) under GNOME, `sdl3-demo`
  renders into tiled dmabufs which are forwarded and released as they should be. A system
  compositor which offers linux-dmabuf v3 only (as on a VM without 3D acceleration) isn't
  forwarded, and GL plugins then fall back to Mesa's software rendering into `wl_shm`. Also
  missing:
  - plugins which only know linux-dmabuf v3, which get no `format`/`modifier` events
  - explicit sync (`wp_linux_drm_syncobj_v1`), which NVIDIA's driver relies on
- **Bad dmabufs:** a plugin which ignores the feedback and submits a format that the system
  compositor never advertised gets the *host's* connection killed. The compositor can't check
  that itself.

## License

MIT, see [LICENSE](LICENSE). The submodules and free-audio/clap-plugins have their own licenses.
