# Changelog

## v1.1.0 — C++17 migration

- **Project language target lowered from C++23 to C++17** (Phase D of the C++17
  migration). The library still picks up `std::expected`, `std::span`, and
  `std::print` when the toolchain has them; otherwise the `drm::expected`,
  `drm::span`, `drm::print`, and `drm::format` adapter headers transparently
  fall back to `tl::expected`, `tcb::span`, and `{fmt}`.
- Source tree `drm-cxx/` renamed to `src/`. Public `<drm-cxx/...>` include
  layout is unchanged for consumers — served at build time via a `drm-cxx`
  symlink into `src/` in the build tree and at install time via the normal
  `${includedir}/drm-cxx` install layout. Polyfill headers are vendored
  under `${includedir}/drm-cxx/vendor` so downstream consumers don't need
  `tl-expected` / `tcb-span` installed separately.
- `std::move_only_function` replaced with `std::function` in `Seat`,
  `EventDispatcher`, and `PageFlip` handler types. Existing call sites and
  lambdas continue to work unchanged (all were copy-constructible).
- `std::erase_if`, `std::string::starts_with`, and `Container::contains`
  call sites rewritten with their C++17 equivalents; transparent
  heterogeneous `unordered_map::find(string_view)` replaced with a
  `std::string` materialization at the single call site that used it.

## v1.0.0

Initial release of drm-cxx, a C++23 native re-implementation of drmpp.

### Core
- `drm::Device` — DRM device fd RAII with capability enables
- `drm::Resources` / `drm::Connector` / `drm::Encoder` / `drm::CrtcPtr` — RAII wrappers for DRM mode objects
- `drm::PropertyStore` — KMS property ID cache with `drmModeObjectGetProperties`
- `drm::core::format_name()` / `format_bpp()` — DRM format helpers

### Modeset
- `drm::AtomicRequest` — atomic commit builder with `drmModeAtomicAlloc` RAII
- `drm::ModeInfo` — mode selection: preferred, resolution match, refresh targeting
- `drm::PageFlip` — vblank event loop with epoll + `drmHandleEvent` v3

### Plane Allocator (replaces libliftoff)
- `drm::planes::PlaneRegistry` — hardware plane enumeration with capability detection
- `drm::planes::Layer` — virtual layer with dirty tracking, content hints, geometry
- `drm::planes::Output` — per-CRTC output with layer management and zpos sorting
- `drm::planes::Allocator` — constraint-solving allocator with 7 improvements:
  1. Static compatibility matrix pruning
  2. Best-first search order
  3. Warm-start from previous frame
  4. Test-commit failure memoization
  5. Hopcroft-Karp bipartite pre-solve
  6. Content-type layer priority
  7. Spatial intersection splitting
- `drm::planes::BipartiteMatching` — standalone Hopcroft-Karp implementation

### Input
- `drm::input::Seat` — libinput + udev RAII with typed event dispatch
- `drm::input::Keyboard` — xkbcommon RAII with RMLVO and file-based keymap loading
- `drm::input::Pointer` — motion accumulator and button state tracker
- `drm::input::EventDispatcher` — multi-handler fan-out
- Rich event types: `KeyboardEvent`, `PointerEvent` (motion/button/axis), `TouchEvent`, `SwitchEvent`

### Display
- `drm::display::parse_edid()` — libdisplay-info EDID parsing with colorimetry, HDR, EOTF extraction
- `drm::display::ConnectorInfo` / `ColorimetryInfo` / `HdrStaticMetadata`

### GBM
- `drm::gbm::GbmDevice` — `gbm_create_device` RAII
- `drm::gbm::Surface` — `gbm_surface_create` with front buffer locking
- `drm::gbm::Buffer` — `gbm_bo` accessor with smart release (surface-aware)

### Sync
- `drm::sync::SyncFence` — native sync via `linux/sync_file.h` (replaces libsync)

### Vulkan (optional)
- `drm::vulkan::Display` — VK_KHR_display enumeration with dynamic Vulkan loading
- `drm::vulkan::DrmSurface` — surface handle placeholder

### Infrastructure
- `drm::print` / `std::print` logging with `drm::LogLevel` runtime gating
- pkg-config generation
- GTest unit test suite (13 suites, 100+ tests)
- CI: GCC-13/14, Clang-16/17 matrix
- Examples: `atomic_modeset`, `overlay_planes`, `vulkan_display`

### Breaking changes from drmpp
- Namespace: `drmpp::` -> `drm::`
- All returns use `drm::expected<T, std::error_code>` (aliases `std::expected` on C++23, `tl::expected` on C++17)
- No libliftoff, bsdrm, libsync, spdlog, or rapidjson dependencies
- Callbacks use `std::function<>` handlers instead of virtual dispatch
- Header paths: `#include <drm-cxx/...>` canonical layout
