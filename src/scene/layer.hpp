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
#include "commit_report.hpp"
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

  /// Allocator content-type hint. Seeded from `LayerDesc`; mutable via
  /// `set_content_type`.
  [[nodiscard]] drm::planes::ContentType content_type() const noexcept { return content_type_; }

  /// Producer's expected refresh rate in Hz, 0 if no hint. Seeded from
  /// `LayerDesc`; mutable via `set_update_hint`.
  [[nodiscard]] std::uint32_t update_hint_hz() const noexcept { return update_hint_hz_; }

  /// Application-level within-content-class placement priority, 0 =
  /// default. Seeded from `LayerDesc`; mutable via `set_app_priority`.
  [[nodiscard]] std::uint8_t app_priority() const noexcept { return app_priority_; }

  /// The plane id this layer is hard-pinned to, or `std::nullopt` for
  /// normal allocation. Seeded from `LayerDesc::pinned_plane_id` and
  /// read-only afterward; the scene reserves this plane and writes the
  /// layer's plane state to it directly each commit.
  [[nodiscard]] std::optional<std::uint32_t> pinned_plane_id() const noexcept {
    return pinned_plane_id_;
  }

  /// Change the allocator content-type hint (e.g. promote a stream from
  /// `Generic` to `Video` once its pipeline is confirmed). Unlike the
  /// display setters this changes plane *scoring*, not just the values
  /// written to an already-chosen plane, so it also flags the layer for
  /// re-allocation (`hints_dirty`) — the scene drops the allocator's
  /// warm-start that frame so the layer can move to a better plane.
  void set_content_type(drm::planes::ContentType ct) noexcept {
    content_type_ = ct;
    dirty_ = true;
    hints_dirty_ = true;
  }
  /// Change the producer refresh-rate hint (Hz; 0 clears it). Feeds the
  /// allocator's scheduling; flags the layer for re-allocation like
  /// `set_content_type`.
  void set_update_hint(std::uint32_t hz) noexcept {
    update_hint_hz_ = hz;
    dirty_ = true;
    hints_dirty_ = true;
  }
  /// Change the application placement priority. Like `set_content_type`
  /// this feeds plane *scoring*, so it flags the layer for re-allocation
  /// (`hints_dirty`), not just a property re-emit on the current plane.
  void set_app_priority(std::uint8_t priority) noexcept {
    app_priority_ = priority;
    dirty_ = true;
    hints_dirty_ = true;
  }
  /// True when `set_content_type` / `set_update_hint` ran since the last
  /// `mark_clean` — the scene's signal to force a full re-allocation.
  [[nodiscard]] bool hints_dirty() const noexcept { return hints_dirty_; }

  /// Opaque pointer forwarded verbatim from `LayerDesc::identity_tag`,
  /// nullptr if the caller didn't set one. The scene never
  /// dereferences, frees, or copies (beyond the value copy onto this
  /// `Layer`) this pointer. Recovered by
  /// `LayerScene::find_by_identity_tag`. Read-only post-create; stable
  /// across `rebind()` and `on_session_resumed()` because the `Layer`
  /// itself is preserved across both.
  [[nodiscard]] void* identity_tag() const noexcept { return identity_tag_; }

  // ── Display-side mutation ──────────────────────────────────────────
  // Each setter flips the dirty flag. The scene clears it after a
  // successful commit via mark_clean(). The granularity is per-layer,
  // not per-field — property minimization refines that.

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
  /// Update `display.color_primaries`. Drives the scene's
  /// auto-derived connector `Colorspace` write. `std::nullopt`
  /// removes this layer's contribution to the widest-gamut decision.
  /// Marks the layer dirty.
  void set_color_primaries(std::optional<ColorPrimaries> cp) noexcept {
    display_.color_primaries = cp;
    dirty_ = true;
  }
  /// Update `display.source_eotf`. Setting an HDR transfer
  /// (PQ / HLG) on any layer triggers the scene's auto-derived
  /// HDR_OUTPUT_METADATA write. `std::nullopt` removes this layer's
  /// contribution. Marks the layer dirty.
  void set_source_eotf(std::optional<drm::display::TransferFunction> eotf) noexcept {
    display_.source_eotf = eotf;
    dirty_ = true;
  }

  // ── Conditional display-side mutation ──────────────────────────────
  // Same effect as the setters above, but only flip `dirty_` when the
  // new value actually differs from the current one. The engine's
  // present-layers callback hands the embedder a full layer array every
  // frame, including layers whose geometry hasn't moved; routing those
  // resubmissions through these variants keeps unchanged layers clean,
  // so the commit re-emits properties only for layers that genuinely
  // changed. Use the unconditional setters above when the caller
  // already knows the value is changing (e.g. animation tickers) and
  // wants to skip the comparison.

  /// Update `display.src_rect` only if it differs. Marks dirty on change.
  void set_src_rect_if_changed(Rect r) noexcept {
    if (display_.src_rect != r) {
      display_.src_rect = r;
      dirty_ = true;
    }
  }
  /// Update `display.dst_rect` only if it differs. Marks dirty on change.
  void set_dst_rect_if_changed(Rect r) noexcept {
    if (display_.dst_rect != r) {
      display_.dst_rect = r;
      dirty_ = true;
    }
  }
  /// Update `display.rotation` only if it differs. Marks dirty on change.
  void set_rotation_if_changed(std::uint64_t rot) noexcept {
    if (display_.rotation != rot) {
      display_.rotation = rot;
      dirty_ = true;
    }
  }
  /// Update `display.alpha` only if it differs. Marks dirty on change.
  /// The `alpha_explicit_` sticky bit is set on the first call for any
  /// value (matching `set_alpha`), so the round-trip back to opaque is
  /// still emitted; only `dirty_` is gated on the value differing. The
  /// first-ever call always dirties because the implicit pre-call alpha
  /// is conceptually distinct from any explicit value.
  void set_alpha_if_changed(std::uint16_t a) noexcept {
    if (!alpha_explicit_ || display_.alpha != a) {
      display_.alpha = a;
      dirty_ = true;
    }
    alpha_explicit_ = true;
  }
  /// Update `display.zpos` only if it differs. Marks dirty on change.
  void set_zpos_if_changed(std::optional<int> z) noexcept {
    if (display_.zpos != z) {
      display_.zpos = z;
      dirty_ = true;
    }
  }
  /// Update `display.color_primaries` only if it differs. Marks dirty on
  /// change. Drives the scene's auto-derived connector `Colorspace`
  /// write the same way `set_color_primaries` does.
  void set_color_primaries_if_changed(std::optional<ColorPrimaries> cp) noexcept {
    if (display_.color_primaries != cp) {
      display_.color_primaries = cp;
      dirty_ = true;
    }
  }
  /// Update `display.source_eotf` only if it differs. Marks dirty on
  /// change. Avoids the per-frame HDR_OUTPUT_METADATA re-derive when the
  /// video stream's transfer function is resubmitted unchanged.
  void set_source_eotf_if_changed(std::optional<drm::display::TransferFunction> eotf) noexcept {
    if (display_.source_eotf != eotf) {
      display_.source_eotf = eotf;
      dirty_ = true;
    }
  }

  /// True when at least one display-side field has been mutated since
  /// the last `mark_clean`. The scene reads this during commit build to
  /// decide whether the layer's plane state needs re-emission.
  [[nodiscard]] bool is_dirty() const noexcept { return dirty_; }

  /// Reset the dirty flags. Called by `LayerScene` after a commit
  /// succeeds; consumers shouldn't normally call it directly.
  void mark_clean() noexcept {
    dirty_ = false;
    hints_dirty_ = false;
  }

  // ── Last-commit placement readout ──────────────────────────────────
  //
  // Updated by `LayerScene::commit()` after every successful real
  // commit so consumers can tell where each layer's pixels actually
  // landed. Not updated by `test()` — `test()`'s placement is in the
  // returned `CommitReport::placements` only, since `test()` is meant
  // not to mutate scene state. Default-constructed values
  // (`Unassigned`, no plane id) hold until the first successful
  // commit places this layer; they also persist across a commit that
  // fails before placement runs.

  /// How this layer reached scanout in its most recent successful
  /// commit. `Unassigned` until the first commit places the layer.
  [[nodiscard]] LayerPlacement last_placement() const noexcept { return last_placement_; }

  /// The plane id this layer's content reached scanout on in its most
  /// recent successful commit. `nullopt` when `last_placement() ==
  /// Unassigned` (or the layer hasn't been committed yet). For
  /// composited layers this is the canvas plane id.
  [[nodiscard]] std::optional<std::uint32_t> last_assigned_plane_id() const noexcept {
    return last_assigned_plane_id_;
  }

  /// Record this layer's placement for the just-completed commit.
  /// Called by `LayerScene::commit()` only — `test()` does not write
  /// scene state. Consumers shouldn't call directly.
  void record_placement(LayerPlacement placement, std::optional<std::uint32_t> plane_id) noexcept {
    last_placement_ = placement;
    last_assigned_plane_id_ = plane_id;
  }

  /// Construct a Layer. In practice this is only called from
  /// LayerScene::add_layer — the signature is deliberately awkward
  /// (needs a LayerHandle the scene mints, takes an rvalue source) to
  /// steer consumers toward the add_layer path. Exposed in the public
  /// API to avoid friending a pimpl'd nested class.
  Layer(LayerHandle handle, std::unique_ptr<LayerBufferSource> source, DisplayParams display,
        drm::planes::ContentType content_type, std::uint32_t update_hint_hz,
        std::uint8_t app_priority = 0, void* identity_tag = nullptr,
        std::optional<std::uint32_t> pinned_plane_id = std::nullopt) noexcept
      : handle_(handle),
        source_(std::move(source)),
        display_(std::move(display)),
        content_type_(content_type),
        update_hint_hz_(update_hint_hz),
        app_priority_(app_priority),
        identity_tag_(identity_tag),
        pinned_plane_id_(pinned_plane_id) {}

 private:
  LayerHandle handle_;
  std::unique_ptr<LayerBufferSource> source_;
  DisplayParams display_;
  drm::planes::ContentType content_type_;
  std::uint32_t update_hint_hz_;
  std::uint8_t app_priority_;
  std::optional<std::uint32_t> pinned_plane_id_;
  // Opaque caller-side identity. Stored verbatim, never dereferenced or
  // freed by the scene. Recovered by
  // `LayerScene::find_by_identity_tag`. `nullptr` is the unset sentinel.
  void* identity_tag_{nullptr};

  // Starts true so the first commit writes every property on every
  // freshly-added layer — no stale plane state carries over from
  // whatever used the plane before the scene did.
  bool dirty_{true};

  // Set by set_content_type / set_update_hint. Distinct from `dirty_`:
  // those hints feed plane *scoring*, so a change must drop the
  // allocator's warm-start (a full re-search), not just re-emit the
  // current plane's properties. Cleared by mark_clean() like `dirty_`;
  // starts false (the cold-start commit full-searches regardless).
  bool hints_dirty_{false};

  // Sticky across the layer's lifetime once `set_alpha` is called.
  // Distinct from `dirty_` (cleared after every commit) because we
  // need the lowering pass to keep emitting alpha for any layer the
  // caller has ever touched, not just on the frame where the touch
  // happens.
  bool alpha_explicit_{false};

  // Last-commit placement state. Default to "never assigned" so a
  // layer queried before its first commit reports nullopt rather than
  // a misleading 0. Updated by record_placement() from the scene's
  // commit path.
  LayerPlacement last_placement_{LayerPlacement::Unassigned};
  std::optional<std::uint32_t> last_assigned_plane_id_;
};

}  // namespace drm::scene
