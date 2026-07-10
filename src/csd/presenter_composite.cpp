// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "presenter_composite.hpp"

#include "../core/device.hpp"
#include "../scene/composite_canvas.hpp"
#include "../scene/composition_target.hpp"
#include "presenter.hpp"
#include "presenter_plane.hpp"
#include "surface.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/property_store.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/atomic.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::csd {

namespace {

drm::unexpected<std::error_code> err(std::errc e) {
  return drm::unexpected<std::error_code>(std::make_error_code(e));
}

const drm::planes::PlaneCapabilities* find_caps(const drm::planes::PlaneRegistry& registry,
                                                std::uint32_t plane_id) {
  for (const auto& cap : registry.all()) {
    if (cap.id == plane_id) {
      return &cap;
    }
  }
  return nullptr;
}

drm::expected<std::uint32_t, std::error_code> resolve(const drm::PropertyStore& props,
                                                      std::uint32_t plane_id,
                                                      std::string_view name) {
  auto id = props.property_id(plane_id, name);
  if (!id) {
    return drm::unexpected<std::error_code>(id.error());
  }
  return *id;
}

constexpr std::uint64_t to_16_16(std::uint32_t pixels) noexcept {
  return static_cast<std::uint64_t>(pixels) << 16U;
}

}  // namespace

// ── Pure helper ────────────────────────────────────────────────────────

std::vector<PropertyWrite> compute_canvas_writes(const PlaneSlot& slot, std::uint32_t fb_id,
                                                 std::uint32_t canvas_w, std::uint32_t canvas_h) {
  std::vector<PropertyWrite> out;
  out.reserve(10);
  const auto add = [&](std::uint32_t prop, std::uint64_t val) {
    out.push_back({slot.plane_id, prop, val});
  };
  // The canvas plane is always armed: full-CRTC destination, full-canvas
  // source. No disarm path — the canvas is present every frame; a closed
  // decoration just stops being blended into it.
  add(slot.fb_id_prop, fb_id);
  add(slot.crtc_id_prop, slot.crtc_id);
  add(slot.crtc_x_prop, 0);
  add(slot.crtc_y_prop, 0);
  add(slot.crtc_w_prop, canvas_w);
  add(slot.crtc_h_prop, canvas_h);
  add(slot.src_x_prop, 0);
  add(slot.src_y_prop, 0);
  add(slot.src_w_prop, to_16_16(canvas_w));
  add(slot.src_h_prop, to_16_16(canvas_h));
  return out;
}

// ── CompositePresenter ─────────────────────────────────────────────────

drm::expected<std::unique_ptr<CompositePresenter>, std::error_code> CompositePresenter::create(
    drm::Device& dev, const drm::planes::PlaneRegistry& registry, std::uint32_t crtc_id,
    std::uint32_t canvas_plane_id, std::uint32_t canvas_w, std::uint32_t canvas_h,
    drm::span<const std::uint8_t> background_argb) {
  if (crtc_id == 0U || canvas_w == 0U || canvas_h == 0U) {
    return err(std::errc::invalid_argument);
  }
  const auto* caps = find_caps(registry, canvas_plane_id);
  if (caps == nullptr) {
    return err(std::errc::invalid_argument);
  }

  // Pick the best scanout format the plane can host for the canvas —
  // ARGB8888 when advertised (byte-identical to the blend), else an
  // 8888 channel swap or a 16bpp pack. nullopt => this plane can't host
  // a composition canvas at all.
  const auto fourcc = drm::scene::canvas_format_for_plane(*caps);
  if (!fourcc) {
    return err(std::errc::not_supported);
  }

  drm::scene::CompositeCanvasConfig cfg;
  cfg.max_canvases = 1;
  cfg.canvas_width = canvas_w;
  cfg.canvas_height = canvas_h;
  cfg.output_fourcc = *fourcc;
  auto canvas = drm::scene::CompositeCanvas::create(dev, cfg);
  if (!canvas) {
    return drm::unexpected<std::error_code>(canvas.error());
  }

  std::unique_ptr<CompositePresenter> self(new CompositePresenter());
  self->canvas_w_ = canvas_w;
  self->canvas_h_ = canvas_h;
  self->canvas_ = std::move(*canvas);

  // Copy the desktop backdrop only when it's exactly the canvas size;
  // a mismatched or empty span leaves the desktop transparent-black.
  const std::size_t bg_bytes = static_cast<std::size_t>(canvas_w) * canvas_h * 4U;
  if (background_argb.size() == bg_bytes) {
    self->background_.assign(background_argb.begin(), background_argb.end());
  }

  drm::PropertyStore props;
  if (auto r = props.cache_properties(dev.fd(), canvas_plane_id, DRM_MODE_OBJECT_PLANE); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  self->slot_.plane_id = canvas_plane_id;
  self->slot_.crtc_id = crtc_id;

  // Only the geometry set — the canvas plane is the primary (or a single
  // full-screen overlay); blend-mode / per-plane-alpha / zpos are the
  // Plane tier's per-decoration concern, not the composite canvas's.
  static constexpr std::pair<std::string_view, std::uint32_t PlaneSlot::*> required[] = {
      {"FB_ID", &PlaneSlot::fb_id_prop},   {"CRTC_ID", &PlaneSlot::crtc_id_prop},
      {"CRTC_X", &PlaneSlot::crtc_x_prop}, {"CRTC_Y", &PlaneSlot::crtc_y_prop},
      {"CRTC_W", &PlaneSlot::crtc_w_prop}, {"CRTC_H", &PlaneSlot::crtc_h_prop},
      {"SRC_X", &PlaneSlot::src_x_prop},   {"SRC_Y", &PlaneSlot::src_y_prop},
      {"SRC_W", &PlaneSlot::src_w_prop},   {"SRC_H", &PlaneSlot::src_h_prop},
  };
  for (const auto& [name, member] : required) {
    auto id = resolve(props, canvas_plane_id, name);
    if (!id) {
      return drm::unexpected<std::error_code>(id.error());
    }
    self->slot_.*member = *id;
  }

  return self;
}

CompositePresenter::~CompositePresenter() = default;

drm::expected<void, std::error_code> CompositePresenter::apply(drm::span<const SurfaceRef> surfaces,
                                                               drm::AtomicRequest& req) {
  // Standard composition-target drive: clear last frame's footprint,
  // blend this frame's decorations bottom-to-top, publish. The canvas's
  // clear()/flush() pair scissors the shadow->scanout copy to just the
  // touched bands, so an idle desktop pays only for what moved.
  canvas_->begin_frame();
  canvas_->clear();

  // Desktop backdrop, if any, as the bottom layer. Full-screen, so the
  // canvas's damage tracking widens to the whole frame this pass — fine
  // for the composite tier, whose apply() runs on content change, not
  // every vblank.
  if (!background_.empty()) {
    drm::scene::CompositeSrc bg;
    bg.pixels = drm::span<const std::uint8_t>(background_.data(), background_.size());
    bg.src_stride_bytes = canvas_w_ * 4U;
    bg.src_width = canvas_w_;
    bg.src_height = canvas_h_;
    // XRGB (not ARGB): the backdrop is opaque, and tagging it opaque takes
    // blend_into's straight-copy fast path instead of a per-pixel SRC_OVER.
    bg.drm_fourcc = DRM_FORMAT_XRGB8888;
    const drm::scene::CompositeRect full{0, 0, canvas_w_, canvas_h_};
    canvas_->blend(bg, full, full);
  }

  for (const auto& ref : surfaces) {
    const auto* surface = ref.surface;
    if (surface == nullptr || surface->empty()) {
      continue;  // vacant slot — the closed decoration vacates via clear()
    }
    // SurfaceRef holds a const Surface* — the presenter contract is
    // "read the decoration, don't mutate it". A read-only CPU map honors
    // that, but Surface::paint() is non-const (the GBM backend stages a
    // scratch buffer at map time), so a const_cast is the honest bridge;
    // MapAccess::Read never alters the decoration's pixels.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    auto map = const_cast<Surface*>(surface)->paint(drm::MapAccess::Read);
    if (!map) {
      return drm::unexpected<std::error_code>(map.error());
    }
    drm::scene::CompositeSrc src;
    src.pixels = map->pixels();
    src.src_stride_bytes = map->stride();
    src.src_width = surface->width();
    src.src_height = surface->height();
    src.drm_fourcc = surface->format();
    const drm::scene::CompositeRect src_rect{0, 0, surface->width(), surface->height()};
    const drm::scene::CompositeRect dst_rect{ref.x, ref.y, surface->width(), surface->height()};
    canvas_->blend(src, src_rect, dst_rect);
  }

  if (auto r = canvas_->flush(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }

  const auto writes = compute_canvas_writes(slot_, canvas_->fb_id(), canvas_w_, canvas_h_);
  for (const auto& w : writes) {
    if (auto r = req.add_property(w.object_id, w.property_id, w.value); !r) {
      return r;
    }
  }
  return {};
}

std::uint32_t CompositePresenter::canvas_fb_id() const noexcept {
  return canvas_ ? canvas_->fb_id() : 0U;
}

std::uint32_t CompositePresenter::canvas_format() const noexcept {
  return canvas_ ? canvas_->drm_fourcc() : 0U;
}

}  // namespace drm::csd
