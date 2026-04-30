// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "layer_scene.hpp"

#include "buffer_source.hpp"
#include "commit_report.hpp"
#include "compatibility_report.hpp"
#include "composite_canvas.hpp"
#include "layer.hpp"
#include "layer_desc.hpp"
#include "layer_handle.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/property_store.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/log.hpp>
#include <drm-cxx/modeset/atomic.hpp>
#include <drm-cxx/planes/allocator.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/planes/output.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::scene {

namespace {

// 16.16 fixed-point promotion for SRC_X/Y/W/H — KMS convention for
// subpixel source rects.
constexpr std::uint64_t to_16_16(std::uint32_t v) noexcept {
  return static_cast<std::uint64_t>(v) << 16U;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// Impl
// ─────────────────────────────────────────────────────────────────────

class LayerScene::Impl {
 public:
  Impl(drm::Device& dev, const Config& cfg, drm::planes::PlaneRegistry registry) noexcept
      : dev_(&dev),
        crtc_id_(cfg.crtc_id),
        connector_id_(cfg.connector_id),
        mode_(cfg.mode),
        registry_(std::move(registry)),
        output_(cfg.crtc_id, composition_planes_layer_) {
    allocator_.emplace(*dev_, registry_);
  }

  ~Impl() { destroy_mode_blob(); }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  Impl(Impl&&) = delete;
  Impl& operator=(Impl&&) = delete;

  // Called once during factory setup, after cache_object_properties.
  // Resolves the PRIMARY plane's zpos_min on this scene's CRTC so
  // single-layer commits can pin the layer there (see the hint handling
  // in do_commit). Best-effort: if the mapping fails or no PRIMARY
  // exposes zpos, the hint stays empty and the allocator falls back to
  // its default scoring (which is fine on drivers that don't enforce
  // CRTC-needs-primary).
  void compute_primary_zpos_hint() {
    std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)> res(
        drmModeGetResources(dev_->fd()), drmModeFreeResources);
    if (!res) {
      return;
    }
    std::optional<std::uint32_t> crtc_bit;
    for (int i = 0; i < res->count_crtcs; ++i) {
      if (res->crtcs[i] == crtc_id_) {
        crtc_bit = static_cast<std::uint32_t>(i);
        break;
      }
    }
    if (!crtc_bit.has_value()) {
      return;
    }
    for (const auto& plane : registry_.all()) {
      if (plane.type == drm::planes::DRMPlaneType::PRIMARY &&
          plane.compatible_with_crtc(*crtc_bit) && plane.zpos_min.has_value()) {
        primary_zpos_hint_ = plane.zpos_min;
        return;
      }
    }
  }

  // Called once during factory setup; bundles the property-caching
  // work that can fail (and therefore has to surface through the
  // factory's drm::expected return) instead of doing it in the ctor.
  drm::expected<void, std::error_code> cache_object_properties() {
    if (auto r = props_.cache_properties(dev_->fd(), crtc_id_, DRM_MODE_OBJECT_CRTC); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
    if (auto r = props_.cache_properties(dev_->fd(), connector_id_, DRM_MODE_OBJECT_CONNECTOR);
        !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
    for (const auto& plane : registry_.all()) {
      if (auto r = props_.cache_properties(dev_->fd(), plane.id, DRM_MODE_OBJECT_PLANE); !r) {
        return drm::unexpected<std::error_code>(r.error());
      }
    }
    return {};
  }

  // ── Handle table ──────────────────────────────────────────────────

  drm::expected<LayerHandle, std::error_code> add_layer(LayerDesc desc) {
    if (!desc.source) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }

    std::uint32_t slot_idx = 0;
    if (!free_ids_.empty()) {
      slot_idx = free_ids_.back();
      free_ids_.pop_back();
    } else {
      slot_idx = static_cast<std::uint32_t>(slots_.size());
      slots_.emplace_back();
    }

    auto& slot = slots_[slot_idx];
    slot.generation += 1;
    slot.alive = true;

    // Handle id is 1-based so default-constructed (id=0) is an invalid
    // handle the caller can test cheaply with .valid().
    const LayerHandle handle{slot_idx + 1, slot.generation};

    auto& planes_layer = output_.add_layer();
    slot.planes_layer = &planes_layer;
    if (desc.force_composited) {
      planes_layer.set_composited();
    }

    slot.scene_layer = std::make_unique<Layer>(handle, std::move(desc.source), desc.display,
                                               desc.content_type, desc.update_hint_hz);
    return handle;
  }

  void remove_layer(LayerHandle handle) {
    auto* slot = slot_for(handle);
    if (slot == nullptr) {
      return;
    }
    if (slot->planes_layer != nullptr) {
      // Drop any allocator state keyed on this Layer's address before
      // freeing it. last_committed_ stores raw Layer pointers as
      // logical-identity keys; if we let those entries survive, the
      // heap can hand the same address back to a freshly-added Layer
      // and the diff-write path will treat the new Layer as a
      // continuation of the old one, suppressing property writes the
      // kernel needs (EINVAL on the next commit).
      if (allocator_.has_value()) {
        allocator_->forget_layer(slot->planes_layer);
      }
      output_.remove_layer(*slot->planes_layer);
      slot->planes_layer = nullptr;
    }
    slot->scene_layer.reset();
    slot->alive = false;
    // Slot index is slot_idx in the vector; store slot_idx (= handle.id-1)
    // in the freelist.
    free_ids_.push_back(handle.id - 1);
  }

  Layer* get_layer(LayerHandle handle) noexcept {
    auto* slot = slot_for(handle);
    return (slot != nullptr) ? slot->scene_layer.get() : nullptr;
  }

  const Layer* get_layer(LayerHandle handle) const noexcept {
    const auto* slot = slot_for(handle);
    return (slot != nullptr) ? slot->scene_layer.get() : nullptr;
  }

  [[nodiscard]] std::size_t layer_count() const noexcept {
    return slots_.size() - free_ids_.size();
  }

  // ── Commit path ───────────────────────────────────────────────────

  drm::expected<CommitReport, std::error_code> do_commit(std::uint32_t caller_flags, bool test_only,
                                                         void* user_data) {
    CommitReport report;
    report.layers_total = layer_count();

    // Short-circuit while the seat is suspended. The kernel revokes
    // commit privileges before libseat delivers pause_cb, so commit()
    // starts returning EACCES some frames ahead of the notification. A
    // sticky flag here keeps us from burning frames in the allocator
    // between that first EACCES and the resume_cb that clears it.
    if (suspended_) {
      return CommitReport{};
    }

    // Acquire every live layer's buffer up front. On any failure the
    // already-acquired buffers are handed back to their sources.
    std::vector<AcquisitionSlot> acquisitions;
    acquisitions.reserve(report.layers_total);
    if (auto r = acquire_all(acquisitions); !r) {
      release_all(acquisitions);
      return drm::unexpected<std::error_code>(r.error());
    }

    // Lower each live scene::Layer into its corresponding planes::Layer
    // property bag. This is what the allocator reads to pick planes.
    //
    // Steer one layer onto PRIMARY by handing it the PRIMARY plane's
    // zpos_min as a hint: the allocator's preseed prefers OVERLAY (+2
    // score) over PRIMARY for non-composition layers, which causes its
    // TEST commits to explicitly disable PRIMARY while activating the
    // CRTC — amdgpu rejects that combination with EINVAL (active CRTC
    // requires an armed PRIMARY plane). Pinning zpos to PRIMARY's
    // zpos_min lights up the primary-affinity bonus in score_pair and
    // steers exactly that layer onto PRIMARY. The chosen target is the
    // first layer in commit order without a caller-set zpos — for
    // bottom-to-top scenes (the convention) that's the background; if a
    // caller already pinned a layer to PRIMARY's slot, no hint is needed.
    const Layer* hint_target = nullptr;
    if (primary_zpos_hint_.has_value()) {
      bool already_targeted = false;
      for (const auto& acq : acquisitions) {
        const auto z = acq.scene_layer->display().zpos;
        if (z.has_value() && static_cast<std::uint64_t>(*z) == *primary_zpos_hint_) {
          already_targeted = true;
          break;
        }
      }
      if (!already_targeted) {
        for (const auto& acq : acquisitions) {
          if (!acq.scene_layer->display().zpos.has_value()) {
            hint_target = acq.scene_layer;
            break;
          }
        }
      }
    }
    for (const auto& acq : acquisitions) {
      // acquire_all only pushes slots whose scene_layer + planes_layer
      // are both non-null (the slot.alive guard plus add_layer always
      // wires planes_layer before alive flips true), so the dereferences
      // below are safe even though the analyzer can't prove it.
      const std::optional<std::uint64_t> per_layer_hint =
          (acq.scene_layer == hint_target) ? primary_zpos_hint_ : std::nullopt;
      lower_layer(*acq.scene_layer,  // NOLINT(clang-analyzer-core.NonNullParamChecker)
                  *acq.planes_layer, acq.buffer.fb_id, crtc_id_, per_layer_hint);
    }

    // Build the frame's AtomicRequest.
    drm::AtomicRequest req(*dev_);
    if (!req.valid()) {
      release_all(acquisitions);
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
    }

    // Modeset state: on the first commit after create()/rebind() we must
    // bring the CRTC up (MODE_ID blob + ACTIVE=1) and bind the connector
    // to it. The allocator's internal test commits re-apply this via the
    // test_preparer hook we installed in create().
    std::uint32_t effective_flags = caller_flags;
    if (first_commit_) {
      effective_flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
      if (auto r = ensure_mode_blob(); !r) {
        release_all(acquisitions);
        return drm::unexpected<std::error_code>(r.error());
      }
      if (auto r = inject_modeset_state(req); !r) {
        release_all(acquisitions);
        return drm::unexpected<std::error_code>(r.error());
      }
    }

    // Let the allocator pick planes. apply() writes the winning layer-
    // to-plane assignments into `req`. It returns how many layers were
    // placed on hardware; anything else is unassigned and routed
    // through compose_unassigned() below. allocator_ is engaged in
    // the ctor and re-engaged across on_session_resumed — never empty
    // by the time do_commit runs.
    //
    // If the previous frame ended with the composition canvas armed
    // on a specific plane, hand that plane id to apply() as an
    // "external reservation" so its disable_unused_planes pass leaves
    // it alone — compose_unassigned will overwrite the properties
    // moments later either way, but the reservation saves the
    // round-trip and removes a dependency on last-write-wins ordering
    // inside the kernel's atomic state machine.
    // Pre-reserve a canvas plane when overflow is anticipated. If the
    // scene has more layers than CRTC-compatible non-cursor planes,
    // the allocator would greedily fill every plane and leave
    // compose_unassigned with no plane to land the canvas on; the
    // overflow layers would then drop. Reserving a plane up front
    // forces the allocator to place at most N-1 layers on hardware,
    // leaving one OVERLAY free for the canvas. The reservation is
    // sticky once a frame uses it (via `last_canvas_plane_id_`), so
    // subsequent frames don't keep flipping which plane the canvas
    // lives on.
    if (!last_canvas_plane_id_.has_value()) {
      if (auto resv = pick_canvas_reservation_if_needed(); resv.has_value()) {
        last_canvas_plane_id_ = resv;
      }
    }
    const std::array<std::uint32_t, 1> reserved_planes{last_canvas_plane_id_.value_or(0)};
    const auto reserved_span = last_canvas_plane_id_.has_value()
                                   ? drm::span<const std::uint32_t>(reserved_planes.data(), 1)
                                   : drm::span<const std::uint32_t>{};
    auto assigned =  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        allocator_->apply(output_, req, effective_flags, reserved_span);
    if (!assigned) {
      release_all(acquisitions);
      return drm::unexpected<std::error_code>(assigned.error());
    }
    report.layers_assigned = *assigned;
    // Snapshot the allocator's diagnostics now — compose_unassigned's
    // direct property writes below don't go through the allocator and
    // therefore aren't reflected in its counters; we add them in by
    // hand a few lines down.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto alloc_diag = allocator_->diagnostics();
    report.properties_written = alloc_diag.properties_written;
    report.fbs_attached = alloc_diag.fbs_attached;
    report.test_commits_issued = alloc_diag.test_commits_issued;

    // Reset stale `pixel blend mode` on layer planes the allocator
    // just claimed. layer.properties() carries no entry for it (the
    // enum integer for "Pre-multiplied" is per-driver, only known
    // once the layer-to-plane assignment is in hand), so the
    // allocator's apply_layer_to_plane_real never emits it; without
    // this pass the plane keeps whatever mode the previous compositor
    // last wrote — typically "None" or "Coverage" — and a partially-
    // transparent layer ghosts whatever scans out below it on the
    // CRTC. Match the canvas's premultiplied output convention so
    // both natively-assigned and composited cells blend identically.
    arm_layer_plane_blend_defaults(acquisitions, req, report);

    // Phase 2.3: rescue unassigned layers via CPU composition before
    // counting them as dropped. compose_unassigned() updates
    // report.layers_composited and report.composition_buckets; the
    // dropped tally below is the residual that wasn't rescued
    // (no CPU mapping, no free plane, canvas alloc failed).
    compose_unassigned(acquisitions, req, report);

    if (report.layers_total > report.layers_assigned + report.layers_composited) {
      report.layers_unassigned =
          report.layers_total - report.layers_assigned - report.layers_composited;
      drm::log_warn("scene::LayerScene: {} layer(s) dropped this frame", report.layers_unassigned);
    }

    // Apply or validate.
    if (test_only) {
      if (auto r = req.test(); !r) {
        release_all(acquisitions);
        return drm::unexpected<std::error_code>(r.error());
      }
    } else {
      if (auto r = req.commit(effective_flags, user_data); !r) {
        if (r.error() == std::errc::permission_denied) {
          suspended_ = true;
        }
        release_all(acquisitions);
        return drm::unexpected<std::error_code>(r.error());
      }
      // Only real commits flip the scene past first-commit; tests don't.
      first_commit_ = false;
      // Mark dirty layers clean for a successful commit.
      for (auto& slot : slots_) {
        if (slot.alive && slot.scene_layer) {
          slot.scene_layer->mark_clean();
        }
      }
      output_.mark_clean();
    }

    release_all(acquisitions);
    return report;
  }

  // Driver-quirk forwarder. Stored on Impl so on_session_resumed can
  // re-propagate it after the allocator is rebuilt against the new
  // fd; consumers expect "I asked for force-full once, it stays on
  // across pause/resume" semantics.
  void set_force_full_property_writes(bool force) noexcept {
    force_full_writes_ = force;
    if (allocator_.has_value()) {
      allocator_->set_force_full_property_writes(force);
    }
  }
  [[nodiscard]] bool force_full_property_writes() const noexcept { return force_full_writes_; }

  // Wire up the allocator's internal test_preparer to this Impl's
  // modeset-state injector. Called once during LayerScene::create after
  // the Impl is fully constructed — can't go in the ctor because the
  // preparer closure needs `this`, and passing it through the Impl
  // constructor would leak implementation types into the header.
  void install_test_preparer() {
    // allocator_ is engaged in the ctor (and re-engaged in
    // on_session_resumed) before any caller can reach this method.
    allocator_->set_test_preparer(  // NOLINT(bugprone-unchecked-optional-access)
        [this](drm::AtomicRequest& req,
               std::uint32_t flags) -> drm::expected<void, std::error_code> {
          if ((flags & DRM_MODE_ATOMIC_ALLOW_MODESET) == 0) {
            return {};
          }
          if (auto r = ensure_mode_blob(); !r) {
            return drm::unexpected<std::error_code>(r.error());
          }
          return inject_modeset_state(req);
        });
  }

  // ── Session hooks ─────────────────────────────────────────────────
  //
  // Mirror drm::cursor::Renderer's on_session_paused / on_session_resumed
  // contract. Handles + generations survive; fd-bound kernel state
  // (plane enum, property caches, MODE_ID blob, per-source buffers)
  // is rebuilt against the new Device.

  void on_session_paused() noexcept {
    for (auto& slot : slots_) {
      if (slot.alive && slot.scene_layer) {
        slot.scene_layer->source().on_session_paused();
      }
    }
    if (composition_canvas_) {
      composition_canvas_->on_session_paused();
    }
  }

  drm::expected<void, std::error_code> on_session_resumed(drm::Device& new_dev) {
    // Old fd is already dead by the time libseat signals resume. The
    // mode blob, property ids, and plane registry it populated are
    // gone with it — zero/clear here without issuing destroy ioctls
    // (the kernel reclaimed the blob on fd close). Doing the clearing
    // up front so re-cache / re-enumerate below can't trip over stale
    // content if anything fails mid-way.
    mode_blob_id_ = 0;
    props_.clear();
    primary_zpos_hint_.reset();

    dev_ = &new_dev;

    // Re-enumerate planes before rebuilding the allocator — the
    // allocator's reference binds to `registry_` at construction and
    // the emplace below captures the current content.
    auto fresh_registry = drm::planes::PlaneRegistry::enumerate(new_dev);
    if (!fresh_registry) {
      return drm::unexpected<std::error_code>(fresh_registry.error());
    }
    registry_ = std::move(*fresh_registry);

    if (auto r = cache_object_properties(); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }

    // Rebuild the allocator against the new Device + registry. Drops
    // the previous-frame warm state, the failure cache, and the
    // per-plane property snapshot — correct, since each references
    // kernel state that's gone with the old fd. Reinstall the
    // test_preparer closure (its `this` capture is still valid; only
    // the underlying allocator changed) and re-propagate the
    // force-full-writes preference (it's a long-lived consumer
    // setting, not per-fd state).
    allocator_.reset();
    allocator_.emplace(*dev_, registry_);
    install_test_preparer();
    if (force_full_writes_) {
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      allocator_->set_force_full_property_writes(true);
    }

    compute_primary_zpos_hint();

    // Walk every live source; each one re-allocates its dumb/GBM/etc.
    // buffers against the new fd. If any fails we bail out — the
    // scene is in a half-resumed state, but the caller can retry or
    // tear down cleanly.
    for (auto& slot : slots_) {
      if (!slot.alive || !slot.scene_layer) {
        continue;
      }
      if (auto r = slot.scene_layer->source().on_session_resumed(new_dev); !r) {
        return drm::unexpected<std::error_code>(r.error());
      }
    }

    // Resume the composition canvas if it was created. Allocation
    // failure here is non-fatal: the scene re-attempts canvas creation
    // on the next compose_unassigned() call, so a transient resume
    // failure just means composition is unavailable until then. The
    // Plane ids and the CRTC index can in principle change across an
    // fd swap (registry was re-enumerated above); drop the cached
    // values so the next composing frame re-resolves from scratch.
    last_canvas_plane_id_.reset();
    cached_crtc_index_.reset();
    if (composition_canvas_) {
      if (auto r = composition_canvas_->on_session_resumed(new_dev); !r) {
        composition_canvas_.reset();
      }
    }

    // Next commit must carry ALLOW_MODESET to bring the new CRTC
    // binding up from cold; matches create-time semantics.
    first_commit_ = true;
    suspended_ = false;
    return {};
  }

  // ── Phase 2.4: rebind to a new CRTC / connector / mode ────────────
  drm::expected<CompatibilityReport, std::error_code> rebind(std::uint32_t new_crtc_id,
                                                             std::uint32_t new_connector_id,
                                                             drmModeModeInfo new_mode) {
    // Tear down state tied to the old binding before swapping in the
    // new ids. The old MODE_ID blob is owned by the kernel and freed
    // explicitly here — leaking it would accumulate one blob per
    // rebind, which a hotplug-heavy app could turn into a real leak.
    destroy_mode_blob();
    mode_blob_id_ = 0;

    // The plane registry depends on the fd, not the CRTC, so we keep
    // it. The PROPERTY caches keyed on connector_id_ and crtc_id_ ARE
    // tied to the old configuration — clear and recache below for
    // the new ids. Per-plane property ids keyed on plane_id are
    // still valid (planes don't change with CRTC rebinds, only the
    // CRTCs their `possible_crtcs` mask covers does).
    props_.clear();

    crtc_id_ = new_crtc_id;
    connector_id_ = new_connector_id;
    mode_ = new_mode;

    if (auto r = cache_object_properties(); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }

    // The Output's stored crtc_id is consulted by the allocator's
    // disable-unused pass; rebuild the allocator (which captures
    // PlaneRegistry& but not CRTC id directly) so warm-state and
    // failure cache from the old binding don't leak into the new.
    // The output's composition_layer keeps its handle into Impl's
    // member; the Output struct is reconstructed in place.
    output_ = drm::planes::Output(crtc_id_, composition_planes_layer_);

    // Re-add the live scene Layers' planes::Layer twins back into
    // the fresh Output. Existing planes::Layer instances are still
    // owned by the previous Output via unique_ptr; that Output
    // instance is now destroyed by the move-assign above, taking the
    // planes::Layer pointers down with it. Each Slot's
    // planes_layer pointer is now dangling — we re-create the twins
    // and reattach.
    for (auto& slot : slots_) {
      if (!slot.alive || !slot.scene_layer) {
        slot.planes_layer = nullptr;
        continue;
      }
      auto& fresh = output_.add_layer();
      slot.planes_layer = &fresh;
    }

    // Allocator is bound to (Device, Registry); both are still
    // valid. Re-emplace it anyway so previous_allocation_ (which
    // holds Layer* pointers that were just invalidated above) and
    // last_committed_ (kernel state from the old CRTC) get reset.
    allocator_.reset();
    allocator_.emplace(*dev_, registry_);
    install_test_preparer();
    if (force_full_writes_) {
      // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      allocator_->set_force_full_property_writes(true);
    }

    primary_zpos_hint_.reset();
    compute_primary_zpos_hint();

    // Composition canvas: the new mode's dimensions may differ from
    // the old. Drop the canvas wholesale; it'll be re-allocated on
    // the next compose_unassigned call at the new size. The
    // dirty-rect tracker resets too.
    composition_canvas_.reset();
    last_canvas_plane_id_.reset();
    cached_crtc_index_.reset();

    // First commit after rebind must modeset the new CRTC.
    first_commit_ = true;

    // Walk live layers and flag anything that looks problematic
    // against the new mode. We don't probe the new plane registry
    // for format / scaling support here — those are checked by the
    // allocator at apply time and reported via CommitReport. The
    // dst_rect off-screen check is the cheap one we can do without
    // running the allocator, and it's the most common reason a
    // post-rebind commit visibly breaks (the kernel rejects the
    // commit and the user sees nothing on screen).
    CompatibilityReport report;
    const std::int64_t mode_w = new_mode.hdisplay;
    const std::int64_t mode_h = new_mode.vdisplay;
    for (const auto& slot : slots_) {
      if (!slot.alive || !slot.scene_layer) {
        continue;
      }
      const auto& d = slot.scene_layer->display();
      // Half-open interval check — a layer that ends exactly at
      // mode_w/mode_h is on-screen; one that *starts* there is off.
      const std::int64_t x0 = d.dst_rect.x;
      const std::int64_t y0 = d.dst_rect.y;
      const std::int64_t x1 = x0 + static_cast<std::int64_t>(d.dst_rect.w);
      const std::int64_t y1 = y0 + static_cast<std::int64_t>(d.dst_rect.h);
      if (x1 <= 0 || y1 <= 0 || x0 >= mode_w || y0 >= mode_h) {
        report.incompatibilities.push_back(
            {slot.scene_layer->handle(), LayerIncompatibility::Reason::DstRectOffScreen});
      }
    }

    return report;
  }

 private:
  struct Slot {
    std::unique_ptr<Layer> scene_layer;
    // Non-owning. drm::planes::Output owns the planes::Layer via
    // unique_ptr in a vector; vector reallocation moves unique_ptrs
    // but never invalidates the pointee, so this pointer stays valid
    // for the planes::Layer's full lifetime.
    drm::planes::Layer* planes_layer{nullptr};
    std::uint32_t generation{0};
    bool alive{false};
  };

  struct AcquisitionSlot {
    Layer* scene_layer{nullptr};
    drm::planes::Layer* planes_layer{nullptr};
    AcquiredBuffer buffer{};
    // Populated by compose_unassigned for the layers it picks up so the
    // blend pass below doesn't have to call the virtual `map()` a
    // second time. Stays nullopt for hardware-assigned layers. The
    // mapping is held across `compose_unassigned`'s blend loop and
    // dropped on slot destruction (i.e. after the canvas plane has
    // been armed and before atomic commit) — `BufferMapping`'s dtor
    // pairs with `gbm_bo_unmap` for GBM-backed sources.
    std::optional<drm::BufferMapping> cached_mapping;
  };

  // Slot-table helpers — return nullptr on any handle that doesn't
  // currently name a live layer.
  Slot* slot_for(LayerHandle h) noexcept {
    return const_cast<Slot*>(static_cast<const Impl*>(this)->slot_for(h));  // NOLINT
  }
  const Slot* slot_for(LayerHandle h) const noexcept {
    if (h.id == 0 || h.id > slots_.size()) {
      return nullptr;
    }
    const auto& slot = slots_[h.id - 1];
    if (!slot.alive || slot.generation != h.generation) {
      return nullptr;
    }
    return &slot;
  }

  drm::expected<void, std::error_code> acquire_all(std::vector<AcquisitionSlot>& out) {
    for (auto& slot : slots_) {
      if (!slot.alive || !slot.scene_layer) {
        continue;
      }
      auto acq = slot.scene_layer->source().acquire();
      if (!acq) {
        drm::log_warn("scene::LayerScene: source acquire() failed for layer {}",
                      slot.scene_layer->handle().id);
        return drm::unexpected<std::error_code>(acq.error());
      }
      out.push_back({slot.scene_layer.get(), slot.planes_layer, *acq});
    }
    return {};
  }

  static void release_all(std::vector<AcquisitionSlot>& acquisitions) noexcept {
    for (auto& a : acquisitions) {
      if (a.scene_layer != nullptr) {
        a.scene_layer->source().release(a.buffer);
      }
    }
    acquisitions.clear();
  }

  // Copy scene::Layer state into the planes::Layer property bag. The
  // allocator and the AtomicRequest it builds read from this bag to
  // write plane properties.
  static void lower_layer(const Layer& src, drm::planes::Layer& dst, std::uint32_t fb_id,
                          std::uint32_t crtc_id, std::optional<std::uint64_t> default_zpos_hint) {
    const auto& d = src.display();
    const auto fmt = src.source().format();

    dst.set_property("FB_ID", fb_id);
    // CRTC_ID binds the plane to this scene's CRTC. Without it the
    // kernel rejects the plane commit (FB armed, but the plane is still
    // bound to nothing / to whatever the previous committed CRTC was),
    // the allocator's test commits fail for every plane, and the
    // allocator reports 0 layers assigned.
    dst.set_property("CRTC_ID", crtc_id);

    // KMS plane rectangles: CRTC_* is destination on the scanout, in
    // signed 32-bit pixels. SRC_* is source within the buffer, encoded
    // as 16.16 fixed-point (kernel expects this for subpixel blits).
    dst.set_property("CRTC_X", static_cast<std::uint64_t>(d.dst_rect.x));
    dst.set_property("CRTC_Y", static_cast<std::uint64_t>(d.dst_rect.y));
    dst.set_property("CRTC_W", static_cast<std::uint64_t>(d.dst_rect.w));
    dst.set_property("CRTC_H", static_cast<std::uint64_t>(d.dst_rect.h));
    dst.set_property("SRC_X", to_16_16(static_cast<std::uint32_t>(d.src_rect.x)));
    dst.set_property("SRC_Y", to_16_16(static_cast<std::uint32_t>(d.src_rect.y)));
    dst.set_property("SRC_W", to_16_16(d.src_rect.w));
    dst.set_property("SRC_H", to_16_16(d.src_rect.h));

    // Format + modifier let the allocator statically screen planes for
    // compatibility before any test commit. The allocator reads both
    // through planes::Layer's format()/modifier() accessors, which are
    // backed by the "pixel_format" / "FB_MODIFIER" property keys (not
    // the KMS plane-property names — these are internal-to-Layer hints).
    dst.set_property("pixel_format", fmt.drm_fourcc);
    dst.set_property("FB_MODIFIER", fmt.modifier);

    // Optional plane properties. rotation and zpos are only written when
    // the caller set a value; emitting zpos=0 unconditionally would
    // static-compat-reject any PRIMARY plane with an immutable non-zero
    // zpos (amdgpu pins PRIMARY at zpos=2), leaving the scene with
    // nowhere to put a single layer.
    if (d.rotation != 0) {
      dst.set_property("rotation", d.rotation);
    }
    if (d.zpos.has_value()) {
      dst.set_property("zpos", static_cast<std::uint64_t>(*d.zpos));
    } else if (default_zpos_hint.has_value()) {
      dst.set_property("zpos", *default_zpos_hint);
    }
    // Always emit the layer's intended alpha. Earlier versions skipped
    // the write when `d.alpha == 0xFFFF` and no caller had touched it,
    // assuming the kernel default was also 0xFFFF — but that
    // assumption fails when a previous compositor session left a
    // partial value on the plane (e.g. mutter at 0x4000 / sway at
    // 0x8000 from a fade animation). The plane's `alpha` property is
    // sticky across processes; without this write, our layers scan
    // out at someone else's faded intensity. amdgpu PRIMARY doesn't
    // expose `alpha` so the apply-time `property_id()` lookup skips
    // the write there silently, matching the earlier wedge-avoidance
    // intent without needing the conditional.
    dst.set_property("alpha", static_cast<std::uint64_t>(d.alpha));

    dst.set_content_type(src.content_type());
    if (src.update_hint_hz() != 0) {
      dst.set_update_hint(src.update_hint_hz());
    }
  }

  // ── Composition fallback (Phase 2.3) ───────────────────────────────
  //
  // After allocator.apply() runs, layers it couldn't place report
  // needs_composition() == true. compose_unassigned() walks them, blends
  // their CPU-mapped sources into a lazily-allocated full-screen ARGB8888
  // canvas, finds a free plane on this CRTC, and arms the canvas onto
  // that plane via direct PropertyStore writes — no second allocator
  // pass.
  //
  // **Failure mode is all-or-nothing for the frame, not per-layer.**
  // Composition writes the canvas's properties (FB_ID, CRTC_*, SRC_*,
  // zpos) to the same AtomicRequest the allocator already populated,
  // and the kernel atomically tests/applies the whole request. If the
  // chosen plane rejects the canvas (driver-specific scaling /
  // positioning constraint we don't pre-test), the *entire commit*
  // fails — including every layer the allocator did place. The
  // pre-`compose_unassigned` exits below (no free plane, no CPU
  // mapping, canvas alloc failed) are best-effort and silently drop
  // the affected layers; only the kernel-side reject path takes the
  // whole frame down.
  //
  // The composition layer's zpos is chosen so it sits above any
  // hardware-assigned layer (max assigned zpos + 1) AND above any
  // explicit zpos the unassigned layers themselves carry. Without
  // this, a scene where the unassigned layers default zpos=0 lands
  // the canvas at zpos=0 too and competes with the bg plane (on
  // amdgpu PRIMARY at zpos=2 the canvas would be hidden underneath).
  //
  // The composited group's zpos collapses to a single value — for
  // contiguous unassigned z-runs this is correct; for non-contiguous
  // runs (an assigned layer interleaved between two unassigned ones)
  // the composited group floats above its lowest member's natural slot.
  // Documented limitation; multi-canvas v1.1 will resolve it.

  // Resolve and cache the CRTC index in drmModeRes::crtcs[]. The index
  // never changes for the scene's lifetime on a given fd, so we ioctl
  // at most once per session (initial commit + once per resume).
  std::optional<std::uint32_t> resolve_crtc_index() {
    if (cached_crtc_index_.has_value()) {
      return cached_crtc_index_;
    }
    std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)> res(
        drmModeGetResources(dev_->fd()), drmModeFreeResources);
    if (!res) {
      return std::nullopt;
    }
    for (int i = 0; i < res->count_crtcs; ++i) {
      if (res->crtcs[i] == crtc_id_) {
        cached_crtc_index_ = static_cast<std::uint32_t>(i);
        return cached_crtc_index_;
      }
    }
    return std::nullopt;
  }

  // Decide whether to reserve a canvas plane up front for this
  // commit. Returns the plane id to reserve, or nullopt when neither
  // overflow nor primary-anchor reservation is needed.
  //
  // Two triggers:
  //
  //  - **Overflow.** Live layer count exceeds canvas-eligible plane
  //    count (CRTC-compatible, non-cursor, ARGB8888-supporting); the
  //    allocator would otherwise claim every plane and leave
  //    compose_unassigned with nowhere to land the canvas. Reserve an
  //    OVERLAY (or PRIMARY as a fallback) so one stays free.
  //
  //  - **Primary anchor.** PRIMARY exposes an immutable zpos slot on
  //    this CRTC (amdgpu pins PRIMARY at zpos=2; ARM Mali display
  //    cores have similar pins) and no live layer is eligible to land
  //    on it. Without a layer pinned to PRIMARY's slot, the
  //    allocator's disable_unused_planes pass writes FB_ID=0 /
  //    CRTC_ID=0 to PRIMARY during every TEST, which amdgpu (and any
  //    driver that enforces "active CRTC requires armed PRIMARY")
  //    rejects with EINVAL — every TEST fails and every layer falls
  //    through to composition. Reserving PRIMARY pulls it out of the
  //    disable pass, so TESTs validate against PRIMARY's prior state
  //    (fbcon FB on cold start, previous canvas FB on warm) and
  //    overlay placements succeed. A layer is "eligible" when its
  //    caller-set zpos matches the pin, OR when it has no caller-set
  //    zpos (the do_commit primary-affinity hint will pin it). Both
  //    cases bypass this trigger.
  std::optional<std::uint32_t> pick_canvas_reservation_if_needed() {
    const auto crtc_index = resolve_crtc_index();
    if (!crtc_index.has_value()) {
      return std::nullopt;
    }
    std::vector<const drm::planes::PlaneCapabilities*> eligible;
    eligible.reserve(8);
    const drm::planes::PlaneCapabilities* primary_fallback = nullptr;
    for (const auto* p : registry_.for_crtc(*crtc_index)) {
      if (p->type == drm::planes::DRMPlaneType::CURSOR) {
        continue;
      }
      if (!p->supports_format(DRM_FORMAT_ARGB8888)) {
        continue;
      }
      eligible.push_back(p);
      if (p->type == drm::planes::DRMPlaneType::PRIMARY && primary_fallback == nullptr) {
        primary_fallback = p;
      }
    }
    if (eligible.empty()) {
      return std::nullopt;
    }

    // Primary-anchor trigger. Mirrors the eligibility check in
    // do_commit's hint pass — kept self-contained here so the
    // reservation decision is one function call from do_commit.
    if (primary_zpos_hint_.has_value() && primary_fallback != nullptr) {
      const auto pin = *primary_zpos_hint_;
      bool primary_eligible_layer = false;
      for (const auto& slot : slots_) {
        if (!slot.alive || slot.scene_layer == nullptr) {
          continue;
        }
        const auto z = slot.scene_layer->display().zpos;
        if (!z.has_value() || static_cast<std::uint64_t>(*z) == pin) {
          primary_eligible_layer = true;
          break;
        }
      }
      if (!primary_eligible_layer && layer_count() > 0) {
        return primary_fallback->id;
      }
    }

    if (layer_count() <= eligible.size()) {
      // Every layer can take its own plane — reserving one for the
      // canvas would just force one of them through composition for
      // no gain. Only reserve when overflow is actually anticipated.
      return std::nullopt;
    }
    // Prefer an OVERLAY for the canvas — keeping PRIMARY available
    // for the bg layer is the conventional layout and avoids any
    // amdgpu PRIMARY-zpos-pin interaction with the canvas's runtime
    // zpos pick.
    for (const auto* p : eligible) {
      if (p->type == drm::planes::DRMPlaneType::OVERLAY) {
        return p->id;
      }
    }
    return (primary_fallback != nullptr) ? std::optional<std::uint32_t>(primary_fallback->id)
                                         : std::nullopt;
  }

  // Pick a plane on this CRTC that the allocator did not assign and
  // that the canvas's ARGB8888-linear buffer is compatible with.
  // When `last_canvas_plane_id_` is set the search prefers that plane
  // (the do_commit pre-reservation kept it out of the allocator's
  // pool, so it should still be free here); otherwise we fall back to
  // the OVERLAY-then-PRIMARY scan. Never returns CURSOR.
  const drm::planes::PlaneCapabilities* find_free_canvas_plane(
      std::uint32_t crtc_index, const std::vector<AcquisitionSlot>& acquisitions) {
    // Build the set of planes already armed by the allocator (from any
    // assigned layer) plus any plane the allocator routed the empty
    // composition_planes_layer_ to. Reused scratch member to keep this
    // off the per-frame heap.
    //
    // Exception: when composition_planes_layer_'s slot equals
    // last_canvas_plane_id_, that's the do_commit pre-reservation
    // landing on the same plane the allocator's any_composited path
    // pre-armed for the empty composition layer. They're the *same*
    // canvas slot — we want to land there, not avoid it. Skipping the
    // composition_planes_layer_ entry in that case lets the preferred
    // path below return the reserved plane instead of bailing out
    // with "no free plane for canvas" while every other plane is
    // assigned to a placed layer.
    scratch_in_use_.clear();
    scratch_in_use_.reserve(acquisitions.size() + 1);
    for (const auto& acq : acquisitions) {
      if (auto pid = acq.planes_layer->assigned_plane_id(); pid.has_value()) {
        scratch_in_use_.push_back(*pid);
      }
    }
    if (auto pid = composition_planes_layer_.assigned_plane_id(); pid.has_value()) {
      if (!last_canvas_plane_id_.has_value() || *pid != *last_canvas_plane_id_) {
        scratch_in_use_.push_back(*pid);
      }
    }
    auto is_in_use = [&](std::uint32_t pid) {
      return std::find(scratch_in_use_.begin(), scratch_in_use_.end(), pid) !=
             scratch_in_use_.end();
    };

    // Preferred path: the do_commit pre-reservation already pinned a
    // plane by setting `last_canvas_plane_id_`, and the allocator
    // honoured the reservation by leaving it out of best_assignment.
    // Try to reuse it before falling through to the generic scan —
    // sticky plane choice across frames lets the per-plane property
    // snapshot keep working between commits.
    if (last_canvas_plane_id_.has_value()) {
      for (const auto* p : registry_.for_crtc(crtc_index)) {
        if (p->id != *last_canvas_plane_id_) {
          continue;
        }
        if (is_in_use(p->id) || p->type == drm::planes::DRMPlaneType::CURSOR ||
            !p->supports_format(DRM_FORMAT_ARGB8888)) {
          break;  // reservation invalidated this frame; fall back below
        }
        return p;
      }
    }

    const drm::planes::PlaneCapabilities* primary_fallback = nullptr;
    for (const auto* p : registry_.for_crtc(crtc_index)) {
      if (p->type == drm::planes::DRMPlaneType::CURSOR) {
        continue;
      }
      if (is_in_use(p->id)) {
        continue;
      }
      if (!p->supports_format(DRM_FORMAT_ARGB8888)) {
        continue;
      }
      if (p->type == drm::planes::DRMPlaneType::OVERLAY) {
        return p;
      }
      // Remember the first eligible PRIMARY but keep looking for an
      // OVERLAY — OVERLAY is the safer placement when the scene has a
      // dedicated background layer on PRIMARY.
      if (primary_fallback == nullptr) {
        primary_fallback = p;
      }
    }
    return primary_fallback;
  }

  // Pick a zpos for the canvas plane that puts composited content
  // above every hardware-assigned layer AND above any explicit zpos
  // the composited layers carry. Returns 0 when no signal exists,
  // which is the conservative "let the kernel pick" sentinel.
  static std::int32_t choose_canvas_zpos(const std::vector<AcquisitionSlot>& acquisitions,
                                         const std::vector<AcquisitionSlot*>& composited) {
    std::int32_t max_explicit = 0;
    bool have_signal = false;
    for (const auto& acq : acquisitions) {
      if (!acq.planes_layer->assigned_plane_id().has_value()) {
        continue;  // unassigned layers feed the composited list, not the floor
      }
      if (auto z = acq.scene_layer->display().zpos; z.has_value()) {
        max_explicit = std::max(max_explicit, *z);
        have_signal = true;
      }
    }
    for (const auto* acq : composited) {
      if (auto z = acq->scene_layer->display().zpos; z.has_value()) {
        max_explicit = std::max(max_explicit, *z);
        have_signal = true;
      }
    }
    if (!have_signal) {
      return 0;
    }
    // +1 so the canvas sits strictly above the highest layer's natural
    // slot. If max_explicit is INT32_MAX (an absurd configuration the
    // kernel would already have rejected upstream) the saturating add
    // here keeps us at INT32_MAX rather than wrapping negative.
    if (max_explicit == std::numeric_limits<std::int32_t>::max()) {
      return max_explicit;
    }
    return max_explicit + 1;
  }

  // Emit a per-frame warn line when the only thing keeping the layer
  // off every plane is its modifier — i.e. the bare format would have
  // matched some plane on this CRTC but no plane's IN_FORMATS list
  // carries the (format, modifier) pair. The common AFBC/DCC/vendor-
  // tiling failure mode otherwise looks identical to "format
  // unsupported" in the existing dropped-tally log, which is unhelpful
  // when debugging an embedded SoC bring-up. Skipped for layers whose
  // modifier is LINEAR or INVALID — those can never be modifier-rejected
  // (the allocator's eligibility path normalizes both to LINEAR).
  // Non-const because resolve_crtc_index() updates the cached CRTC
  // index on first call.
  void diagnose_modifier_rejection(const drm::planes::Layer& planes_layer) {
    const auto fmt = planes_layer.format();
    if (!fmt.has_value()) {
      return;
    }
    const auto modifier = planes_layer.modifier();
    if (modifier == 0 /* LINEAR */ || modifier == DRM_FORMAT_MOD_INVALID) {
      return;
    }
    const auto crtc_index = resolve_crtc_index();
    if (!crtc_index.has_value()) {
      return;
    }
    bool any_plane_has_format = false;
    for (const auto* p : registry_.for_crtc(*crtc_index)) {
      if (p->type == drm::planes::DRMPlaneType::CURSOR) {
        continue;
      }
      if (p->supports_format(*fmt)) {
        any_plane_has_format = true;
        break;
      }
    }
    if (!any_plane_has_format) {
      // Format itself is unsupported on every plane — diagnostic for
      // that case is the existing "dropped this frame" warn; this
      // helper's job is only to call out the format-OK / modifier-NOT
      // case so a reader can tell them apart in the log.
      return;
    }
    drm::log_warn(
        "scene::LayerScene: layer routed to composition — modifier 0x{:016x} not advertised on "
        "any compatible plane for format 0x{:08x}",
        modifier, *fmt);
  }

  // Best-effort composition. Updates `report.layers_composited` and
  // `report.composition_buckets` for layers it absorbs.
  void compose_unassigned(std::vector<AcquisitionSlot>& acquisitions, drm::AtomicRequest& req,
                          CommitReport& report) {
    // Collect layers needing composition that also have CPU pixels
    // available. Sources without a CPU mapping (future EGL Streams,
    // tiled GBM BOs) report `errc::function_not_supported`; we drop
    // them. Stash the mapping inline so the blend pass below doesn't
    // have to call the virtual `map()` a second time, and so the
    // GBM-backed unmap pairs with this scope rather than per-blend.
    scratch_composited_.clear();
    scratch_composited_.reserve(acquisitions.size());
    for (auto& acq : acquisitions) {
      if (!acq.planes_layer->needs_composition()) {
        continue;
      }
      // Diagnose modifier-driven plane rejection here, before composition
      // either rescues the layer or drops it. Only fires when the layer's
      // modifier is non-trivial — LINEAR/INVALID layers can't be
      // modifier-rejected by construction, and force_composited layers
      // (test rigs, diagnostic overlays) typically use linear buffers, so
      // this gate also keeps the log quiet for them.
      diagnose_modifier_rejection(*acq.planes_layer);
      auto mapping = acq.scene_layer->source().map(drm::MapAccess::Read);
      if (!mapping) {
        drm::log_warn(
            "scene::LayerScene: layer {} needs composition but its source map() failed ({}); "
            "dropping",
            acq.scene_layer->handle().id, mapping.error().message());
        continue;
      }
      acq.cached_mapping.emplace(std::move(*mapping));
      scratch_composited_.push_back(&acq);
    }
    if (scratch_composited_.empty()) {
      // Composition not used this frame — drop any record of the
      // previous canvas plane so disable_unused_planes can reclaim it
      // on the next frame.
      last_canvas_plane_id_.reset();
      return;
    }

    const auto crtc_index = resolve_crtc_index();
    if (!crtc_index.has_value()) {
      drm::log_warn("scene::LayerScene: composition fallback could not resolve CRTC index");
      return;
    }
    const auto* target_plane = find_free_canvas_plane(*crtc_index, acquisitions);
    if (target_plane == nullptr) {
      drm::log_warn(
          "scene::LayerScene: composition fallback found no free plane for canvas; {} "
          "layer(s) dropped",
          scratch_composited_.size());
      return;
    }

    if (!composition_canvas_) {
      CompositeCanvasConfig cfg;
      cfg.canvas_width = mode_.hdisplay;
      cfg.canvas_height = mode_.vdisplay;
      auto canvas = CompositeCanvas::create(*dev_, cfg);
      if (!canvas) {
        drm::log_warn("scene::LayerScene: CompositeCanvas::create failed: {}",
                      canvas.error().message());
        return;
      }
      composition_canvas_ = std::move(*canvas);
      // Fresh canvas is kernel-zeroed; nothing left over from a
      // previous frame to scrub on the first composition.
    }
    if (!composition_canvas_->armable()) {
      drm::log_warn("scene::LayerScene: composition canvas isn't armable (post-resume?)");
      return;
    }

    // Swap to the back buffer before any paint ops — the front is
    // currently being scanned out by the kernel and writing into it
    // would tear visibly (CPU writes racing the display read pointer).
    // begin_frame is the first canvas mutation this frame; arming
    // below uses fb_id() which now reflects the new back.
    composition_canvas_->begin_frame();

    // Sort composited layers by zpos ascending — paint from the back
    // forward so SRC_OVER order matches scene Z-order.
    std::sort(scratch_composited_.begin(), scratch_composited_.end(),
              [](const AcquisitionSlot* a, const AcquisitionSlot* b) {
                const auto za = a->scene_layer->display().zpos.value_or(0);
                const auto zb = b->scene_layer->display().zpos.value_or(0);
                return za < zb;
              });

    // Always full-clear the back buffer before blending this frame's
    // composited layers on top. Partial clear (union of last-frame +
    // this-frame dirty boxes) is correct only with single-buffer
    // canvases, where "last-frame's dirty box" is the content
    // currently in the same buffer. Double buffering means the back
    // we're about to paint last held content from FRAME N-2, not
    // N-1; tracking that correctly needs a per-buffer dirty record.
    // The simpler-correct choice for v1 is the full clear — at
    // 1024×768 it's 3 MB of memset per frame, well under the
    // memory-bandwidth budget of any consumer-class CPU.
    composition_canvas_->clear();

    for (const auto* acq : scratch_composited_) {
      const auto& d = acq->scene_layer->display();
      const auto fmt = acq->scene_layer->source().format();
      // Mapping was stashed during the collection pass above — no
      // second virtual call.
      if (!acq->cached_mapping.has_value()) {
        continue;
      }
      CompositeSrc src;
      src.pixels = acq->cached_mapping->pixels();
      src.src_stride_bytes = acq->cached_mapping->stride();
      src.src_width = fmt.width;
      src.src_height = fmt.height;
      src.drm_fourcc = fmt.drm_fourcc;
      src.plane_alpha = d.alpha;
      const CompositeRect src_rect{d.src_rect.x, d.src_rect.y, d.src_rect.w, d.src_rect.h};
      const CompositeRect dst_rect{d.dst_rect.x, d.dst_rect.y, d.dst_rect.w, d.dst_rect.h};
      composition_canvas_->blend(src, src_rect, dst_rect);
    }

    const std::int32_t canvas_zpos = choose_canvas_zpos(acquisitions, scratch_composited_);
    if (auto r = arm_composition_canvas(req, *target_plane, canvas_zpos, report); !r) {
      drm::log_warn("scene::LayerScene: arm composition canvas failed: {}", r.error().message());
      return;
    }
    report.layers_composited += scratch_composited_.size();
    report.composition_buckets += 1U;
    last_canvas_plane_id_ = target_plane->id;
  }

  // For every layer the allocator placed natively, force the plane's
  // `pixel blend mode` to Pre-multiplied (when the plane exposes the
  // property and advertises that enum value). This neutralizes any
  // stale mode left by a previous session — the property is
  // process-sticky and amdgpu/i915 keep whatever value the last
  // compositor wrote across our open() of the device. Skipped silently
  // when the plane lacks the property (amdgpu PRIMARY is a notable
  // example: no pixel_blend_mode at all). Per-frame cost: one prop_id
  // lookup + one add_property per native layer; amortized fully into
  // the allocator's existing snapshot-diff in apply_layer_to_plane_real
  // would need allocator-side knowledge of plane enum values, which
  // doesn't belong in the per-plane assignment loop.
  void arm_layer_plane_blend_defaults(const std::vector<AcquisitionSlot>& acquisitions,
                                      drm::AtomicRequest& req, CommitReport& report) {
    const auto crtc_index_opt = resolve_crtc_index();
    if (!crtc_index_opt.has_value()) {
      return;
    }
    for (const auto& acq : acquisitions) {
      const auto plane_id = acq.planes_layer->assigned_plane_id();
      if (!plane_id.has_value()) {
        continue;
      }
      const drm::planes::PlaneCapabilities* caps = nullptr;
      for (const auto* p : registry_.for_crtc(*crtc_index_opt)) {
        if (p->id == *plane_id) {
          caps = p;
          break;
        }
      }
      if (caps == nullptr || !caps->blend_mode_premultiplied.has_value()) {
        continue;
      }
      const auto prop_id = props_.property_id(*plane_id, "pixel blend mode");
      if (!prop_id) {
        continue;
      }
      if (props_.is_immutable(*plane_id, "pixel blend mode").value_or(false)) {
        continue;
      }
      if (auto r = req.add_property(*plane_id, *prop_id, *caps->blend_mode_premultiplied);
          r.has_value()) {
        ++report.properties_written;
      }
    }
  }

  // Write the canvas's plane properties directly to `req`. Mirrors
  // Allocator::apply_layer_to_plane minus the immutable-property guard
  // (the canvas only sets writable properties). Bumps the report's
  // diagnostic counters per actual write so the totals account for
  // canvas-arm traffic the allocator never sees.
  drm::expected<void, std::error_code> arm_composition_canvas(
      drm::AtomicRequest& req, const drm::planes::PlaneCapabilities& plane, std::int32_t zpos,
      CommitReport& report) {
    const std::uint32_t plane_id = plane.id;
    auto write = [&](std::string_view name,
                     std::uint64_t value) -> drm::expected<void, std::error_code> {
      auto id = props_.property_id(plane_id, name);
      if (!id) {
        // Property not advertised on this plane — skip silently
        // (zpos is optional on PRIMARY for some drivers).
        return {};
      }
      if (props_.is_immutable(plane_id, name).value_or(false)) {
        return {};
      }
      auto r = req.add_property(plane_id, *id, value);
      if (r.has_value()) {
        ++report.properties_written;
        if (name == "FB_ID") {
          ++report.fbs_attached;
        }
      }
      return r;
    };
    const std::uint32_t cw = composition_canvas_->width();
    const std::uint32_t ch = composition_canvas_->height();
    if (auto r = write("FB_ID", composition_canvas_->fb_id()); !r) {
      return r;
    }
    if (auto r = write("CRTC_ID", crtc_id_); !r) {
      return r;
    }
    if (auto r = write("CRTC_X", 0); !r) {
      return r;
    }
    if (auto r = write("CRTC_Y", 0); !r) {
      return r;
    }
    if (auto r = write("CRTC_W", cw); !r) {
      return r;
    }
    if (auto r = write("CRTC_H", ch); !r) {
      return r;
    }
    if (auto r = write("SRC_X", 0); !r) {
      return r;
    }
    if (auto r = write("SRC_Y", 0); !r) {
      return r;
    }
    if (auto r = write("SRC_W", to_16_16(cw)); !r) {
      return r;
    }
    if (auto r = write("SRC_H", to_16_16(ch)); !r) {
      return r;
    }
    // Force premultiplied alpha-blending on the canvas plane: the
    // canvas covers the whole screen at the topmost zpos (or, on
    // PRIMARY-pinned drivers, at whatever zpos the kernel grants),
    // and its cleared regions sit over the natively-assigned cells
    // beneath. Without an explicit blend-mode write the plane keeps
    // whatever value it inherited (driver default OR a previous frame
    // where the same plane held an opaque XRGB layer); on amdgpu in
    // particular this can be "None", which makes alpha=0 canvas
    // pixels paint *opaque black* over the natives — exactly the
    // "non-uniform black squares" video_grid symptom. The blend-mode
    // enum integer is driver-defined, so use the value the registry
    // cached at enumeration time. Skip silently when the plane doesn't
    // expose the property at all (very old hardware / DRM stacks).
    if (plane.blend_mode_premultiplied.has_value()) {
      if (auto r = write("pixel blend mode", *plane.blend_mode_premultiplied); !r) {
        return r;
      }
    }
    // Per-plane alpha: same risk as blend mode — a previous frame may
    // have left a partial value on this plane (e.g. a fade animation
    // that ended at 0x8000). Pin to fully opaque so the canvas's own
    // per-pixel alpha is the only modulator.
    if (plane.has_per_plane_alpha) {
      if (auto r = write("alpha", 0xFFFFU); !r) {
        return r;
      }
    }
    // zpos is best-effort — skipped silently when the plane doesn't
    // expose it or pins it immutable (amdgpu PRIMARY at 2). The canvas
    // still scans out at whatever the kernel gives us.
    if (zpos > 0) {
      if (auto r = write("zpos", static_cast<std::uint64_t>(zpos)); !r) {
        return r;
      }
    }
    return {};
  }

  // Allocate the MODE_ID blob on demand. Cheap — one blob per scene —
  // but deferred to the first commit so a scene that's built but never
  // committed doesn't leak a blob into the kernel.
  drm::expected<void, std::error_code> ensure_mode_blob() {
    if (mode_blob_id_ != 0) {
      return {};
    }
    std::uint32_t id = 0;
    if (drmModeCreatePropertyBlob(dev_->fd(), &mode_, sizeof(mode_), &id) != 0) {
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::function_not_supported));
    }
    mode_blob_id_ = id;
    return {};
  }

  void destroy_mode_blob() noexcept {
    if (mode_blob_id_ != 0 && dev_ != nullptr && dev_->fd() >= 0) {
      drmModeDestroyPropertyBlob(dev_->fd(), mode_blob_id_);
    }
    mode_blob_id_ = 0;
  }

  // Write CRTC.MODE_ID + CRTC.ACTIVE + CONN.CRTC_ID to a request — the
  // minimum set that brings a cold CRTC up. Used for both the caller's
  // real request and the allocator's internal test requests.
  drm::expected<void, std::error_code> inject_modeset_state(drm::AtomicRequest& req) {
    auto add = [&](std::uint32_t obj, std::string_view name,
                   std::uint64_t value) -> drm::expected<void, std::error_code> {
      auto id = props_.property_id(obj, name);
      if (!id) {
        return drm::unexpected<std::error_code>(id.error());
      }
      return req.add_property(obj, *id, value);
    };
    if (auto r = add(crtc_id_, "MODE_ID", mode_blob_id_); !r) {
      return r;
    }
    if (auto r = add(crtc_id_, "ACTIVE", 1); !r) {
      return r;
    }
    if (auto r = add(connector_id_, "CRTC_ID", crtc_id_); !r) {
      return r;
    }
    return {};
  }

  drm::Device* dev_;
  std::uint32_t crtc_id_;
  std::uint32_t connector_id_;
  drmModeModeInfo mode_;

  drm::planes::PlaneRegistry registry_;
  drm::PropertyStore props_;
  // Held in optional so on_session_resumed can tear down and rebuild
  // against a freshly-reopened Device + a freshly-enumerated registry
  // — Allocator holds `const Device&` and `PlaneRegistry&` references
  // bound at construction, so rebinding them without reconstruction
  // would require API surface on Allocator itself. One optional is
  // cheaper than widening Allocator's contract.
  std::optional<drm::planes::Allocator> allocator_;

  // Placeholder composition layer until Phase 2.3 lands the canvas.
  // Disabled at rest — the Output's allocator-apply path skips it, so
  // having it present but unused costs nothing.
  drm::planes::Layer composition_planes_layer_;
  drm::planes::Output output_;

  std::vector<Slot> slots_;
  std::vector<std::uint32_t> free_ids_;

  std::uint32_t mode_blob_id_{0};
  bool first_commit_{true};
  bool suspended_{false};
  // Mirror of the allocator's force-full-writes flag — kept on Impl
  // so it survives across the allocator teardown/rebuild that
  // on_session_resumed performs.
  bool force_full_writes_{false};

  // Cached PRIMARY-plane zpos_min for this scene's CRTC. Used as a
  // default zpos for single-layer commits where the caller didn't pin
  // zpos — see do_commit for the rationale.
  std::optional<std::uint64_t> primary_zpos_hint_;

  // Lazy composition canvas. Allocated on first compose_unassigned()
  // call that actually needs it; survives across frames so the dumb
  // buffer + fb_id are reused. on_session_paused() forgets the kernel
  // handles; on_session_resumed re-creates the canvas against the new
  // device.
  std::unique_ptr<CompositeCanvas> composition_canvas_;

  // CRTC index in drmModeRes::crtcs, cached at create() / on resume —
  // saves a drmModeGetResources ioctl per composing frame. nullopt
  // means "unresolved"; compose_unassigned re-runs the lookup once
  // each commit until it succeeds (vanishingly rare on real drivers).
  std::optional<std::uint32_t> cached_crtc_index_;

  // The plane the canvas was armed onto on the previous frame. Passed
  // to Allocator::apply as an "external reserved" hint so the
  // allocator's disable_unused_planes pass doesn't write FB_ID=0 /
  // CRTC_ID=0 to that plane only for compose_unassigned to overwrite
  // them moments later. Cleared on session resume (fresh fd) and on
  // any frame where composition didn't run.
  std::optional<std::uint32_t> last_canvas_plane_id_;

  // Per-frame scratch vectors. Keeping them as members avoids the
  // per-frame heap allocation that a fresh local vector would incur
  // on every commit that triggers composition. Capacity carries
  // across frames; clear() preserves it.
  std::vector<AcquisitionSlot*> scratch_composited_;
  std::vector<std::uint32_t> scratch_in_use_;
};

// ─────────────────────────────────────────────────────────────────────
// LayerScene
// ─────────────────────────────────────────────────────────────────────

LayerScene::LayerScene(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
LayerScene::~LayerScene() = default;

drm::expected<std::unique_ptr<LayerScene>, std::error_code> LayerScene::create(drm::Device& dev,
                                                                               const Config& cfg) {
  if (cfg.crtc_id == 0 || cfg.connector_id == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  auto registry = drm::planes::PlaneRegistry::enumerate(dev);
  if (!registry) {
    return drm::unexpected<std::error_code>(registry.error());
  }

  auto impl = std::make_unique<Impl>(dev, cfg, std::move(*registry));
  if (auto r = impl->cache_object_properties(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  impl->compute_primary_zpos_hint();

  // Install the modeset-state preparer on the allocator so its internal
  // test commits carry CRTC + connector state on the first commit.
  // Capture-by-`this` inside install_test_preparer is safe because the
  // preparer's closure lifetime is bounded by the allocator, which is
  // a member of Impl — lifetimes nest.
  impl->install_test_preparer();

  return std::unique_ptr<LayerScene>(new LayerScene(std::move(impl)));
}

drm::expected<LayerHandle, std::error_code> LayerScene::add_layer(LayerDesc desc) {
  return impl_->add_layer(std::move(desc));
}

void LayerScene::remove_layer(LayerHandle handle) {
  impl_->remove_layer(handle);
}

Layer* LayerScene::get_layer(LayerHandle handle) noexcept {
  return impl_->get_layer(handle);
}

const Layer* LayerScene::get_layer(LayerHandle handle) const noexcept {
  return impl_->get_layer(handle);
}

std::size_t LayerScene::layer_count() const noexcept {
  return impl_->layer_count();
}

drm::expected<CommitReport, std::error_code> LayerScene::test() {
  return impl_->do_commit(0, /*test_only=*/true, /*user_data=*/nullptr);
}

drm::expected<CommitReport, std::error_code> LayerScene::commit(std::uint32_t flags,
                                                                void* user_data) {
  return impl_->do_commit(flags, /*test_only=*/false, user_data);
}

void LayerScene::set_force_full_property_writes(bool force) noexcept {
  impl_->set_force_full_property_writes(force);
}

bool LayerScene::force_full_property_writes() const noexcept {
  return impl_->force_full_property_writes();
}

void LayerScene::on_session_paused() noexcept {
  impl_->on_session_paused();
}

drm::expected<void, std::error_code> LayerScene::on_session_resumed(drm::Device& new_dev) {
  return impl_->on_session_resumed(new_dev);
}

drm::expected<CompatibilityReport, std::error_code> LayerScene::rebind(
    std::uint32_t new_crtc_id, std::uint32_t new_connector_id, drmModeModeInfo new_mode) {
  return impl_->rebind(new_crtc_id, new_connector_id, new_mode);
}

}  // namespace drm::scene
