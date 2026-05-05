// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "shell.hpp"

#include "csd/animator.hpp"
#include "csd/presenter.hpp"
#include "csd/renderer.hpp"
#include "csd/surface.hpp"
#include "csd/theme.hpp"
#include "csd/window_state.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>

#include <algorithm>
#include <chrono>
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

// True iff (local_x, local_y) lands inside a circle centered at
// (cx, button_cy) with radius `r`. Axis-aligned bbox check rather
// than Euclidean distance — at r ≈ 7 px the corner pixels would
// otherwise need a sqrt per probe and the visual difference is below
// the cursor-hotspot precision the user sees.
bool in_button_box(std::int32_t local_x, std::int32_t local_y, std::int32_t cx,
                   std::int32_t button_cy, std::int32_t r) {
  return std::abs(local_x - cx) <= r && std::abs(local_y - button_cy) <= r;
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
  docs_[front_doc]->anim.retarget_focus(false);
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
  // when documents overlap. Geometry comes from
  // drm::csd::decoration_geometry so panel + button positions match
  // exactly what csd::Renderer paints — anything else here would be
  // duplicated math that drifts on every theme tweak.
  for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
    const auto idx = *it;
    const auto& doc = *docs_[idx];
    const std::int32_t lx = px - doc.x;
    const std::int32_t ly = py - doc.y;
    if (lx < 0 || ly < 0 || lx >= static_cast<std::int32_t>(doc.width) ||
        ly >= static_cast<std::int32_t>(doc.height)) {
      continue;
    }
    const auto geom = drm::csd::decoration_geometry(*theme_, doc.width, doc.height);
    // Reject clicks on the soft drop-shadow halo: the panel rect is
    // the interactive surface; outside it is purely decorative.
    if (lx < geom.panel_x || ly < geom.panel_y || lx >= geom.panel_x + geom.panel_w ||
        ly >= geom.panel_y + geom.panel_h) {
      continue;
    }
    HitResult r;
    r.doc_index = idx;
    r.local_x = lx;
    r.local_y = ly;
    if (ly < geom.panel_y + geom.title_bar_height) {
      if (in_button_box(lx, ly, geom.close_cx, geom.button_cy, geom.button_radius)) {
        r.zone = HitZone::ButtonClose;
      } else if (in_button_box(lx, ly, geom.minimize_cx, geom.button_cy, geom.button_radius)) {
        r.zone = HitZone::ButtonMinimize;
      } else if (in_button_box(lx, ly, geom.maximize_cx, geom.button_cy, geom.button_radius)) {
        r.zone = HitZone::ButtonMaximize;
      } else {
        r.zone = HitZone::TitleBar;
      }
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
  if (dragging_ && drag_doc_idx_ < docs_.size()) {
    auto& doc = *docs_[drag_doc_idx_];
    doc.x = px - drag_offset_x_;
    doc.y = py - drag_offset_y_;
    // Move only — no decoration repaint (geometry is a presenter
    // property, not a renderer input). Skip hover updates while
    // dragging: the user has committed to one doc and shouldn't
    // light up other docs' buttons.
    return true;
  }

  // Hover wiring: hit-test, then retarget every doc's animator. The
  // doc currently under the pointer (if any) gets the hovered
  // button; everyone else gets None. The animator drives the
  // fill→hover cross-fade; the next tick_animations() will surface
  // the change as a frame_dirty trigger so we don't have to return
  // true here for hover-only motion.
  const auto hit = hit_test(px, py);
  for (std::size_t i = 0; i < docs_.size(); ++i) {
    drm::csd::HoverButton target = drm::csd::HoverButton::None;
    if (hit && hit->doc_index == i) {
      switch (hit->zone) {
        case HitZone::ButtonClose:
          target = drm::csd::HoverButton::Close;
          break;
        case HitZone::ButtonMinimize:
          target = drm::csd::HoverButton::Minimize;
          break;
        case HitZone::ButtonMaximize:
          target = drm::csd::HoverButton::Maximize;
          break;
        case HitZone::TitleBar:
        case HitZone::Body:
        case HitZone::None:
          target = drm::csd::HoverButton::None;
          break;
      }
    }
    docs_[i]->anim.retarget_hover(target);
  }
  return false;
}

bool Shell::tick_animations(std::chrono::milliseconds dt) {
  if (theme_->animation_duration_ms <= 0) {
    return false;
  }
  const auto duration = std::chrono::milliseconds(theme_->animation_duration_ms);
  bool any_active = false;
  for (auto& doc : docs_) {
    const bool was_active = doc->anim.active();
    if (!was_active) {
      continue;
    }
    doc->anim.tick(dt, duration);
    doc->anim.apply_to(doc->state);
    doc->state.dirty |= drm::csd::k_dirty_animation;
    doc->dirty = true;
    any_active = true;
  }
  return any_active;
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
      docs_[i]->anim.retarget_focus(is_focused);
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