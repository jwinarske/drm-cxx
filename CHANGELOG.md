# Changelog

## v1.2.0 — Scene API + example tree

### `drm::scene` — high-level layer scene
- **`drm::scene::LayerScene`** — declarative layer API above `planes::Allocator::apply`. `add_layer` / `remove_layer` / `set_dst_rect` / `set_src_rect` / `set_zpos` / `set_alpha` / `set_source` mutate state; `commit()` runs the allocator, builds the `AtomicRequest`, and returns a `CommitReport` with `layers_assigned` / `layers_composited` / `layers_unassigned` / `properties_written` / `fbs_attached` / `test_commits`.
- **Property minimization** — per-plane snapshot diffing skips redundant property writes; `FB_ID` always re-emits (page-flip protocol). `force_full_property_writes` opt-out for debugging.
- **Composition fallback** — `CompositeCanvas` (double-buffered ARGB8888 surface, ping-pong via `begin_frame()`); `compose_unassigned()` blends layers that did not reach a hardware plane and arms the canvas onto a free plane. `LayerDesc::force_composited` knob; canvas plane pre-reservation when `layer_count() > eligible_canvas_planes`.
- **`LayerScene::rebind(crtc, connector, mode)`** — explicit teardown + re-enumerate + rebuild; layer handles + sources survive. `CompatibilityReport` flags off-screen layers.
- **VT-switch lifecycle** — `on_session_paused()` / `on_session_resumed()` tear down + restore buffer mappings; pairs with `drm::session::Seat`.
- **Per-layer placement readout** — `Layer::assigned_plane_id()` exposes which hardware plane the allocator landed each layer on.
- **Polymorphic buffer sources** — `LayerBufferSource` abstract base + `AcquiredBuffer { fb_id, acquire_fence_fd, opaque }`. `cpu_mapping()` returns `nullopt` for tiled / non-LINEAR sources.
  - `DumbBufferSource` — scene-allocated 32bpp dumb buffer.
  - `ExternalDmaBufSource` — caller-owned DMA-BUF fds with `(format, modifier, plane[])` metadata; single-plane LINEAR + multi-plane (NV12, YUV420). `on_release()` callback fires after scanout completes.

### `drm::cursor` — hardware cursor with software fallback
- XCursor theme resolver + KMS cursor renderer with runtime rotation, `HOTSPOT_X` / `HOTSPOT_Y` virtualization, hardware-validated rotation harness.

### `drm::session::Seat` — session manager glue (gated by `DRM_CXX_SESSION`)
- libseat-backed logind / seatd / builtin mux. `enable_seat` / `disable_seat` / `switch_session`. `InputDeviceOpener` lets `input::Seat` route privileged opens through libseat.

### `drm::display::HotplugMonitor`
- Connector hotplug event stream over `udev`. `fd()` for poll/epoll integration, `dispatch()` to drain.

### `drm::capture` — Blend2D-backed CRTC snapshot
- Per-plane composition snapshot of an active CRTC, PNG encode via Blend2D. Companion `capture_demo` example, VKMS integration-test harness.

### Allocator improvements
- **Format-modifier-aware bipartite matching** — `IN_FORMATS` modifier list considered in plane eligibility; `LayerDesc::modifier` field.
- **Priority eviction** — `ContentType::Video` = 100, `update_hint_hz > 30` = 80, `update_hint_hz > 0` = 50, default = 10. Eviction is priority-driven.
- **Warm-start path** — `apply_previous_allocation` re-validates with one `TEST_ONLY`, producing `test_commits=0` (after the validating one) in steady state.
- **Two-tier placement** — per-group spatial placement, then a scene-wide partial fallback (drop most-constrained, retry) when total_assigned == 0.

### Plane registry
- `ColorEncoding` (`BT_601` / `BT_709` / `BT_2020`) + `ColorRange` (`Limited` / `Full`) enums.
- `PlaneCapabilities::has_color_encoding` / `has_color_range` plus cached enum integers.
- `DisplayParams::color_encoding` / `color_range` per-frame overrides; `LayerScene::arm_layer_plane_color_props` arms them on planes that expose the props.

### `drm::PageFlip`
- `add_source(fd, callback)` — register foreign fds (libcamera `eventfd`, `signalfd`, etc.) on the same epoll loop the page-flip dispatcher uses.

### `drm::Device`
- `Device::from_fd(int)` — wrap a caller-owned fd (e.g. one handed back by `libseat_open_device`).

### `drm::input::Seat`
- `InputDeviceOpener { open, close }` — caller-supplied open/close callbacks routed through libseat for `/dev/input/event*` opens. Per-fd cap re-enable on resume.

### Examples
- Bucketed tree: `examples/{basics,scene,allocator,advanced}/`.
- New: `signage_player`, `hotplug_monitor`, `cursor_rotate`, `capture_demo`, `video_grid`, `layered_demo`, `scene_warm_start`, `scene_priority`, `scene_formats`, `test_patterns`, `camera`, `thorvg_janitor`.
- Rewritten: `atomic_modeset` on `LayerScene`, `mouse_cursor` on `drm::cursor`.
- Shared helpers: `examples/common/open_output.hpp` (`open_device` + `open_and_pick_output` factor the libseat fd-open + first-connected-connector pickup), `select_connector.hpp` (`pick_connector` with `k_main_rank` / `k_internal_rank` / `k_external_rank`), `select_device.hpp`, `vt_switch.hpp` (Ctrl+Alt+F<n> chord), `format_probe.hpp`.

### Benchmarks (gated by `DRM_CXX_BUILD_BENCHMARKS=ON` / `-Dbenchmarks=true`)
- `plane_stress` — synthetic LayerScene workload; `--layers / --formats / --size / --churn / --churn-rate / --duration / --csv / --quiet` with per-frame CSV output.
- `allocator_torture` — six adversarial cases (N+1, format cascade, scaler monopoly, rapid churn, slow drift, burst-then-calm); PASS/FAIL/SKIP exit codes.

### Documentation
- `README.md` rewritten around `LayerScene` as the headline feature.
- `docs/scene.md` — design rationale, buffer-source model, extension points (EGL Streams, foreign DMA-BUF, multi-CRTC, animation), out-of-charter items.
- Per-example `README.md` files across the bucketed tree.
- Doxygen briefs filled in across the public scene headers.

### Build + CI
- thorvg 1.0.4, Blend2D, and libcamera v0.5.2 built from source in CI; cached.
- libseat-dev installed from apt.
- Weekly `drmdb` compat CI.
- VKMS integration-test pattern (`tests/integration/test_*_vkms.cpp` with `GTEST_SKIP` self-skip when VKMS isn't loaded).

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
