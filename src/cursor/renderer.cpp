// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "renderer.hpp"

#include "../core/device.hpp"
#include "../core/property_store.hpp"
#include "../core/resources.hpp"
#include "../dumb/buffer.hpp"
#include "../modeset/atomic.hpp"
#include "../planes/plane_registry.hpp"
#include "../time/clock.hpp"
#include "cursor.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

namespace drm::cursor {

namespace {

// Legacy drmModeSetCursor paths are hard-wired to 64x64 on every driver
// the kernel has shipped since 2.6 — there's no capability query for
// it, just a baked-in assumption.
constexpr std::uint32_t k_legacy_cursor_size = 64;

// Find the index of `crtc_id` in the CRTC array. PlaneRegistry's
// possible_crtcs bitmask is indexed (not id-keyed), so we translate
// once at create() time and cache the result.
std::optional<std::uint32_t> crtc_index_for_id(int fd, std::uint32_t crtc_id) {
  const auto res = drm::get_resources(fd);
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

// Snapshot the current CRTC binding of a plane so we can prefer free
// planes over already-assigned ones. Returns 0 if the plane is free
// or the query failed (the caller treats 0 as "free enough").
std::uint32_t plane_current_crtc(int fd, std::uint32_t plane_id) {
  auto* p = drmModeGetPlane(fd, plane_id);
  if (p == nullptr) {
    return 0;
  }
  const std::uint32_t current = p->crtc_id;
  drmModeFreePlane(p);
  return current;
}

// Translate the public Rotation enum into the DRM_MODE_ROTATE_* bitmask
// used by the plane's rotation property. Defined here (not in the
// plane-selection anon namespace further down) so stage_position() —
// which now writes the rotation prop on every commit — can see it.
std::uint64_t rotation_to_drm_mask(Rotation r) {
  switch (r) {
    case Rotation::k0:
      return DRM_MODE_ROTATE_0;
    case Rotation::k90:
      return DRM_MODE_ROTATE_90;
    case Rotation::k180:
      return DRM_MODE_ROTATE_180;
    case Rotation::k270:
      return DRM_MODE_ROTATE_270;
  }
  return DRM_MODE_ROTATE_0;
}

}  // namespace

struct Renderer::Impl {
  // --- fd + config snapshot -----------------------------------------
  // drm_fd is a non-owning snapshot: the caller's Device (or the
  // Seat-managed fd it wraps) owns the lifetime. Replaced by
  // on_session_resumed(new_fd).
  int drm_fd{-1};
  std::uint32_t crtc_id{0};
  std::uint32_t crtc_index{0};
  std::uint32_t plane_id{0};  // 0 in legacy path
  std::uint32_t forced_plane_id{0};
  std::uint32_t preferred_size{0};
  PlanePath path{PlanePath::kLegacy};
  Rotation rotation{Rotation::k0};
  bool allow_legacy{true};
  drm::Clock* clock{nullptr};

  // --- dumb buffer --------------------------------------------------
  // Backed by drm::dumb::Buffer. buffer.fb_id() is 0 on the legacy path (we tell
  // the factory to skip drmModeAddFB2 in that case). Linear ARGB8888
  // in all cases; stride is driver-chosen (typically width * 4 but may
  // be padded for alignment — always consult buffer.stride()).
  drm::dumb::Buffer buffer;

  [[nodiscard]] std::uint32_t* mapped32() noexcept {
    return reinterpret_cast<std::uint32_t*>(
        buffer.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  }
  [[nodiscard]] const std::uint32_t* mapped32() const noexcept {
    return reinterpret_cast<const std::uint32_t*>(
        buffer.data());  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
  }

  // --- cached plane props (atomic paths only) -----------------------
  drm::PropertyStore props;

  // HOTSPOT_X / HOTSPOT_Y are optional (virtualized drivers only). We
  // resolve their property ids once at create + session-resume time so
  // the per-event stage_position doesn't pay for a lookup that is
  // almost always going to miss on bare metal. nullopt = the plane
  // does not expose this property at all.
  std::optional<std::uint32_t> hotspot_x_prop;
  std::optional<std::uint32_t> hotspot_y_prop;

  // "rotation" is optional on the plane: planes that don't expose it
  // can only run Rotation::k0. Cached alongside the hotspot ids so
  // set_rotation() can validate without a property_id scan on the
  // set path, and stage_position() can write the current value as
  // part of its normal property list instead of a separate setup
  // commit. Resurveyed at every session resume because property ids
  // can change when the DRM fd is replaced.
  std::optional<std::uint32_t> rotation_prop;

  // --- cursor binding + animation -----------------------------------
  // shared_ptr so multi-CRTC compositors can load once and hand the
  // same Cursor to every per-head Renderer. Const-qualified because
  // Cursor is immutable after load; the shared_ptr owns a const object
  // so concurrent reads across threads are safe without any locking
  // on the Cursor itself. last_frame points into the shared Cursor's
  // pixel storage — safe because that storage is allocated once at
  // Cursor::load time and never resized, and this Renderer keeps a
  // strong reference for as long as last_frame is in use.
  std::shared_ptr<const Cursor> cursor;
  drm::Clock::time_point anim_start;
  const Frame* last_frame{nullptr};

  // --- commit / visibility state ------------------------------------
  // first_commit_needed starts true; the first successful commit
  // clears it. on_session_resumed() re-sets it so the post-resume
  // commit re-arms every plane property + ALLOW_MODESET.
  bool first_commit_needed{true};
  bool visible{true};
  bool paused{false};
  int last_x{0};
  int last_y{0};

  // --- buffer + resource management ---------------------------------
  drm::expected<void, std::error_code> alloc_buffer(std::uint32_t w, std::uint32_t h);
  void free_buffer();

  // --- rendering -----------------------------------------------------
  void blit_frame(const Frame& f);

  // Advance animation to `now`. Returns true iff the selected frame
  // differs from last_frame (caller should re-upload / re-commit).
  bool advance(drm::Clock::time_point now);

  // --- position commit (self-commit mode) ---------------------------
  drm::expected<void, std::error_code> commit_position(int crtc_x, int crtc_y);

  // --- position staging (caller-commit mode) ------------------------
  drm::expected<void, std::error_code> stage_position(drm::AtomicRequest& req, int crtc_x,
                                                      int crtc_y) const;

  // Probe the just-cached plane properties for the optional fields
  // the Renderer tracks per-Impl — currently HOTSPOT_X / HOTSPOT_Y
  // (virtualized drivers only) and rotation (hardware-specific).
  // Each `*_prop` slot becomes nullopt if the plane does not expose
  // that property. Called from create() and on_session_resumed()
  // right after props.cache_properties().
  void probe_plane_properties() noexcept;

  // Translate a CRTC coordinate to the plane-destination coordinate,
  // accounting for the current frame's hotspot and the centering
  // offset when the frame is smaller than the backing buffer.
  [[nodiscard]] std::pair<int, int> hotspot_adjust(int crtc_x, int crtc_y) const;

  // Rotation applied in software by blit_frame. Returns Rotation::k0
  // when either (a) the user requested k0 or (b) the plane exposes
  // the hardware rotation property — in both cases blit_frame lays
  // pixels out in source orientation and rotation_prop (if present)
  // does the actual rotation at scanout. Non-k0 is returned only
  // when the plane can't rotate in hardware and we have to pre-rotate
  // the pixel buffer ourselves.
  [[nodiscard]] Rotation effective_sw_rotation() const noexcept;

  // Hotspot coordinate expressed in the backing buffer's own
  // coordinate space, accounting for both the centering offset and
  // any software rotation applied in blit_frame. Used by
  // hotspot_adjust (to translate the CRTC position into the plane's
  // top-left corner) and by stage_position (to write HOTSPOT_X /
  // HOTSPOT_Y for virtualized driver planes). Returns {0, 0} when
  // no frame has been blit yet.
  struct BufferHotspot {
    int x{0};
    int y{0};
  };
  [[nodiscard]] BufferHotspot buffer_hotspot() const noexcept;
};

// ---------------------------------------------------------------------------
// Impl: buffer lifecycle
// ---------------------------------------------------------------------------

drm::expected<void, std::error_code> Renderer::Impl::alloc_buffer(std::uint32_t w,
                                                                  std::uint32_t h) {
  // Legacy drmModeSetCursor takes a raw GEM handle and never a KMS FB
  // ID, so we skip drmModeAddFB2 in that path — the allocation is
  // otherwise identical. Positional init keeps this C++17-pedantic
  // clean (designated initializers are C++20).
  drm::dumb::Config cfg;
  cfg.width = w;
  cfg.height = h;
  cfg.drm_format = DRM_FORMAT_ARGB8888;
  cfg.bpp = 32;
  cfg.add_fb = (path != PlanePath::kLegacy);
  auto dev = drm::Device::from_fd(drm_fd);
  auto r = drm::dumb::Buffer::create(dev, cfg);
  if (!r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  buffer = std::move(*r);
  // drm::dumb::Buffer already zero-fills the mapping during create().
  return {};
}

void Renderer::Impl::free_buffer() {
  buffer = drm::dumb::Buffer{};
}

// ---------------------------------------------------------------------------
// Impl: rendering
// ---------------------------------------------------------------------------

void Renderer::Impl::blit_frame(const Frame& f) {
  if (buffer.empty()) {
    return;
  }
  auto* const dst = mapped32();
  const auto stride_px = static_cast<std::size_t>(buffer.stride() / 4);

  // Wipe the full buffer — a smaller-than-buffer frame must not leak
  // stale pixels around its edges (prior frames in an animation or a
  // different shape from a shape-cycle call). Rotation changes also
  // rely on this wipe: the post-rotation sprite may not overlap the
  // pre-rotation sprite's footprint, and any stale pixels outside
  // the new footprint would otherwise ghost on screen.
  for (std::size_t y = 0; y < buffer.height(); ++y) {
    std::memset(dst + (y * stride_px), 0, static_cast<std::size_t>(buffer.width()) * 4);
  }

  // Rotated sprite dimensions: k90 and k270 swap width and height,
  // k0 and k180 keep them. `last_frame` is set before we return so
  // buffer_hotspot() can see the current frame.
  const Rotation rot = effective_sw_rotation();
  const bool swap_dims = (rot == Rotation::k90 || rot == Rotation::k270);
  const std::uint32_t rot_w = swap_dims ? f.height : f.width;
  const std::uint32_t rot_h = swap_dims ? f.width : f.height;

  const std::uint32_t w = std::min(rot_w, buffer.width());
  const std::uint32_t h = std::min(rot_h, buffer.height());
  const std::size_t x_off = (buffer.width() > rot_w) ? (buffer.width() - rot_w) / 2 : 0;
  const std::size_t y_off = (buffer.height() > rot_h) ? (buffer.height() - rot_h) / 2 : 0;

  if (rot == Rotation::k0) {
    // Fast path: full-row memcpy when no pre-rotation is needed.
    for (std::size_t y = 0; y < h; ++y) {
      const std::size_t src_offset = y * static_cast<std::size_t>(f.width);
      std::memcpy(dst + ((y + y_off) * stride_px) + x_off, f.pixels.data() + src_offset,
                  static_cast<std::size_t>(w) * 4);
    }
  } else {
    // Software pre-rotation — per-pixel remap. Each destination pixel
    // pulls from the source via the inverse rotation so pixel reads
    // and writes go through the fast (sequential-write) direction.
    // Cost is trivially low at cursor sizes: 64x64 = 4 K iterations,
    // each a handful of integer ops plus one 32-bit load+store.
    //
    // Inverse-rotation formulas (clockwise naming):
    //   k90 fwd (sx,sy) → (H-1-sy, sx); inverse dst(dx,dy) → src(dy, H-1-dx)
    //   k180 fwd (sx,sy) → (W-1-sx, H-1-sy); inverse (W-1-dx, H-1-dy)
    //   k270 fwd (sx,sy) → (sy, W-1-sx); inverse dst(dx,dy) → src(W-1-dy, dx)
    const std::size_t src_w = f.width;
    const std::size_t src_h = f.height;
    for (std::size_t dy = 0; dy < h; ++dy) {
      for (std::size_t dx = 0; dx < w; ++dx) {
        std::size_t sx = 0;
        std::size_t sy = 0;
        switch (rot) {
          case Rotation::k90:
            sx = dy;
            sy = src_h - 1 - dx;
            break;
          case Rotation::k180:
            sx = src_w - 1 - dx;
            sy = src_h - 1 - dy;
            break;
          case Rotation::k270:
            sx = src_w - 1 - dy;
            sy = dx;
            break;
          case Rotation::k0:
            // Handled by the fast path above — unreachable here.
            break;
        }
        dst[((dy + y_off) * stride_px) + (dx + x_off)] = f.pixels[(sy * src_w) + sx];
      }
    }
  }

  last_frame = &f;
}

bool Renderer::Impl::advance(drm::Clock::time_point now) {
  if (!cursor || !cursor->animated()) {
    return false;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - anim_start);
  const Frame& f = cursor->frame_at(elapsed);
  if (&f == last_frame) {
    return false;
  }
  blit_frame(f);
  return true;
}

std::pair<int, int> Renderer::Impl::hotspot_adjust(int crtc_x, int crtc_y) const {
  if (last_frame == nullptr) {
    return {crtc_x, crtc_y};
  }
  const auto h = buffer_hotspot();
  return {crtc_x - h.x, crtc_y - h.y};
}

Rotation Renderer::Impl::effective_sw_rotation() const noexcept {
  // Legacy path doesn't support any rotation at all; set_rotation and
  // create() both reject non-k0 up front, so this is defensive.
  if (path == PlanePath::kLegacy) {
    return Rotation::k0;
  }
  // Hardware rotation property present → the kernel handles it at
  // scanout; blit_frame writes pixels in source orientation.
  if (rotation_prop) {
    return Rotation::k0;
  }
  // Atomic plane without the rotation property → pre-rotate in software.
  return rotation;
}

Renderer::Impl::BufferHotspot Renderer::Impl::buffer_hotspot() const noexcept {
  if (last_frame == nullptr) {
    return {};
  }

  // Apply the same rotation to the hotspot that blit_frame applied to
  // the pixels. For hardware rotation (effective_sw_rotation == k0 even
  // though impl_->rotation != k0) we still ship the source-orientation
  // hotspot: the kernel rotates both the pixels and the hotspot together
  // via the rotation property, so what HOTSPOT_X/Y sees is pre-rotation.
  const Rotation rot = effective_sw_rotation();
  const int fw = static_cast<int>(last_frame->width);
  const int fh = static_cast<int>(last_frame->height);
  int rx = last_frame->xhot;
  int ry = last_frame->yhot;
  std::uint32_t rw = last_frame->width;
  std::uint32_t rh = last_frame->height;
  switch (rot) {
    case Rotation::k0:
      break;
    case Rotation::k90:
      rx = fh - 1 - last_frame->yhot;
      ry = last_frame->xhot;
      rw = last_frame->height;
      rh = last_frame->width;
      break;
    case Rotation::k180:
      rx = fw - 1 - last_frame->xhot;
      ry = fh - 1 - last_frame->yhot;
      break;
    case Rotation::k270:
      rx = last_frame->yhot;
      ry = fw - 1 - last_frame->xhot;
      rw = last_frame->height;
      rh = last_frame->width;
      break;
  }
  const int x_off = (buffer.width() > rw) ? static_cast<int>((buffer.width() - rw) / 2) : 0;
  const int y_off = (buffer.height() > rh) ? static_cast<int>((buffer.height() - rh) / 2) : 0;
  return {rx + x_off, ry + y_off};
}

void Renderer::Impl::probe_plane_properties() noexcept {
  hotspot_x_prop.reset();
  hotspot_y_prop.reset();
  rotation_prop.reset();
  if (path == PlanePath::kLegacy) {
    return;
  }
  if (auto id = props.property_id(plane_id, "HOTSPOT_X")) {
    hotspot_x_prop = *id;
  }
  if (auto id = props.property_id(plane_id, "HOTSPOT_Y")) {
    hotspot_y_prop = *id;
  }
  if (auto id = props.property_id(plane_id, "rotation")) {
    rotation_prop = *id;
  }
}

// ---------------------------------------------------------------------------
// Impl: commit + stage
// ---------------------------------------------------------------------------

drm::expected<void, std::error_code> Renderer::Impl::stage_position(drm::AtomicRequest& req,
                                                                    int crtc_x, int crtc_y) const {
  const auto [px, py] = hotspot_adjust(crtc_x, crtc_y);

  auto add = [&](const char* name, std::uint64_t val) -> drm::expected<void, std::error_code> {
    auto prop_id = props.property_id(plane_id, name);
    if (!prop_id) {
      return drm::unexpected<std::error_code>(prop_id.error());
    }
    return req.add_property(plane_id, *prop_id, val);
  };

  // Full plane state every commit: drivers accept property-subset
  // commits, but a cursor plane that toggled off during a hide() needs
  // every field re-armed to go visible again, and it's cheaper to just
  // always write them than to track a dirty bitmap.
  if (auto r = add("FB_ID", buffer.fb_id()); !r) {
    return r;
  }
  if (auto r = add("CRTC_ID", crtc_id); !r) {
    return r;
  }
  if (auto r = add("CRTC_X", static_cast<std::uint64_t>(px)); !r) {
    return r;
  }
  if (auto r = add("CRTC_Y", static_cast<std::uint64_t>(py)); !r) {
    return r;
  }
  if (auto r = add("CRTC_W", buffer.width()); !r) {
    return r;
  }
  if (auto r = add("CRTC_H", buffer.height()); !r) {
    return r;
  }
  if (auto r = add("SRC_X", 0); !r) {
    return r;
  }
  if (auto r = add("SRC_Y", 0); !r) {
    return r;
  }
  // Source rect is in 16.16 fixed-point; CRTC rect is plain pixels.
  if (auto r = add("SRC_W", static_cast<std::uint64_t>(buffer.width()) << 16U); !r) {
    return r;
  }
  if (auto r = add("SRC_H", static_cast<std::uint64_t>(buffer.height()) << 16U); !r) {
    return r;
  }

  // rotation — optional (some planes don't expose it). Written on
  // every commit so the value is re-armed on the first post-resume
  // commit (fresh fd lost the prior kernel state) and so set_rotation
  // doesn't need its own commit path. Validated at create time: a
  // plane without this property can only run Rotation::k0.
  if (rotation_prop) {
    if (auto r = req.add_property(plane_id, *rotation_prop, rotation_to_drm_mask(rotation)); !r) {
      return r;
    }
  }

  // HOTSPOT_X / HOTSPOT_Y — virtualized-driver hint. Only written if
  // the plane exposes the properties (probed once at create/resume).
  // Pre-blit (last_frame == nullptr) we skip the write — there's no
  // meaningful hotspot yet and we'd rather leave the prior value
  // than clobber it with a stale zero. buffer_hotspot() handles the
  // centering offset and, when we're pre-rotating in software, also
  // accounts for rotation — so HOTSPOT_X/Y always describes where the
  // tip lives in the buffer as it is actually pixel-blit.
  if (last_frame != nullptr) {
    const auto h = buffer_hotspot();
    // xhot/yhot are int in the XCursor format but are always >= 0 in
    // practice (libxcursor surfaces them as XCursorDim, unsigned).
    // Clamp defensively so a malformed file can't overflow the cast.
    const std::uint64_t hx = static_cast<std::uint64_t>(std::max(0, h.x));
    const std::uint64_t hy = static_cast<std::uint64_t>(std::max(0, h.y));
    if (hotspot_x_prop) {
      if (auto r = req.add_property(plane_id, *hotspot_x_prop, hx); !r) {
        return r;
      }
    }
    if (hotspot_y_prop) {
      if (auto r = req.add_property(plane_id, *hotspot_y_prop, hy); !r) {
        return r;
      }
    }
  }
  return {};
}

drm::expected<void, std::error_code> Renderer::Impl::commit_position(int crtc_x, int crtc_y) {
  if (path == PlanePath::kLegacy) {
    const auto [px, py] = hotspot_adjust(crtc_x, crtc_y);
    // First legacy install: point the CRTC at our GEM handle.
    // Subsequent moves are cheap drmModeMoveCursor calls.
    if (first_commit_needed) {
      if (drmModeSetCursor(drm_fd, crtc_id, buffer.handle(), buffer.width(), buffer.height()) !=
          0) {
        return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
      }
      first_commit_needed = false;
    }
    if (drmModeMoveCursor(drm_fd, crtc_id, px, py) != 0) {
      return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
    }
    return {};
  }

  // Atomic paths build a one-plane request and commit it.
  const drm::Device dev = drm::Device::from_fd(drm_fd);
  drm::AtomicRequest req(dev);
  if (!req.valid()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
  }
  if (auto r = stage_position(req, crtc_x, crtc_y); !r) {
    return r;
  }
  // First commit needs ALLOW_MODESET since we're enabling the plane;
  // later commits are plain flips. Blocking, because non-blocking
  // cursor commits contend with vblank pacing and EBUSY rapidly when
  // mouse events outpace the refresh rate.
  const std::uint32_t flags = first_commit_needed ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0U;
  if (auto r = req.commit(flags); !r) {
    return r;
  }
  first_commit_needed = false;
  return {};
}

// ---------------------------------------------------------------------------
// Plane selection + rotation setup
// ---------------------------------------------------------------------------

namespace {

struct SelectedPlane {
  std::uint32_t plane_id{0};
  PlanePath path{PlanePath::kLegacy};
  std::uint32_t cursor_max_w{0};
  std::uint32_t cursor_max_h{0};
};

bool plane_supports_argb8888(const drm::planes::PlaneCapabilities& cap) {
  return cap.supports_format(DRM_FORMAT_ARGB8888);
}

drm::expected<SelectedPlane, std::error_code> select_plane(const drm::Device& dev,
                                                           std::uint32_t crtc_index,
                                                           std::uint32_t forced_plane_id,
                                                           bool allow_legacy) {
  auto registry = drm::planes::PlaneRegistry::enumerate(dev);
  if (!registry) {
    return drm::unexpected<std::error_code>(registry.error());
  }

  // forced_plane_id bypasses the scan entirely — validate the plane
  // exists and carries ARGB8888 on this CRTC, then take it on trust.
  if (forced_plane_id != 0) {
    for (const auto& cap : registry->all()) {
      if (cap.id != forced_plane_id) {
        continue;
      }
      if (!cap.compatible_with_crtc(crtc_index) || !plane_supports_argb8888(cap)) {
        return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
      }
      const auto path = cap.type == drm::planes::DRMPlaneType::CURSOR ? PlanePath::kAtomicCursor
                                                                      : PlanePath::kAtomicOverlay;
      return SelectedPlane{cap.id, path, cap.cursor_max_w, cap.cursor_max_h};
    }
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
  }

  const auto candidates = registry->for_crtc(crtc_index);

  // First choice: a CURSOR plane with ARGB8888. These are per-CRTC
  // hardware scanout units with no z-ordering fuss, so they beat
  // overlay selection every time they exist.
  for (const auto* cap : candidates) {
    if (cap->type == drm::planes::DRMPlaneType::CURSOR && plane_supports_argb8888(*cap)) {
      return SelectedPlane{cap->id, PlanePath::kAtomicCursor, cap->cursor_max_w, cap->cursor_max_h};
    }
  }

  // Second choice: an OVERLAY plane with ARGB8888, preferring one not
  // already bound to another CRTC. Some platforms only expose a
  // handful of overlays; grabbing a free one matters when the
  // compositor wants the rest for layers.
  const drm::planes::PlaneCapabilities* chosen_overlay = nullptr;
  for (const auto* cap : candidates) {
    if (cap->type != drm::planes::DRMPlaneType::OVERLAY || !plane_supports_argb8888(*cap)) {
      continue;
    }
    if (plane_current_crtc(dev.fd(), cap->id) == 0) {
      chosen_overlay = cap;
      break;
    }
    if (chosen_overlay == nullptr) {
      chosen_overlay = cap;
    }
  }
  if (chosen_overlay != nullptr) {
    return SelectedPlane{chosen_overlay->id, PlanePath::kAtomicOverlay, 0, 0};
  }

  if (allow_legacy) {
    return SelectedPlane{0, PlanePath::kLegacy, 0, 0};
  }

  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
}

}  // namespace

// ---------------------------------------------------------------------------
// create()
// ---------------------------------------------------------------------------

drm::expected<Renderer, std::error_code> Renderer::create(Device& dev, const RendererConfig& cfg) {
  if (cfg.crtc_id == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  auto idx = crtc_index_for_id(dev.fd(), cfg.crtc_id);
  if (!idx) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
  }

  auto selected = select_plane(dev, *idx, cfg.forced_plane_id, cfg.allow_legacy);
  if (!selected) {
    return drm::unexpected<std::error_code>(selected.error());
  }

  auto impl = std::make_unique<Impl>();
  impl->drm_fd = dev.fd();
  impl->crtc_id = cfg.crtc_id;
  impl->crtc_index = *idx;
  impl->plane_id = selected->plane_id;
  impl->forced_plane_id = cfg.forced_plane_id;
  impl->preferred_size = cfg.preferred_size;
  impl->path = selected->path;
  impl->rotation = cfg.rotation;
  impl->allow_legacy = cfg.allow_legacy;
  impl->clock = (cfg.clock != nullptr) ? cfg.clock : &drm::default_clock();

  // Buffer size. Atomic paths take the driver's cursor cap (if the
  // caller didn't pin a size) so HW-accelerated CURSOR planes get the
  // dimensions they want. Legacy is always 64 — the old drmModeSetCursor
  // call has no cap query and every driver hard-codes it.
  std::uint32_t w = cfg.preferred_size;
  std::uint32_t h = cfg.preferred_size;
  if (selected->path == PlanePath::kLegacy) {
    if (w == 0) {
      w = k_legacy_cursor_size;
    }
    if (h == 0) {
      h = k_legacy_cursor_size;
    }
  } else {
    if (w == 0) {
      std::uint64_t cap_w = 0;
      drmGetCap(dev.fd(), DRM_CAP_CURSOR_WIDTH, &cap_w);
      w = (cap_w != 0) ? static_cast<std::uint32_t>(cap_w) : k_legacy_cursor_size;
    }
    if (h == 0) {
      std::uint64_t cap_h = 0;
      drmGetCap(dev.fd(), DRM_CAP_CURSOR_HEIGHT, &cap_h);
      h = (cap_h != 0) ? static_cast<std::uint32_t>(cap_h) : k_legacy_cursor_size;
    }
  }

  if (auto r = impl->alloc_buffer(w, h); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }

  // Atomic paths cache plane properties so per-event commits don't
  // re-walk kernel property ids. Also gives us up-front confirmation
  // the plane exposes every field we need.
  if (impl->path != PlanePath::kLegacy) {
    if (auto r = impl->props.cache_properties(dev.fd(), impl->plane_id, DRM_MODE_OBJECT_PLANE);
        !r) {
      impl->free_buffer();
      return drm::unexpected<std::error_code>(r.error());
    }
    impl->probe_plane_properties();

    // Rotation: hardware rotation is driven through the cached
    // rotation_prop by stage_position; planes that don't expose it
    // fall back to software pre-rotation inside blit_frame. Either
    // path handles non-k0 on atomic planes, so no up-front rejection
    // here — only legacy SetCursor, which has no way to apply a
    // rotation at all, still rejects non-k0 below.
  } else if (cfg.rotation != Rotation::k0) {
    // Legacy SetCursor has no rotation knob.
    impl->free_buffer();
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
  }

  return Renderer(std::move(impl));
}

// ---------------------------------------------------------------------------
// Renderer special members + set_cursor
// ---------------------------------------------------------------------------

Renderer::Renderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

Renderer::~Renderer() {
  if (!impl_ || impl_->drm_fd < 0) {
    return;
  }
  // Best-effort teardown: hide the cursor from the CRTC so the next
  // process doesn't inherit our sprite, then release the dumb buffer
  // + FB. We skip both if we're paused (a seat-revoked fd -ENODEVs on
  // every ioctl) or if nothing was ever installed (first_commit_needed
  // still true — atomic FB_ID=0 on a plane the kernel never saw us
  // use can return EINVAL).
  if (!impl_->paused && !impl_->first_commit_needed) {
    if (impl_->path == PlanePath::kLegacy) {
      drmModeSetCursor(impl_->drm_fd, impl_->crtc_id, 0, 0, 0);
    } else {
      const drm::Device tmp_dev = drm::Device::from_fd(impl_->drm_fd);
      drm::AtomicRequest req(tmp_dev);
      if (req.valid()) {
        if (auto fb = impl_->props.property_id(impl_->plane_id, "FB_ID")) {
          (void)req.add_property(impl_->plane_id, *fb, 0);
        }
        if (auto c = impl_->props.property_id(impl_->plane_id, "CRTC_ID")) {
          (void)req.add_property(impl_->plane_id, *c, 0);
        }
        (void)req.commit(0);
      }
    }
  }
  impl_->free_buffer();
}

drm::expected<void, std::error_code> Renderer::set_cursor(Cursor cursor) {
  // The single-owner path wraps the moved-in Cursor in a shared_ptr so
  // the two set_cursor overloads can share one assignment + first-blit
  // implementation. Allocation happens once per set_cursor call, which
  // is a shape-swap path (middle-click, digit-key) rather than a
  // per-event hot path.
  return set_cursor(std::make_shared<Cursor>(std::move(cursor)));
}

drm::expected<void, std::error_code> Renderer::set_cursor(std::shared_ptr<const Cursor> cursor) {
  if (!cursor) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  impl_->cursor = std::move(cursor);
  impl_->anim_start = impl_->clock->now();

  // Blit the first frame eagerly so the next commit has real pixels.
  // last_frame points into the shared Cursor's frame vector, whose
  // storage is stable for the Cursor's lifetime — the shared_ptr we
  // just assigned keeps it alive until the next set_cursor or until
  // this Renderer is destroyed.
  impl_->blit_frame(impl_->cursor->first());
  impl_->first_commit_needed = true;

  // Legacy needs an explicit re-upload of the new buffer contents
  // if we'd already installed it in a prior session — same-handle
  // re-uploads after a SetCursor are driver-dependent. Do nothing
  // here; commit_position() re-calls SetCursor on first_commit_needed.
  return {};
}

std::shared_ptr<const Cursor> Renderer::current_cursor() const noexcept {
  return impl_->cursor;
}

// ---------------------------------------------------------------------------
// move_to / tick / stage
// ---------------------------------------------------------------------------

drm::expected<void, std::error_code> Renderer::move_to(int crtc_x, int crtc_y) {
  if (!impl_->cursor) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_permitted));
  }
  impl_->last_x = crtc_x;
  impl_->last_y = crtc_y;
  if (impl_->paused || !impl_->visible) {
    return {};
  }
  // Advance animation as a side effect so a caller that only drives
  // move_to() still gets animated cursors while the pointer is in
  // motion. tick() remains the authoritative path for idle animation.
  (void)impl_->advance(impl_->clock->now());
  return impl_->commit_position(crtc_x, crtc_y);
}

drm::expected<bool, std::error_code> Renderer::tick() {
  return tick(impl_->clock->now());
}

drm::expected<bool, std::error_code> Renderer::tick(drm::Clock::time_point now) {
  if (!impl_->cursor) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_permitted));
  }
  if (impl_->paused || !impl_->visible) {
    return false;
  }
  if (!impl_->advance(now)) {
    return false;
  }
  if (auto r = impl_->commit_position(impl_->last_x, impl_->last_y); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  return true;
}

drm::expected<bool, std::error_code> Renderer::stage(AtomicRequest& req, int crtc_x, int crtc_y,
                                                     bool& first_commit) {
  return stage(req, crtc_x, crtc_y, first_commit, impl_->clock->now());
}

drm::expected<bool, std::error_code> Renderer::stage(AtomicRequest& req, int crtc_x, int crtc_y,
                                                     bool& first_commit,
                                                     drm::Clock::time_point now) {
  if (!impl_->cursor) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_permitted));
  }
  if (impl_->path == PlanePath::kLegacy) {
    // stage() is an atomic-only API — there's no way to coalesce
    // drmModeSetCursor into an AtomicRequest.
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
  }
  impl_->last_x = crtc_x;
  impl_->last_y = crtc_y;
  const bool reblit = impl_->advance(now);
  if (auto r = impl_->stage_position(req, crtc_x, crtc_y); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  // The out-param tells the caller whether to OR ALLOW_MODESET into
  // its commit flags. We clear unconditionally after reporting: if the
  // caller's commit fails, the kernel state didn't advance, and the
  // caller must retry with ALLOW_MODESET set explicitly (the first
  // plane-enable always requires it regardless of our bookkeeping).
  first_commit = impl_->first_commit_needed;
  impl_->first_commit_needed = false;
  return reblit;
}

// ---------------------------------------------------------------------------
// show / hide
// ---------------------------------------------------------------------------

drm::expected<void, std::error_code> Renderer::hide() {
  if (!impl_->visible) {
    return {};
  }
  impl_->visible = false;
  if (impl_->paused) {
    return {};
  }
  if (impl_->path == PlanePath::kLegacy) {
    if (drmModeSetCursor(impl_->drm_fd, impl_->crtc_id, 0, 0, 0) != 0) {
      return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
    }
    // Any subsequent show() must re-upload the buffer — legacy
    // SetCursor(0) detaches the handle entirely.
    impl_->first_commit_needed = true;
    return {};
  }

  const drm::Device tmp_dev = drm::Device::from_fd(impl_->drm_fd);
  drm::AtomicRequest req(tmp_dev);
  if (!req.valid()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
  }
  auto add = [&](const char* name, std::uint64_t val) -> drm::expected<void, std::error_code> {
    auto prop_id = impl_->props.property_id(impl_->plane_id, name);
    if (!prop_id) {
      return drm::unexpected<std::error_code>(prop_id.error());
    }
    return req.add_property(impl_->plane_id, *prop_id, val);
  };
  if (auto r = add("FB_ID", 0); !r) {
    return r;
  }
  if (auto r = add("CRTC_ID", 0); !r) {
    return r;
  }
  if (auto r = req.commit(0); !r) {
    return r;
  }
  // Next show() resurrects the plane, which is a fresh enable from
  // the kernel's perspective — demands ALLOW_MODESET again.
  impl_->first_commit_needed = true;
  return {};
}

drm::expected<void, std::error_code> Renderer::show() {
  if (impl_->visible) {
    return {};
  }
  impl_->visible = true;
  if (!impl_->cursor || impl_->paused) {
    return {};
  }
  return impl_->commit_position(impl_->last_x, impl_->last_y);
}

// ---------------------------------------------------------------------------
// Session hooks
// ---------------------------------------------------------------------------

void Renderer::on_session_paused() noexcept {
  impl_->paused = true;
}

drm::expected<void, std::error_code> Renderer::on_session_resumed(int new_fd) {
  if (new_fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  // Old fd's kernel state is gone — we can't DESTROY_DUMB or RmFB on
  // it. But the mmap VMA persists past DRM-fd close (fd close doesn't
  // auto-munmap; VMAs hold their own file ref), so we must release it
  // explicitly or leak one VMA per VT switch on long-lived compositors.
  // drm::dumb::Buffer::forget() munmaps locally and drops the GEM/FB
  // handles without issuing ioctls against the dead fd. Snapshot the
  // dimensions first so we can re-allocate on the new fd.
  const auto prev_w = impl_->buffer.width();
  const auto prev_h = impl_->buffer.height();
  impl_->buffer.forget();
  impl_->props.clear();
  impl_->drm_fd = new_fd;

  if (auto r = impl_->alloc_buffer(prev_w, prev_h); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  if (impl_->path != PlanePath::kLegacy) {
    if (auto r = impl_->props.cache_properties(new_fd, impl_->plane_id, DRM_MODE_OBJECT_PLANE);
        !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
    impl_->probe_plane_properties();
  }

  impl_->paused = false;
  impl_->first_commit_needed = true;

  // Re-blit whatever frame the animation is on right now, so the
  // first post-resume commit carries the correct pixels. Skipped
  // when no cursor is bound (caller will call set_cursor() later).
  if (impl_->cursor) {
    const Cursor& bound = *impl_->cursor;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(impl_->clock->now() -
                                                                               impl_->anim_start);
    const Frame& f = bound.animated() ? bound.frame_at(elapsed) : bound.first();
    impl_->blit_frame(f);

    if (impl_->visible) {
      return impl_->commit_position(impl_->last_x, impl_->last_y);
    }
  }
  return {};
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------

PlanePath Renderer::path() const noexcept {
  return impl_->path;
}

std::uint32_t Renderer::plane_id() const noexcept {
  return impl_->plane_id;
}

Rotation Renderer::rotation() const noexcept {
  return impl_->rotation;
}

drm::expected<void, std::error_code> Renderer::set_rotation(Rotation rotation) {
  if (impl_->rotation == rotation) {
    return {};
  }
  // Legacy drmModeSetCursor has no rotation channel at all — the
  // buffer is uploaded by handle, not by pixel content we control
  // past that point. Atomic planes handle both modes: hardware
  // rotation via the cached rotation_prop, or software pre-rotation
  // in blit_frame when the plane doesn't expose it.
  if (rotation != Rotation::k0 && impl_->path == PlanePath::kLegacy) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
  }
  impl_->rotation = rotation;

  // If software pre-rotation is in play (either the new or the old
  // rotation flips through blit_frame), the buffer's current contents
  // don't match the new orientation — re-blit so the next commit
  // ships rotated pixels. The check is a cheap property-id probe.
  if (!impl_->rotation_prop && impl_->last_frame != nullptr) {
    impl_->blit_frame(*impl_->last_frame);
  }

  // Commit immediately if there's something to display so the new
  // rotation takes effect without waiting for the next mouse event.
  // If no cursor is bound, the cursor is hidden, or the session is
  // paused, the stored value picks up on the next commit after
  // set_cursor() / show() / on_session_resumed() — stage_position
  // always re-emits the rotation prop when cached, and blit_frame
  // re-rotates when it isn't.
  if (impl_->cursor && impl_->visible && !impl_->paused) {
    return impl_->commit_position(impl_->last_x, impl_->last_y);
  }
  return {};
}

bool Renderer::has_hotspot_properties() const noexcept {
  return impl_->hotspot_x_prop.has_value() && impl_->hotspot_y_prop.has_value();
}

bool Renderer::has_hardware_rotation() const noexcept {
  return impl_->rotation_prop.has_value();
}

}  // namespace drm::cursor
