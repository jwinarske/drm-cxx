// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "layer_scene.hpp"

#include "buffer_source.hpp"
#include "layer.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/property_store.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/log.hpp>
#include <drm-cxx/modeset/atomic.hpp>
#include <drm-cxx/planes/allocator.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/planes/output.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <memory>
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
    // Single-layer scenes without an explicit zpos get the PRIMARY
    // plane's zpos_min as a hint: the allocator's preseed prefers
    // OVERLAY (+2 score) over PRIMARY for non-composition layers, which
    // causes its TEST commits to explicitly disable PRIMARY while
    // activating the CRTC — amdgpu rejects that combination with EINVAL
    // (active CRTC requires an armed PRIMARY plane). Pinning zpos to
    // PRIMARY's zpos_min lights up the primary-affinity bonus in
    // score_pair and steers the single layer onto PRIMARY directly.
    std::optional<std::uint64_t> zpos_hint;
    if (layer_count() == 1 && primary_zpos_hint_.has_value()) {
      zpos_hint = primary_zpos_hint_;
    }
    for (const auto& acq : acquisitions) {
      lower_layer(*acq.scene_layer, *acq.planes_layer, acq.buffer.fb_id, crtc_id_, zpos_hint);
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
    // placed on hardware; anything else is unassigned (Phase 2.1 drops
    // with a log_warn; Phase 2.3 will composite).
    auto assigned = allocator_->apply(output_, req, effective_flags);
    if (!assigned) {
      release_all(acquisitions);
      return drm::unexpected<std::error_code>(assigned.error());
    }
    report.layers_assigned = *assigned;
    report.layers_unassigned = report.layers_total - report.layers_assigned;
    if (report.layers_unassigned != 0) {
      drm::log_warn(
          "scene::LayerScene: {} layer(s) unassigned by allocator — "
          "dropped this frame (composition fallback lands in Phase 2.3)",
          report.layers_unassigned);
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

  // Wire up the allocator's internal test_preparer to this Impl's
  // modeset-state injector. Called once during LayerScene::create after
  // the Impl is fully constructed — can't go in the ctor because the
  // preparer closure needs `this`, and passing it through the Impl
  // constructor would leak implementation types into the header.
  void install_test_preparer() {
    allocator_->set_test_preparer(
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
    // the previous-frame warm state and the failure cache — correct,
    // since both reference kernel state that's gone. Reinstall the
    // test_preparer closure (its `this` capture is still valid; only
    // the underlying allocator changed).
    allocator_.reset();
    allocator_.emplace(*dev_, registry_);
    install_test_preparer();

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

    // Next commit must carry ALLOW_MODESET to bring the new CRTC
    // binding up from cold; matches create-time semantics.
    first_commit_ = true;
    suspended_ = false;
    return {};
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
    // Only write alpha when the caller asked for something other than
    // fully opaque. The pre-LayerScene thorvg_janitor path never wrote
    // alpha and reliably got PAGE_FLIP_EVENT back on first commit;
    // unconditionally writing alpha=0xFFFF on amdgpu PRIMARY correlates
    // with the kernel accepting the commit but never queuing the vblank
    // event, wedging flip_pending. Skip the no-op write.
    if (d.alpha != 0xFFFF) {
      dst.set_property("alpha", static_cast<std::uint64_t>(d.alpha));
    }

    dst.set_content_type(src.content_type());
    if (src.update_hint_hz() != 0) {
      dst.set_update_hint(src.update_hint_hz());
    }
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

  // Cached PRIMARY-plane zpos_min for this scene's CRTC. Used as a
  // default zpos for single-layer commits where the caller didn't pin
  // zpos — see do_commit for the rationale.
  std::optional<std::uint64_t> primary_zpos_hint_;
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

void LayerScene::on_session_paused() noexcept {
  impl_->on_session_paused();
}

drm::expected<void, std::error_code> LayerScene::on_session_resumed(drm::Device& new_dev) {
  return impl_->on_session_resumed(new_dev);
}

}  // namespace drm::scene
