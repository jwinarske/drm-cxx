// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// cursor/renderer.hpp — KMS-side cursor driver.
//
// Renderer owns every fd-bound resource for displaying a Cursor on a
// specific CRTC:
//   - one dumb buffer + mmap (resized to fit the largest frame of the
//     currently-assigned Cursor, at least DRM_CAP_CURSOR_* for the HW
//     path);
//   - one FB_ID (for the atomic overlay path; legacy HW cursor uses a
//     raw GEM handle);
//   - a cached drm::PropertyStore for the chosen plane so move_to()
//     doesn't re-query property ids on every event;
//   - chosen PlanePath, plane_id, rotation state, animation bookkeeping.
//
// Plane selection on create():
//   - Scans drm::planes::PlaneRegistry for the requested CRTC in this
//     order: DRM_PLANE_TYPE_CURSOR → DRM_PLANE_TYPE_OVERLAY with
//     ARGB8888+LINEAR support → legacy drmModeSetCursor (if the
//     config allows it).
//   - RendererConfig::forced_plane_id bypasses the scan — pass a
//     specific plane id and create() fails if it can't carry
//     ARGB8888+LINEAR on this CRTC.
//
// Commit ownership is parameter-driven:
//   - move_to() and tick() build the atomic request internally and
//     commit. First commit after create() / resume sets
//     ALLOW_MODESET; subsequent commits are non-blocking. This is
//     the right default for compositors that don't need to batch
//     the cursor with anything else.
//   - stage() writes the cursor's plane state into a caller-supplied
//     AtomicRequest and returns without committing. The caller owns
//     the commit, including the ALLOW_MODESET decision (Renderer
//     reports it through the first_commit out-param). Use this when
//     the cursor needs to ride the same atomic commit as the
//     compositor's own plane updates.
//
// Session lifecycle:
//   - Renderer does not hold a Device reference across calls. create()
//     snapshots the fd it needs; on_session_resumed(int new_fd)
//     replaces the snapshot and rebuilds every fd-bound resource.
//     This avoids the aliasing hazard of keeping Device& through a
//     VT-switch handoff (old Device may have been destructed and
//     replaced while we weren't looking).
//   - on_session_paused() stops drawing but keeps the Cursor pointer
//     and the animation start time so resume is seamless.
//
// Multi-CRTC sharing:
//   - Renderer holds its Cursor as shared_ptr<const Cursor>, so a
//     compositor with N heads can load once and hand the same
//     shared_ptr to every per-CRTC Renderer instead of loading the
//     same shape N times. Use set_cursor(std::shared_ptr<const Cursor>)
//     for that path. The single-owner entry point set_cursor(Cursor)
//     still works — it wraps the moved-in Cursor internally and is
//     the right choice for compositors that don't need sharing.
//   - The shared Cursor is immutable (all accessors are const) so
//     concurrent reads are safe even across threads. Renderer itself
//     is not thread-safe — one Renderer per thread if you're driving
//     CRTCs from multiple threads.
//
// Rotation:
//   - Atomic planes that expose the "rotation" property *and* list
//     the requested value in its bitmask rotate at scanout —
//     Renderer writes the current value on every commit and the
//     kernel handles it for free.
//   - Atomic planes without the property, or with the property but
//     with the requested value missing from its supported bitmask
//     (i915 cursor: only rotate-0/180 in the enum, so 90/270 EINVAL
//     atomic_check), fall back to software pre-rotation: blit_frame
//     lays the pixels into the dumb buffer already rotated, the
//     hotspot math rotates with them, and the plane commit ships
//     ROTATE_0. Cost is one extra per-pixel remap inside blit_frame
//     (trivial at cursor sizes) plus a re-blit on set_rotation() so
//     the buffer matches the new orientation. The HW↔SW boundary is
//     re-evaluated per set_rotation(), so a single Renderer can sit
//     in HW for k0/k180 and SW for k90/k270 on the same i915 plane.
//   - Legacy drmModeSetCursor has no rotation channel — non-k0 is
//     rejected by create() and set_rotation() on that path.
//   - has_hardware_rotation() reports whether the *current* rotation
//     is HW-driven, so example/diagnostic callers can label each
//     value as "hardware" vs "software blit".
//
// Virtualized-plane hotspot hinting:
//   - When the chosen plane exposes the HOTSPOT_X / HOTSPOT_Y
//     properties (virtio-gpu, vmwgfx, and other virtualized display
//     drivers) Renderer writes the buffer-local hotspot coordinates
//     alongside CRTC_X / CRTC_Y on every commit. The host VMM reads
//     these to align its native mouse cursor with the guest's
//     logical pointer tip — without them, the host and guest agree
//     on where the sprite is drawn but disagree on where the tip is,
//     producing ~xhot pixels of offset between click and target.
//     Hotspot math on CRTC_X / CRTC_Y is unchanged (the sprite's
//     top-left corner still goes to `requested - hotspot`); HOTSPOT
//     is purely informational to the host. Bare-metal planes don't
//     expose these properties and the write is silently skipped.

#pragma once

#include "detail/expected.hpp"
#include "time/clock.hpp"

#include <cstdint>
#include <memory>
#include <system_error>

namespace drm {
class AtomicRequest;
class Device;
}  // namespace drm

namespace drm::cursor {

class Cursor;

/// Which pipeline the Renderer ended up using. Surfaced for logging —
/// compositors that care about cursor-via-atomic-CURSOR-plane (most
/// modern drivers) vs cursor-via-overlay (older or limited-plane
/// hardware) vs legacy drmModeSetCursor can report which path is live.
enum class PlanePath : std::uint8_t {
  kAtomicCursor,
  kAtomicOverlay,
  kLegacy,
};

/// Display rotation applied to the cursor sprite. If the chosen plane
/// exposes the `rotation` property Renderer drives it in hardware;
/// otherwise pixels are pre-rotated at blit time. Reflection flags
/// are deferred until a driver asks for them.
enum class Rotation : std::uint8_t {
  k0,
  k90,
  k180,
  k270,
};

/// Settings for Renderer::create(). Only crtc_id is required — every
/// other field has a defensible default. Adding fields here is the
/// forward-compatibility path; fields are documented individually so
/// callers know which defaults they're getting.
struct RendererConfig {
  /// The CRTC the cursor is attached to. Must be a valid CRTC id on
  /// the passed Device at create() time. One Renderer per CRTC.
  std::uint32_t crtc_id = 0;

  /// Logical pixel size of the cursor. 0 = use DRM_CAP_CURSOR_WIDTH
  /// for the HW/overlay path (the driver's natively-supported size)
  /// or 64 for the legacy path. Callers computing per-CRTC DPI pass
  /// a scaled value here.
  std::uint32_t preferred_size = 0;

  /// Override plane selection. 0 = Renderer picks via PlaneRegistry.
  /// Non-zero = Renderer uses this plane id and fails create() if
  /// the plane can't carry ARGB8888+LINEAR on this CRTC.
  std::uint32_t forced_plane_id = 0;

  /// Permit falling through to drmModeSetCursor when no atomic-capable
  /// plane can serve this CRTC. Disable in compositors that must
  /// commit the cursor atomically with other plane updates (legacy
  /// calls won't coalesce with an AtomicRequest).
  bool allow_legacy = true;

  /// Cursor orientation at create() time. Can be changed later via
  /// set_rotation(); stored here so create() knows whether to
  /// configure a hardware rotation property up front or commit to
  /// pre-rotating in software.
  Rotation rotation = Rotation::k0;

  /// Clock source for animation. nullptr = drm::default_clock().
  /// Injected for tests (freeze / step) or to sync animation with a
  /// compositor's presentation clock.
  drm::Clock* clock = nullptr;
};

class Renderer {
 public:
  /// Construct a Renderer for the given CRTC. Allocates the cursor
  /// buffer and (if the atomic path is chosen) adds an FB and caches
  /// plane properties. No Cursor is bound yet — call set_cursor()
  /// before move_to() / tick() / stage().
  ///
  /// The Device reference is used only during this call; Renderer
  /// snapshots `dev.fd()` and does not hold the Device afterwards.
  [[nodiscard]] static drm::expected<Renderer, std::error_code> create(Device& dev,
                                                                       const RendererConfig& cfg);

  Renderer(Renderer&&) noexcept;
  Renderer& operator=(Renderer&&) noexcept;
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  ~Renderer();

  /// Bind a Cursor. Renderer takes ownership — callers hand over with
  /// `std::move(c)` or directly from `Cursor::load(...).value()`.
  /// Shape-cycling callers keep their own cache and move one in per
  /// swap. Re-blits the first frame, resets the animation start time,
  /// and marks the next commit as needing to re-upload. Internally
  /// the moved-in Cursor is wrapped in a shared_ptr so sharing with
  /// other Renderers remains possible via current_cursor().
  [[nodiscard]] drm::expected<void, std::error_code> set_cursor(Cursor cursor);

  /// Bind a shared Cursor. Use this in multi-CRTC compositors to load
  /// a shape once (`std::make_shared<Cursor>(Cursor::load(...).value())`)
  /// and hand the same shared_ptr to every per-CRTC Renderer, instead
  /// of parsing the XCursor file and allocating the pixel buffer N
  /// times. Returns std::errc::invalid_argument if `cursor` is null.
  [[nodiscard]] drm::expected<void, std::error_code> set_cursor(
      std::shared_ptr<const Cursor> cursor);

  /// Snapshot of the currently bound Cursor, or nullptr if none. Lets
  /// a compositor that started with set_cursor(Cursor) still propagate
  /// the shape to a late-joining Renderer without re-parsing the
  /// XCursor file.
  [[nodiscard]] std::shared_ptr<const Cursor> current_cursor() const noexcept;

  // --- Self-commit mode ---------------------------------------------

  /// Set the cursor position in CRTC coordinates. Renderer subtracts
  /// the current frame's hotspot and any centering offset internally
  /// before committing.
  [[nodiscard]] drm::expected<void, std::error_code> move_to(int crtc_x, int crtc_y);

  /// Drive animation. Call every main-loop tick. Samples the
  /// configured Clock, advances the frame if it's time, re-blits +
  /// re-uploads on a frame change, and does nothing otherwise.
  /// Returns true when a re-upload actually happened (lets the
  /// caller's own commit coalescer skip if it can).
  [[nodiscard]] drm::expected<bool, std::error_code> tick();

  /// As tick(), but uses `now` instead of sampling the Clock. Pass the
  /// same time_point to every Renderer in a multi-CRTC compositor so
  /// all cursors pick the same animation frame even when the calls
  /// are microseconds apart.
  [[nodiscard]] drm::expected<bool, std::error_code> tick(drm::Clock::time_point now);

  // --- Caller-commit mode -------------------------------------------

  /// Append cursor plane state to `req`; do not commit. Advances the
  /// animation clock and may re-blit the backing pixels as a side
  /// effect. `first_commit` is written to true when the caller must
  /// OR DRM_MODE_ATOMIC_ALLOW_MODESET into its commit flags — true
  /// on the first call after create() and after each
  /// on_session_resumed(). Returns true if the pixels were re-blit
  /// this call (otherwise the caller can skip re-committing if
  /// nothing else changed).
  [[nodiscard]] drm::expected<bool, std::error_code> stage(AtomicRequest& req, int crtc_x,
                                                           int crtc_y, bool& first_commit);

  /// As stage(), but uses `now` instead of sampling the Clock. Same
  /// use case as tick(time_point): multi-CRTC lockstep animation.
  [[nodiscard]] drm::expected<bool, std::error_code> stage(AtomicRequest& req, int crtc_x,
                                                           int crtc_y, bool& first_commit,
                                                           drm::Clock::time_point now);

  // --- Show / hide --------------------------------------------------

  /// Detach the cursor from its plane without destroying state. The
  /// dumb buffer, FB_ID, and Cursor binding all survive; show()
  /// re-attaches with the current frame.
  [[nodiscard]] drm::expected<void, std::error_code> hide();

  /// Re-attach after hide(). No-op if already visible.
  [[nodiscard]] drm::expected<void, std::error_code> show();

  // --- Session hooks ------------------------------------------------

  /// The seat has been disabled. Stop drawing (any atomic commit
  /// would fail — we don't own master) and remember we're paused.
  /// Buffers are left allocated; they die with the fd at the kernel
  /// level but no ioctls are attempted.
  void on_session_paused() noexcept;

  /// The seat is back with a fresh fd. Snapshot the new fd, rebuild
  /// dumb buffer / FB_ID / property cache, re-blit the currently
  /// bound Cursor's current animation frame, re-install it (atomic:
  /// first commit has ALLOW_MODESET; legacy: drmModeSetCursor).
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(int new_fd);

  // --- Introspection ------------------------------------------------

  [[nodiscard]] PlanePath path() const noexcept;
  [[nodiscard]] std::uint32_t plane_id() const noexcept;
  [[nodiscard]] Rotation rotation() const noexcept;

  /// Change rotation after create(). Returns std::errc::not_supported
  /// on the legacy path (drmModeSetCursor has no rotation knob);
  /// atomic planes accept any value and route through hardware if the
  /// plane exposes the "rotation" property, or through software
  /// pre-rotation in blit_frame if it doesn't. On success, the new
  /// value takes effect immediately when a Cursor is bound + visible
  /// + not paused; otherwise the stored value is picked up on the
  /// next commit after set_cursor() / show() / on_session_resumed().
  [[nodiscard]] drm::expected<void, std::error_code> set_rotation(Rotation rotation);

  /// True when the *current* rotation is being driven by the kernel
  /// at scanout — i.e. the plane exposes the "rotation" property and
  /// the current value is in the property's supported bitmask. Returns
  /// false on the legacy path, on atomic planes without the property,
  /// and (notably) on planes that expose the property but list only a
  /// subset of values: i915's cursor plane, for example, advertises
  /// rotate-0 / rotate-180 only, so this returns true at k0/k180 and
  /// false at k90/k270 — those run through blit_frame's software
  /// pre-rotation path. Re-evaluated after every set_rotation().
  [[nodiscard]] bool has_hardware_rotation() const noexcept;

  /// True when the selected plane exposes the HOTSPOT_X / HOTSPOT_Y
  /// properties — typically only virtualized display drivers do.
  /// Compositors running in a VM can check this to confirm the host
  /// will see the guest's cursor tip alignment; bare-metal compositors
  /// can ignore it. Always false on the legacy drmModeSetCursor path.
  [[nodiscard]] bool has_hotspot_properties() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit Renderer(std::unique_ptr<Impl> impl);
};

}  // namespace drm::cursor