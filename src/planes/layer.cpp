// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "layer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

namespace drm::planes {

namespace {

// Canonical KMS-property name table, indexed by `static_cast<size_t>(PropTag)`.
// Order must match the enum exactly — `prop_name` reads this directly.
constexpr std::array<std::string_view, k_num_props> k_prop_names{
    "FB_ID",  "FB_MODIFIER", "CRTC_ID",      "CRTC_X",      "CRTC_Y", "CRTC_W",
    "CRTC_H", "SRC_X",       "SRC_Y",        "SRC_W",       "SRC_H",  "rotation",
    "alpha",  "zpos",        "pixel_format", "IN_FENCE_FD",
};

}  // namespace

std::string_view prop_name(PropTag tag) noexcept {
  const auto idx = static_cast<std::size_t>(tag);
  if (idx >= k_num_props) {
    return {};
  }
  return k_prop_names.at(idx);
}

std::optional<PropTag> parse_prop_tag(std::string_view name) noexcept {
  // Linear scan over k_num_props (currently 16). Branchless and
  // cache-friendly; faster than std::unordered_map<string,PropTag> at
  // this size.
  for (std::size_t i = 0; i < k_num_props; ++i) {
    if (k_prop_names.at(i) == name) {
      return static_cast<PropTag>(i);
    }
  }
  return std::nullopt;
}

// ── Layer::PropertyView ─────────────────────────────────────────────

Layer::PropertyView::iterator::iterator(const Layer* owner, std::size_t idx) noexcept
    : owner_(owner), idx_(idx) {
  advance_to_set();
}

Layer::PropertyView::iterator::value_type Layer::PropertyView::iterator::operator*()
    const noexcept {
  return {prop_name(static_cast<PropTag>(idx_)), owner_->values_.at(idx_)};
}

Layer::PropertyView::iterator& Layer::PropertyView::iterator::operator++() noexcept {
  ++idx_;
  advance_to_set();
  return *this;
}

bool Layer::PropertyView::iterator::operator==(const iterator& other) const noexcept {
  return idx_ == other.idx_;
}

bool Layer::PropertyView::iterator::operator!=(const iterator& other) const noexcept {
  return !(*this == other);
}

void Layer::PropertyView::iterator::advance_to_set() noexcept {
  while (idx_ < k_num_props && !owner_->set_mask_.test(idx_)) {
    ++idx_;
  }
}

Layer::PropertyView::iterator Layer::PropertyView::begin() const noexcept {
  return iterator{owner_, 0};
}

Layer::PropertyView::iterator Layer::PropertyView::end() const noexcept {
  return iterator{owner_, k_num_props};
}

bool Layer::PropertyView::empty() const noexcept {
  return owner_->set_mask_.none();
}

std::size_t Layer::PropertyView::size() const noexcept {
  return owner_->set_mask_.count();
}

// ── Layer ───────────────────────────────────────────────────────────

Layer& Layer::set_property(const std::string_view name, uint64_t value) {
  const auto tag = parse_prop_tag(name);
  if (!tag.has_value()) {
    return *this;
  }
  return set_property(*tag, value);
}

Layer& Layer::set_property(PropTag tag, uint64_t value) noexcept {
  const auto idx = static_cast<std::size_t>(tag);
  const bool was_set = set_mask_.test(idx);
  const bool value_changed = !was_set || values_.at(idx) != value;
  values_.at(idx) = value;
  set_mask_.set(idx);
  if (value_changed) {
    dirty_ = true;
    cached_hash_.reset();
  }
  return *this;
}

Layer& Layer::disable() noexcept {
  return set_property(PropTag::FbId, uint64_t{0});
}

Layer& Layer::set_composited() noexcept {
  force_composited_ = true;
  dirty_ = true;
  return *this;
}

Layer& Layer::set_transient_composited(bool composited) noexcept {
  if (transient_composited_ != composited) {
    transient_composited_ = composited;
    dirty_ = true;
  }
  return *this;
}

bool Layer::is_transient_composited() const noexcept {
  return transient_composited_;
}

Layer& Layer::set_content_type(ContentType type) noexcept {
  content_type_ = type;
  return *this;
}

Layer& Layer::set_update_hint(uint32_t hz) noexcept {
  update_hz_ = hz;
  return *this;
}

Layer& Layer::set_app_priority(uint8_t priority) noexcept {
  app_priority_ = priority;
  return *this;
}

bool Layer::needs_composition() const noexcept {
  return needs_composition_;
}

std::optional<uint32_t> Layer::assigned_plane_id() const noexcept {
  return assigned_plane_;
}

std::optional<uint64_t> Layer::property(std::string_view name) const {
  const auto tag = parse_prop_tag(name);
  if (!tag.has_value()) {
    return std::nullopt;
  }
  return property(*tag);
}

std::optional<uint64_t> Layer::property(PropTag tag) const noexcept {
  const auto idx = static_cast<std::size_t>(tag);
  if (idx >= k_num_props || !set_mask_.test(idx)) {
    return std::nullopt;
  }
  return values_.at(idx);
}

Layer::PropertyView Layer::properties() const noexcept {
  return PropertyView{this};
}

Layer::PropertySnapshot Layer::snapshot() const noexcept {
  return PropertySnapshot{values_, set_mask_};
}

std::optional<uint32_t> Layer::format() const {
  if (const auto fmt = property(PropTag::PixelFormat)) {
    return static_cast<uint32_t>(*fmt);
  }
  return std::nullopt;
}

uint64_t Layer::modifier() const {
  return property(PropTag::FbModifier).value_or(0);
}

uint64_t Layer::rotation() const {
  return property(PropTag::Rotation).value_or(0);  // 0 = DRM_MODE_ROTATE_0
}

bool Layer::requires_scaling() const {
  const auto src_w = property(PropTag::SrcW);
  const auto crtc_w = property(PropTag::CrtcW);
  const auto src_h = property(PropTag::SrcH);
  const auto crtc_h = property(PropTag::CrtcH);

  if (!src_w || !crtc_w || !src_h || !crtc_h) {
    return false;
  }

  // SRC coordinates are in 16.16 fixed point
  auto const sw = static_cast<uint32_t>(*src_w >> 16U);
  auto const sh = static_cast<uint32_t>(*src_h >> 16U);
  auto const cw = static_cast<uint32_t>(*crtc_w);
  auto const ch = static_cast<uint32_t>(*crtc_h);

  return sw != cw || sh != ch;
}

uint32_t Layer::width() const {
  return static_cast<uint32_t>(property(PropTag::CrtcW).value_or(0));
}

uint32_t Layer::height() const {
  return static_cast<uint32_t>(property(PropTag::CrtcH).value_or(0));
}

Rect Layer::crtc_rect() const {
  return Rect{
      static_cast<int32_t>(property(PropTag::CrtcX).value_or(0)),
      static_cast<int32_t>(property(PropTag::CrtcY).value_or(0)),
      width(),
      height(),
  };
}

bool Layer::is_composition_layer() const noexcept {
  return is_composition_layer_;
}

bool Layer::is_externally_bound() const noexcept {
  return externally_bound_;
}

Layer& Layer::set_externally_bound(bool externally_bound) noexcept {
  externally_bound_ = externally_bound;
  return *this;
}

bool Layer::is_dirty() const noexcept {
  return dirty_;
}

ContentType Layer::content_type() const noexcept {
  return content_type_;
}

uint32_t Layer::update_hz() const noexcept {
  return update_hz_;
}

uint8_t Layer::app_priority() const noexcept {
  return app_priority_;
}

std::size_t Layer::property_hash() const {
  if (cached_hash_.has_value()) {
    return *cached_hash_;
  }
  // Walk the set bits in tag order so two layers with the same logical
  // property set always produce the same hash. FB_ID and IN_FENCE_FD are
  // skipped — both change every frame (new buffer / new fence fd) and neither
  // affects plane compatibility, so including them would dirty the failure
  // cache uselessly.
  std::size_t h = 0x9e3779b97f4a7c15ULL;  // Golden ratio seed
  for (std::size_t i = 0; i < k_num_props; ++i) {
    if (!set_mask_.test(i)) {
      continue;
    }
    const auto tag = static_cast<PropTag>(i);
    if (tag == PropTag::FbId || tag == PropTag::InFenceFd) {
      continue;
    }
    // boost-style hash_combine, order-dependent (hence the tag-order walk above).
    h ^= std::hash<std::size_t>{}(i) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
    h ^= std::hash<uint64_t>{}(values_.at(i)) + 0x9e3779b97f4a7c15ULL + (h << 6U) + (h >> 2U);
  }
  cached_hash_ = h;
  return h;
}

void Layer::mark_clean() noexcept {
  dirty_ = false;
}

}  // namespace drm::planes
