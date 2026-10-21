// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "source_mirror.hpp"

#include "../buffer_source.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::scene::detail {

namespace {

// One scene's view of the mirrored source.
class MirrorConsumer final : public LayerBufferSource {
 public:
  MirrorConsumer(std::shared_ptr<SourceMirror> mirror, std::size_t id) noexcept
      : mirror_(std::move(mirror)), id_(id) {}
  MirrorConsumer(const MirrorConsumer&) = delete;
  MirrorConsumer& operator=(const MirrorConsumer&) = delete;
  MirrorConsumer(MirrorConsumer&&) = delete;
  MirrorConsumer& operator=(MirrorConsumer&&) = delete;
  ~MirrorConsumer() override { mirror_->drop(id_); }

  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override {
    return mirror_->acquire(id_);
  }
  void release(AcquiredBuffer acquired) noexcept override {
    mirror_->release(id_, std::move(acquired), std::nullopt);
  }
  void release_with_fence(AcquiredBuffer acquired,
                          std::optional<drm::sync::SyncFence> release_fence) noexcept override {
    mirror_->release(id_, std::move(acquired), std::move(release_fence));
  }
  [[nodiscard]] bool wants_release_fence() const noexcept override {
    return mirror_->wants_release_fence();
  }
  [[nodiscard]] bool has_fresh_content() const noexcept override {
    return mirror_->has_fresh_content(id_);
  }
  void on_retired() noexcept override { mirror_->retire(id_); }
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return mirror_->inner().binding_model();
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return mirror_->inner().format(); }
  // Composition reads the source's current buffer, which is the frame just
  // handed out: a consumer maps after its own acquire.
  [[nodiscard]] drm::expected<drm::BufferMapping, std::error_code> map(
      drm::MapAccess access) override {
    return mirror_->inner().map(access);
  }
  [[nodiscard]] drm::expected<DmaBufDesc, std::error_code> export_dma_buf() override {
    return mirror_->inner().export_dma_buf();
  }
  drm::expected<void, std::error_code> bind_to_plane(std::uint32_t plane_id) override {
    return mirror_->inner().bind_to_plane(plane_id);
  }
  void unbind_from_plane(std::uint32_t plane_id) noexcept override {
    mirror_->inner().unbind_from_plane(plane_id);
  }
  void on_session_paused() noexcept override { mirror_->on_session_paused(); }
  drm::expected<void, std::error_code> on_session_resumed(const drm::Device& new_dev) override {
    return mirror_->on_session_resumed(new_dev);
  }

 private:
  std::shared_ptr<SourceMirror> mirror_;
  std::size_t id_;
};

}  // namespace

SourceMirror::SourceMirror(std::shared_ptr<LayerBufferSource> inner) noexcept
    : inner_(std::move(inner)) {}

std::shared_ptr<SourceMirror> SourceMirror::create(std::shared_ptr<LayerBufferSource> inner) {
  return std::shared_ptr<SourceMirror>(new SourceMirror(std::move(inner)));
}

SourceMirror::~SourceMirror() {
  // Every consumer is gone, and their scenes released what they held.
  for (auto& f : frames_) {
    inner_->release(std::move(f->buffer));
  }
  if (!inner_retired_) {
    inner_->on_retired();
  }
}

std::unique_ptr<LayerBufferSource> SourceMirror::make_consumer() {
  consumers_.push_back(Consumer{true, false, 0});
  return std::make_unique<MirrorConsumer>(shared_from_this(), consumers_.size() - 1);
}

SourceMirror::Frame* SourceMirror::current_frame() noexcept {
  for (auto& f : frames_) {
    if (f->current) {
      return f.get();
    }
  }
  return nullptr;
}

drm::expected<AcquiredBuffer, std::error_code> SourceMirror::hand_out(Frame& f, const Consumer& c) {
  AcquiredBuffer out;
  out.fb_id = f.buffer.fb_id;
  out.opaque = &f;
  if (f.buffer.acquire_fence.has_value() && f.buffer.acquire_fence->valid()) {
    // Each scene owns the fence it is given; hand each its own descriptor.
    auto dup = drm::sync::SyncFence::import_fd(f.buffer.acquire_fence->fd());
    if (!dup) {
      return drm::unexpected<std::error_code>(dup.error());
    }
    out.acquire_fence = std::move(*dup);
  }
  // Damage is relative to the previous frame: only a consumer that showed it
  // can use it.
  if (c.last_seq + 1 == f.seq) {
    out.damage = f.buffer.damage;
  }
  return out;
}

drm::expected<AcquiredBuffer, std::error_code> SourceMirror::acquire(std::size_t id) {
  auto& c = consumers_.at(id);
  Frame* cur = current_frame();
  if (cur == nullptr || c.last_seq >= cur->seq) {
    auto got = inner_->acquire();
    if (!got) {
      return drm::unexpected<std::error_code>(got.error());
    }
    drop_current();
    auto f = std::make_unique<Frame>();
    f->seq = next_seq_++;
    f->buffer = std::move(*got);
    f->current = true;
    frames_.push_back(std::move(f));
    cur = frames_.back().get();
  }
  auto out = hand_out(*cur, c);
  if (out) {
    ++cur->holders;
    c.last_seq = cur->seq;
  }
  return out;
}

void SourceMirror::release(std::size_t /*id*/, AcquiredBuffer acquired,
                           std::optional<drm::sync::SyncFence> release_fence) noexcept {
  const auto it = std::find_if(frames_.begin(), frames_.end(),
                               [&](const auto& f) { return f.get() == acquired.opaque; });
  if (it == frames_.end() || (*it)->holders == 0) {
    return;
  }
  Frame& f = **it;
  if (release_fence.has_value() && release_fence->valid()) {
    // The buffer is free once every CRTC that showed it has moved on.
    if (f.release_fence.has_value()) {
      if (!f.release_fence->merge(std::move(*release_fence))) {
        // Unmerged, the fence would free the buffer early: drop it and let
        // the release edge stand in.
        f.release_fence.reset();
      }
    } else {
      f.release_fence = std::move(release_fence);
    }
  }
  --f.holders;
  return_if_done(static_cast<std::size_t>(it - frames_.begin()));
}

void SourceMirror::return_if_done(std::size_t index) noexcept {
  const Frame& f = *frames_[index];
  if (f.holders != 0 || f.current) {
    return;
  }
  auto frame = std::move(frames_[index]);
  frames_.erase(frames_.begin() + static_cast<std::ptrdiff_t>(index));
  if (frame->release_fence.has_value()) {
    inner_->release_with_fence(std::move(frame->buffer), std::move(frame->release_fence));
  } else {
    inner_->release(std::move(frame->buffer));
  }
}

void SourceMirror::drop_current() noexcept {
  for (std::size_t i = 0; i < frames_.size(); ++i) {
    if (frames_[i]->current) {
      frames_[i]->current = false;
      return_if_done(i);
      return;
    }
  }
}

bool SourceMirror::wants_release_fence() const noexcept {
  return inner_->wants_release_fence();
}

bool SourceMirror::has_fresh_content(std::size_t id) const noexcept {
  for (const auto& f : frames_) {
    if (f->current && consumers_[id].last_seq < f->seq) {
      return true;
    }
  }
  return inner_->has_fresh_content();
}

void SourceMirror::retire(std::size_t id) noexcept {
  consumers_[id].retired = true;
  // The source is retired once no live consumer will acquire again.
  const bool all = std::all_of(consumers_.begin(), consumers_.end(),
                               [](const Consumer& c) { return !c.live || c.retired; });
  if (all && !inner_retired_) {
    inner_retired_ = true;
    drop_current();
    inner_->on_retired();
  }
}

void SourceMirror::drop(std::size_t id) noexcept {
  consumers_[id].live = false;
}

void SourceMirror::on_session_paused() noexcept {
  if (paused_) {
    return;
  }
  paused_ = true;
  // The newest frame belongs to the session being lost; nobody takes it now.
  drop_current();
  inner_->on_session_paused();
}

drm::expected<void, std::error_code> SourceMirror::on_session_resumed(const drm::Device& new_dev) {
  if (paused_) {
    paused_ = false;
    resume_result_ = inner_->on_session_resumed(new_dev);
  }
  return resume_result_;
}

}  // namespace drm::scene::detail
