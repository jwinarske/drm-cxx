# drm-cxx — Porting Plan
### C++23 Native Re-implementation of drmpp · Meson · Breaking API

---

## 0. Scope & Goals

| | drmpp (source) | drm-cxx (target) |
|---|---|---|
| **C++ standard** | C++17 | C++23 |
| **Build system** | Meson ≥ 0.55 | Meson ≥ 1.3 |
| **API contract** | Existing drmpp ABI | **Breaking** — clean native API |
| **libliftoff** | External subproject dependency | Native C++23 implementation (no libliftoff) |
| **bsdrm** | External subproject | Inline; replaced by native wrappers |
| **logging** | spdlog (CMake subproject) | `std::print` / `std::format` (C++23 stdlib) |
| **JSON** | rapidjson (subproject) | `std::string_view` + lightweight native parser |
| **Namespace** | `drmpp::` | `drm::` |
| **Header style** | Split `include/` + `src/` | Unified `drm-cxx/` canonical layout |

---

## 1. Repository Structure

```
drm-cxx/
├── meson.build                  # Root — C++23, project 'drm-cxx'
├── meson.options                # Replaces meson_options.txt
├── config.h.meson
├── drm-cxx/                     # Canonical single-directory layout (headers + impl)
│   ├── core/
│   │   ├── device.hpp / device.cpp          # DRM device fd ownership (RAII)
│   │   ├── resources.hpp / resources.cpp    # Connector/CRTC/encoder enumeration
│   │   ├── property_store.hpp               # Type-safe KMS property cache
│   │   └── format.hpp                       # DRM format helpers (replaces bsdrm parts)
│   ├── modeset/
│   │   ├── atomic.hpp / atomic.cpp          # Atomic commit builder (RAII req wrapper)
│   │   ├── mode.hpp / mode.cpp              # Mode selection / validation
│   │   └── page_flip.hpp / page_flip.cpp    # Page-flip event loop
│   ├── planes/                              # *** libliftoff replacement — native C++23 ***
│   │   ├── plane_registry.hpp / .cpp        # Enumerate + capability-score all HW planes
│   │   ├── layer.hpp / layer.cpp            # Virtual layer (replaces liftoff_layer)
│   │   ├── output.hpp / output.cpp          # Per-CRTC output (replaces liftoff_output)
│   │   ├── allocator.hpp / allocator.cpp    # Constraint-solving plane allocator
│   │   └── composition_layer.hpp            # Fallback composition layer support
│   ├── input/
│   │   ├── seat.hpp / seat.cpp              # libinput seat + udev integration
│   │   ├── keyboard.hpp / keyboard.cpp      # xkbcommon keyboard handling
│   │   ├── pointer.hpp / pointer.cpp        # Pointer / touch events
│   │   └── event_dispatcher.hpp             # std::function-based dispatch
│   ├── display/
│   │   ├── edid.hpp / edid.cpp              # libdisplay-info wrapper → native parse
│   │   ├── hdr_metadata.hpp                 # HDR static/dynamic metadata
│   │   └── connector_info.hpp               # Connector capabilities, DPMS
│   ├── gbm/
│   │   ├── device.hpp / device.cpp          # GBM device RAII
│   │   ├── surface.hpp / surface.cpp        # GBM surface management
│   │   └── buffer.hpp / buffer.cpp          # GBM BO / DMA-BUF
│   ├── sync/
│   │   └── fence.hpp / fence.cpp            # Explicit sync (replaces libsync subproject)
│   ├── vulkan/                              # Optional feature gate
│   │   ├── display.hpp / display.cpp        # VK_KHR_display integration
│   │   └── drm_surface.hpp / drm_surface.cpp
│   └── drm-cxx.hpp                          # Aggregate public header
├── examples/
│   ├── meson.build
│   ├── atomic_modeset/
│   ├── overlay_planes/                      # Demonstrates native plane allocator
│   └── vulkan_display/
├── tests/
│   ├── meson.build
│   ├── unit/
│   │   ├── test_property_store.cpp
│   │   ├── test_plane_allocator.cpp
│   │   └── test_atomic_builder.cpp
│   └── integration/
│       └── test_modeset.cpp
├── subprojects/
│   ├── libdisplay-info.wrap                 # Retained
│   └── Vulkan-Headers.wrap                  # Retained (optional)
├── .clang-format
├── .clang-tidy
└── README.md
```

---

## 2. Build System (meson.build)

### Root `meson.build`

```meson
# SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
# SPDX-License-Identifier: Apache-2.0

project('drm-cxx', ['cpp'],
  version : '1.0.0',
  meson_version : '>=1.3.0',
  license : 'Apache-2.0',
  default_options : [
    'cpp_std=c++23',
    'b_lto=true',
    'warning_level=3',
  ],
)

cpp = meson.get_compiler('cpp')

# Enforce C++23 features are truly available
assert(cpp.has_argument('-std=c++23'),
  'A C++23-capable compiler is required (GCC ≥ 13, Clang ≥ 16)')

add_project_arguments(
  cpp.get_supported_arguments([
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
    '-fno-exceptions',          # Optional: exception-free build path
  ]),
  language : 'cpp',
)

#── Dependencies ──────────────────────────────────────────────
drm_dep     = dependency('libdrm',    include_type : 'system', required : true)
gbm_dep     = dependency('gbm',       include_type : 'system', required : true)
input_dep   = dependency('libinput',  include_type : 'system', required : true)
udev_dep    = dependency('libudev',   include_type : 'system', required : true)
xkb_dep     = dependency('xkbcommon', include_type : 'system', required : true)
rt_dep      = cpp.find_library('rt',  required : true)
dl_dep      = cpp.find_library('dl',  required : true)

di_proj = subproject('libdisplay-info',
  default_options : ['default_library=static'])
di_dep = di_proj.get_variable('di_dep')

if get_option('vulkan')
  cmake = import('cmake')
  vk_opts = cmake.subproject_options()
  vk_headers = cmake.subproject('Vulkan-Headers', options : vk_opts)
  vk_dep = vk_headers.dependency('Vulkan-Headers')
endif

#── Source ────────────────────────────────────────────────────
subdir('drm-cxx')

if get_option('examples')
  subdir('examples')
endif

if get_option('tests')
  subdir('tests')
endif
```

### `meson.options`

```meson
option('vulkan',   type : 'boolean', value : true,  description : 'Build Vulkan display support')
option('examples', type : 'boolean', value : true,  description : 'Build example programs')
option('tests',    type : 'boolean', value : true,  description : 'Build unit and integration tests')
option('lto',      type : 'boolean', value : true,  description : 'Link-time optimisation')
```

### Key meson changes vs drmpp

| drmpp | drm-cxx |
|---|---|
| `cpp_std=c++17` | `cpp_std=c++23` |
| `meson_version >=0.55` | `meson_version >=1.3` (modules, structured opts) |
| `subproject('sync')` | Native `<fcntl.h>` fence helpers in `drm-cxx/sync/` |
| `subproject('bsdrm')` | Inlined as `drm-cxx/core/` wrappers |
| `subproject('libliftoff')` | **Removed** — replaced by `drm-cxx/planes/` |
| `subproject('rapidjson')` | **Removed** — `std::string_view` + custom mini-parser |
| `subproject('spdlog')` (CMake) | **Removed** — `std::print` / `std::format` |
| `meson_options.txt` | `meson.options` (Meson 1.1+ canonical name) |

---

## 3. C++23 API Design

### 3.1 Namespace & Conventions

```cpp
namespace drm {
  // All types live in drm::
  // Sub-areas: drm::planes::, drm::input::, drm::display::, drm::gbm::
}
```

- All resource handles are **RAII value types** or `std::unique_ptr` with custom deleters.
- No raw pointers in public API surfaces.
- Callbacks use `std::function<>` or `std::move_only_function<>` (C++23).
- Properties use `std::expected<T, Error>` (C++23) for fallible accessors.
- Ranges use `std::ranges::` algorithms throughout.
- `std::string_view` replaces `const char*` in all APIs.
- `std::mdspan` (C++23) for pixel/format plane data.
- `std::print` / `std::println` replace spdlog entirely.

### 3.2 Core Device

```cpp
// drm-cxx/core/device.hpp
namespace drm {

class Device {
public:
  static std::expected<Device, std::error_code>
    open(std::string_view path);            // /dev/dri/cardN

  [[nodiscard]] int fd() const noexcept;

  std::expected<void, std::error_code>
    set_client_cap(uint64_t cap, uint64_t value);

  // Universal planes + atomic must be enabled before use
  std::expected<void, std::error_code> enable_universal_planes();
  std::expected<void, std::error_code> enable_atomic();

  ~Device();                               // Closes fd

  Device(Device&&) noexcept;
  Device& operator=(Device&&) noexcept;
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;
private:
  int fd_{-1};
};

} // namespace drm
```

### 3.3 Atomic Commit Builder

```cpp
// drm-cxx/modeset/atomic.hpp
namespace drm {

class AtomicRequest {
public:
  explicit AtomicRequest(const Device& dev);

  std::expected<void, std::error_code>
    add_property(uint32_t object_id,
                 uint32_t property_id,
                 uint64_t value);

  // Test without committing
  std::expected<void, std::error_code>
    test(uint32_t flags = DRM_MODE_ATOMIC_TEST_ONLY);

  std::expected<void, std::error_code>
    commit(uint32_t flags, void* user_data = nullptr);

  ~AtomicRequest();  // frees drmModeAtomicReq

private:
  drmModeAtomicReq* req_{};
  int drm_fd_{-1};
};

} // namespace drm
```

---

## 4. Native libliftoff Replacement (`drm-cxx/planes/`)

This is the most significant new work. The entire libliftoff C library is replaced by a
native C++23 subsystem. Functional parity is required; the ABI/API is entirely new.

### 4.1 Design Overview

libliftoff's algorithm is a **constraint-solving plane allocator**:
1. Enumerate all hardware planes and their capabilities.
2. Accept a stack of virtual **layers** (each carrying KMS property intents: `FB_ID`,
   `CRTC_X/Y/W/H`, `SRC_*`, `zpos`, `rotation`, …).
3. For each output (CRTC), attempt to assign layers → hardware planes via **atomic test
   commits**, preferring full hardware composition to reduce GPU load.
4. Layers that cannot be assigned to hardware planes are marked for **software
   composition** onto the composition layer.

### 4.2 `PlaneRegistry`

```cpp
// drm-cxx/planes/plane_registry.hpp
namespace drm::planes {

struct PlaneCapabilities {
  uint32_t id;
  uint32_t possible_crtcs;       // bitmask
  DRMPlaneType type;             // PRIMARY, OVERLAY, CURSOR
  std::vector<uint32_t> formats; // DRM_FORMAT_*
  std::optional<uint64_t> zpos_min, zpos_max;
  bool supports_rotation{false};
  bool supports_scaling{false};
};

class PlaneRegistry {
public:
  static std::expected<PlaneRegistry, std::error_code>
    enumerate(const Device& dev);

  std::span<const PlaneCapabilities> all()   const noexcept;
  std::span<const PlaneCapabilities> for_crtc(uint32_t crtc_index) const;

private:
  std::vector<PlaneCapabilities> planes_;
};

} // namespace drm::planes
```

### 4.3 `Layer` (replaces `liftoff_layer`)

```cpp
// drm-cxx/planes/layer.hpp
namespace drm::planes {

class Layer {
public:
  // Set any KMS property by name; stored in internal map
  Layer& set_property(std::string_view name, uint64_t value);

  // Disable: equivalent to FB_ID = 0
  Layer& disable() noexcept;

  // Mark as software-only (will never be put in a hw plane)
  Layer& set_composited() noexcept;

  bool needs_composition() const noexcept; // set by Allocator after apply()

  std::optional<uint32_t> assigned_plane_id() const noexcept;

private:
  friend class Allocator;
  std::unordered_map<std::string, uint64_t> properties_;
  bool force_composited_{false};
  bool needs_composition_{false};
  std::optional<uint32_t> assigned_plane_{};
};

} // namespace drm::planes
```

### 4.4 `Output` (replaces `liftoff_output`)

```cpp
// drm-cxx/planes/output.hpp
namespace drm::planes {

class Output {
public:
  Output(uint32_t crtc_id, Layer& composition_layer);

  Layer& add_layer();
  void   remove_layer(const Layer& layer);

  void   set_composition_layer(Layer& layer);

  // Ordered by zpos descending; used by Allocator
  std::span<Layer*> layers() noexcept;

  uint32_t crtc_id() const noexcept;

private:
  uint32_t crtc_id_;
  std::vector<std::unique_ptr<Layer>> owned_layers_;
  Layer* composition_layer_{};
};

} // namespace drm::planes
```

### 4.5 `Allocator` (core of the replacement)

```cpp
// drm-cxx/planes/allocator.hpp
namespace drm::planes {

class Allocator {
public:
  Allocator(const Device& dev, PlaneRegistry& registry);

  // Main entry point — mirrors liftoff_output_apply()
  // Populates req with atomic properties for all assigned planes.
  // Returns how many layers were hardware-assigned.
  std::expected<std::size_t, std::error_code>
    apply(Output& output, AtomicRequest& req, uint32_t commit_flags);

private:
  // Attempt a full or partial hw assignment via atomic test commits.
  // Uses a greedy + backtrack algorithm (depth bounded by plane count).
  bool try_assign(Output& output,
                  std::span<Layer*> unassigned,
                  std::span<const PlaneCapabilities> available_planes,
                  AtomicRequest& req,
                  uint32_t flags);

  bool plane_compatible_with_layer(const PlaneCapabilities& plane,
                                   const Layer& layer,
                                   uint32_t crtc_index) const;

  const Device&   dev_;
  PlaneRegistry&  registry_;
  PropertyStore   prop_store_;  // cached property IDs per object
};

} // namespace drm::planes
```

### 4.6 Algorithm Details

The allocator implements the same greedy + atomic-test approach as libliftoff:

1. **Sort layers** by zpos (explicit or inferred from `PRIMARY > OVERLAY > CURSOR` type).
2. **Prune candidate planes** per layer: format support, CRTC mask, zpos range, scaling/rotation flags.
3. **Greedy first pass**: assign each layer to the highest-priority compatible plane; build atomic req; test-commit. If test passes → accept.
4. **Backtrack on failure**: remove the most recently assigned layer, mark it `needs_composition`, retry. Bounded by O(layers × planes) attempts.
5. **Composition layer**: if any layer `needs_composition`, the composition layer is always kept on the primary plane.
6. **Caching**: skip re-allocation if no layer properties changed since last commit (equivalent to libliftoff's change detection).

---

## 5. Replacing Removed Subprojects

### 5.1 `libsync` → native fence helpers

```cpp
// drm-cxx/sync/fence.hpp
namespace drm::sync {

class SyncFence {
public:
  static std::expected<SyncFence, std::error_code> import(int fence_fd);
  std::expected<void, std::error_code> wait(std::chrono::milliseconds timeout);
  void merge(SyncFence& other);
  ~SyncFence();   // closes fd
private:
  int fd_{-1};
};

} // namespace drm::sync
```

Uses `<sys/ioctl.h>` + `linux/sync_file.h` directly — no subproject needed.

### 5.2 `bsdrm` → `drm-cxx/core/`

bsdrm provides DRM resource wrappers. Replace with RAII C++23 wrappers around
`drmMode*` structs:

```cpp
// drm-cxx/core/resources.hpp
namespace drm {

using Resources   = std::unique_ptr<drmModeRes,     decltype(&drmModeFreeResources)>;
using Connector   = std::unique_ptr<drmModeConnector,decltype(&drmModeFreeConnector)>;
using Encoder     = std::unique_ptr<drmModeEncoder,  decltype(&drmModeFreeEncoder)>;
using CrtcPtr     = std::unique_ptr<drmModeCrtc,     decltype(&drmModeFreeCrtc)>;
using PlaneResPtr = std::unique_ptr<drmModePlaneRes, decltype(&drmModeFreePlaneResources)>;

Resources   get_resources(int fd);
Connector   get_connector(int fd, uint32_t id);
Encoder     get_encoder(int fd, uint32_t id);
CrtcPtr     get_crtc(int fd, uint32_t id);
PlaneResPtr get_plane_resources(int fd);

} // namespace drm
```

### 5.3 `spdlog` → `std::print`

Every `spdlog::info(...)` call becomes:

```cpp
// drmpp
spdlog::info("Plane {}: type={}, formats={}", id, type, fmt_count);

// drm-cxx
std::println("[drm::planes] Plane {}: type={}, formats={}", id, type, fmt_count);
```

Conditional verbosity via a compile-time `DRM_CXX_LOG_LEVEL` define or a runtime
`drm::set_log_level(drm::LogLevel)` function that gates `std::println` calls.

### 5.4 `rapidjson` → mini-parser / `std::string_view`

drmpp uses rapidjson for config/capability JSON. Replace with a minimal `std::string_view`
token parser for the specific schemas used, or adopt `std::format` + hand-written
serialisation. If a full JSON dependency remains desirable, use **nlohmann/json**
as a header-only Meson wrap (no CMake required).

---

## 6. C++23 Specific Modernizations

| Feature | Usage in drm-cxx |
|---|---|
| `std::expected<T,E>` | All fallible operations (replaces `int` errno returns) |
| `std::print` / `std::println` | All logging output |
| `std::format` | String formatting (replaces spdlog fmtlib) |
| `std::move_only_function` | Event callbacks (cheaper than `std::function`) |
| `std::flat_map` / `std::flat_set` | Property stores (cache-friendly ordered maps) |
| `std::mdspan` | 2D pixel buffer views |
| `std::ranges::` algorithms | Plane filtering, layer sorting |
| `std::span` | Non-owning views of plane/format lists |
| Deducing `this` | CRTP-free mixin patterns for event handlers |
| `[[nodiscard]]` everywhere | Enforce checking of error returns |
| Explicit `auto` return types | Clarity in factory functions |
| Structured bindings (C++17 compat) | Destructuring KMS property pairs |

---

## 7. Input Subsystem Changes

drmpp's `libinput` / `xkbcommon` integration is retained but modernised:

- `libinput` context and seat wrapped in RAII `drm::input::Seat`.
- Event callbacks use `std::move_only_function<void(InputEvent)>` instead of virtual dispatch.
- `InputEvent` is a `std::variant<KeyboardEvent, PointerEvent, TouchEvent, SwitchEvent>`.
- xkbcommon state is owned by `drm::input::Keyboard` with `std::unique_ptr<xkb_state, ...>`.
- The `$HOME/.xkb/keymap.xkb` path remains but is configurable via `Seat::Options`.

---

## 8. Display Info

`libdisplay-info` is retained as a Meson wrap subproject (no CMake; it already uses Meson).
Wrap it behind `drm::display::ConnectorInfo` to isolate the public API from the C types:

```cpp
// drm-cxx/display/edid.hpp
namespace drm::display {

struct ColorimetryInfo {
  struct { float x, y; } red, green, blue, white;
};

struct HdrStaticMetadata {
  float max_luminance;
  float min_luminance;
  float max_cll;
  float max_fall;
};

struct ConnectorInfo {
  std::string name;
  std::optional<ColorimetryInfo> colorimetry;
  std::optional<HdrStaticMetadata> hdr;
  std::vector<uint32_t> supported_eotfs;   // HDMI_EOTF_*
};

std::expected<ConnectorInfo, std::error_code>
  parse_edid(std::span<const uint8_t> edid_blob);

} // namespace drm::display
```

---

## 9. Vulkan Integration

Optionally gated behind `get_option('vulkan')`:

- Replaces CMake `subproject('Vulkan-Headers')` with a Meson wrap.
- `drm::vulkan::Display` wraps `VK_KHR_display` plane enumeration.
- DRM plane IDs cross-referenced with Vulkan display planes.

---

## 10. Migration Mapping (drmpp → drm-cxx)

| drmpp symbol | drm-cxx equivalent |
|---|---|
| `drmpp::drm::Device` | `drm::Device` |
| `drmpp::drm::Connector` | `drm::Resources` + `drm::get_connector()` |
| `drmpp::input::Libinput` | `drm::input::Seat` |
| `drmpp::input::Keyboard` | `drm::input::Keyboard` |
| `liftoff_device_create()` | `drm::planes::PlaneRegistry::enumerate()` |
| `liftoff_output_create()` | `drm::planes::Output` constructor |
| `liftoff_layer_create()` | `output.add_layer()` |
| `liftoff_layer_set_property()` | `layer.set_property()` |
| `liftoff_output_apply()` | `allocator.apply(output, req, flags)` |
| `liftoff_layer_needs_composition()` | `layer.needs_composition()` |
| `liftoff_output_set_composition_layer()` | `output.set_composition_layer(layer)` |
| `spdlog::info(...)` | `std::println(...)` |
| `rapidjson::Document` | custom `drm::config::parse()` |
| `libsync` `sync_wait()` | `drm::sync::SyncFence::wait()` |
| `bsdrm` helpers | `drm::core::*` RAII wrappers |

---

## 11. Migration Phases

### Phase 1 — Scaffold & Build System (Week 1)

- [x] Create `drm-cxx/` repo, canonical directory layout.
- [x] Write root `meson.build` and `meson.options` targeting C++23.
- [x] Confirm compiler availability (GCC ≥ 13 / Clang ≥ 16).
- [x] Add CI workflow (GitHub Actions) with matrix: GCC-13, GCC-14, Clang-16, Clang-17.
- [x] Port `config.h.meson`.
- [x] Stub all translation units (empty `.cpp` files that compile clean).

### Phase 2 — Core + Resources (Week 2)

- [ ] `drm::Device` — fd RAII, capability enables.
- [ ] `drm::core::resources.hpp` — RAII wrappers replacing bsdrm.
- [ ] `drm::core::PropertyStore` — property ID cache.
- [ ] `drm::modeset::AtomicRequest` — atomic req builder.
- [ ] `drm::sync::SyncFence` — native sync (removes libsync subproject).
- [ ] Unit tests for all of the above.

### Phase 3 — Native Plane Allocator (Week 3–4)

- [ ] `PlaneRegistry::enumerate()` — detect all planes + caps.
- [ ] `Layer` — property map, composited flag.
- [ ] `Output` — CRTC output, layer list, composition layer.
- [ ] `Allocator::apply()` — greedy + backtrack algorithm.
- [ ] Atomic test-commit loop.
- [ ] Change detection / allocation caching.
- [ ] Unit tests using mock DRM fd.
- [ ] Integration test: `overlay_planes` example against real hardware.

### Phase 4 — Input Subsystem (Week 5)

- [ ] `drm::input::Seat` — libinput context + udev.
- [ ] `drm::input::Keyboard` — xkbcommon RAII.
- [ ] `drm::input::Pointer`, `Touch`, `Switch`.
- [ ] `InputEvent` variant + `move_only_function` dispatch.
- [ ] Port `$HOME/.xkb/keymap.xkb` loading.

### Phase 5 — Display Info & Modeset (Week 6)

- [ ] `drm::display::ConnectorInfo` wrapping libdisplay-info.
- [ ] HDR metadata, colorimetry, EOTF support.
- [ ] `drm::modeset::Mode` — mode matching, preferred mode selection.
- [ ] `drm::modeset::PageFlip` — vblank event loop with `epoll`.
- [ ] `atomic_modeset` example end-to-end.

### Phase 6 — GBM + Vulkan (Week 7)

- [ ] `drm::gbm::Device`, `Surface`, `Buffer`.
- [ ] Vulkan `VK_KHR_display` integration (feature-gated).
- [ ] `vulkan_display` example.

### Phase 7 — Cleanup, Docs, Release (Week 8)

- [ ] Remove all references to libliftoff, bsdrm, libsync, spdlog, rapidjson.
- [ ] `std::print` logging with `DRM_CXX_LOG_LEVEL` gating.
- [ ] `pkg-config` file via Meson.
- [ ] `drm-cxx.hpp` aggregate header.
- [ ] `install_headers()` for all public headers.
- [ ] README, CHANGELOG, migration guide.
- [ ] clang-tidy + clang-format pass.
- [ ] Tag `v1.0.0`.

---

## 12. Compiler & Toolchain Requirements

| Tool | Minimum version | Reason |
|---|---|---|
| GCC | 13.1 | `std::expected`, `std::print`, `std::flat_map` |
| Clang | 16.0 | Same |
| Meson | 1.3.0 | `meson.options`, structured subproject options |
| Ninja | 1.11 | Parallel C++23 builds |
| libdrm | 2.4.113 | Universal planes, atomic, property blob |
| libinput | 1.21 | Full touch/gesture API |
| xkbcommon | 1.5 | |
| libdisplay-info | 0.1.1 | HDR metadata |
| Vulkan-Headers | 1.3 | Optional |

---

## 13. Native Plane Allocator — Algorithm Improvements over libliftoff

The native `drm::planes::Allocator` replaces libliftoff's C backtracking search. The
baseline algorithm in libliftoff v0.5.0 is a depth-first backtracking search with
branch-and-bound pruning: it walks planes in kernel-enumeration order, tries each
unassigned layer per plane via `drmModeAtomicCommit(TEST_ONLY)`, and prunes branches
when no remaining combination can beat the current best score. A 1ms wall-clock timeout
was added in v0.5.0 precisely because worst-case exponential behavior is observable on
real hardware. The drm-cxx allocator eliminates that need through seven targeted
improvements.

### 13.1 Tighter Upper Bound for Branch-and-Bound Pruning

The v0.5.0 prune condition counts remaining _compatible_ planes (CRTC mask only). The
improved bound builds a full static compatibility matrix before the search begins —
checking format/modifier support, rotation capability, scaling support, zpos range, and
cursor dimension caps — all of which are pure memory reads with zero ioctls. The bound
becomes `min(layers_needing_planes, per-layer-compatible-plane-count)`. This prunes
entire subtrees that the current algorithm enters before discovering failure.

```cpp
// drm-cxx/planes/allocator.cpp
int Allocator::static_upper_bound(
    std::span<const Layer*> remaining_layers,
    std::span<const PlaneCapabilities> remaining_planes) const
{
    int bound = 0;
    for (const Layer* layer : remaining_layers) {
        bool any = std::ranges::any_of(remaining_planes,
            [&](const PlaneCapabilities& p) {
                return plane_statically_compatible(p, *layer);
            });
        if (any) ++bound;
    }
    return bound;
}

bool Allocator::plane_statically_compatible(
    const PlaneCapabilities& plane, const Layer& layer) const
{
    if (!(plane.possible_crtcs & crtc_mask_))               return false;
    if (layer.format() &&
        !plane.supports_format(*layer.format(), layer.modifier())) return false;
    if (layer.rotation() != DRM_MODE_ROTATE_0
        && !plane.supports_rotation)                          return false;
    if (layer.requires_scaling() && !plane.supports_scaling) return false;
    if (auto z = layer.property("zpos")) {
        if (plane.zpos_min && *z < *plane.zpos_min)           return false;
        if (plane.zpos_max && *z > *plane.zpos_max)           return false;
    }
    if (plane.type == DRMPlaneType::CURSOR) {
        if (layer.width()  > plane.cursor_max_w)              return false;
        if (layer.height() > plane.cursor_max_h)              return false;
    }
    return true;
}
```

**Important**: every static check must be a necessary condition only, never sufficient.
The atomic test commit remains the final arbiter. Over-pruning silently skips valid
assignments, which is worse than the baseline algorithm.

### 13.2 Best-First Search Order

libliftoff walks planes in kernel-enumeration order — effectively random with respect to
likelihood of success. A better assignment found early triggers the pruning bound sooner,
cutting exponentially more of the remaining tree. Pre-score each (layer, plane) pair using
static attributes before entering the search; try highest-scoring pairs first.

```cpp
struct CandidatePair {
    const PlaneCapabilities* plane;
    Layer* layer;
    int score;
};

std::vector<CandidatePair> Allocator::rank_candidates(Output& output) const {
    std::vector<CandidatePair> pairs;
    for (auto& plane : registry_.all())
        for (auto* layer : output.layers())
            if (plane_statically_compatible(plane, *layer))
                pairs.push_back({&plane, layer, score_pair(plane, *layer)});
    std::ranges::sort(pairs, std::greater{}, &CandidatePair::score);
    return pairs;
}

int Allocator::score_pair(const PlaneCapabilities& plane, const Layer& layer) const {
    int s = 0;
    if (layer.format() && plane.supports_format_natively(*layer.format()))   s += 4;
    if (layer.is_composition_layer() && plane.type == DRMPlaneType::PRIMARY)  s += 8;
    if (!layer.is_composition_layer() && plane.type == DRMPlaneType::OVERLAY) s += 2;
    s -= failure_cache_.hit_count(plane.id, layer.property_hash());
    return s;
}
```

This turns blind iteration into an informed best-first descent, routinely finding the
optimal assignment on the very first path.

### 13.3 Warm-Start from the Previous Frame's Allocation

This is the highest-leverage single improvement. At 60+ fps the overwhelming majority of
frames are structurally identical to the previous one — the plane assignment doesn't
change, only the buffer (FB_ID) and possibly position/alpha do. Dirty-bit tracking on
layers lets the allocator detect this.

```cpp
std::expected<std::size_t, std::error_code>
Allocator::apply(Output& output, AtomicRequest& req, uint32_t flags)
{
    // Fast path: nothing changed
    if (!output.any_layer_dirty())
        return apply_previous_allocation(output, req, flags);

    // Warm-start: try previous allocation first (one test commit)
    if (previous_allocation_valid_) {
        auto result = try_allocation(previous_allocation_, output, req, flags);
        if (result) { output.mark_clean(); return result; }
    }

    // Incremental: only one layer changed — re-test that assignment only
    auto changed = output.changed_layers();
    if (changed.size() == 1) {
        auto result = try_incremental(changed[0], output, req, flags);
        if (result) { output.mark_clean(); return result; }
    }

    // Full search — reached only on structural changes
    return full_search(output, req, flags);
}
```

Structural changes (new layers, format changes, surface destruction) are rare. Position,
zpos, and alpha changes happen every frame but don't invalidate the plane assignment.
Result: 0–1 test commits per frame in steady state.

### 13.4 Test-Commit Result Caching (Failure Memoization)

When `drmModeAtomicCommit(TEST_ONLY)` fails the result is deterministic for a given
(plane, property-set) combination. Memoizing failures avoids repeating the same losing
ioctl across frames or across backtracking branches.

```cpp
class TestCache {
public:
    std::optional<bool> lookup(uint32_t plane_id, std::size_t prop_hash) const {
        auto it = cache_.find({plane_id, prop_hash});
        if (it == cache_.end()) return std::nullopt;
        return it->second.passed;
    }
    void record(uint32_t plane_id, std::size_t prop_hash, bool passed) {
        cache_[{plane_id, prop_hash}] = {prop_hash, passed};
    }
private:
    std::flat_map<std::pair<uint32_t, std::size_t>, bool> cache_;
};
```

Properties stable across frames (format, modifier, rotation, scaling) form the cache key.
FB_ID changes every frame but buffer properties (format, modifier, dimensions) can be
tracked separately and used as a proxy.

### 13.5 Bipartite Pre-Solve (Hopcroft-Karp)

Model the allocation as a maximum weight bipartite matching problem using only static
constraints, then use the result to seed the backtracking search. Hopcroft-Karp finds the
maximum cardinality matching in O(E√V) — for typical plane/layer counts of ≤ 8, this
runs in microseconds with zero ioctls.

```
Layers (L)          Planes (P)
  L0  ──────────── P0 (primary)
  L1  ───────────/ P1 (overlay)
  L2  ──────────── P2 (overlay)
  L3               P3 (cursor)
       static compatibility edges only
```

The matching gives the best-possible assignment assuming static checks are complete.
Validate with one test commit:
- **Passes**: done — one ioctl for the whole frame.
- **Fails**: the search starts from a near-optimal state with a tight upper bound, so
  backtracking is minimal.

This removes the need for libliftoff's 1ms hard timeout in normal operation. The drm-cxx
allocator exposes a configurable `max_test_commits` budget (default: 16) which is
effectively never reached with improvements 1–5 active.

### 13.6 Content-Type Layer Priority

Not all layers are equally costly to drop to software composition. A 4K video surface
dropped to composition costs a full GPU blit at 60fps; a static desktop wallpaper dropped
to composition costs a one-time blit. The allocator should preferentially assign scarce
overlay planes to high-churn surfaces.

```cpp
int Allocator::layer_priority(const Layer& layer) const {
    if (layer.content_type() == ContentType::Video)  return 100;
    if (layer.update_hz() > 30)                       return  80;
    if (layer.update_hz() > 0)                        return  50;
    return 10;  // static / infrequently updated
}
```

Exposed via `Layer::set_content_type()` and `Layer::set_update_hint()` — the compositor
already knows which surfaces are video. Feeds directly into `score_pair()` (§13.2).

### 13.7 Spatial Intersection Splitting

The zpos ordering constraint — layer `L_a` above `L_b` must be on a plane with higher
zpos — only applies when `L_a` and `L_b` spatially overlap. Non-overlapping layers have
no ordering constraint between them and their plane assignments are fully independent
sub-problems.

```cpp
bool Allocator::layers_intersect(const Layer& a, const Layer& b) const {
    auto ra = a.crtc_rect(), rb = b.crtc_rect();
    return !(ra.x + ra.w <= rb.x || rb.x + rb.w <= ra.x ||
             ra.y + ra.h <= rb.y || rb.y + rb.h <= ra.y);
}
```

Non-overlapping groups are solved independently via union-find grouping before the search
begins. On a typical tiled window manager with non-overlapping surfaces, this can split
an 8-layer problem into several independent 2–3 layer problems — each trivially solved in
one or two test commits.

### 13.8 Improvement Summary

| # | Improvement | Mechanism | Impact |
|---|---|---|---|
| 1 | Tighter upper bound | Static compat matrix pre-search | High |
| 2 | Best-first search order | Score (layer,plane) pairs statically | High |
| 3 | Warm-start from previous frame | Dirty tracking + incremental re-test | **Critical** |
| 4 | Test-commit result caching | Memoize (plane, prop_hash) → pass/fail | High |
| 5 | Bipartite pre-solve | Hopcroft-Karp max matching → seed search | **Critical** |
| 6 | Content-type layer priority | Video surfaces prefer overlay planes | Medium |
| 7 | Spatial intersection splitting | Non-overlapping → independent sub-problems | Medium |

**Net result vs. libliftoff v0.5.0:**
- Steady state (compositor running): 0–1 test commits per frame vs. potentially dozens.
- Cold start / structural change: sub-linear ioctl count vs. exponential.
- Hard 1ms timeout: unnecessary — replaced by a configurable `max_test_commits` budget.

**Phase 3 note**: All seven improvements are in scope for the Phase 3 deliverable
(§11 Improvement 3, Week 3–4). The Hopcroft-Karp matching (§13.5) should be implemented
as a standalone `drm-cxx/planes/matching.hpp` with its own unit tests before being wired
into `Allocator`.

---

## 14. Breaking Change Summary

The following is **not** backward compatible with drmpp. Consumers must migrate.

1. **Namespace**: `drmpp::` → `drm::`.
2. **No libliftoff header**: `#include <liftoff.h>` must be replaced with `#include <drm-cxx/planes/allocator.hpp>` etc.
3. **`std::expected` returns**: All `int`-returning functions now return `std::expected<T, std::error_code>`.
4. **No spdlog dependency**: Any consumer that linked spdlog via drmpp's public headers must provide their own spdlog.
5. **Callbacks via `std::move_only_function`**: Virtual base class callbacks (if any existed) are gone.
6. **Header paths**: `#include <drmpp/drmpp.h>` → `#include <drm-cxx/drm-cxx.hpp>`.
7. **Single-directory layout**: No `include/` / `src/` split; all headers installed from `drm-cxx/`.