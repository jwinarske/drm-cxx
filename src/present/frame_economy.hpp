// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// frame_economy.hpp — the per-frame present decision: Skip, DamagedCommit, or
// FullCommit, from idle/new-frame state + whether the producer supplied damage.
//
// The win is the Skip: when nothing changed since the last commit, the present
// loop issues no atomic commit at all — no page flip, no scanout reprogram. That
// is a power saving on every panel (and lets a PSR-capable panel stay in
// self-refresh); it is therefore UNCONDITIONAL and never consults any PSR
// capability — not flipping is a win regardless.
//
// Pure decision + counters; no DRM state. Drive it from the present loop:
//
//   auto d = econ.decide(content_changed, damage_available);
//   if (d.action == FrameAction::Skip) continue;            // no commit this vblank
//   producer/scene present, with damage iff !d.full;
//
// Not thread-safe: one present loop owns it.

#pragma once

#include <cstdint>

namespace drm::present {

enum class FrameAction : std::uint8_t {
  Skip,    // nothing changed — issue no commit this frame
  Commit,  // commit the frame (see FrameDecision::full)
};

struct FrameDecision {
  FrameAction action{FrameAction::Commit};
  // When action == Commit: full == true means a full-frame commit (omit
  // FB_DAMAGE_CLIPS); false means a damaged commit (the producer's damage rects
  // are valid and should be emitted).
  bool full{true};
};

class FrameEconomy {
 public:
  FrameEconomy() = default;

  // Decide this frame's action.
  //   content_changed  — a layer produced a new frame, or the scene mutated,
  //                      since the last committed frame.
  //   damage_available — the producer supplied a bounded per-frame damage rect
  //                      set (so a damaged commit is possible).
  //
  // The first call after construction or force_full() always commits full (the
  // scanout buffer's contents are otherwise undefined / a modeset just happened).
  [[nodiscard]] FrameDecision decide(bool content_changed, bool damage_available) noexcept {
    if (first_ || force_full_) {
      first_ = false;
      force_full_ = false;
      ++committed_;
      return {FrameAction::Commit, /*full=*/true};
    }
    if (!content_changed) {
      ++skipped_;
      return {FrameAction::Skip, /*full=*/false};
    }
    ++committed_;
    return {FrameAction::Commit, /*full=*/!damage_available};
  }

  // Force the next decide() to commit full — call after a modeset / mode change
  // / rebind, where the previous scanout contents no longer apply.
  void force_full() noexcept { force_full_ = true; }

  [[nodiscard]] std::uint64_t committed() const noexcept { return committed_; }
  [[nodiscard]] std::uint64_t skipped() const noexcept { return skipped_; }

 private:
  bool first_{true};
  bool force_full_{false};
  std::uint64_t committed_{0};
  std::uint64_t skipped_{0};
};

}  // namespace drm::present
