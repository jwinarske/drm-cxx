# Building drm-cxx under GN (Chromium / Flutter Engine)

drm-cxx ships a GN build (`BUILD.gn`, `drm_cxx.gni`) alongside its CMake/meson
builds so it can be vendored into a GN tree — Chromium, or the Flutter Engine /
buildroot — and compiled **with the host's own toolchain** (Chromium's bundled
**clang + libc++**).

## Why a GN build

The target use case is a Chromium- or Flutter-Engine-based compositor that
presents through drm-cxx as its KMS backend, linking drm-cxx **directly** into
the GPU process. Chromium builds with a hermetic **clang + bundled libc++**; a
CMake build of drm-cxx uses the system **gcc/libstdc++**, whose `std::*` ABI
(`std::__cxx11` vs libc++ `std::__1`) does **not** match — linking the two mixes
ABIs and breaks. A C-ABI shim (`extern "C"` + a firewalled `.so`) sidesteps this
but flattens rich types (plane lists, fences, modifiers) into C structs.

Building drm-cxx **as a GN target** makes GN compile it with the **same
toolchain + libc++** as the embedder, so the ABI matches by construction and
consumers call `drm::Device` / `drm::AtomicRequest` / `drm::Allocator` directly —
no shim, full type fidelity.

## What's in scope: the minimal subset

`drm_cxx.gni`'s `drm_cxx_min_sources` is the dep-light KMS/scanout subset,
verified free of blend2d / gstreamer / EGL / Vulkan / v4l2 / libinput / libudev /
libdisplay-info / `scene/`:

- `core/` (minus `egl_loader`/`gles_loader`), `modeset/`, `planes/`, `gbm/`,
  `dumb/`, `sync/`, `fmt/`, `time/`, and
  `present/{negotiate,buffer_ring,scanout_format}`.

**Needs only `libdrm` + `gbm`.** This is the surface a KMS backend built on
drm-cxx uses: atomic commit, plane assignment + TEST_ONLY, GBM alloc + AddFB2,
fences. Deliberately **excluded**: `display/` (EDID/connector/HDR — pulls
libdisplay-info, and a Chromium/Flutter consumer owns display config itself),
`scene/` (the compositor — a consumer's own compositor replaces it), the
GL/Vulkan `scanout_producer`s, `scanout_backend` (pulls `scene/`), and
gst/v4l2/blend2d/cursor/vulkan — those stay CMake-only (a consumer doesn't need
them and they pull deps absent from a stock GN tree).

## Vendor + consume

1. Vendor drm-cxx at `//third_party/drm-cxx` (DEPS entry or submodule). Its
   `third_party/{tl-expected,tcb-span,fmt}` submodules must come along (used on
   the C++20 leg).
2. Depend on it:
   ```gn
   deps += [ "//third_party/drm-cxx:drm_cxx_min" ]
   ```
3. Call drm-cxx directly:
   ```cpp
   #include <drm-cxx/core/device.hpp>
   #include <drm-cxx/modeset/atomic.hpp>
   // drm::Device::from_fd(host_drm_fd); drm::AtomicRequest{...}.commit(...);
   ```

### GN args (`drm_cxx.gni`)
- `drm_cxx_version` — keep in sync with `project()` in `CMakeLists.txt`; surfaced
  into the generated `config.h`.
- `drm_cxx_have_vulkan` — leave `false`; the minimal subset has no Vulkan sources.

### Targets / actions in `BUILD.gn`
- `pkg_config("drm_cxx_libdrm")`, `pkg_config("drm_cxx_gbm")` — system deps via
  `//build/config/linux/pkg_config.gni` (present in both Chromium and the Flutter
  buildroot). Swap libdrm for `//third_party/libdrm` to use Chromium's in-tree
  copy.
- `action("drm_cxx_include_symlink")` — creates the build-tree `drm-cxx → src`
  symlink so `<drm-cxx/...>` includes resolve (mirrors the CMake build-tree
  symlink). `config("drm_cxx_public_config")` adds `$gen/include` (symlink),
  `$gen` (config.h), `src` (relative cross-includes), and the polyfill include
  dirs + `FMT_HEADER_ONLY`.
- `action("drm_cxx_gen_config")` — generates `config.h` from `config.h.in`
  (version + `HAVE_VULKAN=0`). Optional for the KMS subset — no subset source
  includes `config.h` today; kept for safety / future sources.

The `gn/` helper scripts (`gen_config.py`, `make_include_symlink.py`) are plain
Python 3 with no third-party imports.

## CI

The `build-gn` job in `.github/workflows/ci.yml` compiles the exact
`drm_cxx_min_sources` list under **clang + libc++** at both C++20 (vendored
`tl-expected`/`tcb-span`/`fmt`) and C++23 (libc++ `std::expected`/`std::span`/
`std::format`) — the toolchain a GN consumer builds drm-cxx with. A full
`gn gen` needs the consumer tree's `//build`, so CI validates the load-bearing
property (the KMS sources are libc++-clean) without pulling in Chromium.

## Extending beyond the minimal subset

To GN-build a larger subset (e.g. drm-cxx's own `scene/`/compositor for a
non-Chromium GN consumer), add the sources to a new target and supply GN deps for
their externals: EGL/GLES (`//ui/gl` or pkg_config), Vulkan
(`//third_party/vulkan-deps`), blend2d/gstreamer/v4l2 (pkg_config or new
third_party targets). Keep these out of the `drm_cxx_min` target so the leaf the
GPU process links stays minimal.
