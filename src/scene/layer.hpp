// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// layer.hpp — live, scene-owned layer state.
//
// Created by LayerScene::add_layer (which is the only code that can
// instantiate one — the constructor is private + LayerScene is a
// friend). Mutated through setters that mark the layer dirty so the
// next commit re-issues the affected plane properties. Destroyed when
// LayerScene::remove_layer is called or the scene itself is destroyed.

#pragma once

#include "buffer_source.hpp"
#include "display_params.hpp"
#include "layer_handle.hpp"

#include <drm-cxx/planes/layer.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace drm::scene {

class LayerScene;

class Layer {
 public:
  Layer(const Layer&) = delete;
  Layer& operator=(const Layer&) = delete;
  Layer(Layer&&) noexcept = default;
  Layer& operator=(Layer&&) noexcept = default;
  ~Layer() = default;

  [[nodiscard]] LayerHandle handle() const noexcept { return handle_; }

  [[nodiscard]] LayerBufferSource& source() noexcept { return *source_; }
  [[nodiscard]] const LayerBufferSource& source() const noexcept { return *source_; }

  [[nodiscard]] const DisplayParams& display() const noexcept { return display_; }
  [[nodiscard]] drm::planes::ContentType content_type() const noexcept { return content_type_; }
  [[nodiscard]] std::uint32_t update_hint_hz() const noexcept { return update_hint_hz_; }

  // ── Display-side mutation ──────────────────────────────────────────
  // Each setter flips the dirty flag. The scene clears it after a
  // successful commit via mark_clean(). The granularity is per-layer,
  // not per-field — Phase 2.2's property minimization refines that.

  void set_src_rect(Rect r) noexcept {
    display_.src_rect = r;
    dirty_ = true;
  }
  void set_dst_rect(Rect r) noexcept {
    display_.dst_rect = r;
    dirty_ = true;
  }
  void set_rotation(std::uint64_t rot) noexcept {
    display_.rotation = rot;
    dirty_ = true;
  }
  void set_alpha(std::uint16_t a) noexcept {
    display_.alpha = a;
    dirty_ = true;
  }
  void set_zpos(std::optional<int> z) noexcept {
    display_.zpos = z;
    dirty_ = true;
  }

  [[nodiscard]] bool is_dirty() const noexcept { return dirty_; }
  void mark_clean() noexcept { dirty_ = false; }

  /// Construct a Layer. In practice this is only called from
  /// LayerScene::add_layer — the signature is deliberately awkward
  /// (needs a LayerHandle the scene mints, takes an rvalue source) to
  /// steer consumers toward the add_layer path. Exposed in the public
  /// API to avoid friending a pimpl'd nested class.
  Layer(LayerHandle handle, std::unique_ptr<LayerBufferSource> source, const DisplayParams& display,
        drm::planes::ContentType content_type, std::uint32_t update_hint_hz) noexcept
      : handle_(handle),
        source_(std::move(source)),
        display_(display),
        content_type_(content_type),
        update_hint_hz_(update_hint_hz) {}

 private:
  LayerHandle handle_;
  std::unique_ptr<LayerBufferSource> source_;
  DisplayParams display_;
  drm::planes::ContentType content_type_;
  std::uint32_t update_hint_hz_;

  // Starts true so the first commit writes every property on every
  // freshly-added layer — no stale plane state carries over from
  // whatever used the plane before the scene did.
  bool dirty_{true};
};

}  // namespace drm::scene
