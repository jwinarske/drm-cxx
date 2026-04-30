// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// format_probe.hpp — runtime capability probe + best-effort warnings.
//
// LayerScene degrades gracefully when planes lack zpos or when overlay
// planes don't accept the format the example asks for: the allocator's
// apply_layer_to_plane silently drops properties the plane doesn't
// advertise, and unsupported (format, modifier) pairs route the layer
// through the composition fallback. Both behaviors are correct, but
// neither prints anything, so a user running an example on (e.g.) an
// older Intel sprite plane that lacks ARGB8888 sees pixels rendered
// "differently" without knowing why.
//
// `probe_output()` summarises what the active CRTC's planes can do, and
// `warn_compat()` prints one-line notes for the capability gaps that
// matter to the example. Examples opt in to whichever warnings are
// relevant via `WarnFlags`.

#pragma once

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <drm_fourcc.h>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace drm::examples {

struct OutputCapabilities {
  /// Number of usable PRIMARY+OVERLAY planes reachable from this CRTC
  /// (cursor planes excluded). 0 means the registry probe failed.
  std::size_t n_planes{0};
  std::size_t n_primary{0};
  std::size_t n_overlay{0};
  std::size_t n_cursor{0};
  /// Any non-cursor plane on this CRTC supports DRM_FORMAT_ARGB8888.
  bool any_plane_argb{false};
  /// Any OVERLAY plane on this CRTC supports DRM_FORMAT_ARGB8888.
  /// Drives the alpha-overlay warning; pre-Gen10 Intel "sprite" planes
  /// expose XRGB8888 + YUV but no per-pixel-alpha format, so an
  /// example that pins ARGB8888 on every overlay layer will land all
  /// of them on the composition canvas instead of their own plane.
  bool overlays_argb{false};
  /// Any OVERLAY plane on this CRTC supports DRM_FORMAT_XRGB8888.
  bool overlays_xrgb{false};
  bool primary_argb{false};
  bool primary_xrgb{false};
  /// Any non-cursor plane on this CRTC exposes the zpos property.
  /// When false, layer ordering is fully up to the driver and any
  /// `display.zpos = N` the example sets on a layer is silently
  /// dropped at commit time.
  bool any_plane_zpos{false};
  /// Number of OVERLAY planes on this CRTC that expose
  /// `"pixel blend mode"` AND advertise the `"Pre-multiplied"` enum
  /// value — i.e. planes the composition canvas can land on without
  /// pixel-replacing the natively-assigned cells beneath. When this
  /// is 0, scenes that overflow the hardware-plane budget cannot be
  /// composited without the canvas painting opaque-black over native
  /// regions (the "non-uniform black squares" failure mode).
  std::size_t n_overlay_alpha_blend{0};
  /// Same probe applied to PRIMARY planes. PRIMARY is the canvas's
  /// fallback target when every OVERLAY is in use, so its blend mode
  /// matters too.
  std::size_t n_primary_alpha_blend{0};
  /// Number of PRIMARY+OVERLAY planes that expose the per-plane
  /// `"alpha"` u16 modulator. Independent of `pixel blend mode` —
  /// some hardware exposes one without the other.
  std::size_t n_planes_with_alpha_property{0};
};

namespace detail {

inline std::optional<std::uint32_t> crtc_index_of(const drm::Device& dev,
                                                  std::uint32_t crtc_id) noexcept {
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

}  // namespace detail

/// Probe the planes reachable from `crtc_id`. Failures (no resources,
/// no registry, unknown CRTC) collapse to a default-constructed result;
/// callers should treat all-zero/all-false as "unknown — skip warnings"
/// rather than re-flagging them as failures.
[[nodiscard]] inline OutputCapabilities probe_output(const drm::Device& dev,
                                                     std::uint32_t crtc_id) noexcept {
  OutputCapabilities caps{};
  const auto idx = detail::crtc_index_of(dev, crtc_id);
  if (!idx) {
    return caps;
  }
  auto reg = drm::planes::PlaneRegistry::enumerate(dev);
  if (!reg) {
    return caps;
  }
  for (const auto* p : reg->for_crtc(*idx)) {
    const bool argb = p->supports_format(DRM_FORMAT_ARGB8888);
    const bool xrgb = p->supports_format(DRM_FORMAT_XRGB8888);
    if (p->type == drm::planes::DRMPlaneType::CURSOR) {
      ++caps.n_cursor;
      continue;
    }
    ++caps.n_planes;
    caps.any_plane_argb = caps.any_plane_argb || argb;
    if (p->zpos_min.has_value() || p->zpos_max.has_value()) {
      caps.any_plane_zpos = true;
    }
    if (p->has_per_plane_alpha) {
      ++caps.n_planes_with_alpha_property;
    }
    const bool can_blend = p->blend_mode_premultiplied.has_value();
    if (p->type == drm::planes::DRMPlaneType::PRIMARY) {
      ++caps.n_primary;
      caps.primary_argb = caps.primary_argb || argb;
      caps.primary_xrgb = caps.primary_xrgb || xrgb;
      if (can_blend) {
        ++caps.n_primary_alpha_blend;
      }
    } else {
      ++caps.n_overlay;
      caps.overlays_argb = caps.overlays_argb || argb;
      caps.overlays_xrgb = caps.overlays_xrgb || xrgb;
      if (can_blend) {
        ++caps.n_overlay_alpha_blend;
      }
    }
  }
  return caps;
}

/// What an example tells the probe it cares about. The probe only warns
/// about capabilities the example actually exercises — a primary-only
/// demo doesn't want to be told its overlays are XRGB-only.
struct WarnFlags {
  /// Set when the example puts ARGB8888 layers on overlay planes
  /// (signage_player, layered_demo, scene_warm_start, hotplug_monitor,
  /// test_patterns, video_grid's ARGB cells).
  bool wants_alpha_overlays{false};
  /// Set when the example pins `display.zpos` on its layers
  /// (every scene-family example).
  bool wants_explicit_zpos{false};
  /// Set when the example builds N hardware-placed overlay layers.
  /// 0 disables the count-based warning. Compared against n_overlay.
  std::size_t wants_overlay_count{0};
  /// Set when the example expects scene composition (canvas fallback)
  /// to land on a plane that alpha-blends over the natively-assigned
  /// cells beneath it. True for any scene example whose layer count
  /// can exceed the hardware plane budget — every grid/wall/MDI demo
  /// in this tree. When no plane on the CRTC supports
  /// `"pixel blend mode" = "Pre-multiplied"`, the canvas pixel-replaces
  /// the natives → opaque-black holes where the natives should show.
  bool wants_alpha_blending{false};
};

/// Emit one-line stderr notes for each capability gap.
inline void warn_compat(const OutputCapabilities& caps, const WarnFlags& wants) noexcept {
  if (caps.n_planes == 0) {
    // Probe failed — don't pretend we know anything.
    return;
  }
  if (wants.wants_alpha_overlays && caps.n_overlay > 0 && !caps.overlays_argb) {
    drm::println(stderr,
                 "[compat] No OVERLAY plane on this CRTC supports ARGB8888 — alpha "
                 "layers will land on the composition canvas instead of dedicated "
                 "planes. Common on pre-Gen10 Intel sprite planes.");
  }
  if (wants.wants_explicit_zpos && !caps.any_plane_zpos) {
    drm::println(stderr,
                 "[compat] No plane on this CRTC exposes the zpos property — layer "
                 "ordering will be driver-defined and any display.zpos hints set on "
                 "layers are silently dropped at commit time. Common on radeon, "
                 "older amdgpu/i915 kernels, and the NVIDIA proprietary blob.");
  }
  if (wants.wants_overlay_count > 0 && caps.n_overlay < wants.wants_overlay_count) {
    drm::println(stderr,
                 "[compat] Only {} OVERLAY plane(s) on this CRTC; example expects {}+. "
                 "Excess layers will land on the composition canvas.",
                 caps.n_overlay, wants.wants_overlay_count);
  }
  if (wants.wants_alpha_blending && caps.n_overlay_alpha_blend == 0 &&
      caps.n_primary_alpha_blend == 0) {
    drm::println(
        stderr,
        "[compat] No PRIMARY or OVERLAY plane on this CRTC supports "
        "`pixel blend mode` = `Pre-multiplied`. The composition canvas will pixel-replace "
        "natively-assigned cells with opaque pixels — expect dark holes where natives sit. "
        "Driver default is opaque on most legacy stacks.");
  }
}

}  // namespace drm::examples
