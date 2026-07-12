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

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

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
///   * `opaque` is a source-private token echoed back verbatim to
///     `release()` — typically the slot index in a buffer ring.
///
/// For `BindingModel::DriverOwnsBinding` sources (v2, unimplemented):
///   * `fb_id` must be 0 — the scene skips FB_ID writes.
///
/// `acquire_fence` is an optional render-done sync_file: the buffer's
/// pixels are only valid once it signals. Set it when the source's
/// producer renders asynchronously (e.g. VkScanoutProducer exporting a
/// semaphore as a sync_file) instead of CPU-blocking before returning.
/// The scene takes ownership: when the assigned plane advertises
/// `IN_FENCE_FD`, the scene hands the fd to KMS so the kernel waits
/// before scanout; otherwise it CPU-`wait()`s the fence before commit.
/// Either way the SyncFence rides this buffer's lifecycle and closes the
/// fd when the buffer is released or dropped — the kernel does not take
/// ownership of `IN_FENCE_FD`. Sources that produce ready buffers
/// synchronously (dumb, GBM, V4L2, imported DMA-BUFs) leave it nullopt.
///
/// NOTE: a CPU-mappable fenced source must `wait()` before `map()` for
/// composition fallback to read complete pixels; the only fenced source
/// today (ExternalDmaBufSource) is uncompositable, so this is deferred.
///
/// A changed region of the buffer since the slot was last scanned out, in
/// buffer pixels (top-left origin). Reported per-frame on `AcquiredBuffer` so
/// the scene can emit `FB_DAMAGE_CLIPS` and the driver repaints only the dirty
/// area (a power/bandwidth win, and what lets a PSR panel stay in self-refresh).
struct DamageRect {
  std::int32_t x{0};
  std::int32_t y{0};
  std::uint32_t w{0};
  std::uint32_t h{0};
};

/// `damage` is the optional per-frame dirty-region list. **Empty means
/// full-frame** — the scene emits no `FB_DAMAGE_CLIPS` and the whole buffer is
/// assumed changed (correct, just not power-optimal). A non-empty list is the
/// changed regions; the scene clamps the count and falls back to full-frame
/// above its bound, and omits the blob on planes/drivers without the property.
/// Sources that always repaint everything (most v1 sources) leave it empty.
///
/// Carrying a move-only SyncFence makes AcquiredBuffer move-only.
struct AcquiredBuffer {
  std::uint32_t fb_id{0};
  void* opaque{nullptr};
  std::optional<drm::sync::SyncFence> acquire_fence;
  std::vector<DamageRect> damage;
};

/// A borrowed view of a source buffer's DMA-BUF planes, for a consumer
/// that imports the buffer directly (e.g. `GlCompositor` via
/// `EGL_EXT_image_dma_buf_import`) instead of reading CPU pixels through
/// `map()`. Returned by `export_dma_buf()` for the currently-acquired
/// buffer.
///
/// The `fds` are **borrowed**: they stay owned by the source and are
/// valid only until the matching `release()` of the current buffer. The
/// consumer must NOT close them — an EGLImage import references the fds
/// internally, so no dup is needed. `n_planes` DRM planes are described;
/// plane `p` imports from `fds[p]` at `offsets[p]` / `pitches[p]`.
/// Single-fd multi-plane formats (NV12) repeat the same fd across planes
/// with distinct offsets. `drm_fourcc` / `modifier` / `width` / `height`
/// match `format()`.
struct DmaBufDesc {
  std::array<int, 4> fds{{-1, -1, -1, -1}};
  std::array<std::uint32_t, 4> offsets{};
  std::array<std::uint32_t, 4> pitches{};
  std::uint32_t n_planes{0};
  std::uint32_t drm_fourcc{0};
  std::uint64_t modifier{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
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
  ///
  /// `errc::resource_unavailable_try_again` (EAGAIN) is a documented
  /// flow-control return — not an error. It means "I have no frame to
  /// contribute this vblank." `LayerScene::commit()` skips the layer
  /// for that commit; the next commit re-calls `acquire()`. Live
  /// sources should return EAGAIN before their first sample lands and
  /// any time the producer falls behind without a cached frame to
  /// re-issue. Sources that always have a valid buffer (DumbBuffer,
  /// GbmBuffer, ExternalDmaBuf) never need to return EAGAIN. Any
  /// other error code is treated as a real failure and aborts the
  /// commit.
  [[nodiscard]] virtual drm::expected<AcquiredBuffer, std::error_code> acquire() = 0;

  /// Return the buffer to the source's free pool. Must be infallible.
  /// The scene calls this after page-flip completion (or on commit
  /// failure, or during shutdown); error channels have been deliberately
  /// omitted to guarantee the scene's cleanup paths are simple.
  virtual void release(AcquiredBuffer acquired) noexcept = 0;

  /// Release variant carrying the OUT_FENCE of the commit that displaced this
  /// buffer — once it signals, the buffer is off-screen and safe to
  /// render into again, so a GPU producer can wait on it GPU-side instead of
  /// CPU-blocking on the (later) release edge. `release_fence` is nullopt when
  /// the CRTC has no OUT_FENCE_PTR or the source didn't opt in. The scene calls
  /// this for deferred releases; the default ignores the fence and forwards to
  /// `release()`, so only sources that want it (see `wants_release_fence`)
  /// override. Must be infallible.
  virtual void release_with_fence(AcquiredBuffer acquired,
                                  std::optional<drm::sync::SyncFence> release_fence) noexcept {
    (void)release_fence;
    release(std::move(acquired));
  }

  /// True if this source wants the per-buffer release fence delivered via
  /// `release_with_fence`. The scene only requests an internal OUT_FENCE (and
  /// stamps it onto in-flight buffers) when some live source returns true here —
  /// zero cost otherwise. Default false.
  [[nodiscard]] virtual bool wants_release_fence() const noexcept { return false; }

  /// True if this source has new content to present this frame (a producer
  /// submitted a fresh buffer since the last commit). Drives the scene's all-idle
  /// whole-commit Skip (`LayerScene::content_changed`): when every live source
  /// returns false and no layer is dirty, the present loop issues no atomic
  /// commit at all — a power win, and lets a PSR panel stay in self-refresh.
  ///
  /// Default true is the conservative answer: a source that can't tell whether
  /// its backing buffer changed (a CPU producer painting a dumb buffer the scene
  /// can't introspect) must report "changed" so a real update is never skipped.
  /// Only sources that *know* they are idle — a rotating producer with nothing
  /// submitted (ExternalDmaBufRing, DumbRingSource) — override to return false,
  /// which is what enables the Skip.
  [[nodiscard]] virtual bool has_fresh_content() const noexcept { return true; }

  /// Which binding contract this source participates in.
  [[nodiscard]] virtual BindingModel binding_model() const noexcept = 0;

  /// Describe the buffer's shape. Called by the scene during layer
  /// lowering to populate the `planes::Layer` format/modifier/size
  /// properties the allocator reads.
  [[nodiscard]] virtual SourceFormat format() const noexcept = 0;

  /// Acquire a scoped CPU mapping over the source's pixel storage.
  /// Sources that hold a linear CPU mapping (DumbBufferSource,
  /// GbmBufferSource with LINEAR + WRITE usage) implement this against
  /// the underlying buffer's `map()`; sources whose pixels only live
  /// in GPU memory or behind a producer the scene can't reach (future
  /// EGL Stream consumers, NPU outputs) return
  /// `errc::function_not_supported` from the default implementation
  /// here.
  ///
  /// Composition fallback acquires `MapAccess::Read` to pull pixels
  /// from layers the allocator couldn't place on hardware; consumers
  /// painting into the source acquire `MapAccess::Write` or
  /// `MapAccess::ReadWrite`. The guard's lifetime should match the
  /// region of code that touches pixels — the scene drops it before
  /// arming the canvas plane, consumers drop it before each commit.
  ///
  /// Sources that return `function_not_supported` are uncompositable —
  /// when the allocator drops them, the scene cannot rescue them via
  /// composition and they stay dropped for the frame.
  [[nodiscard]] virtual drm::expected<drm::BufferMapping, std::error_code> map(
      drm::MapAccess /*access*/) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::function_not_supported));
  }

  /// Export the currently-acquired buffer's DMA-BUF planes so a consumer
  /// can import the buffer directly — the GPU compositor's EGLImage path —
  /// instead of reading CPU pixels through `map()`. Mirrors `map()`'s
  /// contract: the default returns `errc::function_not_supported`, so a
  /// source that can't (or needn't) export is simply never import-
  /// composited. DMA-BUF-backed sources (`ExternalDmaBufSource` /
  /// `ExternalDmaBufRing`, `V4l2DecoderSource`) override it. The returned
  /// `DmaBufDesc::fds` are borrowed and valid only between `acquire()` and
  /// the matching `release()` — see `DmaBufDesc`.
  ///
  /// This is the escape from the `map()`-uncompositable trap: a source
  /// whose pixels never reach CPU memory (a V4L2 camera capture buffer)
  /// can still be composited by importing its DMA-BUF, so the layer no
  /// longer blanks when the allocator can't place it on a plane.
  [[nodiscard]] virtual drm::expected<DmaBufDesc, std::error_code> export_dma_buf() {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::function_not_supported));
  }

  // ── v2 DriverOwnsBinding hooks ─────────────────────────────────────
  //
  // Default no-op is correct for every SceneSubmitsFbId source. v2 EGL
  // Streams sources will override these to drive the stream-consumer
  // bind / unbind lifecycle; v1 sources must not override them. The
  // scene calls `bind_to_plane` after the allocator has settled on a
  // plane for this layer and before the first acquire() for that
  // plane, and calls `unbind_from_plane` when the layer is reassigned
  // off the plane or retired.

  virtual drm::expected<void, std::error_code> bind_to_plane(std::uint32_t /*plane_id*/) {
    return {};
  }

  virtual void unbind_from_plane(std::uint32_t /*plane_id*/) noexcept {}

  // ── Session hooks ─────────────────────────────────────────────────
  //
  // Mirror drm::cursor::Renderer's session contract. Called by
  // LayerScene::on_session_paused / on_session_resumed when libseat
  // hands the process a fresh DRM fd after a VT switch. Default no-op
  // is correct for sources that hold no fd-bound state (pure-memory
  // test sources, future EGL Streams where the driver owns the
  // binding). fd-backed sources (dumb buffers, GBM BOs) must override
  // on_session_resumed to drop handles bound to the dead fd and
  // re-allocate against new_dev.

  /// The seat is losing master. The source must not issue DRM ioctls
  /// until on_session_resumed. Safe to no-op.
  virtual void on_session_paused() noexcept {}

  /// The seat is back with `new_dev` (wrapping a fresh fd). The source
  /// must drop every GEM handle / FB ID bound to the old fd (use
  /// forget()-style paths — ioctls against the dead fd will fail) and
  /// re-allocate equivalent buffers on `new_dev`. Buffer dimensions
  /// and formats are preserved; callers rely on format() returning the
  /// same SourceFormat post-resume.
  virtual drm::expected<void, std::error_code> on_session_resumed(const drm::Device& /*new_dev*/) {
    return {};
  }
};

}  // namespace drm::scene