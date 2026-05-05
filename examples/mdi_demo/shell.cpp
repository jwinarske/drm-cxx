// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "shell.hpp"

#include "csd/presenter.hpp"
#include "csd/renderer.hpp"
#include "csd/surface.hpp"
#include "csd/theme.hpp"
#include "csd/window_state.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mdi_demo {

namespace {

// Layout constants tuned to match csd::Renderer's glass theme. The
// renderer paints a fixed 32-pixel title bar with three traffic-light
// buttons in the rightmost ~96 px (button radius ≈ 8 px, spacing
// 24 px); the layout below mirrors those values so hit testing finds
// the same pixels the user sees.
constexpr std::int32_t k_title_bar_height = 32;
constexpr std::int32_t k_button_radius = 8;
constexpr std::int32_t k_button_spacing = 24;
constexpr std::int32_t k_button_right_inset = 16;
// Default per-doc decoration size. Big enough that the title bar +
// glass body has visible body area; small enough that two of them
// fit side-by-side on a 1920×1080 panel without tiling beyond the
// stagger offsets below.
constexpr std::uint32_t k_default_doc_w = 600;
constexpr std::uint32_t k_default_doc_h = 360;
constexpr std::int32_t k_stagger_x = 48;
constexpr std::int32_t k_stagger_y = 48;
constexpr std::int32_t k_first_doc_x = 80;
constexpr std::int32_t k_first_doc_y = 80;

// Inverse of the renderer's button x-positions. The renderer paints
// Close at the rightmost slot, then Minimize, then Maximize (Linux
// convention — Close is the rightmost). Mirror that here so a click
// on the close glyph hits ButtonClose.
HitZone classify_button(std::int32_t local_x, std::int32_t deco_w) {
  const auto cx_close = deco_w - k_button_right_inset;
  const auto cx_min = cx_close - k_button_spacing;
  const auto cx_max = cx_min - k_button_spacing;
  const auto in_button = [&](std::int32_t cx) { return std::abs(local_x - cx) <= k_button_radius; };
  if (in_button(cx_close)) {
    return HitZone::ButtonClose;
  }
  if (in_button(cx_min)) {
    return HitZone::ButtonMinimize;
  }
  if (in_button(cx_max)) {
    return HitZone::ButtonMaximize;
  }
  return HitZone::TitleBar;
}

}  // namespace

Shell::Shell(drm::Device& dev, drm::gbm::GbmDevice* gbm, const drm::csd::Theme& theme,
             std::size_t plane_budget) noexcept
    : dev_(&dev), gbm_(gbm), theme_(&theme), plane_budget_(plane_budget) {}

bool Shell::spawn_document(std::uint32_t fb_w, std::uint32_t fb_h) {
  if (docs_.size() >= plane_budget_) {
    drm::println("mdi_demo: plane budget ({}) full; cannot spawn more docs", plane_budget_);
    return false;
  }

  drm::csd::SurfaceConfig cfg;
  cfg.width = k_default_doc_w;
  cfg.height = k_default_doc_h;
  auto surface_res = drm::csd::Surface::create(*dev_, gbm_, cfg);
  if (!surface_res) {
    drm::println(stderr, "mdi_demo: csd::Surface::create: {}", surface_res.error().message());
    return false;
  }

  auto doc = std::make_unique<Document>();
  doc->id = next_doc_id_++;
  doc->title = drm::format("Document {}", doc->id);
  doc->width = cfg.width;
  doc->height = cfg.height;

  // Stagger doc origins so successive docs don't fully overlap. Wrap
  // back onto the screen if the diagonal pushes past the right /
  // bottom edge.
  const auto step = static_cast<std::int32_t>(docs_.size());
  doc->x = k_first_doc_x + (step * k_stagger_x);
  doc->y = k_first_doc_y + (step * k_stagger_y);
  if (doc->x + static_cast<std::int32_t>(doc->width) > static_cast<std::int32_t>(fb_w)) {
    doc->x = k_first_doc_x;
  }
  if (doc->y + static_cast<std::int32_t>(doc->height) > static_cast<std::int32_t>(fb_h)) {
    doc->y = k_first_doc_y;
  }

  doc->state.title = doc->title;
  doc->state.focused = false;
  doc->state.hover = drm::csd::HoverButton::None;
  doc->state.dirty = drm::csd::k_dirty_all;
  doc->deco = std::move(*surface_res);
  doc->dirty = true;

  docs_.push_back(std::move(doc));
  const auto new_idx = docs_.size() - 1;
  stack_.push_back(new_idx);
  mark_focused(new_idx);
  return true;
}

void Shell::close_focused() {
  const auto focused = focused_index();
  if (!focused) {
    return;
  }
  const auto idx = *focused;

  // Erase the doc itself.
  docs_.erase(docs_.begin() + static_cast<std::ptrdiff_t>(idx));

  // Rewrite the stack: drop `idx`, decrement every later index by one
  // to track the vector erase.
  std::vector<std::size_t> new_stack;
  new_stack.reserve(stack_.size());
  for (const auto entry : stack_) {
    if (entry == idx) {
      continue;
    }
    new_stack.push_back(entry > idx ? entry - 1 : entry);
  }
  stack_ = std::move(new_stack);

  // Promote the new top of stack (if any) to focused.
  if (!stack_.empty()) {
    mark_focused(stack_.back());
  }
}

void Shell::cycle_focus() {
  if (stack_.size() < 2) {
    return;
  }
  // Move stack.back() to the front of the stack so the next-older
  // doc surfaces as focused. Equivalent to "send focused to back."
  const auto front_doc = stack_.back();
  stack_.pop_back();
  stack_.insert(stack_.begin(), front_doc);
  mark_focused(stack_.back());
  // The previously focused doc needs a repaint too — its rim color
  // flips from focused to blurred.
  docs_[front_doc]->state.focused = false;
  docs_[front_doc]->state.dirty = drm::csd::k_dirty_all;
  docs_[front_doc]->dirty = true;
}

void Shell::set_theme(const drm::csd::Theme& theme) noexcept {
  theme_ = &theme;
  for (auto& doc : docs_) {
    doc->state.dirty = drm::csd::k_dirty_all;
    doc->dirty = true;
  }
}

drm::expected<void, std::error_code> Shell::redraw_dirty() {
  for (auto& doc : docs_) {
    if (!doc->dirty || doc->deco.empty()) {
      continue;
    }
    auto map = doc->deco.paint(drm::MapAccess::ReadWrite);
    if (!map) {
      drm::println(stderr, "mdi_demo: doc {} paint mapping: {}", doc->id, map.error().message());
      continue;
    }
    if (auto r = renderer_.draw(*theme_, doc->state, *map, shadows_); !r) {
      drm::println(stderr, "mdi_demo: doc {} renderer.draw: {}", doc->id, r.error().message());
      continue;
    }
    doc->dirty = false;
    doc->state.dirty = 0;
  }
  return {};
}

std::optional<HitResult> Shell::hit_test(std::int32_t px, std::int32_t py) const {
  // Walk the stack front-to-back so the visually topmost doc wins
  // when documents overlap.
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    const auto idx = *it;
    const auto& doc = *docs_[idx];
    const std::int32_t lx = px - doc.x;
    const std::int32_t ly = py - doc.y;
    if (lx < 0 || ly < 0 || lx >= static_cast<std::int32_t>(doc.width) ||
        ly >= static_cast<std::int32_t>(doc.height)) {
      continue;
    }
    HitResult r;
    r.doc_index = idx;
    r.local_x = lx;
    r.local_y = ly;
    if (ly < k_title_bar_height) {
      r.zone = classify_button(lx, static_cast<std::int32_t>(doc.width));
    } else {
      r.zone = HitZone::Body;
    }
    return r;
  }
  return std::nullopt;
}

bool Shell::on_pointer_press(std::int32_t px, std::int32_t py) {
  const auto hit = hit_test(px, py);
  if (!hit) {
    return false;
  }
  const auto idx = hit->doc_index;
  bool changed = false;

  if (focused_index() != idx) {
    // Promote the clicked doc to the top of the stack.
    stack_.erase(std::remove(stack_.begin(), stack_.end(), idx), stack_.end());
    stack_.push_back(idx);
    mark_focused(idx);
    changed = true;
  }

  switch (hit->zone) {
    case HitZone::ButtonClose:
      close_focused();
      return true;
    case HitZone::ButtonMinimize:
    case HitZone::ButtonMaximize:
      // V1: traffic-light Minimize / Maximize are visual-only — the
      // renderer paints them, the shell ignores them. The plan's M5
      // (or a follow-up) wires them up; until then, clicking just
      // focuses the doc.
      return changed;
    case HitZone::TitleBar:
      dragging_ = true;
      drag_doc_idx_ = idx;
      drag_offset_x_ = px - docs_[idx]->x;
      drag_offset_y_ = py - docs_[idx]->y;
      return changed;
    case HitZone::Body:
    case HitZone::None:
      return changed;
  }
  return changed;
}

void Shell::on_pointer_release() {
  dragging_ = false;
}

bool Shell::on_pointer_motion(std::int32_t px, std::int32_t py) {
  if (!dragging_ || drag_doc_idx_ >= docs_.size()) {
    return false;
  }
  auto& doc = *docs_[drag_doc_idx_];
  doc.x = px - drag_offset_x_;
  doc.y = py - drag_offset_y_;
  // Move only — no decoration repaint (geometry is a presenter
  // property, not a renderer input).
  return true;
}

std::vector<drm::csd::SurfaceRef> Shell::surface_refs() const {
  std::vector<drm::csd::SurfaceRef> refs;
  refs.resize(plane_budget_);
  for (std::size_t i = 0; i < stack_.size() && i < plane_budget_; ++i) {
    const auto& doc = *docs_[stack_[i]];
    refs[i].surface = &doc.deco;
    refs[i].x = doc.x;
    refs[i].y = doc.y;
  }
  return refs;
}

void Shell::mark_focused(std::size_t doc_index) noexcept {
  for (std::size_t i = 0; i < docs_.size(); ++i) {
    const bool was_focused = docs_[i]->state.focused;
    const bool is_focused = (i == doc_index);
    if (was_focused != is_focused) {
      docs_[i]->state.focused = is_focused;
      docs_[i]->state.dirty |= drm::csd::k_dirty_focus;
      docs_[i]->dirty = true;
    }
  }
}

std::optional<std::size_t> Shell::focused_index() const noexcept {
  if (stack_.empty()) {
    return std::nullopt;
  }
  return stack_.back();
}

}  // namespace mdi_demo