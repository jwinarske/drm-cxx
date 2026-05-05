// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// shell.hpp — multi-document shell for mdi_demo.
//
// The Shell owns a vector of `Document`s — each document is one
// movable, focusable glass-themed window — plus the front-to-back
// stack that controls plane assignment for the Tier 0 (Plane)
// presenter. Each Document carries one `csd::Surface` that backs the
// glass panel; the renderer repaints it on focus / hover / theme
// changes (the `dirty` flag), the presenter binds it to one reserved
// overlay per frame.
//
// The Shell is intentionally a small, single-CRTC abstraction: zero
// hotplug awareness, no resize, no animation, no per-doc content
// surface. It exists to exercise `csd::Renderer` + `csd::Surface` +
// `OverlayReservation` + `PlanePresenter` end-to-end on real hardware
// and to give the v1 reader a concrete answer to "what does an MDI
// shell look like on top of csd?"

#pragma once

#include "csd/animator.hpp"
#include "csd/presenter.hpp"
#include "csd/renderer.hpp"
#include "csd/shadow_cache.hpp"
#include "csd/surface.hpp"
#include "csd/theme.hpp"
#include "csd/window_state.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}  // namespace drm
namespace drm::gbm {
class GbmDevice;
}  // namespace drm::gbm

namespace mdi_demo {

/// One MDI document — its decoration buffer plus position / state.
struct Document {
  std::uint32_t id{0};
  std::string title;
  std::int32_t x{0};
  std::int32_t y{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  drm::csd::WindowState state{};
  drm::csd::Surface deco;
  drm::csd::WindowAnim anim;
  bool dirty{true};
};

/// Hit-test result. The caller branches on `zone` to decide whether
/// the click should focus the doc, start a drag, or trigger a button
/// action.
enum class HitZone : std::uint8_t {
  None,
  TitleBar,
  Body,
  ButtonClose,
  ButtonMinimize,
  ButtonMaximize,
};

struct HitResult {
  std::size_t doc_index{0};  // index into Shell::documents()
  HitZone zone{HitZone::None};
  std::int32_t local_x{0};  // pointer position relative to the doc's origin
  std::int32_t local_y{0};
};

class Shell {
 public:
  /// `dev` and `theme` must outlive the shell. `gbm` may be null —
  /// Surface::create then takes the dumb-only path. `plane_budget` is
  /// the number of overlays the caller has already reserved; the
  /// shell's `surface_refs()` always returns exactly `plane_budget`
  /// entries, padded with vacant slots.
  Shell(drm::Device& dev, drm::gbm::GbmDevice* gbm, const drm::csd::Theme& theme,
        std::size_t plane_budget) noexcept;

  /// Spawn one new document, sized + positioned at a stagger offset
  /// suitable for the screen `fb_w x fb_h`. New docs land at the top
  /// of the focus stack. Returns false when the plane budget is full
  /// or Surface allocation fails.
  bool spawn_document(std::uint32_t fb_w, std::uint32_t fb_h);

  /// Remove the focused document. No-op when there are none.
  void close_focused();

  /// Move the focused document to the back of the z-stack.
  /// Ctrl+Tab cycles forward through the stack via this entry point.
  void cycle_focus();

  /// Replace the active theme. Marks every document dirty so the
  /// next `redraw_dirty()` repaints with the new colors.
  void set_theme(const drm::csd::Theme& theme) noexcept;

  /// Repaint every document with `dirty` set. Logs and continues on
  /// per-doc paint failures so a single bad mapping doesn't halt the
  /// shell. Returns success unless the shell itself is in an unusable
  /// state.
  drm::expected<void, std::error_code> redraw_dirty();

  /// Pure hit-test against the current document stack. Walks
  /// front-to-back so the topmost doc wins. Returns nullopt when the
  /// pointer hits empty desktop.
  [[nodiscard]] std::optional<HitResult> hit_test(std::int32_t px, std::int32_t py) const;

  // ── Pointer wiring ───────────────────────────────────────────────
  //
  // The main loop accumulates motion + button events from libinput
  // and replays them here. The shell tracks drag state internally —
  // no caller bookkeeping needed beyond passing in (px, py).

  /// Left-button press at (px, py). Either focuses + starts dragging
  /// the title bar, fires a traffic-light action, or does nothing.
  /// Returns true when the press changed shell state (caller should
  /// treat the frame as dirty).
  bool on_pointer_press(std::int32_t px, std::int32_t py);

  /// Left-button release. Ends any active drag.
  void on_pointer_release();

  /// Pointer motion. During an active drag this moves the focused
  /// doc; otherwise it retargets each doc's hover button (used by the
  /// animator to drive the fill→hover cross-fade). Returns true if a
  /// doc moved (drag path) so the caller can mark the frame dirty
  /// for geometry; hover-only changes do not return true — they
  /// surface through the next `tick_animations()` once the animator
  /// reports active.
  bool on_pointer_motion(std::int32_t px, std::int32_t py);

  /// Advance every doc's animator by `dt` and mirror progress into the
  /// per-doc WindowState. Returns true if any animation is still in
  /// flight (or settled this tick), so the main loop can keep
  /// frame_dirty set across the tween. The duration comes from the
  /// active theme's `animation_duration_ms`; non-positive values
  /// short-circuit (animations effectively disabled).
  bool tick_animations(std::chrono::milliseconds dt);

  /// Build the SurfaceRef list for `PlanePresenter::apply`. Length is
  /// always `plane_budget()`; entries beyond the live doc count are
  /// vacant (`surface == nullptr`) so the presenter disarms those
  /// reserved planes.
  [[nodiscard]] std::vector<drm::csd::SurfaceRef> surface_refs() const;

  // ── Diagnostics / accessors ──────────────────────────────────────

  [[nodiscard]] std::size_t plane_budget() const noexcept { return plane_budget_; }
  [[nodiscard]] std::size_t document_count() const noexcept { return docs_.size(); }
  [[nodiscard]] bool quit_requested() const noexcept { return quit_; }

  /// True between an `on_pointer_press` that hit a TitleBar and the
  /// matching `on_pointer_release`. Lets the main loop swap the
  /// cursor sprite to a "grabbing" shape for the duration of the drag.
  [[nodiscard]] bool is_dragging() const noexcept { return dragging_; }
  void request_quit() noexcept { quit_ = true; }

  /// Stack ordered back-to-front (last entry = focused). Useful for
  /// diagnostic dumps and for the v1 README's "describe what's
  /// happening" path.
  [[nodiscard]] drm::span<const std::size_t> stack() const noexcept {
    return {stack_.data(), stack_.size()};
  }

 private:
  void mark_focused(std::size_t doc_index) noexcept;
  [[nodiscard]] std::optional<std::size_t> focused_index() const noexcept;

  drm::Device* dev_;
  drm::gbm::GbmDevice* gbm_;
  const drm::csd::Theme* theme_;
  std::size_t plane_budget_;

  drm::csd::Renderer renderer_;
  drm::csd::ShadowCache shadows_;

  // Stable storage so SurfaceRef::surface (a Surface*) survives
  // vector growth and document churn.
  std::vector<std::unique_ptr<Document>> docs_;

  // Z-order, back to front. `stack_.back()` is the focused doc.
  std::vector<std::size_t> stack_;

  std::uint32_t next_doc_id_{1};

  // Drag state. Active only between on_pointer_press(TitleBar) and
  // on_pointer_release().
  bool dragging_{false};
  std::size_t drag_doc_idx_{0};
  std::int32_t drag_offset_x_{0};
  std::int32_t drag_offset_y_{0};

  bool quit_{false};
};

}  // namespace mdi_demo