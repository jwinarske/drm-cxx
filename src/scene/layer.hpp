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

/// Scene-owned layer state. Returned by `LayerScene::get_layer`; never
/// constructed directly by consumers (the constructor is exposed for
/// pimpl reasons but is intended for `LayerScene::add_layer` only).
/// Accessors return the source, the current `DisplayParams`, and the
/// allocator hints; setters mutate `DisplayParams` and mark the layer
/// dirty so the next commit re-emits the affected plane properties.
class Layer {
 public:
  Layer(const Layer&) = delete;
  Layer& operator=(const Layer&) = delete;
  Layer(Layer&&) noexcept = default;
  Layer& operator=(Layer&&) noexcept = default;
  ~Layer() = default;

  /// The handle the scene minted for this layer. Stable across
  /// `rebind()` and `on_session_resumed()`.
  [[nodiscard]] LayerHandle handle() const noexcept { return handle_; }

  /// Mutable / const access to the layer's buffer source. The scene
  /// keeps ownership; consumers may call `source().map(MapAccess)` to
  /// paint into or read from the source's pixels but must not destroy
  /// the source.
  [[nodiscard]] LayerBufferSource& source() noexcept { return *source_; }
  [[nodiscard]] const LayerBufferSource& source() const noexcept { return *source_; }

  /// Current display configuration (src/dst rect, rotation, alpha,
  /// zpos). Mutate via the setters below — do not const_cast.
  [[nodiscard]] const DisplayParams& display() const noexcept { return display_; }

  /// Allocator hint passed at `add_layer` time. Read-only post-create.
  [[nodiscard]] drm::planes::ContentType content_type() const noexcept { return content_type_; }

  /// Producer's expected refresh rate in Hz from `LayerDesc`, 0 if no
  /// hint was supplied. Read-only post-create.
  [[nodiscard]] std::uint32_t update_hint_hz() const noexcept { return update_hint_hz_; }

  // ── Display-side mutation ──────────────────────────────────────────
  // Each setter flips the dirty flag. The scene clears it after a
  // successful commit via mark_clean(). The granularity is per-layer,
  // not per-field — Phase 2.2's property minimization refines that.

  /// Update `display.src_rect`. Marks the layer dirty.
  void set_src_rect(Rect r) noexcept {
    display_.src_rect = r;
    dirty_ = true;
  }
  /// Update `display.dst_rect`. Marks the layer dirty.
  void set_dst_rect(Rect r) noexcept {
    display_.dst_rect = r;
    dirty_ = true;
  }
  /// Update `display.rotation` (DRM_MODE_ROTATE_* | DRM_MODE_REFLECT_*).
  /// Marks the layer dirty.
  void set_rotation(std::uint64_t rot) noexcept {
    display_.rotation = rot;
    dirty_ = true;
  }
  /// Update `display.alpha` (0xFFFF = fully opaque). Marks the layer
  /// dirty.
  void set_alpha(std::uint16_t a) noexcept {
    display_.alpha = a;
    alpha_explicit_ = true;
    dirty_ = true;
  }

  /// True once `set_alpha` has been called on this layer for any
  /// value, including 0xFFFF. Sticky for the layer's lifetime; the
  /// scene reads it during commit lowering to decide whether to
  /// emit the per-plane "alpha" property even when its current value
  /// matches the kernel's default — needed so a tile that lowered
  /// alpha and then raised it back to fully-opaque actually goes
  /// back to opaque on scanout. (Without the sticky bit, the diff
  /// path leaves the kernel's last-written non-default value in
  /// place because lowering's emit-only-when-non-default guard hides
  /// the round-trip from the property snapshot.)
  [[nodiscard]] bool alpha_was_explicitly_set() const noexcept { return alpha_explicit_; }
  /// Update `display.zpos`. `std::nullopt` lets the allocator pick.
  /// Marks the layer dirty.
  void set_zpos(std::optional<int> z) noexcept {
    display_.zpos = z;
    dirty_ = true;
  }

  /// True when at least one display-side field has been mutated since
  /// the last `mark_clean`. The scene reads this during commit build to
  /// decide whether the layer's plane state needs re-emission.
  [[nodiscard]] bool is_dirty() const noexcept { return dirty_; }

  /// Reset the dirty flag. Called by `LayerScene` after a commit
  /// succeeds; consumers shouldn't normally call it directly.
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

  // Sticky across the layer's lifetime once `set_alpha` is called.
  // Distinct from `dirty_` (cleared after every commit) because we
  // need the lowering pass to keep emitting alpha for any layer the
  // caller has ever touched, not just on the frame where the touch
  // happens.
  bool alpha_explicit_{false};
};

}  // namespace drm::scene
