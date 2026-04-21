// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "renderer.hpp"

#include "../core/device.hpp"
#include "../core/property_store.hpp"
#include "../core/resources.hpp"
#include "../modeset/atomic.hpp"
#include "../planes/plane_registry.hpp"
#include "../time/clock.hpp"
#include "cursor.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
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
  std::uint32_t gem_handle{0};
  std::uint32_t buf_w{0};
  std::uint32_t buf_h{0};
  std::uint32_t stride{0};
  std::uint64_t buf_size{0};
  std::uint32_t fb_id{0};  // 0 in legacy path
  std::uint32_t* mapped{nullptr};

  // --- cached plane props (atomic paths only) -----------------------
  drm::PropertyStore props;

  // HOTSPOT_X / HOTSPOT_Y are optional (virtualized drivers only). We
  // resolve their property ids once at create + session-resume time so
  // the per-event stage_position doesn't pay for a lookup that is
  // almost always going to miss on bare metal. nullopt = the plane
  // does not expose this property at all.
  std::optional<std::uint32_t> hotspot_x_prop;
  std::optional<std::uint32_t> hotspot_y_prop;

  // --- cursor binding + animation -----------------------------------
  std::optional<Cursor> cursor;
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

  // Probe the just-cached plane properties for HOTSPOT_X / HOTSPOT_Y.
  // Fills hotspot_x_prop / hotspot_y_prop; both remain nullopt on
  // bare-metal planes. Cheap — the property_id lookup is a small
  // vector scan. Called from create() and on_session_resumed() right
  // after props.cache_properties().
  void probe_hotspot_properties() noexcept;

  // Translate a CRTC coordinate to the plane-destination coordinate,
  // accounting for the current frame's hotspot and the centering
  // offset when the frame is smaller than the backing buffer.
  [[nodiscard]] std::pair<int, int> hotspot_adjust(int crtc_x, int crtc_y) const;
};

// ---------------------------------------------------------------------------
// Impl: buffer lifecycle
// ---------------------------------------------------------------------------

drm::expected<void, std::error_code> Renderer::Impl::alloc_buffer(std::uint32_t w,
                                                                  std::uint32_t h) {
  drm_mode_create_dumb create{};
  create.width = w;
  create.height = h;
  create.bpp = 32;
  if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }

  gem_handle = create.handle;
  buf_w = w;
  buf_h = h;
  stride = create.pitch;
  buf_size = create.size;

  drm_mode_map_dumb map_req{};
  map_req.handle = gem_handle;
  if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) < 0) {
    const auto ec = std::error_code(errno, std::system_category());
    free_buffer();
    return drm::unexpected<std::error_code>(ec);
  }

  void* ptr = mmap(nullptr, buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd,
                   static_cast<off_t>(map_req.offset));
  if (ptr == MAP_FAILED) {
    const auto ec = std::error_code(errno, std::system_category());
    free_buffer();
    return drm::unexpected<std::error_code>(ec);
  }
  mapped = static_cast<std::uint32_t*>(ptr);

  // Atomic paths need an FB_ID to reference the buffer; legacy uses
  // the raw GEM handle directly.
  if (path != PlanePath::kLegacy) {
    std::uint32_t handles[4] = {gem_handle};
    std::uint32_t strides[4] = {stride};
    std::uint32_t offsets[4] = {0};
    if (drmModeAddFB2(drm_fd, buf_w, buf_h, DRM_FORMAT_ARGB8888, handles, strides, offsets, &fb_id,
                      0) != 0) {
      const auto ec = std::error_code(errno, std::system_category());
      free_buffer();
      return drm::unexpected<std::error_code>(ec);
    }
  }

  // Zero-fill so the first blit only has to write the cursor's own
  // pixels — the rest stays transparent (alpha=0) instead of being
  // uninitialized mmap contents.
  std::memset(mapped, 0, buf_size);
  return {};
}

void Renderer::Impl::free_buffer() {
  if (fb_id != 0) {
    drmModeRmFB(drm_fd, fb_id);
    fb_id = 0;
  }
  if (mapped != nullptr) {
    munmap(mapped, buf_size);
    mapped = nullptr;
  }
  if (gem_handle != 0) {
    drm_mode_destroy_dumb destroy{};
    destroy.handle = gem_handle;
    ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    gem_handle = 0;
  }
  buf_size = 0;
}

// ---------------------------------------------------------------------------
// Impl: rendering
// ---------------------------------------------------------------------------

void Renderer::Impl::blit_frame(const Frame& f) {
  if (mapped == nullptr) {
    return;
  }
  const auto stride_px = static_cast<std::size_t>(stride / 4);

  // Wipe the full buffer — a smaller-than-buffer frame must not leak
  // stale pixels around its edges (prior frames in an animation or a
  // different shape from a shape-cycle call).
  for (std::size_t y = 0; y < buf_h; ++y) {
    std::memset(mapped + (y * stride_px), 0, static_cast<std::size_t>(buf_w) * 4);
  }

  const std::uint32_t w = std::min(f.width, buf_w);
  const std::uint32_t h = std::min(f.height, buf_h);
  const std::size_t x_off = (buf_w > f.width) ? (buf_w - f.width) / 2 : 0;
  const std::size_t y_off = (buf_h > f.height) ? (buf_h - f.height) / 2 : 0;

  for (std::size_t y = 0; y < h; ++y) {
    const std::size_t src_offset = y * static_cast<std::size_t>(f.width);
    std::memcpy(mapped + ((y + y_off) * stride_px) + x_off, f.pixels.data() + src_offset,
                static_cast<std::size_t>(w) * 4);
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
  const int x_off =
      (buf_w > last_frame->width) ? static_cast<int>((buf_w - last_frame->width) / 2) : 0;
  const int y_off =
      (buf_h > last_frame->height) ? static_cast<int>((buf_h - last_frame->height) / 2) : 0;
  return {crtc_x - (last_frame->xhot + x_off), crtc_y - (last_frame->yhot + y_off)};
}

void Renderer::Impl::probe_hotspot_properties() noexcept {
  hotspot_x_prop.reset();
  hotspot_y_prop.reset();
  if (path == PlanePath::kLegacy) {
    return;
  }
  if (auto id = props.property_id(plane_id, "HOTSPOT_X")) {
    hotspot_x_prop = *id;
  }
  if (auto id = props.property_id(plane_id, "HOTSPOT_Y")) {
    hotspot_y_prop = *id;
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
  if (auto r = add("FB_ID", fb_id); !r) {
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
  if (auto r = add("CRTC_W", buf_w); !r) {
    return r;
  }
  if (auto r = add("CRTC_H", buf_h); !r) {
    return r;
  }
  if (auto r = add("SRC_X", 0); !r) {
    return r;
  }
  if (auto r = add("SRC_Y", 0); !r) {
    return r;
  }
  // Source rect is in 16.16 fixed-point; CRTC rect is plain pixels.
  if (auto r = add("SRC_W", static_cast<std::uint64_t>(buf_w) << 16U); !r) {
    return r;
  }
  if (auto r = add("SRC_H", static_cast<std::uint64_t>(buf_h) << 16U); !r) {
    return r;
  }

  // HOTSPOT_X / HOTSPOT_Y — virtualized-driver hint. Only written if
  // the plane exposes the properties (probed once at create/resume).
  // The buffer-local hotspot is the frame's own hotspot plus the
  // centering offset applied in blit_frame, so the host VMM reads a
  // coordinate in the same space as CRTC_X/CRTC_Y + buffer size.
  // Pre-blit (last_frame == nullptr) we skip the write — there's no
  // meaningful hotspot yet and we'd rather leave the prior value
  // than clobber it with a stale zero.
  if (last_frame != nullptr) {
    const int x_off =
        (buf_w > last_frame->width) ? static_cast<int>((buf_w - last_frame->width) / 2) : 0;
    const int y_off =
        (buf_h > last_frame->height) ? static_cast<int>((buf_h - last_frame->height) / 2) : 0;
    // xhot/yhot are int in the XCursor format but are always >= 0 in
    // practice (libxcursor surfaces them as XCursorDim, unsigned).
    // Clamp defensively so a malformed file can't overflow the cast.
    const std::uint64_t hx = static_cast<std::uint64_t>(std::max(0, last_frame->xhot + x_off));
    const std::uint64_t hy = static_cast<std::uint64_t>(std::max(0, last_frame->yhot + y_off));
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
      if (drmModeSetCursor(drm_fd, crtc_id, gem_handle, buf_w, buf_h) != 0) {
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
    impl->probe_hotspot_properties();

    // Rotation: set the plane's rotation property now if the plane
    // exposes it. Planes that don't expose it can only run k0;
    // anything else is rejected up front rather than at first commit.
    if (cfg.rotation != Rotation::k0) {
      auto prop_id = impl->props.property_id(impl->plane_id, "rotation");
      if (!prop_id) {
        impl->free_buffer();
        return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
      }
      // The rotation property is set inside every commit via the
      // cached property id; we just recorded whether the plane has
      // the knob at all. commit_position / stage_position ride on
      // the cached property list, so no extra work here.
      //
      // Note: we don't stage rotation inside stage_position() because
      // that lives in the hot path and the value only changes on
      // set_rotation() (deferred to V2). For V1 we commit rotation
      // once at create() via a separate atomic test-commit so the
      // plane is configured before the first real frame.
      const drm::Device tmp = drm::Device::from_fd(dev.fd());
      drm::AtomicRequest setup_req(tmp);
      if (!setup_req.valid()) {
        impl->free_buffer();
        return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
      }
      if (auto r =
              setup_req.add_property(impl->plane_id, *prop_id, rotation_to_drm_mask(cfg.rotation));
          !r) {
        impl->free_buffer();
        return drm::unexpected<std::error_code>(r.error());
      }
      if (auto r = setup_req.commit(DRM_MODE_ATOMIC_ALLOW_MODESET); !r) {
        impl->free_buffer();
        return drm::unexpected<std::error_code>(r.error());
      }
    }
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
  // emplace returns a reference to the newly-constructed value — lets us
  // bind a name without tripping the tidy unchecked-optional-access
  // check (which otherwise flags every subsequent access, even .value()).
  const Cursor& bound = impl_->cursor.emplace(std::move(cursor));
  impl_->anim_start = impl_->clock->now();

  // Blit the first frame eagerly so the next commit has real pixels;
  // last_frame points into impl_->cursor's frame vector, which is
  // move-stable (std::optional in-place emplacement preserves the
  // Cursor's Impl pointer, and frames are stored in a vector<Frame>
  // that isn't resized post-load).
  impl_->blit_frame(bound.first());
  impl_->first_commit_needed = true;

  // Legacy needs an explicit re-upload of the new buffer contents
  // if we'd already installed it in a prior session — same-handle
  // re-uploads after a SetCursor are driver-dependent. Do nothing
  // here; commit_position() re-calls SetCursor on first_commit_needed.
  return {};
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
  if (impl_->mapped != nullptr && impl_->buf_size > 0) {
    munmap(impl_->mapped, impl_->buf_size);
  }
  impl_->gem_handle = 0;
  impl_->fb_id = 0;
  impl_->mapped = nullptr;
  impl_->buf_size = 0;
  impl_->props.clear();
  impl_->drm_fd = new_fd;

  if (auto r = impl_->alloc_buffer(impl_->buf_w, impl_->buf_h); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  if (impl_->path != PlanePath::kLegacy) {
    if (auto r = impl_->props.cache_properties(new_fd, impl_->plane_id, DRM_MODE_OBJECT_PLANE);
        !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
    impl_->probe_hotspot_properties();
  }

  impl_->paused = false;
  impl_->first_commit_needed = true;

  // Re-blit whatever frame the animation is on right now, so the
  // first post-resume commit carries the correct pixels. Skipped
  // when no cursor is bound (caller will call set_cursor() later).
  // The local reference collapses tidy's view of the optional access
  // pattern to a single variable so the guard on the next line is
  // recognized (tidy struggles to track narrowing through impl_->).
  auto& cur_opt = impl_->cursor;
  if (cur_opt.has_value()) {
    const Cursor& bound = *cur_opt;
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

bool Renderer::has_hotspot_properties() const noexcept {
  return impl_->hotspot_x_prop.has_value() && impl_->hotspot_y_prop.has_value();
}

}  // namespace drm::cursor
