# drm-cxx

C++23 native library for Linux DRM/KMS display management, input handling, and hardware plane allocation.

## Features

- **RAII everywhere** — DRM devices, GBM buffers, libinput contexts, xkbcommon state
- **`std::expected<T, E>`** for all fallible operations
- **Native plane allocator** replacing libliftoff with 7 algorithmic improvements:
  - Hopcroft-Karp bipartite pre-solve
  - Warm-start from previous frame (0-1 test commits in steady state)
  - Failure memoization, content-type priority, spatial splitting
- **Atomic modesetting** — builder pattern for `drmModeAtomicCommit`
- **Input subsystem** — libinput/xkbcommon with typed event variants and `std::move_only_function` dispatch
- **Display info** — EDID parsing via libdisplay-info (colorimetry, HDR, EOTFs)
- **GBM integration** — device, surface, buffer with DMA-BUF support
- **Vulkan `VK_KHR_display`** — optional, dynamically loaded
- **`std::print` logging** with runtime `LogLevel` gating

## Requirements

| Tool | Minimum |
|------|---------|
| GCC | 13.1 |
| Clang | 16.0 |
| Meson | 1.3.0 |
| libdrm | 2.4.113 |
| libgbm | any |
| libinput | 1.21 |
| xkbcommon | 1.5 |
| libdisplay-info | 0.1.1 (fetched as subproject) |
| Vulkan-Headers | 1.3 (optional) |

## Building

```sh
meson setup builddir
ninja -C builddir
meson test -C builddir
```

### Options

| Option | Default | Description |
|--------|---------|-------------|
| `vulkan` | `true` | Build Vulkan VK_KHR_display support |
| `examples` | `true` | Build example programs |
| `tests` | `true` | Build unit tests |

```sh
meson setup builddir -Dvulkan=false -Dexamples=false
```

## Usage

### Single header

```cpp
#include <drm-cxx/drm-cxx.hpp>
```

### Open a DRM device

```cpp
auto dev = drm::Device::open("/dev/dri/card0").value();
dev.enable_universal_planes();
dev.enable_atomic();
```

### Enumerate planes and allocate layers

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
auto assigned = allocator.apply(output, req, 0);
```

### Input handling

```cpp
auto seat = drm::input::Seat::open().value();
auto keyboard = drm::input::Keyboard::create({.layout = "us"}).value();

seat.set_event_handler([&](const drm::input::InputEvent& ev) {
  if (auto* ke = std::get_if<drm::input::KeyboardEvent>(&ev)) {
    keyboard.process_key(*ke);
    std::println("Key {}: sym=0x{:x} utf8='{}'", ke->key, ke->sym, ke->utf8);
  }
});
```

### EDID parsing

```cpp
auto info = drm::display::parse_edid(edid_blob).value();
if (info.hdr) {
  std::println("HDR: max={}cd/m²", info.hdr->max_luminance);
}
```

### Logging

```cpp
drm::set_log_level(drm::LogLevel::Debug);
drm::log_info("Device opened: {}", path);
```

Or at compile time: `-DDRM_CXX_LOG_LEVEL=4`

## Migration from drmpp

| drmpp | drm-cxx |
|-------|---------|
| `drmpp::` namespace | `drm::` |
| `#include <drmpp/drmpp.h>` | `#include <drm-cxx/drm-cxx.hpp>` |
| `int` errno returns | `std::expected<T, std::error_code>` |
| `spdlog::info(...)` | `drm::log_info(...)` or `std::println(...)` |
| `liftoff_output_apply()` | `allocator.apply(output, req, flags)` |
| `liftoff_layer_set_property()` | `layer.set_property(name, value)` |
| `liftoff_layer_needs_composition()` | `layer.needs_composition()` |
| libsync `sync_wait()` | `drm::sync::SyncFence::wait()` |
| bsdrm helpers | `drm::get_resources()`, `drm::get_connector()`, etc. |
| Virtual callback classes | `std::move_only_function<>` handlers |

## License

MIT
