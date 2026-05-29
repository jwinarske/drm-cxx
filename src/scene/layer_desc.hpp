// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// layer_desc.hpp — create-time descriptor for a scene layer.
//
// Passed by value to LayerScene::add_layer. The unique_ptr to the
// LayerBufferSource transfers ownership of the source into the scene;
// the source's lifetime from that point on is tied to the created
// Layer's lifetime.

#pragma once

#include "buffer_source.hpp"
#include "display_params.hpp"

#include <drm-cxx/planes/layer.hpp>

#include <cstdint>
#include <memory>

namespace drm::scene {

/// Create-time descriptor for a scene layer. Passed by value to
/// `LayerScene::add_layer`; the embedded `unique_ptr<LayerBufferSource>`
/// transfers ownership of the source into the scene, and the source's
/// lifetime from that point on is tied to the created `Layer`.
struct LayerDesc {
  /// Where the layer's pixels come from each frame. Required — nullptr
  /// is rejected by add_layer with std::errc::invalid_argument.
  std::unique_ptr<LayerBufferSource> source;

  /// How to display the source's buffer. src_rect defaults to the
  /// buffer's full extent if zero; dst_rect must be set explicitly
  /// because the scene has no way to guess a sensible screen position.
  DisplayParams display{};

  /// Allocator hint: some plane types (primary, overlay, cursor) have
  /// format and capability asymmetries; this lets the allocator
  /// preferentially assign video-like content to the overlay planes
  /// with the best YUV coverage, UI content to the rest, etc.
  drm::planes::ContentType content_type{drm::planes::ContentType::Generic};

  /// Producer's expected refresh rate in Hz, 0 = no hint. Used by the
  /// allocator for scheduling decisions (content updating at 30 Hz on
  /// a 60 Hz display can sit on a plane that doesn't tear-mitigate).
  std::uint32_t update_hint_hz{0};

  /// When true, the allocator will skip plane assignment for this layer
  /// and route it through the composition fallback unconditionally.
  /// Useful for diagnostic overlays, integration tests of the compositor
  /// path, or layers whose source is known to require CPU compositing
  /// even when a hardware plane is available. Requires
  /// `source->map(MapAccess::Read)` to succeed — sources whose pixels
  /// don't reach CPU memory (future EGL Stream consumers, tiled GBM
  /// BOs) cannot be composited and will be dropped this frame.
  bool force_composited{false};

  /// Opaque pointer the scene stores verbatim on the created `Layer`
  /// and returns from `LayerScene::find_by_identity_tag`. Not
  /// interpreted by the scene — never dereferenced, freed, or copied
  /// beyond a value copy onto the `Layer`. Callers commonly stash the
  /// engine-side identity here (e.g. a `FlutterBackingStore*` or a
  /// platform-view id cast to `void*`) to look the scene-side `Layer`
  /// up without maintaining a parallel handle map.
  ///
  /// NOTE: This is layer identity for caller-side lookup; it is
  /// distinct from the `user_data` pointer passed to
  /// `LayerScene::commit(flags, user_data)`, which is the page-flip /
  /// commit-context pointer the kernel round-trips on vblank. The two
  /// carry unrelated semantics.
  void* identity_tag{nullptr};
};

}  // namespace drm::scene
