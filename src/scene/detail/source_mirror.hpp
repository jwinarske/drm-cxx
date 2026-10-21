// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Private (not installed): one LayerBufferSource shown on several scenes.
//
// SceneSet gives each scene a layer of its own, so a source mirrored across N
// CRTCs is acquired N times per frame. A static buffer (DumbBufferSource,
// ExternalDmaBufSource) hands back the same framebuffer each time, but a
// per-frame ring (V4L2 capture and decode, GbmSurfaceSource, GstAppsinkSource,
// ExternalDmaBufRing) advances on every acquire: the CRTCs would show
// different frames and the ring would drop the ones in between.
//
// SourceMirror sits in front of the source and hands out one consumer per
// scene. The source is acquired once per frame and the buffer shared:
//   * A consumer's acquire() takes the current frame if it has not had it
//     yet; once it has, its next acquire() advances the source. Scenes that
//     commit together (SceneSet::commit) therefore show the same frame, and
//     one that commits alone still sees new frames.
//   * The source gets a buffer back when every consumer that took it has
//     released it and a newer frame has replaced it. Release fences from the
//     consumers are merged, so a GPU producer waits for every CRTC.
//   * Session pause/resume reach the source once per cycle, not once per
//     scene.
// Damage is passed to a consumer only if it also had the previous frame;
// otherwise the frame counts as full damage.
//
// Single-threaded, like the scenes it serves: every call comes from the
// thread that drives SceneSet.

#pragma once

#include "../buffer_source.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene::detail {

class SourceMirror : public std::enable_shared_from_this<SourceMirror> {
 public:
  [[nodiscard]] static std::shared_ptr<SourceMirror> create(
      std::shared_ptr<LayerBufferSource> inner);

  SourceMirror(const SourceMirror&) = delete;
  SourceMirror& operator=(const SourceMirror&) = delete;
  SourceMirror(SourceMirror&&) = delete;
  SourceMirror& operator=(SourceMirror&&) = delete;
  ~SourceMirror();

  /// A source for one scene's layer. The consumer keeps the mirror alive.
  [[nodiscard]] std::unique_ptr<LayerBufferSource> make_consumer();

  // Consumer entry points; `id` names the consumer.
  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire(std::size_t id);
  void release(std::size_t id, AcquiredBuffer acquired,
               std::optional<drm::sync::SyncFence> release_fence) noexcept;
  [[nodiscard]] bool wants_release_fence() const noexcept;
  [[nodiscard]] bool has_fresh_content(std::size_t id) const noexcept;
  void retire(std::size_t id) noexcept;
  void drop(std::size_t id) noexcept;
  void on_session_paused() noexcept;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(const drm::Device& new_dev);
  [[nodiscard]] LayerBufferSource& inner() noexcept { return *inner_; }

 private:
  explicit SourceMirror(std::shared_ptr<LayerBufferSource> inner) noexcept;

  // One buffer acquired from the source, shared by the consumers that took it.
  struct Frame {
    std::uint64_t seq{0};
    AcquiredBuffer buffer;
    std::size_t holders{0};  // consumers yet to release it
    bool current{false};     // the newest frame; held for consumers yet to take it
    std::optional<drm::sync::SyncFence> release_fence;
  };
  struct Consumer {
    bool live{false};
    bool retired{false};
    std::uint64_t last_seq{0};  // newest frame taken; 0 = none
  };

  [[nodiscard]] Frame* current_frame() noexcept;
  [[nodiscard]] static drm::expected<AcquiredBuffer, std::error_code> hand_out(Frame& f,
                                                                               const Consumer& c);
  void return_if_done(std::size_t index) noexcept;
  void drop_current() noexcept;

  std::shared_ptr<LayerBufferSource> inner_;
  std::vector<std::unique_ptr<Frame>> frames_;
  std::vector<Consumer> consumers_;
  std::uint64_t next_seq_{1};
  bool paused_{false};
  bool inner_retired_{false};
  drm::expected<void, std::error_code> resume_result_;
};

}  // namespace drm::scene::detail
