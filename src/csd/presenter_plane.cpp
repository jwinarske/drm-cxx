// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "presenter_plane.hpp"

#include "../core/device.hpp"
#include "presenter.hpp"
#include "surface.hpp"

#include <drm-cxx/core/property_store.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/atomic.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

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

drm::expected<std::vector<PropertyWrite>, std::error_code> compute_writes(
    drm::span<const PlaneSlot> slots, drm::span<const SurfaceRef> surfaces) {
  if (surfaces.size() > slots.size()) {
    return err(std::errc::no_buffer_space);
  }

  std::vector<PropertyWrite> out;
  // Worst case: 12 properties per slot (FB_ID, CRTC_ID, four CRTC,
  // four SRC, blend mode, alpha). Pre-reserve to avoid mid-build
  // reallocation noise during the hot path.
  out.reserve(slots.size() * 12U);

  const auto add = [&](std::uint32_t obj, std::uint32_t prop, std::uint64_t val) {
    out.push_back({obj, prop, val});
  };

  for (std::size_t i = 0; i < slots.size(); ++i) {
    const auto& slot = slots[i];

    const bool armed =
        i < surfaces.size() && surfaces[i].surface != nullptr && !surfaces[i].surface->empty();

    if (!armed) {
      // Disarm: the kernel needs FB_ID=0 + CRTC_ID=0 to release the
      // plane. Skipping these would leave the prior frame's pixels
      // scanning out indefinitely.
      add(slot.plane_id, slot.fb_id_prop, 0);
      add(slot.plane_id, slot.crtc_id_prop, 0);
      continue;
    }

    const auto& ref = surfaces[i];
    const auto* surface = ref.surface;
    const std::uint64_t w = surface->width();
    const std::uint64_t h = surface->height();

    add(slot.plane_id, slot.fb_id_prop, surface->fb_id());
    add(slot.plane_id, slot.crtc_id_prop, slot.crtc_id);
    add(slot.plane_id, slot.crtc_x_prop, static_cast<std::uint64_t>(ref.x));
    add(slot.plane_id, slot.crtc_y_prop, static_cast<std::uint64_t>(ref.y));
    add(slot.plane_id, slot.crtc_w_prop, w);
    add(slot.plane_id, slot.crtc_h_prop, h);
    add(slot.plane_id, slot.src_x_prop, 0);
    add(slot.plane_id, slot.src_y_prop, 0);
    add(slot.plane_id, slot.src_w_prop, to_16_16(surface->width()));
    add(slot.plane_id, slot.src_h_prop, to_16_16(surface->height()));

    // Optional properties — skipped silently when the plane doesn't
    // expose them. Writing absent enums is rejected by the kernel
    // with -EINVAL, so the prop_id-zero gate is load-bearing, not
    // just an optimisation.
    if (slot.blend_mode_prop != 0U) {
      add(slot.plane_id, slot.blend_mode_prop, slot.blend_mode_value);
    }
    if (slot.alpha_prop != 0U) {
      add(slot.plane_id, slot.alpha_prop, 0xFFFFU);
    }
    if (slot.zpos_prop != 0U) {
      add(slot.plane_id, slot.zpos_prop, slot.zpos_value);
    }
  }

  return out;
}

// ── PlanePresenter ─────────────────────────────────────────────────────

drm::expected<std::unique_ptr<PlanePresenter>, std::error_code> PlanePresenter::create(
    drm::Device& dev, const drm::planes::PlaneRegistry& registry, std::uint32_t crtc_id,
    drm::span<const std::uint32_t> reserved_plane_ids, std::uint64_t base_zpos) {
  if (crtc_id == 0U) {
    return err(std::errc::invalid_argument);
  }

  std::unique_ptr<PlanePresenter> self(new PlanePresenter());
  self->slots_.reserve(reserved_plane_ids.size());

  drm::PropertyStore props;
  for (const std::uint32_t plane_id : reserved_plane_ids) {
    const auto* caps = find_caps(registry, plane_id);
    if (caps == nullptr) {
      // Reserved id isn't in the registry — caller must have built
      // the reservation against a different registry, or the
      // registry was rebuilt without the reservation rebuilt too.
      return err(std::errc::invalid_argument);
    }
    if (auto r = props.cache_properties(dev.fd(), plane_id, DRM_MODE_OBJECT_PLANE); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }

    PlaneSlot slot;
    slot.plane_id = plane_id;
    slot.crtc_id = crtc_id;

    // Required properties — every KMS plane exposes these. A missing
    // one means the driver / kernel is too old or the object id
    // doesn't actually point at a plane.
    static constexpr std::pair<std::string_view, std::uint32_t PlaneSlot::*> required[] = {
        {"FB_ID", &PlaneSlot::fb_id_prop},   {"CRTC_ID", &PlaneSlot::crtc_id_prop},
        {"CRTC_X", &PlaneSlot::crtc_x_prop}, {"CRTC_Y", &PlaneSlot::crtc_y_prop},
        {"CRTC_W", &PlaneSlot::crtc_w_prop}, {"CRTC_H", &PlaneSlot::crtc_h_prop},
        {"SRC_X", &PlaneSlot::src_x_prop},   {"SRC_Y", &PlaneSlot::src_y_prop},
        {"SRC_W", &PlaneSlot::src_w_prop},   {"SRC_H", &PlaneSlot::src_h_prop},
    };
    for (const auto& [name, member] : required) {
      auto id = resolve(props, plane_id, name);
      if (!id) {
        return drm::unexpected<std::error_code>(id.error());
      }
      slot.*member = *id;
    }

    // Optional properties — pre-multiplied blend + opaque alpha. Both
    // protect against a previous compositor having left the plane
    // configured with "None" blending or a non-0xFFFF alpha; the
    // glass theme's premultiplied output otherwise paints opaque
    // black wherever the buffer's alpha is zero.
    if (caps->has_pixel_blend_mode && caps->blend_mode_premultiplied.has_value()) {
      auto id = props.property_id(plane_id, "pixel blend mode");
      if (id) {
        slot.blend_mode_prop = *id;
        slot.blend_mode_value = *caps->blend_mode_premultiplied;
      }
    }
    if (caps->has_per_plane_alpha) {
      auto id = props.property_id(plane_id, "alpha");
      if (id) {
        slot.alpha_prop = *id;
      }
    }

    // zpos — written when the plane exposes a mutable zpos property
    // and the caller asked for stacking control via base_zpos > 0.
    // Planes whose zpos is pinned (zpos_min == zpos_max) report the
    // property as immutable; the kernel rejects writes to immutables
    // with -EINVAL, so the prop_id-zero gate is load-bearing.
    const bool zpos_mutable = caps->zpos_min.has_value() && caps->zpos_max.has_value() &&
                              *caps->zpos_min < *caps->zpos_max;
    if (base_zpos != 0U && zpos_mutable) {
      auto id = props.property_id(plane_id, "zpos");
      if (id && !props.is_immutable(plane_id, "zpos").value_or(true)) {
        slot.zpos_prop = *id;
        slot.zpos_value = base_zpos + self->slots_.size();
      }
    }

    self->slots_.push_back(slot);
  }

  return self;
}

drm::expected<void, std::error_code> PlanePresenter::apply(drm::span<const SurfaceRef> surfaces,
                                                           drm::AtomicRequest& req) {
  auto writes = compute_writes(slots_, surfaces);
  if (!writes) {
    return drm::unexpected<std::error_code>(writes.error());
  }
  for (const auto& w : *writes) {
    if (auto r = req.add_property(w.object_id, w.property_id, w.value); !r) {
      return r;
    }
  }
  return {};
}

drm::span<const PlaneSlot> PlanePresenter::slots() const noexcept {
  return {slots_.data(), slots_.size()};
}

}  // namespace drm::csd