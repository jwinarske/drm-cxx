// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// camera — zero-copy libcamera -> KMS scanout viewfinder.
//
// Plan: The end-state is a single-binary CLI tool that
// negotiates a format both libcamera and a chosen DRM plane accept,
// imports libcamera's DMA-BUFs as a per-frame LayerScene buffer
// source, and drives a steady-state commit loop. `--probe` is the
// diagnostic mode: it walks both sides of the pipeline and prints the
// (camera, plane, fourcc, size, modifier) tuple a streaming run would
// pick, without acquiring streaming resources.
//
// libcamera is a hard build dependency of this example only — a
// missing libcamera fails the example target rather than the whole
// drm-cxx build.
//
// Usage:
//   camera --probe [/dev/dri/cardN]
//
// Output:
//   1. connector / CRTC / mode summary
//   2. per-plane table of the IN_FORMATS contents (or the bare format
//      list when the driver doesn't expose IN_FORMATS)
//   3. libcamera-enumerated cameras with id, model, location, and the
//      validated Viewfinder StreamConfiguration plus full StreamFormats
//      matrix
//   4. the negotiated target — the format / size / plane / modifier
//      tuple a streaming run would commit to.

#include "../common/format_probe.hpp"
#include "../common/open_output.hpp"
#include "convert.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/planes/plane_registry.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/display_params.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>
#include <drm-cxx/scene/layer.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_handle.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/session/seat.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <libcamera/base/span.h>
#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/controls.h>
#include <libcamera/framebuffer.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/geometry.h>
#include <libcamera/logging.h>
#include <libcamera/pixel_format.h>
#include <libcamera/property_ids.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <system_error>
#include <utility>
#include <vector>

namespace {

// Probe data threaded through the plane/camera walks and consumed by
// negotiate() at the end of the probe. Both halves are flat: one
// DisplayFmt per (plane, fourcc) and one CaptureFmt per
// (camera, fourcc), with their respective modifier / size lists
// attached. The negotiator iterates fourccs by preference, finds the
// first pair that intersects, and picks a size + modifier from the
// intersection.
struct DisplayFmt {
  std::uint32_t plane_id;
  bool is_overlay;
  std::uint32_t fourcc;
  std::vector<std::uint64_t> modifiers;  // empty when IN_FORMATS unsupported
};

struct CaptureSize {
  std::uint32_t width;
  std::uint32_t height;
};

struct CaptureFmt {
  std::size_t camera_index;
  std::string camera_id;
  std::uint32_t fourcc;
  std::vector<CaptureSize> sizes;
};

// libcamera and V4L2 expose MJPEG via the fourcc 'MJPG' (0x47504a4d).
// drm_fourcc.h doesn't define a constant for it because MJPEG isn't a
// scanout format — when the example picks the MJPEG path the camera
// fourcc and the plane fourcc differ.
constexpr std::uint32_t k_fourcc_mjpeg = 0x47504a4d;

// amdgpu DC requires LINEAR scanout pitches to be 256-byte aligned.
// Other GPUs differ; this is what we predict for in negotiate(), and
// the kernel-side CREATE_DUMB ioctl rounds dumb pitches up to whatever
// the driver actually demands so AlignedNV12Surface works regardless.
constexpr std::uint32_t k_predicted_pitch_align = 256;

enum class ConversionMode : std::uint8_t {
  ZeroCopy,     // NV12 PRIME-import; camera fourcc == plane fourcc, stride satisfies alignment
  Yuy2ToXrgb,   // libyuv YUY2->ARGB into a DumbBufferSource (XRGB8888)
  Nv12ToXrgb,   // libyuv NV12->ARGB; runtime fallback when ZeroCopy AddFB2 is rejected
  MjpegToXrgb,  // libyuv MJPGToARGB (via libjpeg-turbo) into a DumbBufferSource (XRGB8888)
};

struct NegotiatedTarget {
  std::size_t camera_index{};
  std::string camera_id;
  std::uint32_t plane_id{};
  std::uint32_t camera_fourcc{};
  CaptureSize size{};
  std::uint64_t modifier{};  // applies to the plane FB; LINEAR for non-zero-copy
  ConversionMode mode{ConversionMode::ZeroCopy};
};

[[nodiscard]] constexpr std::uint32_t plane_fourcc_for_mode(std::uint32_t camera_fourcc,
                                                            ConversionMode mode) noexcept {
  return (mode == ConversionMode::ZeroCopy) ? camera_fourcc : DRM_FORMAT_XRGB8888;
}

// Render a fourcc as the four ASCII bytes drivers expose in IN_FORMATS,
// falling back to the hex value when any byte is non-printable. Avoids
// the C-style `%c%c%c%c` pun and keeps every output line the same width.
std::array<char, 5> fourcc_to_chars(std::uint32_t f) noexcept {
  std::array<char, 5> out{};
  for (int i = 0; i < 4; ++i) {
    const auto c = static_cast<unsigned char>((f >> (i * 8)) & 0xffU);
    out.at(static_cast<std::size_t>(i)) = (c >= 0x20 && c <= 0x7e) ? static_cast<char>(c) : '?';
  }
  return out;
}

const char* plane_type_label(drm::planes::DRMPlaneType t) noexcept {
  switch (t) {
    case drm::planes::DRMPlaneType::PRIMARY:
      return "PRIMARY";
    case drm::planes::DRMPlaneType::OVERLAY:
      return "OVERLAY";
    case drm::planes::DRMPlaneType::CURSOR:
      return "CURSOR ";
  }
  return "UNKNOWN";
}

// Locate the CRTC's 0-based index in the device's resource list.
// PlaneRegistry::for_crtc() and PlaneCapabilities::possible_crtcs both
// key on this position rather than the CRTC object id.
std::optional<std::uint32_t> crtc_index_of(const drm::Device& dev, std::uint32_t crtc_id) noexcept {
  const auto res = drm::get_resources(dev.fd());
  if (!res) {
    return std::nullopt;
  }
  for (int i = 0; i < res->count_crtcs; ++i) {
    if (res->crtcs[i] == crtc_id) {
      return static_cast<std::uint32_t>(i);
    }
  }
  return std::nullopt;
}

void print_plane(const drm::planes::PlaneCapabilities& p, std::vector<DisplayFmt>& out) {
  drm::println("  plane id={} type={} zpos=[{}..{}] scaling={} rotation={} blend={} alpha_prop={}",
               p.id, plane_type_label(p.type),
               p.zpos_min ? drm::format("{}", *p.zpos_min) : std::string{"-"},
               p.zpos_max ? drm::format("{}", *p.zpos_max) : std::string{"-"},
               p.supports_scaling ? "yes" : "no", p.supports_rotation ? "yes" : "no",
               p.has_pixel_blend_mode ? "yes" : "no", p.has_per_plane_alpha ? "yes" : "no");

  // Cursor planes accept only ARGB8888 at fixed small sizes — they are
  // never candidates for camera scanout, so don't emit DisplayFmt
  // entries for them.
  const bool collect = p.type != drm::planes::DRMPlaneType::CURSOR;
  const bool is_overlay = p.type == drm::planes::DRMPlaneType::OVERLAY;

  if (p.has_format_modifiers) {
    // IN_FORMATS path: group (format, modifier) pairs by format so each
    // line shows one fourcc and its full modifier list. Pairs arrive
    // sorted by format ascending, so a single linear pass groups them.
    std::uint32_t cur = 0;
    bool have_cur = false;
    std::vector<std::uint64_t> mods;
    auto emit = [&]() {
      if (!have_cur) {
        return;
      }
      const auto chars = fourcc_to_chars(cur);
      drm::print("    {} ({:#x})  modifiers:", std::string_view(chars.data(), 4), cur);
      for (const auto m : mods) {
        if (m == DRM_FORMAT_MOD_LINEAR) {
          drm::print(" LINEAR");
        } else if (m == DRM_FORMAT_MOD_INVALID) {
          drm::print(" INVALID");
        } else {
          drm::print(" {:#x}", m);
        }
      }
      drm::println("");
      if (collect) {
        out.push_back(DisplayFmt{p.id, is_overlay, cur, mods});
      }
    };
    for (const auto& [fmt, mod] : p.format_modifiers) {
      if (!have_cur || fmt != cur) {
        emit();
        cur = fmt;
        have_cur = true;
        mods.clear();
      }
      mods.push_back(mod);
    }
    emit();
  } else {
    // Pre-IN_FORMATS legacy path: just the bare format list, no
    // modifier metadata. LINEAR / INVALID are the only viable
    // modifiers in this case.
    for (const auto fmt : p.formats) {
      const auto chars = fourcc_to_chars(fmt);
      drm::println("    {} ({:#x})  modifiers: <IN_FORMATS not exposed>",
                   std::string_view(chars.data(), 4), fmt);
      if (collect) {
        // Empty modifier list signals "implicit LINEAR" to the
        // negotiator.
        out.push_back(DisplayFmt{p.id, is_overlay, fmt, {}});
      }
    }
  }
}

const char* camera_location_label(std::int32_t loc) noexcept {
  switch (loc) {
    case libcamera::properties::CameraLocationFront:
      return "Front";
    case libcamera::properties::CameraLocationBack:
      return "Back";
    case libcamera::properties::CameraLocationExternal:
      return "External";
    default:
      return "Unknown";
  }
}

const char* config_status_label(libcamera::CameraConfiguration::Status s) noexcept {
  switch (s) {
    case libcamera::CameraConfiguration::Valid:
      return "Valid";
    case libcamera::CameraConfiguration::Adjusted:
      return "Adjusted";
    case libcamera::CameraConfiguration::Invalid:
      return "Invalid";
  }
  return "Unknown";
}

// Acquire the camera, generate a Viewfinder-role configuration, dump
// the validated default plus the full StreamFormats matrix
// (pixelformat × sizes) the pipeline can produce, append every
// (camera_index, fourcc, sizes) tuple to `out`, then release.
//
// acquire() may fail with -EBUSY when another process owns the
// camera; that's expected on systems with a running viewer/portal,
// not fatal to the probe — we log and move on.
void print_camera_streams(libcamera::Camera& cam, std::size_t cam_index,
                          std::vector<CaptureFmt>& out) {
  if (const int rc = cam.acquire(); rc < 0) {
    drm::println(stderr, "      acquire: {} (skipping streams)", std::strerror(-rc));
    return;
  }

  auto config = cam.generateConfiguration({libcamera::StreamRole::Viewfinder});
  if (!config || config->empty()) {
    drm::println(stderr, "      generateConfiguration(Viewfinder): empty");
    cam.release();
    return;
  }

  const auto status = config->validate();
  drm::println("      streams (Viewfinder, validate={}):", config_status_label(status));
  for (unsigned int i = 0; i < config->size(); ++i) {
    const auto& sc = config->at(i);
    drm::println("        [{}] default {} {} ({:#x}) frameSize={} bufferCount={}", i,
                 sc.size.toString(), sc.pixelFormat.toString(), sc.pixelFormat.fourcc(),
                 sc.frameSize, sc.bufferCount);
    const auto& fmts = sc.formats();
    const auto pixfmts = fmts.pixelformats();
    drm::println("            supported: {} pixel format(s)", pixfmts.size());
    for (const auto& pf : pixfmts) {
      const auto sizes = fmts.sizes(pf);
      drm::print("              {} ({:#x}):", pf.toString(), pf.fourcc());
      std::vector<CaptureSize> collected;
      collected.reserve(sizes.size());
      for (const auto& sz : sizes) {
        drm::print(" {}", sz.toString());
        collected.push_back(CaptureSize{sz.width, sz.height});
      }
      drm::println("");
      out.push_back(CaptureFmt{cam_index, cam.id(), pf.fourcc(), std::move(collected)});
    }
  }

  cam.release();
}

// Print each visible camera's metadata + Viewfinder StreamConfigurations,
// appending (camera_index, fourcc, sizes) tuples to `out`. The caller
// owns the CameraManager and the camera vector; this helper does not
// stop the manager, so run_show can keep the chosen camera alive across
// negotiation into the streaming step.
void walk_cameras(const std::vector<std::shared_ptr<libcamera::Camera>>& cameras,
                  std::vector<CaptureFmt>& out) {
  drm::println("Cameras: {} visible to libcamera", cameras.size());
  for (std::size_t i = 0; i < cameras.size(); ++i) {
    const std::shared_ptr<libcamera::Camera>& cam = cameras.at(i);
    const libcamera::ControlList& props = cam->properties();
    const auto model = props.get(libcamera::properties::Model);
    const auto location = props.get(libcamera::properties::Location);
    drm::println("  [{}] id={}", i, cam->id());
    drm::println("      model={}  location={}", model ? *model : std::string{"<unknown>"},
                 location ? camera_location_label(*location) : "<unknown>");
    print_camera_streams(*cam, i, out);
  }
}

// Pick a (camera, plane, capture-format, size, conversion) tuple from the
// probe data with three priority tiers:
//
//   1. Zero-copy NV12 LINEAR — the camera offers NV12 at a width whose
//      Y-plane pitch (== width) satisfies the GPU's predicted alignment.
//      This is the "best" path: PRIME-import the libcamera buffer,
//      AddFB2WithModifiers, scan out without touching pixels.
//      Multi-camera --show currently disables this tier (per-frame
//      ExternalDmaBufSource churn would re-allocate plane state every
//      frame). Single-camera flows can re-enable it via the
//      `allow_zero_copy` parameter.
//   2. YUY2 -> XRGB8888 — libyuv applies BT.601 limited YCbCr -> sRGB
//      while writing a CPU-mapped dumb buffer. RGB scanout sidesteps
//      the YCbCr-matrix-on-the-plane question entirely (most amdgpu
//      OVERLAY planes don't expose COLOR_ENCODING / COLOR_RANGE).
//   3. MJPEG -> XRGB8888 — libyuv MJPGToARGB chains libjpeg-turbo's
//      SIMD entropy decode with the same color conversion.
//
// The destination plane must accept NV12 LINEAR (tier 1) or
// XRGB8888 LINEAR (tiers 2-3). Within a tier:
//   - prefer OVERLAY over PRIMARY (overlays don't fight the desktop
//     background);
//   - pick the largest capture size that fits within the display mode,
//     falling back to the smallest if everything exceeds it.
std::optional<NegotiatedTarget> negotiate(const std::vector<DisplayFmt>& display,
                                          const std::vector<CaptureFmt>& capture,
                                          std::uint32_t mode_w, std::uint32_t mode_h,
                                          bool allow_zero_copy = true) {
  auto pick_size_pred = [&](const std::vector<CaptureSize>& sizes,
                            auto&& predicate) -> std::optional<CaptureSize> {
    const CaptureSize* best = nullptr;
    for (const auto& sz : sizes) {
      if (!predicate(sz)) {
        continue;
      }
      if (sz.width <= mode_w && sz.height <= mode_h) {
        if (best == nullptr || (static_cast<std::uint64_t>(sz.width) * sz.height) >
                                   (static_cast<std::uint64_t>(best->width) * best->height)) {
          best = &sz;
        }
      }
    }
    if (best != nullptr) {
      return *best;
    }
    for (const auto& sz : sizes) {
      if (!predicate(sz)) {
        continue;
      }
      if (best == nullptr || (static_cast<std::uint64_t>(sz.width) * sz.height) <
                                 (static_cast<std::uint64_t>(best->width) * best->height)) {
        best = &sz;
      }
    }
    if (best == nullptr) {
      return std::nullopt;
    }
    return *best;
  };

  auto pick_size = [&](const std::vector<CaptureSize>& sizes) -> std::optional<CaptureSize> {
    return pick_size_pred(sizes, [](const CaptureSize&) { return true; });
  };

  auto find_capture = [&](std::uint32_t fourcc) -> const CaptureFmt* {
    for (const auto& cf : capture) {
      if (cf.fourcc == fourcc && !cf.sizes.empty()) {
        return &cf;
      }
    }
    return nullptr;
  };

  auto find_plane = [&](std::uint32_t fourcc) -> const DisplayFmt* {
    for (const auto& df : display) {
      if (df.fourcc == fourcc && df.is_overlay) {
        return &df;
      }
    }
    for (const auto& df : display) {
      if (df.fourcc == fourcc) {
        return &df;
      }
    }
    return nullptr;
  };

  auto plane_supports_linear = [](const DisplayFmt& d) {
    if (d.modifiers.empty()) {
      return true;  // pre-IN_FORMATS drivers — implicit LINEAR
    }
    return std::any_of(d.modifiers.begin(), d.modifiers.end(),
                       [](std::uint64_t m) { return m == DRM_FORMAT_MOD_LINEAR; });
  };

  // Tier 1: zero-copy NV12 LINEAR with a predicted-aligned width.
  if (allow_zero_copy) {
    if (const auto* nv12_plane = find_plane(DRM_FORMAT_NV12);
        nv12_plane != nullptr && plane_supports_linear(*nv12_plane)) {
      if (const auto* nv12_cam = find_capture(DRM_FORMAT_NV12); nv12_cam != nullptr) {
        const auto picked = pick_size_pred(nv12_cam->sizes, [](const CaptureSize& sz) {
          return (sz.width % k_predicted_pitch_align) == 0;
        });
        if (picked) {
          return NegotiatedTarget{nv12_cam->camera_index,
                                  nv12_cam->camera_id,
                                  nv12_plane->plane_id,
                                  DRM_FORMAT_NV12,
                                  *picked,
                                  DRM_FORMAT_MOD_LINEAR,
                                  ConversionMode::ZeroCopy};
        }
      }
    }
  }

  // Tiers 2-3 target an XRGB8888 plane: libyuv applies BT.601 limited
  // YCbCr -> sRGB so the display engine just blits RGB and we don't
  // need to write COLOR_ENCODING / COLOR_RANGE plane properties (which
  // amdgpu DC doesn't expose on its OVERLAY planes anyway).
  const auto* xrgb_plane = find_plane(DRM_FORMAT_XRGB8888);
  if (xrgb_plane == nullptr || !plane_supports_linear(*xrgb_plane)) {
    return std::nullopt;
  }

  // Tier 2: YUY2 -> XRGB.
  if (const auto* yuyv_cam = find_capture(DRM_FORMAT_YUYV); yuyv_cam != nullptr) {
    if (const auto picked = pick_size(yuyv_cam->sizes)) {
      return NegotiatedTarget{yuyv_cam->camera_index,
                              yuyv_cam->camera_id,
                              xrgb_plane->plane_id,
                              DRM_FORMAT_YUYV,
                              *picked,
                              DRM_FORMAT_MOD_LINEAR,
                              ConversionMode::Yuy2ToXrgb};
    }
  }

  // Tier 3: MJPEG -> XRGB.
  if (const auto* mjpeg_cam = find_capture(k_fourcc_mjpeg); mjpeg_cam != nullptr) {
    if (const auto picked = pick_size(mjpeg_cam->sizes)) {
      return NegotiatedTarget{mjpeg_cam->camera_index,
                              mjpeg_cam->camera_id,
                              xrgb_plane->plane_id,
                              k_fourcc_mjpeg,
                              *picked,
                              DRM_FORMAT_MOD_LINEAR,
                              ConversionMode::MjpegToXrgb};
    }
  }

  // Lastly: camera-only NV12 (no zero-copy candidate, no YUYV, no
  // MJPEG). We'll convert NV12 -> XRGB at runtime.
  if (const auto* nv12_cam = find_capture(DRM_FORMAT_NV12); nv12_cam != nullptr) {
    if (const auto picked = pick_size(nv12_cam->sizes)) {
      return NegotiatedTarget{nv12_cam->camera_index,
                              nv12_cam->camera_id,
                              xrgb_plane->plane_id,
                              DRM_FORMAT_NV12,
                              *picked,
                              DRM_FORMAT_MOD_LINEAR,
                              ConversionMode::Nv12ToXrgb};
    }
  }

  return std::nullopt;
}

std::string modifier_label(std::uint64_t modifier) {
  if (modifier == DRM_FORMAT_MOD_LINEAR) {
    return "LINEAR";
  }
  if (modifier == DRM_FORMAT_MOD_INVALID) {
    return "INVALID";
  }
  return drm::format("{:#x}", modifier);
}

[[nodiscard]] const char* mode_label(ConversionMode m) noexcept {
  switch (m) {
    case ConversionMode::ZeroCopy:
      return "zero-copy (PRIME import, NV12)";
    case ConversionMode::Yuy2ToXrgb:
      return "YUY2 -> XRGB (libyuv)";
    case ConversionMode::Nv12ToXrgb:
      return "NV12 -> XRGB (libyuv)";
    case ConversionMode::MjpegToXrgb:
      return "MJPEG -> XRGB (libyuv + libjpeg-turbo)";
  }
  return "unknown";
}

void print_negotiation(const std::optional<NegotiatedTarget>& target) {
  drm::println("");
  if (!target) {
    drm::println("No common format between any visible camera and any scanout plane.");
    return;
  }
  const auto cam_chars = fourcc_to_chars(target->camera_fourcc);
  const std::uint32_t plane_fourcc = plane_fourcc_for_mode(target->camera_fourcc, target->mode);
  const auto plane_chars = fourcc_to_chars(plane_fourcc);
  drm::println("Negotiated target:");
  drm::println("  camera [{}] id={}", target->camera_index, target->camera_id);
  drm::println("  capture {} ({:#x}) {}x{}", std::string_view(cam_chars.data(), 4),
               target->camera_fourcc, target->size.width, target->size.height);
  drm::println("  plane   id={} format={} ({:#x}) modifier={}", target->plane_id,
               std::string_view(plane_chars.data(), 4), plane_fourcc,
               modifier_label(target->modifier));
  drm::println("  path    {}", mode_label(target->mode));
}

// Place `cam_w x cam_h` inside a centered `target_w x target_h`
// rectangle, preserving aspect — fill whichever axis is the binding
// constraint. The returned rect's x/y are absolute display
// coordinates, so the caller can plug it straight into a `LayerDesc`.
drm::scene::Rect fit_within(std::uint32_t cam_w, std::uint32_t cam_h, std::uint32_t target_w,
                            std::uint32_t target_h, std::uint32_t mode_w,
                            std::uint32_t mode_h) noexcept {
  if (cam_w == 0 || cam_h == 0 || target_w == 0 || target_h == 0) {
    return {0, 0, target_w, target_h};
  }
  const std::uint64_t scaled_w_by_h = static_cast<std::uint64_t>(cam_w) * target_h;
  const std::uint64_t scaled_h_by_w = static_cast<std::uint64_t>(cam_h) * target_w;
  std::uint32_t dst_w = target_w;
  std::uint32_t dst_h = target_h;
  if (scaled_w_by_h > scaled_h_by_w) {
    dst_h = static_cast<std::uint32_t>(scaled_h_by_w / cam_w);
  } else {
    dst_w = static_cast<std::uint32_t>(scaled_w_by_h / cam_h);
  }
  const auto x = static_cast<std::int32_t>((mode_w - dst_w) / 2);
  const auto y = static_cast<std::int32_t>((mode_h - dst_h) / 2);
  return {x, y, dst_w, dst_h};
}

// Soft horizontal+vertical gradient for the scene background. Pure
// XRGB byte order so a wrong-channel write tints the result obviously.
// Mirrors `examples/layered_demo/main.cpp::paint_bg_gradient`.
void paint_bg_gradient(drm::BufferMapping& map) noexcept {
  const auto width = map.width();
  const auto height = map.height();
  const auto stride_bytes = map.stride();
  const auto pixels = map.pixels();
  if (width == 0U || height == 0U || stride_bytes < width * 4U) {
    return;
  }
  if (pixels.size() < static_cast<std::size_t>(height) * stride_bytes) {
    return;
  }
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto v = static_cast<std::uint8_t>((y * 96U) / std::max<std::uint32_t>(1U, height - 1U));
    auto* row = reinterpret_cast<std::uint32_t*>(pixels.data() +
                                                 (static_cast<std::size_t>(y) * stride_bytes));
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto h = static_cast<std::uint8_t>((x * 64U) / std::max<std::uint32_t>(1U, width - 1U));
      const auto r = static_cast<std::uint8_t>(20U + h);
      const auto g = static_cast<std::uint8_t>(28U + v);
      const auto b = static_cast<std::uint8_t>(40U + h + v);
      row[x] = (static_cast<std::uint32_t>(r) << 16U) | (static_cast<std::uint32_t>(g) << 8U) | b;
    }
  }
}

// Silent variant of print_camera_streams used by the streaming
// pipeline. Returns the camera's CaptureFmt list so the negotiator can
// pick a target without printing the same matrix twice.
std::vector<CaptureFmt> enumerate_camera_streams(libcamera::Camera& cam, std::size_t cam_index) {
  std::vector<CaptureFmt> out;
  if (cam.acquire() < 0) {
    return out;
  }
  auto config = cam.generateConfiguration({libcamera::StreamRole::Viewfinder});
  if (!config || config->empty()) {
    cam.release();
    return out;
  }
  config->validate();
  for (const auto& sc : *config) {
    const auto& fmts = sc.formats();
    for (const auto& pf : fmts.pixelformats()) {
      const auto sizes = fmts.sizes(pf);
      std::vector<CaptureSize> collected;
      collected.reserve(sizes.size());
      for (const auto& sz : sizes) {
        collected.push_back(CaptureSize{sz.width, sz.height});
      }
      out.push_back(CaptureFmt{cam_index, cam.id(), pf.fourcc(), std::move(collected)});
    }
  }
  cam.release();
  return out;
}

// Inbox for libcamera's `requestCompleted` signal. Held by shared_ptr
// from CameraSlot AND captured as weak_ptr by the signal slot lambda,
// so a callback in flight on libcamera's worker thread when the slot
// is torn down doesn't dereference freed memory — the worker locks
// the weak_ptr and no-ops if the slot has gone. Belt-and-braces: in
// the graceful-shutdown path we already call `cam->stop()` (which
// blocks for in-flight callbacks) before disconnect; in the
// hot-unplug path we skip stop() to avoid V4L2-on-dead-device errors,
// and the weak_ptr is what keeps that race safe.
struct SlotMailbox {
  std::mutex mu;
  std::queue<libcamera::Request*> ready;
};

// Per-camera streaming state. Built by configure_slot, drained per
// iteration by drain_slot. Lifetime is tied to the camera's presence
// in `libcamera::CameraManager::cameras()` — when the camera goes
// away (USB unplug) the slot is removed and torn down.
struct CameraSlot {
  std::shared_ptr<libcamera::Camera> camera;
  std::unique_ptr<libcamera::CameraConfiguration> config;
  std::unique_ptr<libcamera::FrameBufferAllocator> allocator;
  libcamera::Stream* stream{};
  NegotiatedTarget target;
  std::uint32_t src_stride{};
  drm::scene::LayerHandle layer_handle{};
  std::vector<std::unique_ptr<libcamera::Request>> requests;
  std::shared_ptr<SlotMailbox> mailbox{std::make_shared<SlotMailbox>()};
  bool started{false};
  // Sticky once we hit at least one successful conversion+commit, so
  // hotplug churn doesn't keep "first frame" diagnostics bouncing.
  bool first_frame_seen{false};
};

// Build a CameraSlot for `camera` and add its layer to the scene.
// The returned slot has the camera started and its requests queued —
// the run loop just drains and re-queues. Returns nullptr (logging to
// stderr) on any failure; the caller drops the camera from the active
// set in that case.
std::unique_ptr<CameraSlot> configure_slot(drm::Device& dev, drm::scene::LayerScene& scene,
                                           const std::shared_ptr<libcamera::Camera>& camera,
                                           const std::vector<DisplayFmt>& display_formats,
                                           std::uint32_t mode_w, std::uint32_t mode_h,
                                           std::size_t cam_index) {
  auto slot = std::make_unique<CameraSlot>();
  slot->camera = camera;

  const auto formats = enumerate_camera_streams(*camera, cam_index);
  if (formats.empty()) {
    drm::println(stderr, "configure_slot[{}]: no formats enumerated", cam_index);
    return nullptr;
  }
  // Multi-camera doesn't currently support ZeroCopy (would require
  // per-frame ExternalDmaBufSource churn — re-allocates plane state
  // every frame). Conversion paths use a steady DumbBufferSource per
  // slot and just refresh its pixels each frame.
  auto target = negotiate(display_formats, formats, mode_w, mode_h, /*allow_zero_copy=*/false);
  if (!target) {
    drm::println(stderr, "configure_slot[{}]: no common format / size", cam_index);
    return nullptr;
  }
  slot->target = *target;

  if (camera->acquire() < 0) {
    drm::println(stderr, "configure_slot[{}]: acquire failed", cam_index);
    return nullptr;
  }
  slot->config = camera->generateConfiguration({libcamera::StreamRole::Viewfinder});
  if (!slot->config || slot->config->empty()) {
    drm::println(stderr, "configure_slot[{}]: generateConfiguration empty", cam_index);
    camera->release();
    return nullptr;
  }
  slot->config->at(0).pixelFormat = libcamera::PixelFormat(target->camera_fourcc);
  slot->config->at(0).size = libcamera::Size(target->size.width, target->size.height);
  if (slot->config->validate() == libcamera::CameraConfiguration::Invalid) {
    drm::println(stderr, "configure_slot[{}]: validate Invalid", cam_index);
    camera->release();
    return nullptr;
  }
  // validate() is allowed to adjust pixelFormat / size; sync the
  // negotiated target back to whatever the pipeline actually settled
  // on, so the destination buffer we allocate downstream matches the
  // frames libcamera will deliver.
  {
    const auto& post = slot->config->at(0);
    target->size.width = post.size.width;
    target->size.height = post.size.height;
    target->camera_fourcc = post.pixelFormat.fourcc();
    slot->target = *target;
  }
  if (camera->configure(slot->config.get()) < 0) {
    drm::println(stderr, "configure_slot[{}]: configure failed", cam_index);
    camera->release();
    return nullptr;
  }
  const auto& sc = slot->config->at(0);
  slot->stream = sc.stream();
  slot->src_stride = sc.stride;

  slot->allocator = std::make_unique<libcamera::FrameBufferAllocator>(camera);
  if (slot->allocator->allocate(slot->stream) < 0) {
    drm::println(stderr, "configure_slot[{}]: allocator failed", cam_index);
    camera->release();
    return nullptr;
  }
  const auto& buffers = slot->allocator->buffers(slot->stream);
  if (buffers.empty()) {
    drm::println(stderr, "configure_slot[{}]: no buffers", cam_index);
    camera->release();
    return nullptr;
  }

  // XRGB destination layer for libyuv conversion output.
  auto dest = drm::scene::DumbBufferSource::create(dev, target->size.width, target->size.height,
                                                   DRM_FORMAT_XRGB8888);
  if (!dest) {
    drm::println(stderr, "configure_slot[{}]: DumbBufferSource: {}", cam_index,
                 dest.error().message());
    camera->release();
    return nullptr;
  }
  drm::scene::LayerDesc desc;
  desc.source = std::move(*dest);
  desc.display.src_rect = {0, 0, target->size.width, target->size.height};
  // Placeholder dst_rect — apply_layout fills it on the next iteration.
  desc.display.dst_rect = {0, 0, target->size.width, target->size.height};
  desc.display.zpos = static_cast<int>(3 + cam_index);  // above bg (zpos=2), staggered per cam
  desc.content_type = drm::planes::ContentType::Generic;
  desc.update_hint_hz = 30;
  auto handle_r = scene.add_layer(std::move(desc));
  if (!handle_r) {
    drm::println(stderr, "configure_slot[{}]: add_layer: {}", cam_index,
                 handle_r.error().message());
    camera->release();
    return nullptr;
  }
  slot->layer_handle = *handle_r;

  // requestCompleted fires from libcamera's internal thread; push the
  // request pointer onto the mailbox and let the main loop drain. The
  // weak_ptr capture means a callback racing the slot's destruction
  // (e.g. hot-unplug, where we skip cam->stop) safely no-ops.
  const std::weak_ptr<SlotMailbox> mailbox_weak = slot->mailbox;
  camera->requestCompleted.connect(camera.get(), [mailbox_weak](libcamera::Request* req) {
    if (auto mb = mailbox_weak.lock()) {
      const std::scoped_lock lock(mb->mu);
      mb->ready.push(req);
    }
  });

  if (camera->start() < 0) {
    drm::println(stderr, "configure_slot[{}]: start failed", cam_index);
    scene.remove_layer(slot->layer_handle);
    camera->release();
    return nullptr;
  }
  slot->started = true;

  for (const auto& fb : buffers) {
    auto req = camera->createRequest();
    if (!req) {
      drm::println(stderr, "configure_slot[{}]: createRequest failed", cam_index);
      continue;
    }
    if (req->addBuffer(slot->stream, fb.get()) < 0) {
      drm::println(stderr, "configure_slot[{}]: addBuffer failed", cam_index);
      continue;
    }
    if (camera->queueRequest(req.get()) < 0) {
      drm::println(stderr, "configure_slot[{}]: queueRequest failed", cam_index);
      continue;
    }
    slot->requests.push_back(std::move(req));
  }

  drm::println("camera[{}] {} {}x{} stride={} -> {}", cam_index, camera->id(), target->size.width,
               target->size.height, slot->src_stride, mode_label(target->mode));
  return slot;
}

// Tear down a slot. Must run before the LayerScene that owns the slot's
// layer is destroyed (so the FB ID is RmFB'd against the still-live fd)
// and before the camera shared_ptr drops.
//
// `still_present`: true when the camera is still in cm.cameras() (graceful
// shutdown), false when the camera has been hot-unplugged. In the latter
// case we skip the camera-side ioctls (stop / allocator->free /
// release) — they would fail with "No such device" and libcamera prints
// V4L2 errors via its logger. The kernel reclaims the dead fd's
// resources on Camera destruction.
void teardown_slot(CameraSlot& slot, drm::scene::LayerScene& scene, bool still_present) {
  if (still_present && slot.started) {
    slot.camera->stop();
  }
  {
    const std::scoped_lock lock(slot.mailbox->mu);
    while (!slot.mailbox->ready.empty()) {
      slot.mailbox->ready.pop();
    }
  }
  slot.camera->requestCompleted.disconnect(slot.camera.get());
  scene.remove_layer(slot.layer_handle);
  slot.requests.clear();
  if (still_present && slot.allocator) {
    slot.allocator->free(slot.stream);
  }
  slot.allocator.reset();
  slot.config.reset();
  if (still_present) {
    slot.camera->release();
  }
}

// Run one drain iteration: take the most recent completed Request,
// mmap its FrameBuffer, libyuv-convert into the layer's source, then
// re-queue. Older completed requests in the queue are discarded
// (re-queued without conversion) so we don't accumulate back-pressure
// when the display loop runs slower than capture.
void drain_slot(CameraSlot& slot, drm::scene::LayerScene& scene) {
  std::queue<libcamera::Request*> local;
  {
    const std::scoped_lock lock(slot.mailbox->mu);
    std::swap(local, slot.mailbox->ready);
  }
  if (local.empty()) {
    return;
  }
  // Re-queue all but the latest without converting.
  while (local.size() > 1) {
    auto* old = local.front();
    local.pop();
    old->reuse(libcamera::Request::ReuseBuffers);
    slot.camera->queueRequest(old);
  }
  auto* req = local.front();
  local.pop();
  if (req->status() != libcamera::Request::RequestComplete) {
    req->reuse(libcamera::Request::ReuseBuffers);
    slot.camera->queueRequest(req);
    return;
  }
  auto* fb = req->buffers().begin()->second;
  auto* layer = scene.get_layer(slot.layer_handle);
  if (layer != nullptr) {
    auto map_r = layer->source().map(drm::MapAccess::Write);
    if (map_r) {
      auto& dst_map = *map_r;
      auto* dst = dst_map.pixels().data();
      const auto dst_pitch = dst_map.stride();
      const auto lc_planes = fb->planes();
      if (!lc_planes.empty() && dst != nullptr) {
        const auto& src_plane = *lc_planes.begin();
        const int fd = src_plane.fd.get();
        std::size_t map_len = 0;
        for (const auto& p : lc_planes) {
          const std::size_t end = static_cast<std::size_t>(p.offset) + p.length;
          map_len = std::max(end, map_len);
        }
        void* map = ::mmap(nullptr, map_len, PROT_READ, MAP_SHARED, fd, 0);
        if (map != MAP_FAILED) {
          const auto* base = static_cast<const std::uint8_t*>(map);
          const auto* src = base + src_plane.offset;
          switch (slot.target.mode) {
            case ConversionMode::Yuy2ToXrgb:
              (void)drm::examples::camera::yuy2_to_xrgb(src, slot.src_stride, dst, dst_pitch,
                                                        slot.target.size.width,
                                                        slot.target.size.height);
              break;
            case ConversionMode::Nv12ToXrgb: {
              const auto* src_y = src;
              const auto* src_uv =
                  (lc_planes.size() >= 2)
                      ? base + (lc_planes.begin() + 1)->offset
                      : src + (static_cast<std::size_t>(slot.src_stride) * slot.target.size.height);
              (void)drm::examples::camera::nv12_to_xrgb(src_y, src_uv, slot.src_stride, dst,
                                                        dst_pitch, slot.target.size.width,
                                                        slot.target.size.height);
              break;
            }
            case ConversionMode::MjpegToXrgb: {
              const auto md_planes = fb->metadata().planes();
              const std::size_t jpeg_size =
                  md_planes.empty() ? src_plane.length : md_planes.begin()->bytesused;
              (void)drm::examples::camera::mjpeg_to_xrgb(
                  src, jpeg_size, dst, dst_pitch, slot.target.size.width, slot.target.size.height);
              break;
            }
            case ConversionMode::ZeroCopy:
              break;  // unreachable in multi-cam streaming
          }
          ::munmap(map, map_len);
          if (!slot.first_frame_seen) {
            slot.first_frame_seen = true;
          }
        }
      }
    }
  }
  req->reuse(libcamera::Request::ReuseBuffers);
  slot.camera->queueRequest(req);
}

// Pick a (cols, rows) grid for `n` panes, biased toward roughly
// square cells.
struct GridDims {
  std::uint32_t cols;
  std::uint32_t rows;
};
[[nodiscard]] constexpr GridDims pick_grid(std::uint32_t n) noexcept {
  if (n == 0U) {
    return {0U, 0U};
  }
  if (n <= 2U) {
    return {n, 1U};
  }
  if (n <= 4U) {
    return {2U, 2U};
  }
  if (n <= 6U) {
    return {3U, 2U};
  }
  if (n <= 9U) {
    return {3U, 3U};
  }
  // Fall back to ceil(sqrt(n)) columns; one extra row absorbs the
  // tail when n isn't a perfect square.
  std::uint32_t cols = 1U;
  while (cols * cols < n) {
    ++cols;
  }
  const std::uint32_t rows = (n + cols - 1U) / cols;
  return {cols, rows};
}

// Place each slot into a (cols × rows) grid sized for the current
// camera count. Cells are mode_w/cols × mode_h/rows; the camera image
// is fit-within at 90% of cell width × 90% of cell height, preserving
// aspect, then centered in its cell.
void apply_layout(const std::vector<std::unique_ptr<CameraSlot>>& slots,
                  drm::scene::LayerScene& scene, std::uint32_t mode_w, std::uint32_t mode_h) {
  if (slots.empty()) {
    return;
  }
  const auto n = static_cast<std::uint32_t>(slots.size());
  const auto grid = pick_grid(n);
  const std::uint32_t cell_w = mode_w / grid.cols;
  const std::uint32_t cell_h = mode_h / grid.rows;
  const std::uint32_t pane_w_max = (cell_w * 90U) / 100U;
  const std::uint32_t pane_h_max = (cell_h * 90U) / 100U;
  for (std::uint32_t i = 0; i < n; ++i) {
    const auto& slot = slots[i];
    const std::uint32_t col = i % grid.cols;
    const std::uint32_t row = i / grid.cols;
    auto rect = fit_within(slot->target.size.width, slot->target.size.height, pane_w_max,
                           pane_h_max, cell_w, cell_h);
    rect.x += static_cast<std::int32_t>(col * cell_w);
    rect.y += static_cast<std::int32_t>(row * cell_h);
    if (auto* layer = scene.get_layer(slot->layer_handle); layer != nullptr) {
      layer->set_dst_rect(rect);
    }
  }
}

// Compare libcamera's current camera list against the active slots:
// configure newcomers (add layer + start), tear down departures.
// Returns true when membership changed (caller should re-layout).
bool reconcile_cameras(drm::Device& dev, drm::scene::LayerScene& scene,
                       libcamera::CameraManager& cm,
                       std::vector<std::unique_ptr<CameraSlot>>& slots,
                       const std::vector<DisplayFmt>& display_formats, std::uint32_t mode_w,
                       std::uint32_t mode_h) {
  bool changed = false;
  const auto current = cm.cameras();

  // Drop slots whose camera is no longer in `current`.
  for (auto it = slots.begin(); it != slots.end();) {
    bool still_present = false;
    for (const auto& c : current) {
      if (c.get() == (*it)->camera.get()) {
        still_present = true;
        break;
      }
    }
    if (!still_present) {
      drm::println("camera removed: {}", (*it)->camera->id());
      teardown_slot(**it, scene, /*still_present=*/false);
      it = slots.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }

  // Add slots for cameras that aren't yet active.
  for (const auto& camera : current) {
    bool already_have = false;
    for (const auto& s : slots) {
      if (s->camera.get() == camera.get()) {
        already_have = true;
        break;
      }
    }
    if (already_have) {
      continue;
    }
    drm::println("camera added: {}", camera->id());
    auto slot = configure_slot(dev, scene, camera, display_formats, mode_w, mode_h, slots.size());
    if (slot) {
      slots.push_back(std::move(slot));
      changed = true;
    }
  }

  return changed;
}

// Signal handlers in C++ need a globally-reachable state-bit, and an
// async-signal-safe one at that — `std::atomic<bool>` qualifies because
// it's lock-free on every platform we target. clang-tidy's
// non-const-globals lint can't model that constraint.
//
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_running{true};
extern "C" void on_sigint(int /*signum*/) noexcept {
  g_running.store(false, std::memory_order_relaxed);
}

// Multi-camera streaming loop. Builds the LayerScene with a fullscreen
// gradient background, then runs until SIGINT: each iteration drains
// completed camera requests, re-queues them, commits the scene, and
// blocks on the next vblank (with a poll timeout so hotplug detection
// runs at vblank cadence even when no commits land — e.g. when no
// cameras are visible).
int run_streaming(drm::Device& dev, std::uint32_t crtc_id, std::uint32_t connector_id,
                  const drmModeModeInfo& mode, const std::vector<DisplayFmt>& display_formats,
                  libcamera::CameraManager& cm, std::optional<drm::session::Seat>& seat) {
  // Mute libcamera's V4L2 + DeviceEnumerator chatter on hot-unplug.
  // Hot-unplug ioctls return ENODEV from the kernel, which both
  // categories log at INFO; we already detect the disconnect via
  // cm.cameras() and skip those ioctls in teardown_slot, but the
  // cosmetic noise from libcamera's own teardown remains. Caller can
  // override by setting `LIBCAMERA_LOG_LEVELS` before launch.
  libcamera::logSetLevel("V4L2", "WARN");
  libcamera::logSetLevel("DeviceEnumerator", "WARN");

  auto scene_r = drm::scene::LayerScene::create(dev, {crtc_id, connector_id, mode});
  if (!scene_r) {
    drm::println(stderr, "LayerScene::create: {}", scene_r.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_r);

  // Fullscreen gradient background. Default zpos lets the allocator
  // place it on PRIMARY (amdgpu pins PRIMARY at zpos=2). The handle
  // is kept around so the session-resume path can repaint after
  // libseat re-allocates the underlying dumb buffer (which comes back
  // zero-filled from the kernel).
  auto bg_src =
      drm::scene::DumbBufferSource::create(dev, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888);
  if (!bg_src) {
    drm::println(stderr, "bg DumbBufferSource: {}", bg_src.error().message());
    return EXIT_FAILURE;
  }
  if (auto m = (*bg_src)->map(drm::MapAccess::Write); m) {
    paint_bg_gradient(*m);
  }
  drm::scene::LayerDesc bg_desc;
  bg_desc.source = std::move(*bg_src);
  bg_desc.display.src_rect = {0, 0, mode.hdisplay, mode.vdisplay};
  bg_desc.display.dst_rect = {0, 0, mode.hdisplay, mode.vdisplay};
  bg_desc.content_type = drm::planes::ContentType::Generic;
  auto bg_handle_r = scene->add_layer(std::move(bg_desc));
  if (!bg_handle_r) {
    drm::println(stderr, "add_layer (bg): {}", bg_handle_r.error().message());
    return EXIT_FAILURE;
  }
  const auto bg_handle = *bg_handle_r;

  auto repaint_bg = [&] {
    if (auto* layer = scene->get_layer(bg_handle); layer != nullptr) {
      if (auto m = layer->source().map(drm::MapAccess::Write); m) {
        paint_bg_gradient(*m);
      }
    }
  };

  drm::PageFlip page_flip(dev);
  std::atomic<bool> page_pending{false};
  auto install_page_flip_handler = [&] {
    page_flip.set_handler(
        [&page_pending](std::uint32_t /*c*/, std::uint64_t /*s*/, std::uint64_t /*ts*/) noexcept {
          page_pending.store(false, std::memory_order_relaxed);
        });
  };
  install_page_flip_handler();

  auto attach_seat_source = [&] {
    if (!seat) {
      return;
    }
    if (const int sfd = seat->poll_fd(); sfd >= 0) {
      if (auto r = page_flip.add_source(sfd, [&] { seat->dispatch(); }); !r) {
        drm::println(stderr, "page_flip.add_source(seat): {}", r.error().message());
      }
    }
  };

  // Session integration. On VT switch-out we mark `session_active`
  // false so the loop stops committing. On switch-back libseat hands
  // us a fresh fd; we record it under `pending_resume_fd` and let the
  // main loop perform the actual rebuild on its next iteration. We
  // can't tear down PageFlip from inside the resume callback because
  // that callback fires from inside `seat->dispatch()`, which runs
  // from inside `page_flip.dispatch()` — destroying the PageFlip
  // mid-dispatch is undefined behavior.
  std::atomic<bool> session_active{true};
  std::atomic<int> pending_resume_fd{-1};
  if (seat) {
    seat->set_pause_callback([&] { session_active.store(false, std::memory_order_relaxed); });
    seat->set_resume_callback([&](std::string_view /*path*/, int new_fd) {
      pending_resume_fd.store(new_fd, std::memory_order_relaxed);
    });
    attach_seat_source();
  }

  auto apply_pending_resume = [&]() {
    const int new_fd = pending_resume_fd.exchange(-1, std::memory_order_relaxed);
    if (new_fd < 0) {
      return;
    }
    dev = drm::Device::from_fd(new_fd);
    if (auto r = dev.enable_universal_planes(); !r) {
      drm::println(stderr, "resume: enable_universal_planes: {}", r.error().message());
    }
    if (auto r = dev.enable_atomic(); !r) {
      drm::println(stderr, "resume: enable_atomic: {}", r.error().message());
    }
    if (auto r = scene->on_session_resumed(dev); !r) {
      drm::println(stderr, "resume: scene->on_session_resumed: {}", r.error().message());
    }
    // PageFlip captured the dead fd at construction; rebuild it
    // against the new device. Move-assign destroys the old PageFlip
    // (which closes its userspace epfd but doesn't touch the dead drm
    // fd — Device's owns_fd contract handled that).
    page_flip = drm::PageFlip(dev);
    install_page_flip_handler();
    attach_seat_source();
    repaint_bg();
    // The page-flip event for the pre-pause commit was lost when the
    // fd died; clear the pending flag so we can re-arm.
    page_pending.store(false, std::memory_order_relaxed);
    session_active.store(true, std::memory_order_relaxed);
  };

  std::signal(SIGINT, on_sigint);
  std::signal(SIGTERM, on_sigint);

  std::vector<std::unique_ptr<CameraSlot>> slots;
  drm::println("streaming — Ctrl-C to stop");

  std::uint64_t commits_landed = 0;
  auto fps_window_start = std::chrono::steady_clock::now();

  while (g_running.load(std::memory_order_relaxed)) {
    apply_pending_resume();
    const bool active = session_active.load(std::memory_order_relaxed);
    if (active) {
      const bool changed =
          reconcile_cameras(dev, *scene, cm, slots, display_formats, mode.hdisplay, mode.vdisplay);
      if (changed) {
        apply_layout(slots, *scene, mode.hdisplay, mode.vdisplay);
      }

      for (auto& slot : slots) {
        drain_slot(*slot, *scene);
      }

      if (!page_pending.load(std::memory_order_relaxed)) {
        const auto report = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip);
        if (!report) {
          drm::println(stderr, "commit: {}", report.error().message());
        } else {
          page_pending.store(true, std::memory_order_relaxed);
          ++commits_landed;
        }
      }
    }

    // Wait for vblank, seat-event readiness, or a 33ms timeout so
    // hotplug detection runs at ~30 Hz even when no cameras are
    // visible (and so the loop ticks at all during a session pause).
    if (auto r = page_flip.dispatch(33); !r) {
      if (r.error() != std::errc::timed_out) {
        drm::println(stderr, "dispatch: {}", r.error().message());
      }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - fps_window_start >= std::chrono::seconds(1)) {
      drm::println(stderr, "fps={} cameras={}", commits_landed, slots.size());
      commits_landed = 0;
      fps_window_start = now;
    }
  }
  drm::println("\nshutting down");

  if (seat) {
    if (const int sfd = seat->poll_fd(); sfd >= 0) {
      page_flip.remove_source(sfd);
    }
  }
  for (auto& slot : slots) {
    teardown_slot(*slot, *scene, /*still_present=*/true);
  }
  slots.clear();
  scene.reset();
  return EXIT_SUCCESS;
}

void print_usage() {
  drm::println(stderr, "usage: camera (--probe | --show) [/dev/dri/cardN]");
}

int run_probe(int argc, char* argv[]) {
  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const drmModeModeInfo mode = output->mode;

  drm::println("Output: {}x{}@{}Hz on connector {} / CRTC {}", mode.hdisplay, mode.vdisplay,
               mode.vrefresh, output->connector_id, output->crtc_id);

  // High-level capability summary first, so the per-plane dump below has
  // context. The format probe already parses the same data we walk
  // below; the duplication is intentional — the summary is what a user
  // wants to skim, the per-plane dump is what a developer wants to
  // grep.
  const auto caps = drm::examples::probe_output(dev, output->crtc_id);
  drm::println(
      "Planes: {} total ({} primary, {} overlay, {} cursor); zpos={} alpha_blend_planes={}",
      caps.n_planes + caps.n_cursor, caps.n_primary, caps.n_overlay, caps.n_cursor,
      caps.any_plane_zpos ? "yes" : "no", caps.n_overlay_alpha_blend + caps.n_primary_alpha_blend);

  const auto idx = crtc_index_of(dev, output->crtc_id);
  if (!idx) {
    drm::println(stderr, "Could not resolve CRTC index for id {}", output->crtc_id);
    return EXIT_FAILURE;
  }
  auto reg = drm::planes::PlaneRegistry::enumerate(dev);
  if (!reg) {
    drm::println(stderr, "PlaneRegistry::enumerate: {}", reg.error().message());
    return EXIT_FAILURE;
  }

  drm::println("Plane formats (IN_FORMATS) reachable from CRTC {}:", output->crtc_id);
  std::vector<DisplayFmt> display_formats;
  for (const auto* p : reg->for_crtc(*idx)) {
    print_plane(*p, display_formats);
  }

  drm::println("");
  libcamera::CameraManager cm;
  if (const int rc = cm.start(); rc < 0) {
    drm::println(stderr, "CameraManager::start: {}", std::strerror(-rc));
    return EXIT_FAILURE;
  }
  std::vector<CaptureFmt> capture_formats;
  // Scope cameras so the shared_ptrs drop before cm.stop().
  {
    const auto cameras = cm.cameras();
    walk_cameras(cameras, capture_formats);

    // Per-camera path readout: what `--show` would actually pick for
    // each camera in turn (multi-camera streaming runs each camera
    // through its own negotiation, so the global pick from `negotiate`
    // below isn't representative when more than one camera is visible).
    drm::println("");
    drm::println("Per-camera streaming paths:");
    for (std::size_t i = 0; i < cameras.size(); ++i) {
      std::vector<CaptureFmt> per_cam;
      for (const auto& cf : capture_formats) {
        if (cf.camera_index == i) {
          per_cam.push_back(cf);
        }
      }
      // Multi-camera --show forces ZeroCopy off; reflect that here so
      // the output matches what the streaming run will do.
      const auto t = negotiate(display_formats, per_cam, mode.hdisplay, mode.vdisplay,
                               /*allow_zero_copy=*/false);
      if (!t) {
        drm::println("  [{}] no common format", i);
        continue;
      }
      const auto cam_chars = fourcc_to_chars(t->camera_fourcc);
      drm::println("  [{}] capture {} {}x{}  path: {}", i, std::string_view(cam_chars.data(), 4),
                   t->size.width, t->size.height, mode_label(t->mode));
    }
  }
  cm.stop();

  drm::println("");
  drm::println("Single-camera (--allow_zero_copy) negotiation across all visible cameras:");
  print_negotiation(negotiate(display_formats, capture_formats, mode.hdisplay, mode.vdisplay));
  return EXIT_SUCCESS;
}

int run_show(int argc, char* argv[]) {
  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  const drmModeModeInfo mode = output->mode;

  drm::println("Output: {}x{}@{}Hz on connector {} / CRTC {}", mode.hdisplay, mode.vdisplay,
               mode.vrefresh, output->connector_id, output->crtc_id);

  const auto idx = crtc_index_of(dev, output->crtc_id);
  if (!idx) {
    drm::println(stderr, "Could not resolve CRTC index for id {}", output->crtc_id);
    return EXIT_FAILURE;
  }
  auto reg = drm::planes::PlaneRegistry::enumerate(dev);
  if (!reg) {
    drm::println(stderr, "PlaneRegistry::enumerate: {}", reg.error().message());
    return EXIT_FAILURE;
  }

  drm::println("Plane formats (IN_FORMATS) reachable from CRTC {}:", output->crtc_id);
  std::vector<DisplayFmt> display_formats;
  for (const auto* p : reg->for_crtc(*idx)) {
    print_plane(*p, display_formats);
  }

  drm::println("");
  libcamera::CameraManager cm;
  if (const int rc = cm.start(); rc < 0) {
    drm::println(stderr, "CameraManager::start: {}", std::strerror(-rc));
    return EXIT_FAILURE;
  }

  const int rc = run_streaming(dev, output->crtc_id, output->connector_id, mode, display_formats,
                               cm, output->seat);
  cm.stop();
  return rc;
}

}  // namespace

int main(int argc, char* argv[]) {
  // Pull `--probe` / `--show` off argv so the device-path positional
  // arg lands at argv[1] for select_device(). The two modes are
  // mutually exclusive; specifying both is a usage error until there
  // is a reason for them to coexist.
  bool want_probe = false;
  bool want_show = false;
  std::vector<char*> rest;
  rest.reserve(static_cast<std::size_t>(argc));
  rest.push_back(argv[0]);
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--probe") {
      want_probe = true;
    } else if (arg == "--show") {
      want_show = true;
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      return EXIT_SUCCESS;
    } else if (!arg.empty() && arg.front() == '-') {
      drm::println(stderr, "Unknown option: {}", arg);
      print_usage();
      return EXIT_FAILURE;
    } else {
      rest.push_back(argv[i]);
    }
  }

  if (want_probe && want_show) {
    drm::println(stderr, "camera: --probe and --show are mutually exclusive");
    print_usage();
    return EXIT_FAILURE;
  }
  if (!want_probe && !want_show) {
    print_usage();
    return EXIT_FAILURE;
  }

  if (want_show) {
    return run_show(static_cast<int>(rest.size()), rest.data());
  }
  return run_probe(static_cast<int>(rest.size()), rest.data());
}