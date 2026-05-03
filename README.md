![drm-cxx logo](docs/logo.svg)

C++17 library for Linux DRM/KMS display, input, and hardware plane
allocation. Adapter headers (`drm::expected`, `drm::span`, `drm::print`)
alias the standard types on C++23 toolchains and `tl::expected` /
`tcb::span` / `fmt::print` on older ones.

The headline feature is **`drm::scene::LayerScene`** — a handle-based
layer model that owns plane assignment, dirty tracking, CPU
composition fallback, and session pause/resume on top of the native
`drm::planes::Allocator`. See [`docs/scene.md`](docs/scene.md) for the
design rationale.

## Hello world

```cpp
#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

auto dev = drm::Device::open("/dev/dri/card0").value();
dev.enable_universal_planes().value();
dev.enable_atomic().value();

drm::scene::LayerScene::Config cfg{crtc_id, connector_id, mode};
auto scene = drm::scene::LayerScene::create(dev, cfg).value();

auto bg = drm::scene::DumbBufferSource::create(
              dev, mode.hdisplay, mode.vdisplay, DRM_FORMAT_ARGB8888).value();
{
  auto map = bg->map(drm::scene::MapAccess::Write).value();
  // Paint into map.pixels() ...
}

drm::scene::LayerDesc desc;
desc.source = std::move(bg);
desc.display.dst_rect = {0, 0, mode.hdisplay, mode.vdisplay};
auto handle = scene->add_layer(std::move(desc)).value();

drm::PageFlip page_flip(dev);
page_flip.set_handler([&](auto, auto, auto) { /* flip done */ });

auto report = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip).value();
// report.layers_assigned / layers_composited / layers_unassigned
```

The first commit implicitly carries `DRM_MODE_ATOMIC_ALLOW_MODESET`;
subsequent commits don't. Page-flip events are not auto-injected —
pass `DRM_MODE_PAGE_FLIP_EVENT` and route the kernel's `user_data`
through `drm::PageFlip::dispatch()` yourself.

## Features

- **`drm::scene::LayerScene`** — handle-based layer model with
  allocator-driven plane assignment, session pause/resume, rebind on
  hotplug, and a CPU composition fallback for layers the hardware
  can't fit.
- **Native plane allocator** with bipartite pre-solve, warm-start
  across frames, failure memoization, content-type priority, and
  spatial group splitting. Replaces libliftoff.
- **`drm::cursor`** — XCursor theme resolver and KMS cursor renderer
  with runtime rotation and `HOTSPOT_X/Y` virtualization.
- **`drm::session::Seat`** — libseat-backed session multiplexer over
  logind / seatd / builtin (gated on `DRM_CXX_SESSION`).
- **`drm::display::HotplugMonitor`** — udev netlink hotplug stream
  with connector-id fast path.
- **`drm::capture`** — Blend2D-backed CRTC plane-composition snapshot
  with PNG encode (gated on `DRM_CXX_BLEND2D`).
- RAII wrappers for DRM devices, dumb buffers, GBM buffers, libinput
  contexts, and xkbcommon state, with `drm::expected<T, E>` on every
  fallible operation.
- Atomic modeset builder around `drmModeAtomicCommit`.
- libinput / xkbcommon input with typed event variants and
  `std::function` dispatch.
- EDID parsing via libdisplay-info (colorimetry, HDR, EOTFs).
- GBM device / surface / buffer with DMA-BUF.
- Optional `VK_KHR_display` Vulkan support.
- `drm::print` logging with runtime `LogLevel` gating.

## Requirements

| Tool | Minimum |
|------|---------|
| GCC | 9 (13.1 for native `std::expected` / `std::print`) |
| Clang | 10 (16.0 for native `std::expected` / `std::print`) |
| Meson | 1.3.0 |
| CMake | 3.21 |
| libdrm | 2.4.113 |
| libgbm | any |
| libinput | 1.21 |
| xkbcommon | 1.5 |
| libdisplay-info | 0.1.1 (subproject) |
| libseat | 0.7 (optional, for `DRM_CXX_SESSION`) |
| libxcursor | any (optional, for `DRM_CXX_CURSOR`) |
| Blend2D | 0.10 (optional, for `DRM_CXX_BLEND2D`) |
| ThorVG | 1.0.4 (optional, for the `thorvg_janitor` example) |
| libcamera | 0.3.0 (optional, for the `camera` example) |
| libyuv | any (optional, for the `camera` example) |
| Vulkan-Headers | 1.3 (optional) |

## Building

Meson:

```sh
meson setup builddir
ninja -C builddir
meson test -C builddir
```

CMake:

```sh
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build
```

### Options

| Meson | CMake | Default | Description |
|-------|-------|---------|-------------|
| `vulkan` | `DRM_CXX_VULKAN` | on | `VK_KHR_display` support |
| `examples` | `DRM_CXX_BUILD_EXAMPLES` | on | Example programs |
| `tests` | `DRM_CXX_BUILD_TESTS` | on | Unit + integration tests |
| `session` | `DRM_CXX_SESSION` | auto | `drm::session::Seat` (libseat) |
| `cursor` | `DRM_CXX_CURSOR` | auto | `drm::cursor` (libxcursor) |
| `blend2d` | `DRM_CXX_BLEND2D` | auto | `drm::capture` + `drm::csd` (Blend2D) |
| `thorvg_janitor` | `DRM_CXX_BUILD_THORVG_JANITOR` | auto | `thorvg_janitor` example (ThorVG) |
| `camera` | `DRM_CXX_BUILD_CAMERA` | auto | `camera` example (libcamera + libyuv) |

## Examples

Examples are organized into four buckets under `examples/`:

```
examples/
├── basics/      — minimal LayerScene introductions and small one-feature demos
├── scene/       — substantial LayerScene workloads
├── allocator/   — pedagogical demos for individual planes::Allocator features
└── advanced/    — demos that exercise non-scene modules (Vulkan, capture, cursor)
```

Each example has its own README. Suggested reading order for new
consumers:

1. **`basics/atomic_modeset/`** — the smallest possible scene
   program. Single layer, one commit, exit on the first page-flip.
2. **`basics/test_patterns/`** — a single-layer scene cycling
   reference patterns from the keyboard. Demonstrates per-event
   repaint without allocator churn.
3. **`scene/layered_demo/`** — interactive mutation tour. Add /
   remove / move / re-stack layers and watch the `CommitReport`.
4. **`scene/signage_player/`** — playlist-driven five-layer signage
   workload that exercises composition fallback, hotplug rebind, and
   libseat pause/resume.
5. **`scene/camera/`** — libcamera → KMS scanout viewfinder built on
   `LayerScene`, with libyuv format-repack fallbacks.
6. **`allocator/scene_warm_start/`**, **`scene_priority/`**,
   **`scene_formats/`** — read these when you want to understand
   *which* allocator behavior you're seeing in your own scene.
7. **`allocator/overlay_planes/`** — the bare-minimum raw allocator
   surface, when you need to drop below `LayerScene`.

## Other usage

### Raw plane allocation

```cpp
auto registry = drm::planes::PlaneRegistry::enumerate(dev).value();

drm::planes::Layer comp_layer;
drm::planes::Output output(crtc_id, comp_layer);

auto& layer = output.add_layer();
layer.set_property("FB_ID", fb_id)
     .set_property("CRTC_X", 0).set_property("CRTC_Y", 0)
     .set_property("CRTC_W", 1920).set_property("CRTC_H", 1080);

drm::planes::Allocator allocator(dev, registry);
drm::AtomicRequest req(dev);
auto assigned = allocator.apply(output, req, 0).value();
```

### Input

```cpp
auto seat = drm::input::Seat::open().value();
auto keyboard = drm::input::Keyboard::create({.layout = "us"}).value();

seat.set_event_handler([&](const drm::input::InputEvent& ev) {
  if (auto* ke = std::get_if<drm::input::KeyboardEvent>(&ev)) {
    keyboard.process_key(*ke);
    drm::println("Key {}: sym=0x{:x} utf8='{}'", ke->key, ke->sym, ke->utf8);
  }
});
```

### EDID

```cpp
auto info = drm::display::parse_edid(edid_blob).value();
if (info.hdr) {
  drm::println("HDR: max={}cd/m²", info.hdr->max_luminance);
}
```

### Logging

```cpp
drm::set_log_level(drm::LogLevel::Debug);
drm::log_info("Device opened: {}", path);
```

Compile-time floor: `-DDRM_CXX_LOG_LEVEL=4`.

## Migration from drmpp

| drmpp | drm-cxx |
|-------|---------|
| `drmpp::` namespace | `drm::` |
| `#include <drmpp/drmpp.h>` | `#include <drm-cxx/drm-cxx.hpp>` |
| `int` errno returns | `drm::expected<T, std::error_code>` |
| `spdlog::info(...)` | `drm::log_info(...)` or `drm::println(...)` |
| `liftoff_output_apply()` | `allocator.apply(output, req, flags)` |
| `liftoff_layer_set_property()` | `layer.set_property(name, value)` |
| `liftoff_layer_needs_composition()` | `layer.needs_composition()` |
| libsync `sync_wait()` | `drm::sync::SyncFence::wait()` |
| bsdrm helpers | `drm::get_resources()`, `drm::get_connector()`, etc. |
| Virtual callback classes | `std::function<>` handlers |

## License

MIT
