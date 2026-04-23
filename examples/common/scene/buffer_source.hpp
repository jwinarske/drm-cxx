// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// buffer_source.hpp — polymorphic backing-buffer interface for scene
// layers. See docs/implementation_plan.md M2 Phase 2.1 for full context
// and the v2 roadmap (Forward Compatibility section).
//
// Design invariants, baked into the types here and not intended to drift:
//
//   * SourceFormat describes what the buffer IS (format, modifier,
//     intrinsic size). It is owned by the source. The scene never
//     mutates it.
//
//   * DisplayParams (on LayerDesc, not here) describes how the buffer
//     should be displayed (src_rect, dst_rect, rotation, alpha, zpos).
//     Rotation and scaling are plane properties in KMS — they are not
//     properties of the buffer. Separating the two mirrors the KMS
//     concept boundary exactly and makes it natural to display the
//     same SourceFormat content multiple ways.
//
//   * release() is noexcept AND returns void. The source owns every
//     allocation reachable via AcquiredBuffer; release is infallible
//     by construction. If a source's release path can genuinely fail
//     (remote producers, network-backed surfaces), it must log and
//     leak rather than propagate — the scene cannot observe release
//     failure.
//
//   * bind_to_plane / unbind_from_plane are the v2 insertion points
//     for BindingModel::DriverOwnsBinding (EGL Streams and related).
//     Default no-op is correct for every SceneSubmitsFbId source. v1
//     sources do NOT override these.

#pragma once

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <system_error>

namespace drm::scene {

/// How the scene and the source cooperate on FB_ID ownership.
enum class BindingModel : std::uint8_t {
  /// Scene writes `FB_ID` to the atomic commit. Covers dumb buffers,
  /// GBM BOs, imported DMA-BUFs, V4L2 capture buffers, accel outputs —
  /// everything that turns into a DRM framebuffer via drmModeAddFB2.
  /// All v1 sources use this model.
  SceneSubmitsFbId,

  /// The driver binds a producer (EGL Stream consumer, etc.) directly
  /// to the plane; the scene must not write `FB_ID` for this layer.
  /// Reserved for v2. No v1 source reports this; the scene asserts on
  /// its appearance.
  DriverOwnsBinding,
};

/// Intrinsic properties of the buffer the source produces. Format and
/// modifier are DRM FourCC values (e.g. `DRM_FORMAT_ARGB8888`,
/// `DRM_FORMAT_MOD_LINEAR`). Width and height are the buffer's own
/// dimensions — not the on-screen display rectangle.
struct SourceFormat {
  std::uint32_t drm_fourcc{0};
  std::uint64_t modifier{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
};

/// Handle to a buffer returned from `acquire()`. Valid until matched
/// by `release()`.
///
/// For `BindingModel::SceneSubmitsFbId` sources:
///   * `fb_id` is a DRM framebuffer ID the scene will write to the
///     plane's `FB_ID` property. Must be non-zero.
///   * `acquire_fence_fd` is an optional sync_file fd the scene will
///     write to `IN_FENCE_FD`; `-1` means no fence. The scene takes
///     ownership and closes it after the commit lands.
///   * `opaque` is a source-private token echoed back verbatim to
///     `release()` — typically the slot index in a buffer ring.
///
/// For `BindingModel::DriverOwnsBinding` sources (v2, unimplemented):
///   * `fb_id` must be 0 — the scene skips FB_ID writes.
///   * `acquire_fence_fd` still applies if the producer supplies one.
struct AcquiredBuffer {
  std::uint32_t fb_id{0};
  int acquire_fence_fd{-1};
  void* opaque{nullptr};
};

/// Polymorphic interface for "where does this layer's content come
/// from?". Concrete v1 implementations: `DumbBufferSource` (single
/// dumb buffer, CPU-writable) and `GbmBufferSource` (rotating ring of
/// GBM BOs, optionally fronted by a `gbm_surface` for GL/Vulkan
/// producers).
///
/// Sources are move-only — ownership of a source is ownership of the
/// DRM handles it holds. Scene layers keep a `std::unique_ptr` to the
/// source; destroying the layer destroys the source and its buffers.
class LayerBufferSource {
 public:
  LayerBufferSource() = default;
  virtual ~LayerBufferSource() = default;

  LayerBufferSource(const LayerBufferSource&) = delete;
  LayerBufferSource& operator=(const LayerBufferSource&) = delete;
  LayerBufferSource(LayerBufferSource&&) noexcept = default;
  LayerBufferSource& operator=(LayerBufferSource&&) noexcept = default;

  /// Produce the next frame's buffer. Called by the scene during its
  /// per-frame commit build. On success the source has arranged that
  /// `fb_id` (for SceneSubmitsFbId) points at a ready-to-scanout
  /// framebuffer; the scene will pair the result with a subsequent
  /// `release()` once the commit completes.
  [[nodiscard]] virtual drm::expected<AcquiredBuffer, std::error_code> acquire() = 0;

  /// Return the buffer to the source's free pool. Must be infallible.
  /// The scene calls this after page-flip completion (or on commit
  /// failure, or during shutdown); error channels have been deliberately
  /// omitted to guarantee the scene's cleanup paths are simple.
  virtual void release(AcquiredBuffer acquired) noexcept = 0;

  /// Which binding contract this source participates in.
  [[nodiscard]] virtual BindingModel binding_model() const noexcept = 0;

  /// Describe the buffer's shape. Called by the scene during layer
  /// lowering to populate the `planes::Layer` format/modifier/size
  /// properties the allocator reads.
  [[nodiscard]] virtual SourceFormat format() const noexcept = 0;

  // ── v2 DriverOwnsBinding hooks ─────────────────────────────────────
  //
  // Default no-op is correct for every SceneSubmitsFbId source. v2 EGL
  // Streams sources will override these to drive the stream-consumer
  // bind / unbind lifecycle; v1 sources must not override them. The
  // scene calls `bind_to_plane` after the allocator has settled on a
  // plane for this layer and before the first acquire() for that
  // plane, and calls `unbind_from_plane` when the layer is reassigned
  // off the plane or retired.

  virtual drm::expected<void, std::error_code> bind_to_plane(
      std::uint32_t /*plane_id*/) {
    return {};
  }

  virtual void unbind_from_plane(std::uint32_t /*plane_id*/) noexcept {}
};

}  // namespace drm::scene