// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "layer.hpp"

#include <functional>

namespace drm::planes {

Layer& Layer::set_property(std::string_view name, uint64_t value) {
  auto [it, inserted] = properties_.emplace(std::string(name), value);
  if (!inserted && it->second != value) {
    it->second = value;
    dirty_ = true;
  } else if (inserted) {
    dirty_ = true;
  }
  return *this;
}

Layer& Layer::disable() noexcept {
  properties_["FB_ID"] = 0;
  dirty_ = true;
  return *this;
}

Layer& Layer::set_composited() noexcept {
  force_composited_ = true;
  dirty_ = true;
  return *this;
}

Layer& Layer::set_content_type(ContentType type) noexcept {
  content_type_ = type;
  return *this;
}

Layer& Layer::set_update_hint(uint32_t hz) noexcept {
  update_hz_ = hz;
  return *this;
}

bool Layer::needs_composition() const noexcept {
  return needs_composition_;
}

std::optional<uint32_t> Layer::assigned_plane_id() const noexcept {
  return assigned_plane_;
}

std::optional<uint64_t> Layer::property(std::string_view name) const {
  auto it = properties_.find(std::string(name));
  if (it == properties_.end()) return std::nullopt;
  return it->second;
}

const std::unordered_map<std::string, uint64_t>& Layer::properties() const noexcept {
  return properties_;
}

std::optional<uint32_t> Layer::format() const {
  // The format is set as a separate property hint by the compositor.
  auto fmt = property("pixel_format");
  if (fmt) return static_cast<uint32_t>(*fmt);
  return std::nullopt;
}

uint64_t Layer::modifier() const {
  auto val = property("FB_MODIFIER");
  return val.value_or(0);
}

uint64_t Layer::rotation() const {
  auto val = property("rotation");
  return val.value_or(0); // 0 = DRM_MODE_ROTATE_0
}

bool Layer::requires_scaling() const {
  auto src_w = property("SRC_W");
  auto crtc_w = property("CRTC_W");
  auto src_h = property("SRC_H");
  auto crtc_h = property("CRTC_H");

  if (!src_w || !crtc_w || !src_h || !crtc_h) return false;

  // SRC coordinates are in 16.16 fixed point
  uint32_t sw = static_cast<uint32_t>(*src_w >> 16);
  uint32_t sh = static_cast<uint32_t>(*src_h >> 16);
  uint32_t cw = static_cast<uint32_t>(*crtc_w);
  uint32_t ch = static_cast<uint32_t>(*crtc_h);

  return sw != cw || sh != ch;
}

uint32_t Layer::width() const {
  auto val = property("CRTC_W");
  return val ? static_cast<uint32_t>(*val) : 0;
}

uint32_t Layer::height() const {
  auto val = property("CRTC_H");
  return val ? static_cast<uint32_t>(*val) : 0;
}

Rect Layer::crtc_rect() const {
  return {
    .x = static_cast<int32_t>(property("CRTC_X").value_or(0)),
    .y = static_cast<int32_t>(property("CRTC_Y").value_or(0)),
    .w = width(),
    .h = height(),
  };
}

bool Layer::is_composition_layer() const noexcept {
  return is_composition_layer_;
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

std::size_t Layer::property_hash() const {
  std::size_t h = 0;
  for (const auto& [name, val] : properties_) {
    // Skip FB_ID since it changes every frame
    if (name == "FB_ID") continue;
    h ^= std::hash<std::string>{}(name) ^ std::hash<uint64_t>{}(val);
  }
  return h;
}

void Layer::mark_clean() noexcept {
  dirty_ = false;
}

} // namespace drm::planes
