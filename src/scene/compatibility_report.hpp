// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// compatibility_report.hpp — return value of LayerScene::rebind().
//
// rebind() preserves layer handles across a CRTC/connector/mode swap
// but the new configuration may not accept every layer in the same
// shape it had before — a layer's destination rect could land off the
// new mode's display area, the new plane registry might lack a plane
// that supports the layer's format, or a layer that previously fit on
// a non-scaling plane might now require scaling the new pool can't
// service. CompatibilityReport flags these per layer; the caller
// decides whether to remove / reposition / accept the layer before
// the next commit. Layers absent from the report are believed to fit
// the new configuration.

#pragma once

#include "layer_handle.hpp"

#include <cstdint>
#include <vector>

namespace drm::scene {

struct LayerIncompatibility {
  /// Why the scene flagged this layer. Multiple causes can apply to
  /// the same layer (e.g. a layer that needs scaling AND has a
  /// dst_rect off-screen) — the report emits one entry per cause.
  enum class Reason : std::uint8_t {
    /// `display.dst_rect` lies entirely outside the new mode's
    /// hdisplay × vdisplay box, or extends so far past the edge that
    /// no visible pixel survives. The kernel will reject the commit;
    /// caller should reposition or remove the layer.
    DstRectOffScreen,
    /// `src_rect` and `dst_rect` differ in size but no plane in the
    /// new registry's CRTC-compatible set advertises scaling support.
    /// Caller should equalize the rects (loses the implicit zoom) or
    /// remove the layer.
    RequiresScalingNotAvailable,
    /// The layer's source format isn't supported by any plane on the
    /// new CRTC. Caller should reformat the source or remove.
    FormatNotSupported,
  };

  LayerHandle handle{};
  Reason reason{Reason::DstRectOffScreen};
};

/// Returned by `LayerScene::rebind`. Empty when every live layer
/// looks compatible with the new configuration. Non-empty entries do
/// **not** prevent the rebind from completing — the scene rebinds
/// regardless and the caller is responsible for repositioning /
/// removing flagged layers before the next commit.
struct CompatibilityReport {
  std::vector<LayerIncompatibility> incompatibilities;

  [[nodiscard]] bool empty() const noexcept { return incompatibilities.empty(); }
};

}  // namespace drm::scene
