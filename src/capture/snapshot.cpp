// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "snapshot.hpp"

#include "../core/device.hpp"
#include "../core/property_store.hpp"
#include "../log.hpp"

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// Blend2D's header layout varies by distro / install method:
//   * upstream source install and Fedora:  /usr/include/blend2d/blend2d.h
//   * some older Debian/Ubuntu packages:   /usr/include/blend2d.h
// Pick whichever the preprocessor can reach. NOLINT is required because
// the umbrella re-exports sub-headers — misc-include-cleaner can't
// attribute any symbol directly to it.
//
// When *neither* path is reachable we deliberately DO NOT #error: some
// CI configurations run clang-tidy across every source file regardless
// of whether the build system would exclude this TU, and the tidy run
// does not inherit Blend2D's `-isystem` flags. Degrading to a
// Blend2D-free TU (snapshot() is gated out below) keeps those tidy
// passes green; a real build where Blend2D is expected still fails
// loudly at link-time via the missing snapshot() symbol.
#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>  // NOLINT(misc-include-cleaner)
#define DRM_CXX_CAPTURE_HAS_BL2D
#elif __has_include(<blend2d.h>)
#include <blend2d.h>  // NOLINT(misc-include-cleaner)
#define DRM_CXX_CAPTURE_HAS_BL2D
#endif

// drm::expected, drm::unexpected, std::error_code, std::make_error_code
// and std::errc reach this TU transitively through "snapshot.hpp", so
// <drm-cxx/detail/expected.hpp> and <system_error> are deliberately not
// included here — CI's misc-include-cleaner flags them as redundant.
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace drm::capture {

Image::Image(std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) {
    return;
  }
  w_ = width;
  h_ = height;
  pixels_.assign(static_cast<std::size_t>(width) * height, 0U);
}

#ifdef DRM_CXX_CAPTURE_HAS_BL2D

namespace {

// RAII mmap wrapper. Owns a single mapping whose lifetime covers the
// BLImage that wraps its data — we pass BL_DATA_ACCESS_READ to
// create_from_data so Blend2D never writes, and we tear the mapping
// down after the blit context has ended.
struct FbMapping {
  void* data{nullptr};
  std::size_t size{0};

  FbMapping() = default;
  FbMapping(const FbMapping&) = delete;
  FbMapping& operator=(const FbMapping&) = delete;
  FbMapping(FbMapping&& other) noexcept : data(other.data), size(other.size) {
    other.data = nullptr;
    other.size = 0;
  }
  FbMapping& operator=(FbMapping&& other) noexcept {
    if (this != &other) {
      unmap();
      data = other.data;
      size = other.size;
      other.data = nullptr;
      other.size = 0;
    }
    return *this;
  }
  ~FbMapping() { unmap(); }

  void unmap() noexcept {
    if (data != nullptr && data != MAP_FAILED) {
      ::munmap(data, size);
    }
    data = nullptr;
    size = 0;
  }
};

// One plane's worth of scanout state, ready to blit.
struct PlaneFrame {
  std::uint32_t plane_id{};
  std::uint32_t width{};   // FB width
  std::uint32_t height{};  // FB height
  std::uint32_t pitch{};   // FB stride in bytes
  std::uint32_t format{};  // FB fourcc

  std::int32_t crtc_x{};  // plane placement on CRTC (signed)
  std::int32_t crtc_y{};
  std::uint32_t crtc_w{};  // 0 means "same as FB width"
  std::uint32_t crtc_h{};

  std::int32_t zpos{0};

  FbMapping mapping;
};

// Export GEM handle → DMA-BUF → mmap read-only. Returns an empty
// mapping on any failure (a driver may not support DMA-BUF mmap, or the
// buffer may live in VRAM without a host-visible mapping). O_CLOEXEC
// is used directly rather than the DRM_CLOEXEC alias so
// misc-include-cleaner can see which header (<fcntl.h>) supplies the
// flag — DRM_CLOEXEC is a macro in <drm.h> that expands to O_CLOEXEC.
FbMapping map_plane_fb(int fd, std::uint32_t handle, std::size_t size) {
  FbMapping out;
  int dmabuf_fd = -1;
  const int export_r = drmPrimeHandleToFD(fd, handle, O_CLOEXEC, &dmabuf_fd);
  if (export_r != 0 || dmabuf_fd < 0) {
    return out;
  }
  void* ptr = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, dmabuf_fd, 0);
  ::close(dmabuf_fd);
  if (ptr == MAP_FAILED) {
    return out;
  }
  out.data = ptr;
  out.size = size;
  return out;
}

// V1 accepts only the two 32-bit packed RGB formats. Plan item: NV12
// and tiled variants explicitly skip with a warning.
bool is_supported_format(std::uint32_t fourcc) {
  return fourcc == DRM_FORMAT_ARGB8888 || fourcc == DRM_FORMAT_XRGB8888;
}

// Fill geometry from the plane's cached property values. Returns false
// if CRTC_X/Y/W/H couldn't be read — the plane is skipped in that case.
bool fill_plane_geometry(const drm::PropertyStore& props, std::uint32_t plane_id, PlaneFrame& out) {
  auto x = props.property_value(plane_id, "CRTC_X");
  auto y = props.property_value(plane_id, "CRTC_Y");
  auto w = props.property_value(plane_id, "CRTC_W");
  auto h = props.property_value(plane_id, "CRTC_H");
  if (!x || !y || !w || !h) {
    return false;
  }
  // CRTC_X / CRTC_Y are SIGNED int32 stored in an uint64 slot. Do a
  // two-step cast so sign-extension works across the int32 boundary.
  out.crtc_x = static_cast<std::int32_t>(static_cast<std::int64_t>(*x));
  out.crtc_y = static_cast<std::int32_t>(static_cast<std::int64_t>(*y));
  out.crtc_w = static_cast<std::uint32_t>(*w);
  out.crtc_h = static_cast<std::uint32_t>(*h);
  if (auto z = props.property_value(plane_id, "zpos"); z) {
    out.zpos = static_cast<std::int32_t>(static_cast<std::int64_t>(*z));
  }
  return true;
}

// Blend2D surfaces its public API through the <blend2d/blend2d.h> umbrella,
// which misc-include-cleaner cannot attribute as a symbol provider (the
// umbrella only re-exports sub-headers whose on-disk layout differs across
// distro packages). Suppress the check for everything that touches Blend2D
// types — the include itself is already NOLINT'd at the top of the file.
// NOLINTBEGIN(misc-include-cleaner)

BLFormat bl_format_for(std::uint32_t fourcc) {
  // ARGB8888 scanout is premultiplied by KMS convention — Blend2D's
  // PRGB32 matches that layout byte-for-byte on LE hosts. XRGB8888 has
  // undefined alpha; treated as XRGB32, so Blend2D ignores it, and the
  // SRC_OVER blend sees opaque pixels.
  return fourcc == DRM_FORMAT_ARGB8888 ? BL_FORMAT_PRGB32 : BL_FORMAT_XRGB32;
}

}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage)
drm::expected<Image, std::error_code> snapshot(const drm::Device& device, std::uint32_t crtc_id) {
  const int fd = device.fd();

  // ─── CRTC geometry (output size) ────────────────────────
  auto* crtc_raw = drmModeGetCrtc(fd, crtc_id);
  if (crtc_raw == nullptr) {
    const int saved = errno != 0 ? errno : EINVAL;
    return drm::unexpected<std::error_code>(std::error_code(saved, std::system_category()));
  }
  std::unique_ptr<drmModeCrtc, decltype(&drmModeFreeCrtc)> crtc(crtc_raw, drmModeFreeCrtc);
  if (crtc->mode_valid == 0) {
    drm::log_warn("capture: crtc {} has no valid mode (mode_valid=0) — nothing to snapshot",
                  crtc_id);
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_device_or_address));
  }
  const std::uint32_t out_w = crtc->mode.hdisplay;
  const std::uint32_t out_h = crtc->mode.vdisplay;

  // ─── Enumerate planes ───────────────────────────────────
  auto* pres_raw = drmModeGetPlaneResources(fd);
  if (pres_raw == nullptr) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  std::unique_ptr<drmModePlaneRes, decltype(&drmModeFreePlaneResources)> pres(
      pres_raw, drmModeFreePlaneResources);

  // `frames` owns every FbMapping we hand to Blend2D via
  // create_from_data(BL_DATA_ACCESS_READ, null-destroy-cb). Its
  // destruction (and thus the munmap of each plane's pixels) MUST run
  // after ctx.end() returns — Blend2D may defer blit work to worker
  // threads, so the mappings have to stay alive until composition
  // flushes. Keep this declared at function scope so the natural
  // destructor order is (BLContext via end() first, then frames).
  std::vector<PlaneFrame> frames;
  frames.reserve(pres->count_planes);
  std::uint32_t unbound_planes = 0;

  for (std::uint32_t i = 0; i < pres->count_planes; ++i) {
    const std::uint32_t plane_id = pres->planes[i];
    auto* plane_raw = drmModeGetPlane(fd, plane_id);
    if (plane_raw == nullptr) {
      continue;
    }
    std::unique_ptr<drmModePlane, decltype(&drmModeFreePlane)> plane(plane_raw, drmModeFreePlane);
    if (plane->crtc_id != crtc_id || plane->fb_id == 0) {
      ++unbound_planes;
      continue;
    }

    drmModeFB2Ptr fb_raw = drmModeGetFB2(fd, plane->fb_id);
    if (fb_raw == nullptr) {
      drm::log_warn("capture: drmModeGetFB2 failed for plane {} fb {}: {}", plane_id, plane->fb_id,
                    std::strerror(errno));
      continue;
    }
    std::unique_ptr<drmModeFB2, decltype(&drmModeFreeFB2)> fb(fb_raw, drmModeFreeFB2);

    const std::uint64_t mod = fb->modifier;
    const bool linear = (mod == DRM_FORMAT_MOD_LINEAR) || (mod == DRM_FORMAT_MOD_INVALID);
    if (!linear) {
      drm::log_warn("capture: skipping plane {} — non-linear modifier 0x{:016x}", plane_id, mod);
      continue;
    }
    if (!is_supported_format(fb->pixel_format)) {
      drm::log_warn("capture: skipping plane {} — unsupported fourcc 0x{:08x}", plane_id,
                    fb->pixel_format);
      continue;
    }

    PlaneFrame frame;
    frame.plane_id = plane_id;
    frame.width = fb->width;
    frame.height = fb->height;
    frame.pitch = fb->pitches[0];
    frame.format = fb->pixel_format;

    // cache_properties() mutates props; the misc-const-correctness
    // check doesn't always see through the .{} ; form used below and
    // spuriously suggests `const`.
    drm::PropertyStore props;  // NOLINT(misc-const-correctness)
    if (auto cached = props.cache_properties(fd, plane_id, DRM_MODE_OBJECT_PLANE); !cached) {
      drm::log_warn("capture: skipping plane {} — property cache failed: {}", plane_id,
                    cached.error().message());
      continue;
    }
    if (!fill_plane_geometry(props, plane_id, frame)) {
      drm::log_warn("capture: skipping plane {} — CRTC_X/Y/W/H property values unavailable",
                    plane_id);
      continue;
    }

    const std::size_t map_size = static_cast<std::size_t>(frame.pitch) * frame.height;
    frame.mapping = map_plane_fb(fd, fb->handles[0], map_size);
    if (frame.mapping.data == nullptr) {
      drm::log_warn("capture: skipping plane {} — unable to mmap fb via DMA-BUF", plane_id);
      continue;
    }

    frames.push_back(std::move(frame));
  }

  if (frames.empty()) {
    drm::log_warn(
        "capture: crtc {} has {} enumerated planes, {} unbound to this crtc, "
        "none readable — snapshot aborted",
        crtc_id, pres->count_planes, unbound_planes);
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_device_or_address));
  }

  std::sort(frames.begin(), frames.end(),
            [](const PlaneFrame& a, const PlaneFrame& b) { return a.zpos < b.zpos; });

  // ─── Compose ────────────────────────────────────────────
  BLImage out_image;
  if (out_image.create(static_cast<int>(out_w), static_cast<int>(out_h), BL_FORMAT_PRGB32) !=
      BL_SUCCESS) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
  }

  BLContext ctx(out_image);
  ctx.clear_all();
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

  for (const PlaneFrame& frame : frames) {
    BLImage src;
    const BLResult wr = src.create_from_data(
        static_cast<int>(frame.width), static_cast<int>(frame.height), bl_format_for(frame.format),
        frame.mapping.data, static_cast<intptr_t>(frame.pitch), BL_DATA_ACCESS_READ, nullptr,
        nullptr);
    if (wr != BL_SUCCESS) {
      drm::log_warn("capture: BLImage::create_from_data failed for plane {}", frame.plane_id);
      continue;
    }

    const double dst_w =
        frame.crtc_w != 0 ? static_cast<double>(frame.crtc_w) : static_cast<double>(frame.width);
    const double dst_h =
        frame.crtc_h != 0 ? static_cast<double>(frame.crtc_h) : static_cast<double>(frame.height);
    const BLRect dst{static_cast<double>(frame.crtc_x), static_cast<double>(frame.crtc_y), dst_w,
                     dst_h};
    const BLRectI src_area{0, 0, static_cast<int>(frame.width), static_cast<int>(frame.height)};
    // The BLRect overload of blit_image dispatches to the scaled
    // variant internally — a BLPoint destination would go through the
    // unscaled path.
    ctx.blit_image(dst, src, src_area);
  }
  ctx.end();

  // ─── Copy out → our Image ───────────────────────────────
  BLImageData data{};
  if (out_image.get_data(&data) != BL_SUCCESS) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::io_error));
  }
  // BLImageData::stride is intptr_t. Blend2D guarantees non-negative
  // for images created via BLImage::create(); we rely on that when
  // casting to size_t below. Top-down images with negative stride
  // exist in other APIs but not for our output here.
  assert(data.stride >= 0);

  Image result(out_w, out_h);
  const std::size_t row_bytes = static_cast<std::size_t>(out_w) * 4;
  for (std::uint32_t row = 0; row < out_h; ++row) {
    const auto* row_src = static_cast<const std::byte*>(data.pixel_data) +
                          (static_cast<std::size_t>(row) * static_cast<std::size_t>(data.stride));
    std::memcpy(result.pixels().data() + (static_cast<std::size_t>(row) * out_w), row_src,
                row_bytes);
  }

  return result;
}

// NOLINTEND(misc-include-cleaner)

#endif  // DRM_CXX_CAPTURE_HAS_BL2D

}  // namespace drm::capture