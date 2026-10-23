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
```

The arguments are `<plugin.clap> [plugin-id]`, with the first plugin of the file by default.
Close the window or press Ctrl+C to quit. `WAYLAND_DEBUG=1` traces both sides: the plugin's
requests to the nested compositor, and their forwarding (`->`) to the system compositor.

The host loads the plugin and opens its GUI. The window is a dark bar labelled "host area",
which the host draws, with the plugin's surface embedded below it.

## The example plugins

Each one is a single file, implements the whole plugin side of the API, and outputs a quiet sine
wave. The host never starts audio, though.

| Plugin | Draws with | Paced by | Shows |
|--------|-----------|----------|-------|
| [`wayland-shm-demo`](plugins/wayland-shm-demo/wayland-shm-demo.cpp) | libwayland-client and `wl_shm`, nothing else | frame callbacks, with the connection's fd on the host's `posix-fd-support` | a plasma; the pointer moves a lens and clicks change the palette |
| [`sdl3-demo`](plugins/sdl3-demo/sdl3-demo.cpp) | SDL3's renderer (OpenGL through EGL) | a 16 ms host timer which pumps SDL's events | sine waves, orbiting balls, click ripples, the renderer's name and fps |
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

SDL's video subsystem is global to the process, so only one instance at a time can show its GUI.

## Layout

| Path | What |
|------|------|
| `src/main.cpp` | arguments and signals |
| `src/host.*` | the CLAP host: loading, the host extensions (log, thread-check, timer-support, posix-fd-support, params, gui, the nested compositor), and the GUI sequence |
| `src/wayland-window.*` | the host's toplevel, decorated by libdecor: the "host area" bar on top, the plugin's place below |
| `src/nested-compositor.*` | the Wayland compositor which the plugins connect to |
| `src/decorations.*` | the title bars which the compositor draws around the plugins' own windows |
| `cmake/WaylandProtocols.cmake` | `wayland_target_protocols(target [SERVER] xml...)` around wayland-scanner |
| `cmake/ExampleClapPlugins.cmake` | the `example-plugins` target: free-audio/clap-plugins, pinned with `CLAP_PLUGINS_TAG` |
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
  - **Embedded surface:** its mirror is a desync `wl_subsurface` of the host's window. Its place
    is designated by a random token.
  - **The plugin's own windows** (`xdg_wm_base`): toplevels and popups are forwarded one to one.
    Toplevels are parented to the host's window, and popups may use it as their parent through
    `get_parent_surface`.
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
- **Only one plugin** at a time. Closing and reopening the GUI is untested.
- **GPU buffers are untested.** They were written on a VM without 3D acceleration, whose
  compositor offers linux-dmabuf v3 only, which isn't forwarded. There, GL plugins (`sdl3-demo`)
  fall back to Mesa's software rendering into `wl_shm`. Also missing:
  - plugins which only know linux-dmabuf v3, which get no `format`/`modifier` events
  - explicit sync (`wp_linux_drm_syncobj_v1`)
- **Bad dmabufs:** a plugin which ignores the feedback and submits a format that the system
  compositor never advertised gets the *host's* connection killed. The compositor can't check
  that itself.

## License

MIT, see [LICENSE](LICENSE). The submodules and free-audio/clap-plugins have their own licenses.
