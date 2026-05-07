// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// session_pump.hpp — libseat session pause/resume boilerplate shared by
// the scene-based examples.
//
// What it solves: every libseat-using example duplicates the same
// VT-switch dance — pause callback (drop FB ids before the kernel revokes
// the fd, suspend libinput, mark the session paused so the main loop
// stops committing), resume callback (filter on /dev/dri/* to avoid the
// "fires once per device" multi-fire pitfall, capture the new fd), and
// then a main-loop branch that swaps Device::from_fd, re-enables
// universal-planes + atomic, re-imports the scene, rebuilds the
// PageFlip, and emits a resume commit.
//
// The first three steps (pause callback, resume callback, fd swap +
// scene re-import) are mechanical and identical across examples. They
// live here. The example-specific tail — rebuilding PageFlip, repainting
// dumb-buffer-backed sources into fresh mappings, committing — stays
// inline so the example's resume path reads top-down.
//
// Header-only by intent, mirroring open_output.hpp / vt_switch.hpp.

#pragma once

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/session/seat.hpp>

#include <functional>
#include <string_view>
#include <system_error>
#include <utility>

namespace drm::examples {

/// State the session pump manipulates from libseat callbacks and the
/// main-loop apply step. Caller-owned; the pump captures by reference.
///
/// `paused`            — true between pause_cb fire and the next
///                       successful apply_pending_resume. The main loop
///                       should refuse to commit while this is set.
/// `flip_pending`      — true between a successful commit and the
///                       page-flip handler running. Cleared in pause_cb
///                       because the page-flip event won't fire after
///                       the fd is revoked.
/// `pending_resume_fd` — set by resume_cb to the new DRM fd. Cleared by
///                       apply_pending_resume after the swap.
struct SessionPumpState {
  bool paused = false;
  bool flip_pending = false;
  int pending_resume_fd = -1;
};

/// Wire pause/resume callbacks on `seat` so the standard pause/resume
/// transitions update `state`, drop scene FBs, and (when `input` is
/// non-null) suspend/resume libinput. `state`, `scene`, and `input`
/// must outlive `seat`.
///
/// The resume callback filters on `/dev/dri/` because libseat fires the
/// resume callback once per managed device — DRM card AND every input
/// fd. A naive `pending_resume_fd = new_fd` assignment would let an
/// input-device resume overwrite the card fd we actually need to swap;
/// the input::Seat handles its own input-device fds via input->resume().
///
/// `on_pause_extra` runs at the end of the pause callback — after the
/// scene has dropped its FBs and libinput has been suspended. Use it
/// for example-specific pause work that must complete synchronously
/// before the kernel revokes the fd: pausing a gstreamer pipeline,
/// stopping a key-repeat timer, flipping an "active" flag that other
/// threads consult.
///
/// `on_resume_extra` runs at the end of the resume callback — after
/// `pending_resume_fd` has been latched and libinput has been resumed.
/// The fd swap itself happens later, in `apply_pending_resume`.
inline void wire_session_callbacks(drm::session::Seat& seat, drm::scene::LayerScene& scene,
                                   SessionPumpState& state, drm::input::Seat* input = nullptr,
                                   std::function<void()> on_pause_extra = {},
                                   std::function<void()> on_resume_extra = {}) {
  seat.set_pause_callback([&scene, &state, input, on_pause = std::move(on_pause_extra)]() {
    state.paused = true;
    state.flip_pending = false;
    scene.on_session_paused();
    if (input != nullptr) {
      (void)input->suspend();
    }
    if (on_pause) {
      on_pause();
    }
  });
  seat.set_resume_callback(
      [&state, input, on_resume = std::move(on_resume_extra)](std::string_view path, int new_fd) {
        if (path.substr(0, 9) != "/dev/dri/") {
          return;
        }
        state.pending_resume_fd = new_fd;
        state.paused = false;
        if (input != nullptr) {
          (void)input->resume();
        }
        if (on_resume) {
          on_resume();
        }
      });
}

/// If `state.pending_resume_fd` is set: replace `device`'s fd in place,
/// re-enable universal planes + atomic, and call
/// `scene.on_session_resumed(device)`. On success clears
/// `pending_resume_fd` and returns `true`; the caller must then rebuild
/// PageFlip, repaint owned mappings, and emit the resume commit.
///
/// Returns `false` when there is no pending resume (the caller's main
/// loop should fall through to its normal per-frame work).
///
/// Any failure logs a one-line diagnostic to stderr and returns the
/// error code so the caller can decide whether to break out of the
/// loop. `pending_resume_fd` is cleared in that case too — a stuck fd
/// would just refire the failed path on the next iteration.
[[nodiscard]] inline drm::expected<bool, std::error_code> apply_pending_resume(
    SessionPumpState& state, drm::Device& device, drm::scene::LayerScene& scene) {
  if (state.pending_resume_fd == -1) {
    return false;
  }
  int const new_fd = state.pending_resume_fd;
  state.pending_resume_fd = -1;
  device = drm::Device::from_fd(new_fd);
  if (auto r = device.enable_universal_planes(); !r) {
    drm::println(stderr, "resume: enable_universal_planes: {}", r.error().message());
    return drm::unexpected<std::error_code>(r.error());
  }
  if (auto r = device.enable_atomic(); !r) {
    drm::println(stderr, "resume: enable_atomic: {}", r.error().message());
    return drm::unexpected<std::error_code>(r.error());
  }
  if (auto r = scene.on_session_resumed(device); !r) {
    drm::println(stderr, "resume: on_session_resumed: {}", r.error().message());
    return drm::unexpected<std::error_code>(r.error());
  }
  return true;
}

}  // namespace drm::examples