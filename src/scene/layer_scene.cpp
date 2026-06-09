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
#include "output_signaling.hpp"
#include "stream_capability.hpp"

#if DRM_CXX_HAS_EGL_STREAMS
#include "egl_stream_source.hpp"
#endif

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/property_store.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/display/connector_capabilities.hpp>
#include <drm-cxx/display/hdr_metadata.hpp>
#include <drm-cxx/display/hdr_metadata_cache.hpp>
#include <drm-cxx/dumb/buffer.hpp>
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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <unordered_map>
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
  // FrameBuildState (defined below Impl) reads acquisitions, flags, and
  // pre-commit snapshots out of Impl-private nested types
  // (AcquisitionSlot in particular). Friendship limits that exposure to
  // the two SceneSet-integration entry points without dragging
  // AcquisitionSlot into a header.
  friend class drm::scene::FrameBuildState;
  friend struct drm::scene::FrameBuildStateDeleter;

  Impl(drm::Device& dev, const Config& cfg, drm::planes::PlaneRegistry registry) noexcept
      : dev_(&dev),
        crtc_id_(cfg.crtc_id),
        connector_id_(cfg.connector_id),
        mode_(cfg.mode),
        registry_(std::move(registry)),
        output_(cfg.crtc_id, composition_planes_layer_),
        stream_capability_(cfg.stream_capability) {
    allocator_.emplace(*dev_, registry_);
  }

  [[nodiscard]] const StreamCapability& stream_capability() const noexcept {
    return stream_capability_;
  }

  // Empirical mixing probe. See LayerScene::probe_stream_mixing()
  // docstring for the API contract. Implementation runs a single
  // DRM_MODE_ATOMIC_TEST_ONLY commit that adds an FB-ID-attached
  // plane alongside an existing stream-bound plane on the same
  // CRTC; if the kernel accepts, the driver permits FB-ID + stream
  // consumer cohabitation on one CRTC and the scene upgrades to
  // Mixed.
  drm::expected<StreamMixingMode, std::error_code> probe_stream_mixing() {
    if (mixing_probe_ran_) {
      return stream_capability_.mixing;
    }
    if (!stream_capability_.usable()) {
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::function_not_supported));
    }

    // Find a bound stream layer to test against. Without one, there's
    // no kernel-side stream-consumer plane state for the FB-ID probe
    // plane to coexist with.
    std::optional<std::uint32_t> stream_plane_id;
    for (const auto& slot : slots_) {
      if (slot.alive && slot.stream_pinned_plane_id.has_value()) {
        stream_plane_id = slot.stream_pinned_plane_id;
        break;
      }
    }
    if (!stream_plane_id.has_value()) {
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::function_not_supported));
    }

    const auto crtc_index = resolve_crtc_index();
    if (!crtc_index.has_value()) {
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::function_not_supported));
    }

    // Pick a plane to host the probe FB. Skip cursor, the stream pin,
    // every other stream pin, and the canvas reservation. Require
    // ARGB8888 support so the dumb buffer we're about to allocate
    // matches.
    std::optional<std::uint32_t> probe_plane_id;
    for (const auto* p : registry_.for_crtc(*crtc_index)) {
      if (p->type == drm::planes::DRMPlaneType::CURSOR) {
        continue;
      }
      if (p->id == *stream_plane_id) {
        continue;
      }
      if (last_canvas_plane_id_.has_value() && *last_canvas_plane_id_ == p->id) {
        continue;
      }
      const bool pinned_elsewhere =
          std::any_of(slots_.begin(), slots_.end(), [pid = p->id](const Slot& s) {
            return s.alive && s.stream_pinned_plane_id.has_value() &&
                   *s.stream_pinned_plane_id == pid;
          });
      if (pinned_elsewhere) {
        continue;
      }
      if (!p->supports_format(DRM_FORMAT_ARGB8888)) {
        continue;
      }
      probe_plane_id = p->id;
      break;
    }
    if (!probe_plane_id.has_value()) {
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }

    // Allocate a small throwaway dumb buffer + FB. The kernel only
    // looks at the FB's format / size / pitch during atomic_check;
    // it never reads pixels for TEST commits.
    static constexpr std::uint32_t probe_dim = 16;
    drm::dumb::Config dumb_cfg;
    dumb_cfg.width = probe_dim;
    dumb_cfg.height = probe_dim;
    dumb_cfg.drm_format = DRM_FORMAT_ARGB8888;
    dumb_cfg.bpp = 32;
    dumb_cfg.add_fb = true;
    auto probe_buf_r = drm::dumb::Buffer::create(*dev_, dumb_cfg);
    if (!probe_buf_r) {
      return drm::unexpected<std::error_code>(probe_buf_r.error());
    }
    auto probe_buf = std::move(*probe_buf_r);

    drm::AtomicRequest req(*dev_);
    if (!req.valid()) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
    }

    auto write = [&](std::uint32_t plane_id, const char* name,
                     std::uint64_t value) -> drm::expected<void, std::error_code> {
      auto id = props_.property_id(plane_id, name);
      if (!id.has_value()) {
        return {};
      }
      if (props_.is_immutable(plane_id, name).value_or(false)) {
        return {};
      }
      return req.add_property(plane_id, *id, value);
    };

    const std::uint64_t src_w = static_cast<std::uint64_t>(probe_dim) << 16U;
    const std::uint64_t src_h = static_cast<std::uint64_t>(probe_dim) << 16U;
    const std::array<std::pair<const char*, std::uint64_t>, 10> probe_props{{
        {"FB_ID", probe_buf.fb_id()},
        {"CRTC_ID", crtc_id_},
        {"CRTC_X", 0},
        {"CRTC_Y", 0},
        {"CRTC_W", probe_dim},
        {"CRTC_H", probe_dim},
        {"SRC_X", 0},
        {"SRC_Y", 0},
        {"SRC_W", src_w},
        {"SRC_H", src_h},
    }};
    for (const auto& [name, value] : probe_props) {
      if (auto r = write(*probe_plane_id, name, value); !r) {
        return drm::unexpected<std::error_code>(r.error());
      }
    }

    // TEST-only. The kernel evaluates whether the proposed delta —
    // FB-ID arm on probe_plane — is admissible alongside the current
    // stream-consumer state on stream_plane. Success means the
    // driver does NOT enforce single-stream-layer-per-CRTC; failure
    // typically means it does (EBUSY / EINVAL on the test commit).
    const auto commit_r = req.commit(DRM_MODE_ATOMIC_TEST_ONLY);
    mixing_probe_ran_ = true;
    if (commit_r.has_value()) {
      stream_capability_.mixing = StreamMixingMode::Mixed;
      drm::log_info("scene::LayerScene: stream-mixing probe accepted by kernel; upgraded to Mixed");
      return StreamMixingMode::Mixed;
    }
    drm::log_info(
        "scene::LayerScene: stream-mixing probe rejected by kernel ({}); staying Exclusive",
        commit_r.error().message());
    return stream_capability_.mixing;
  }

  ~Impl() {
    // Drain the deferred-release ring first so sources see their
    // buffers returned before they get destroyed via slots_.
    release_pending_acquisitions();
    destroy_mode_blob();
  }

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
    // Cache the connector's HDR / Colorspace / max_bpc capability set.
    // Drives the integer-for-name lookup when writing the connector's
    // Colorspace property (the kernel-assigned integers are
    // driver-defined, so a stale cache from the old connector would
    // collide on rebind / resume).
    auto caps = drm::display::probe_connector_capabilities(*dev_, connector_id_);
    if (!caps) {
      return drm::unexpected<std::error_code>(caps.error());
    }
    connector_caps_ = *caps;
    return {};
  }

  // ── Handle table ──────────────────────────────────────────────────

  drm::expected<LayerHandle, std::error_code> add_layer(LayerDesc desc) {
    if (!desc.source) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
    }
    // Gate DriverOwnsBinding sources behind a usable stream
    // capability. The scene cannot drive a layer whose FB_ID it isn't
    // permitted to write, so rejecting at registration time keeps the
    // failure local to the caller's add_layer instead of erupting deep
    // in commit().
    if (desc.source->binding_model() == BindingModel::DriverOwnsBinding &&
        !stream_capability_.usable()) {
      drm::log_warn(
          "scene::LayerScene::add_layer: source reports DriverOwnsBinding but the scene was "
          "constructed with StreamMixingMode::Unsupported — pass a StreamCapability from "
          "probe_stream_capability() in Config.stream_capability");
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_supported));
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

    slot.scene_layer =
        std::make_unique<Layer>(handle, std::move(desc.source), desc.display, desc.content_type,
                                desc.update_hint_hz, desc.identity_tag);
    return handle;
  }

  Layer* find_by_identity_tag(void* tag) noexcept {
    if (tag == nullptr) {
      return nullptr;
    }
    for (auto& slot : slots_) {
      if (slot.alive && slot.scene_layer && slot.scene_layer->identity_tag() == tag) {
        return slot.scene_layer.get();
      }
    }
    return nullptr;
  }

  const Layer* find_by_identity_tag(void* tag) const noexcept {
    if (tag == nullptr) {
      return nullptr;
    }
    for (const auto& slot : slots_) {
      if (slot.alive && slot.scene_layer && slot.scene_layer->identity_tag() == tag) {
        return slot.scene_layer.get();
      }
    }
    return nullptr;
  }

  void remove_layer(LayerHandle handle) {
    auto* slot = slot_for(handle);
    if (slot == nullptr) {
      return;
    }
    // If the source's stream consumer is currently bound to a plane,
    // tear that binding down before the source unique_ptr is reset.
    // unbind_from_plane is noexcept; failures are logged inside the
    // source.
    if (slot->stream_pinned_plane_id.has_value() && slot->scene_layer) {
      slot->scene_layer->source().unbind_from_plane(*slot->stream_pinned_plane_id);
      slot->stream_pinned_plane_id.reset();
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
    // Scrub the deferred-release ring of any acquisition that still
    // references this layer. The ring holds raw scene_layer pointers
    // (AcquisitionSlot::scene_layer = slot.scene_layer.get()); resetting
    // the owning unique_ptr below would leave them dangling, and the
    // next finalize_frame's release_all() would dereference freed memory
    // (source().release()) → crash. The buffer may still be mid-scanout,
    // but the layer is leaving the scene, so releasing now — while the
    // source is still alive — is the correct trade (mirrors
    // release_pending_acquisitions()'s synchronous drain). Null the slot
    // so release_all() skips it; entries stay in place to preserve ring
    // ordering.
    if (slot->scene_layer) {
      const Layer* dying = slot->scene_layer.get();
      const auto scrub = [&](std::vector<AcquisitionSlot>& ring) {
        for (auto& a : ring) {
          if (a.scene_layer == dying) {
            a.scene_layer->source().release(std::move(a.buffer));
            a.scene_layer = nullptr;
          }
        }
      };
      scrub(prev_acquisitions_);
      scrub(prev_prev_acquisitions_);
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

  // Thin orchestrator over the two-phase build/finalize split. Built-in
  // commit() and test() route through here; SceneSet drives the same
  // split by hand so it can batch N scenes into one drm::AtomicRequest.
  [[nodiscard]] drm::expected<CommitReport, std::error_code> do_commit(std::uint32_t caller_flags,
                                                                       bool test_only,
                                                                       void* user_data) {
    drm::AtomicRequest req(*dev_);
    if (!req.valid()) {
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::not_enough_memory));
    }
    auto build = build_frame_into(req, caller_flags, test_only);
    if (!build) {
      return drm::unexpected<std::error_code>(build.error());
    }
    if (!*build) {
      // Suspended-mode short-circuit: build_frame_into observed
      // suspended_=true and bailed without producing a state. Mirror
      // the prior do_commit behavior of reporting "no work, no error."
      return CommitReport{};
    }
    auto state = std::move(*build);
    const std::uint32_t kernel_flags = LayerScene::effective_flags_of(*state);
    drm::expected<void, std::error_code> kr;
    if (test_only) {
      kr = req.test(kernel_flags);
    } else {
      kr = req.commit(kernel_flags, user_data);
    }
    return finalize_frame(std::move(state), kr);
  }

  // Build the frame's property writes onto the caller-supplied
  // AtomicRequest. Returns a null FrameBuildPtr when the scene is
  // suspended (caller skips this scene in the combined commit). Errors
  // during build (acquisition failure, allocator failure on placement
  // / composition, signaling injection) propagate as drm::unexpected
  // with internal cleanup of any partial state already taken.
  //
  // Defined out-of-line below FrameBuildState so the returned
  // FrameBuildPtr can be constructed against the complete type.
  [[nodiscard]] drm::expected<FrameBuildPtr, std::error_code> build_frame_into(
      drm::AtomicRequest& req, std::uint32_t caller_flags, bool test_only);

  // Reconcile per-layer scene state with the observed kernel
  // outcome. On commit success: acknowledge the HDR blob cache, clear
  // first_commit_, mark every live layer clean, record placements. On
  // commit failure: stick suspended_ on EACCES, propagate the error.
  // Always release the held acquisitions before returning.
  //
  // Test-only builds skip the post-commit state updates and just
  // release acquisitions — the commit_report from build_frame_into is
  // already complete.
  //
  // Defined out-of-line below FrameBuildState.
  [[nodiscard]] drm::expected<CommitReport, std::error_code> finalize_frame(
      FrameBuildPtr state, drm::expected<void, std::error_code> kernel_result);

  // Driver-quirk forwarder. Stored on Impl so on_session_resumed can
  // re-propagate it after the allocator is rebuilt against the new
  // fd; consumers expect "I asked for force-full once, it stays on
  // across pause/resume" semantics.
  void set_output_metadata(const std::optional<drm::display::HdrSourceMetadata>& src) {
    desired_hdr_ = src;
    hdr_user_set_ = true;
    // Conservative: any user-set call marks the next commit as
    // potentially needing ALLOW_MODESET. Same-content sets won't
    // actually escalate the build pass (the blob-id cache dedups),
    // but `would_request_modeset()` can't tell without computing the
    // blob — over-including is safer than under-including for
    // `SceneSet::NarrowPolicy::AutoOnModeset` partitioning. Cleared
    // by `finalize_frame` on a successful real commit.
    hdr_dirty_pending_ = true;
  }

  void set_force_full_property_writes(bool force) noexcept {
    force_full_writes_ = force;
    if (allocator_.has_value()) {
      allocator_->set_force_full_property_writes(force);
    }
  }
  [[nodiscard]] bool force_full_property_writes() const noexcept { return force_full_writes_; }

  void set_external_reserved_planes(drm::span<const std::uint32_t> planes) {
    external_reserved_planes_.assign(planes.data(), planes.data() + planes.size());
  }

  // Conservative pre-build peek for SceneSet::NarrowPolicy::AutoOnModeset.
  // Returns true iff the next build_frame_into pass will definitely
  // (or very likely) OR ALLOW_MODESET into effective_flags.
  // Captures:
  //   * `first_commit_` — pending after create() / rebind() / a
  //     session resume.
  //   * `hdr_dirty_pending_` — a user-set HDR metadata call landed
  //     since the last successful commit. Over-includes when the new
  //     metadata happens to match the cached blob; under-including
  //     here would silently miss user-initiated HDR transitions, so
  //     the over-include is the right side to err on.
  // Auto-derived colorspace / HDR signaling changes from layer
  // content are still not observable here — only running the build
  // can see them. False return is therefore "probably steady-state,
  // but the build may still escalate"; SceneSet checks post-build
  // flags before issuing the kernel commit either way.
  [[nodiscard]] bool would_request_modeset() const noexcept {
    return first_commit_ || hdr_dirty_pending_;
  }

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
    // Drain the deferred-release ring before sources tear down their
    // producer-side state in on_session_paused. The held buffers
    // belong to those sources; release()ing here returns them while
    // the source is still in a state to accept a release.
    release_pending_acquisitions();
    for (auto& slot : slots_) {
      if (slot.alive && slot.scene_layer) {
        slot.scene_layer->source().on_session_paused();
      }
      // Clear stream pins: the source's on_session_paused tears down
      // the EGL stream and producer surface, so the previous plane
      // binding is gone too. The next commit's ensure_stream_layer_pins
      // pass will re-pick and re-bind once the session resumes.
      slot.stream_pinned_plane_id.reset();
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

    // Old fd is dead; the kernel reclaimed every property blob it
    // owned. Forget our HDR cache entries WITHOUT calling
    // drmModeDestroyPropertyBlob — the destroy ioctls would target
    // an already-closed fd, or worse, hit the new fd and free a
    // different blob that happens to share an id. The next
    // commit() rebuilds whatever the caller still wants signaled.
    hdr_cache_.clear_for_session_loss();
    last_written_hdr_blob_id_ = 0;
    last_written_colorspace_value_.reset();

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
    // Mirror the allocator's last_committed_ wipe — sticky_props_
    // tracks kernel state from the old fd and is now stale.
    sticky_props_.clear();

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
    // Empirical mixing result is tied to the prior fd's kernel state;
    // re-probe under the fresh fd if the caller wants the upgrade.
    mixing_probe_ran_ = false;
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

  // ── rebind to a new CRTC / connector / mode ────────────
  drm::expected<CompatibilityReport, std::error_code> rebind(std::uint32_t new_crtc_id,
                                                             std::uint32_t new_connector_id,
                                                             drmModeModeInfo new_mode) {
    // Drain the deferred-release ring before swapping bindings. The
    // held buffers were committed against the OLD crtc/connector and
    // any in-flight scanout there is now moot; releasing them here
    // also avoids leaking buffer references when the planes::Layer
    // twins are reconstructed below.
    release_pending_acquisitions();

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

    // Drop HDR cache entries: they were keyed on the old crtc_id
    // and would leak under the new one. fd is unchanged here, so
    // destroy the kernel handles eagerly (matches destroy_mode_blob
    // above) rather than waiting for fd close.
    hdr_cache_.flush();
    last_written_hdr_blob_id_ = 0;
    last_written_colorspace_value_.reset();

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
        slot.stream_pinned_plane_id.reset();
        continue;
      }
      // Stream pins are CRTC-local: the new CRTC may not even expose
      // the same plane id, and the kernel-side consumer binding from
      // the old CRTC is no longer valid. Tear it down via the source
      // hook; the next commit's ensure_stream_layer_pins pass will
      // re-pick and re-bind on the new CRTC.
      if (slot.stream_pinned_plane_id.has_value()) {
        slot.scene_layer->source().unbind_from_plane(*slot.stream_pinned_plane_id);
        slot.stream_pinned_plane_id.reset();
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
    // Same reasoning as the allocator reset above — sticky_props_
    // tracks the old CRTC's plane state and is now stale.
    sticky_props_.clear();

    primary_zpos_hint_.reset();
    compute_primary_zpos_hint();

    // Composition canvas: the new mode's dimensions may differ from
    // the old. Drop the canvas wholesale; it'll be re-allocated on
    // the next compose_unassigned call at the new size. The
    // dirty-rect tracker resets too.
    composition_canvas_.reset();
    last_canvas_plane_id_.reset();
    cached_crtc_index_.reset();
    // The mixing probe's last verdict was for the previous CRTC; on
    // the new one the driver may behave differently. Re-probe on
    // next call.
    mixing_probe_ran_ = false;

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
    // For DriverOwnsBinding sources only: the DRM plane this source's
    // EGL stream consumer has been bound to. Picked once on first
    // commit, reused thereafter; cleared on session pause / rebind so
    // the next commit re-picks and re-calls bind_to_plane (the source
    // tears down its stream + producer surface on session pause).
    std::optional<std::uint32_t> stream_pinned_plane_id;
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
    // Copied from Slot::stream_pinned_plane_id at acquire_all time.
    // Drives the post-apply property writes in arm_stream_layer_planes
    // and the blend / color arm passes' fallback when the allocator
    // didn't assign the layer (which is always the case for stream
    // layers — they're filtered out of the bipartite match).
    std::optional<std::uint32_t> stream_pinned_plane_id;
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

  drm::expected<void, std::error_code> acquire_all(std::vector<AcquisitionSlot>& out,
                                                   CommitReport& report) {
    for (auto& slot : slots_) {
      if (!slot.alive || !slot.scene_layer) {
        continue;
      }
      auto acq = slot.scene_layer->source().acquire();
      if (!acq) {
        // EAGAIN is flow control, not an error: the source has no
        // frame to contribute this vblank. Typical case is a live
        // source before its first sample lands (GstAppsinkSource
        // pre-preroll). Skip the layer for this commit; once the
        // source produces a frame, subsequent commits pick it up.
        // Counted in report.layers_skipped_no_frame so the
        // unassigned math doesn't mistake the skip for a drop. Any
        // other error code is a real failure and propagates.
        if (acq.error() == std::make_error_code(std::errc::resource_unavailable_try_again)) {
          ++report.layers_skipped_no_frame;
          continue;
        }
        drm::log_warn("scene::LayerScene: source acquire() failed for layer {}: {}",
                      slot.scene_layer->handle().id, acq.error().message());
        return drm::unexpected<std::error_code>(acq.error());
      }
      out.push_back(AcquisitionSlot{slot.scene_layer.get(), slot.planes_layer, std::move(*acq),
                                    std::nullopt, slot.stream_pinned_plane_id});
    }
    return {};
  }

  static void release_all(std::vector<AcquisitionSlot>& acquisitions) noexcept {
    for (auto& a : acquisitions) {
      if (a.scene_layer != nullptr) {
        a.scene_layer->source().release(std::move(a.buffer));
      }
    }
    acquisitions.clear();
  }

  // Drain the deferred-release ring synchronously. Called from
  // on_session_paused / rebind / Impl destruction — paths where no flip
  // is in flight, so holding buffers back from their sources would just
  // leak. Order matches finalize_frame's release order (oldest first).
  void release_pending_acquisitions() noexcept {
    release_all(prev_prev_acquisitions_);
    release_all(prev_acquisitions_);
  }

  // Copy scene::Layer state into the planes::Layer property bag. The
  // allocator and the AtomicRequest it builds read from this bag to
  // write plane properties.
  static void lower_layer(const Layer& src, drm::planes::Layer& dst, std::uint32_t fb_id,
                          std::uint32_t crtc_id, std::optional<std::uint64_t> default_zpos_hint) {
    const auto& d = src.display();
    const auto fmt = src.source().format();
    const bool driver_owns_binding =
        src.source().binding_model() == BindingModel::DriverOwnsBinding;

    // DriverOwnsBinding sources (EGL stream consumers, ...) get their
    // FB_ID set up by the producer-side extension stack
    // (eglStreamConsumerOutputEXT and friends), not by the scene.
    // Skipping the property write avoids racing the consumer's
    // internal FB_ID state and lets the allocator treat the layer as
    // externally bound — see is_externally_bound() and the parallel
    // guards in the allocator.
    dst.set_externally_bound(driver_owns_binding);
    if (!driver_owns_binding) {
      dst.set_property(drm::planes::PropTag::FbId, fb_id);
    }
    // CRTC_ID binds the plane to this scene's CRTC. Without it the
    // kernel rejects the plane commit (FB armed, but the plane is still
    // bound to nothing / to whatever the previous committed CRTC was),
    // the allocator's test commits fail for every plane, and the
    // allocator reports 0 layers assigned.
    dst.set_property(drm::planes::PropTag::CrtcId, crtc_id);

    // KMS plane rectangles: CRTC_* is destination on the scanout, in
    // signed 32-bit pixels. SRC_* is source within the buffer, encoded
    // as 16.16 fixed-point (kernel expects this for subpixel blits).
    dst.set_property(drm::planes::PropTag::CrtcX, static_cast<std::uint64_t>(d.dst_rect.x));
    dst.set_property(drm::planes::PropTag::CrtcY, static_cast<std::uint64_t>(d.dst_rect.y));
    dst.set_property(drm::planes::PropTag::CrtcW, static_cast<std::uint64_t>(d.dst_rect.w));
    dst.set_property(drm::planes::PropTag::CrtcH, static_cast<std::uint64_t>(d.dst_rect.h));
    // Resolve src_rect per the LayerDesc contract: a zero width/height means
    // "the source buffer's full extent". Callers commonly set only dst_rect
    // (the scene has no way to guess a screen position, but the source extent
    // is known from format()), so without this resolution SRC_W/SRC_H below
    // would be written as 0 — which the kernel rejects with EINVAL (errno=22)
    // on the plane commit. That was the root cause of the direct-scanout
    // atomic TEST failing on every controller (amdgpu DC, RPi5 vc4): the layer
    // never carried a valid source rectangle, so direct assignment to any
    // plane was rejected and the frame fell back to composition.
    const std::uint32_t src_w = d.src_rect.w != 0 ? d.src_rect.w : fmt.width;
    const std::uint32_t src_h = d.src_rect.h != 0 ? d.src_rect.h : fmt.height;
    dst.set_property(drm::planes::PropTag::SrcX,
                     to_16_16(static_cast<std::uint32_t>(d.src_rect.x)));
    dst.set_property(drm::planes::PropTag::SrcY,
                     to_16_16(static_cast<std::uint32_t>(d.src_rect.y)));
    dst.set_property(drm::planes::PropTag::SrcW, to_16_16(src_w));
    dst.set_property(drm::planes::PropTag::SrcH, to_16_16(src_h));

    // Format + modifier let the allocator statically screen planes for
    // compatibility before any test commit. The allocator reads both
    // through planes::Layer's format()/modifier() accessors, which are
    // backed by the PixelFormat / FbModifier PropTag entries (not
    // the KMS plane-property names — these are internal-to-Layer hints).
    dst.set_property(drm::planes::PropTag::PixelFormat, fmt.drm_fourcc);
    dst.set_property(drm::planes::PropTag::FbModifier, fmt.modifier);

    // Optional plane properties. rotation and zpos are only written when
    // the caller set a value; emitting zpos=0 unconditionally would
    // static-compat-reject any PRIMARY plane with an immutable non-zero
    // zpos (amdgpu pins PRIMARY at zpos=2), leaving the scene with
    // nowhere to put a single layer.
    if (d.rotation != 0) {
      dst.set_property(drm::planes::PropTag::Rotation, d.rotation);
    }
    if (d.zpos.has_value()) {
      dst.set_property(drm::planes::PropTag::Zpos, static_cast<std::uint64_t>(*d.zpos));
    } else if (default_zpos_hint.has_value()) {
      dst.set_property(drm::planes::PropTag::Zpos, *default_zpos_hint);
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
    dst.set_property(drm::planes::PropTag::Alpha, static_cast<std::uint64_t>(d.alpha));

    dst.set_content_type(src.content_type());
    if (src.update_hint_hz() != 0) {
      dst.set_update_hint(src.update_hint_hz());
    }
  }

  // ── Composition fallback ───────────────────────────────
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

  // Pick a DRM plane for a DriverOwnsBinding source's stream consumer
  // to bind to. The allocator never matches stream layers (their
  // planes::Layer is_externally_bound), so the scene picks here:
  // first CRTC-compatible non-cursor plane that supports the source's
  // format and isn't already pinned to another stream layer or
  // reserved for the composition canvas. Prefers PRIMARY when free,
  // otherwise the first matching OVERLAY. Returns nullopt when no
  // candidate exists; the caller drops the stream layer for the frame
  // and retries next commit.
  std::optional<std::uint32_t> pick_stream_plane(
      std::uint32_t crtc_index, std::uint32_t source_fourcc,
      const std::vector<std::uint32_t>& already_pinned) const {
    auto is_unavailable = [&](std::uint32_t plane_id) {
      if (last_canvas_plane_id_.has_value() && *last_canvas_plane_id_ == plane_id) {
        return true;
      }
      return std::any_of(already_pinned.begin(), already_pinned.end(),
                         [plane_id](std::uint32_t p) { return p == plane_id; });
    };
    const drm::planes::PlaneCapabilities* primary_match = nullptr;
    for (const auto* p : registry_.for_crtc(crtc_index)) {
      if (p->type == drm::planes::DRMPlaneType::CURSOR) {
        continue;
      }
      if (is_unavailable(p->id)) {
        continue;
      }
      if (!p->supports_format(source_fourcc)) {
        continue;
      }
      if (p->type == drm::planes::DRMPlaneType::PRIMARY) {
        if (primary_match == nullptr) {
          primary_match = p;
        }
        continue;
      }
      // OVERLAY (or any non-cursor non-primary) — take the first one.
      return p->id;
    }
    if (primary_match != nullptr) {
      return primary_match->id;
    }
    return std::nullopt;
  }

  // For every alive slot whose source is DriverOwnsBinding and that
  // has no current plane pin, pick a plane and call the source's
  // bind_to_plane(). On success the slot remembers the plane; on
  // failure the slot stays unpinned and the source's acquire() will
  // return EAGAIN, so the scene drops the layer for this frame and
  // the next commit retries.
  //
  // Called before acquire_all(), because EglStreamSource::acquire()
  // returns EAGAIN until bind_to_plane has succeeded. Also called
  // directly from LayerScene::prepare_stream_layers (the public
  // pre-commit hook the NVIDIA-Streams first-frame flow needs), so
  // it sits in the public section despite being implementation
  // detail for everyone else.
 public:
  // Union of (drm_format-matching) modifiers across every non-cursor
  // plane on this CRTC. Drives producer-side modifier negotiation —
  // see LayerScene::candidate_modifiers docstring.
  std::vector<std::uint64_t> candidate_modifiers(std::uint32_t drm_format) {
    std::vector<std::uint64_t> out;
    const auto crtc_index = resolve_crtc_index();
    if (!crtc_index.has_value()) {
      return out;
    }
    out.reserve(16);
    for (const auto* p : registry_.for_crtc(*crtc_index)) {
      if (p == nullptr || p->type == drm::planes::DRMPlaneType::CURSOR) {
        continue;
      }
      if (p->has_format_modifiers) {
        for (const auto& [fmt, mod] : p->format_modifiers) {
          if (fmt != drm_format) {
            continue;
          }
          if (std::find(out.begin(), out.end(), mod) == out.end()) {
            out.push_back(mod);
          }
        }
      } else if (p->supports_format(drm_format)) {
        // No IN_FORMATS — driver only commits to LINEAR/INVALID
        // semantics for this plane. Report INVALID once; callers
        // treat it as "the driver picks (typically LINEAR)".
        constexpr std::uint64_t k_mod_invalid = (1ULL << 56U) - 1U;
        if (std::find(out.begin(), out.end(), k_mod_invalid) == out.end()) {
          out.push_back(k_mod_invalid);
        }
      }
    }
    return out;
  }

  void ensure_stream_layer_pins() {
    const auto crtc_index = resolve_crtc_index();
    if (!crtc_index.has_value()) {
      return;
    }
    std::vector<std::uint32_t> already_pinned;
    already_pinned.reserve(8);
    for (const auto& slot : slots_) {
      if (slot.alive && slot.stream_pinned_plane_id.has_value()) {
        already_pinned.push_back(*slot.stream_pinned_plane_id);
      }
    }
    for (auto& slot : slots_) {
      if (!slot.alive || slot.scene_layer == nullptr) {
        continue;
      }
      if (slot.stream_pinned_plane_id.has_value()) {
        continue;
      }
      if (slot.scene_layer->source().binding_model() != BindingModel::DriverOwnsBinding) {
        continue;
      }
      const auto fmt = slot.scene_layer->source().format();
      const auto plane_id = pick_stream_plane(*crtc_index, fmt.drm_fourcc, already_pinned);
      if (!plane_id.has_value()) {
        drm::log_warn(
            "scene::LayerScene: no DRM plane available for stream layer {} (format 0x{:x})",
            slot.scene_layer->handle().id, fmt.drm_fourcc);
        continue;
      }
      auto bind_r = slot.scene_layer->source().bind_to_plane(*plane_id);
      if (!bind_r) {
        drm::log_warn("scene::LayerScene: bind_to_plane({}) failed for stream layer {}: {}",
                      *plane_id, slot.scene_layer->handle().id, bind_r.error().message());
        continue;
      }
      slot.stream_pinned_plane_id = plane_id;
      already_pinned.push_back(*plane_id);
    }
  }

  // Apply the `Exclusive` stream-mixing constraint: when the
  // empirical probe has confirmed the driver can't have FB-ID +
  // stream consumer planes on the same CRTC, force every non-stream
  // layer through the composition path so only the stream plane +
  // the canvas plane end up on the CRTC. Resets the per-layer
  // transient-composited flag at every call (clearing the previous
  // commit's decision), then sets it for non-stream layers when:
  //
  //   * The mixing probe has actually run (mixing_probe_ran_),
  //   * its verdict is Exclusive (not the conservative default),
  //   * and at least one stream layer is present.
  //
  // Pre-probe the cached `mixing` defaults to Exclusive, but acting
  // on that on the very first commit would force the background
  // through composition before modeset has armed PRIMARY -- the
  // canvas plane has nowhere to land and the kernel rejects the
  // commit with EINVAL. The probe runs after at least one commit
  // has succeeded with the stream layer bound, so this gate
  // naturally lets the first commit proceed permissively (Mixed
  // shape) and only tightens to Exclusive once the kernel has
  // confirmed it.
  void apply_exclusive_mixing_constraint() {
    for (auto& slot : slots_) {
      if (slot.alive && (slot.planes_layer != nullptr)) {
        slot.planes_layer->set_transient_composited(false);
      }
    }
    if (!mixing_probe_ran_ || stream_capability_.mixing != StreamMixingMode::Exclusive) {
      return;
    }
    const bool has_stream_layer = std::any_of(slots_.begin(), slots_.end(), [](const Slot& s) {
      return s.alive && (s.scene_layer != nullptr) &&
             s.scene_layer->source().binding_model() == BindingModel::DriverOwnsBinding;
    });
    if (!has_stream_layer) {
      return;
    }
    for (auto& slot : slots_) {
      if (!slot.alive || (slot.scene_layer == nullptr) || (slot.planes_layer == nullptr)) {
        continue;
      }
      if (slot.scene_layer->source().binding_model() == BindingModel::DriverOwnsBinding) {
        continue;
      }
      slot.planes_layer->set_transient_composited(true);
    }
  }

 private:
  // Bookkeeping for stream-pinned acquisitions. We don't write any
  // KMS properties for these planes — desktop NVIDIA drives the
  // consumer plane's FB / CRTC binding entirely inside
  // eglStreamConsumerOutputEXT + auto-acquire; mixing in our own
  // CRTC_*/SRC_* writes earns an EINVAL from atomic_check because
  // the plane is "active" without a userspace-armed FB. The
  // accounting here just keeps `report.layers_assigned` honest so
  // the post-loop "dropped" tally doesn't fire on stream layers
  // that are working as intended.
  static void count_stream_layers_assigned(const std::vector<AcquisitionSlot>& acquisitions,
                                           CommitReport& report) {
    for (const auto& acq : acquisitions) {
      if (acq.stream_pinned_plane_id.has_value() && acq.planes_layer != nullptr) {
        ++report.layers_assigned;
      }
    }
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
    // honored the reservation by leaving it out of best_assignment.
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
        // `function_not_supported` is the documented contract for an
        // uncompositable (scanout-only) source — see BufferSource::map().
        // The allocator couldn't place it on a plane and it has no CPU
        // mapping to composite from, so dropping it is expected behaviour,
        // not a failure. Log at debug to avoid a per-frame warn when such a
        // source is in the scene every frame (e.g. a direct-scanout-only
        // SceneSubmitsFbId source on a driver that routes it to composition).
        // Genuine map failures (EIO, ENOMEM, …) still warn.
        if (mapping.error() == std::errc::function_not_supported) {
          drm::log_debug(
              "scene::LayerScene: layer {} dropped — source is uncompositable "
              "(no CPU mapping) and the allocator found no plane for it",
              acq.scene_layer->handle().id);
        } else {
          drm::log_warn(
              "scene::LayerScene: layer {} needs composition but its source "
              "map() failed ({}); dropping",
              acq.scene_layer->handle().id, mapping.error().message());
        }
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
    // One bulk memcpy from the cached userspace shadow into the WC
    // dumb buffer. The shadow tracks a per-frame dirty rect (union of
    // clear+blend regions), so the flush only writes the touched area
    // rather than the full canvas — typically a few percent of the
    // canvas size for dashboard-style scenes.
    composition_canvas_->flush();

    const std::int32_t canvas_zpos = choose_canvas_zpos(acquisitions, scratch_composited_);
    if (auto r = arm_composition_canvas(req, *target_plane, canvas_zpos, report); !r) {
      drm::log_warn("scene::LayerScene: arm composition canvas failed: {}", r.error().message());
      return;
    }
    report.layers_composited += scratch_composited_.size();
    report.composition_buckets += 1U;
    last_canvas_plane_id_ = target_plane->id;
  }

  // Classify every acquired layer as AssignedToPlane / Composited /
  // Unassigned and append a row to report.placements. Run after
  // compose_unassigned so its `cached_mapping` flag is the
  // authoritative composited signal — a layer the allocator dropped
  // and the compositor rescued shows up as Composited; a layer the
  // allocator dropped that compose_unassigned couldn't rescue (no CPU
  // mapping, no free canvas plane) shows up as Unassigned.
  void populate_report_placements(const std::vector<AcquisitionSlot>& acquisitions,
                                  CommitReport& report) const {
    report.placements.clear();
    report.placements.reserve(acquisitions.size());
    for (const auto& acq : acquisitions) {
      if (acq.scene_layer == nullptr || acq.planes_layer == nullptr) {
        continue;
      }
      LayerPlacementEntry entry;
      entry.handle = acq.scene_layer->handle();
      const auto pid = acq.planes_layer->assigned_plane_id();
      if (pid.has_value()) {
        entry.placement = LayerPlacement::AssignedToPlane;
        entry.plane_id = *pid;
      } else if (acq.cached_mapping.has_value() && last_canvas_plane_id_.has_value()) {
        entry.placement = LayerPlacement::Composited;
        entry.plane_id = *last_canvas_plane_id_;
      } else {
        entry.placement = LayerPlacement::Unassigned;
        entry.plane_id = 0;
      }
      report.placements.push_back(entry);
    }
  }

  // Mirror the report's per-layer placement back onto each scene::Layer
  // so future Layer::last_assigned_plane_id() / Layer::last_placement()
  // reads return this commit's outcome. Called from the commit-success
  // branch only — test() leaves the layer's prior placement state
  // intact.
  void record_layer_placements(const std::vector<AcquisitionSlot>& acquisitions) const {
    for (const auto& acq : acquisitions) {
      if (acq.scene_layer == nullptr || acq.planes_layer == nullptr) {
        continue;
      }
      const auto pid = acq.planes_layer->assigned_plane_id();
      if (pid.has_value()) {
        acq.scene_layer->record_placement(LayerPlacement::AssignedToPlane, pid);
      } else if (acq.cached_mapping.has_value() && last_canvas_plane_id_.has_value()) {
        acq.scene_layer->record_placement(LayerPlacement::Composited, last_canvas_plane_id_);
      } else {
        acq.scene_layer->record_placement(LayerPlacement::Unassigned, std::nullopt);
      }
    }
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
                                      drm::AtomicRequest& req, CommitReport& report,
                                      bool test_only) {
    const auto crtc_index_opt = resolve_crtc_index();
    if (!crtc_index_opt.has_value()) {
      return;
    }
    for (const auto& acq : acquisitions) {
      // Fall back to the scene-side stream pin when the allocator
      // didn't assign — stream-bound layers are filtered out of
      // placement but their plane still needs blend defaults armed.
      auto plane_id = acq.planes_layer->assigned_plane_id();
      if (!plane_id.has_value()) {
        plane_id = acq.stream_pinned_plane_id;
      }
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
      const auto value = *caps->blend_mode_premultiplied;
      // Skip the write when we've already committed this value on
      // this plane — see sticky_props_ docs for the why. TEST_ONLY
      // doesn't promote into the cache.
      if (sticky_props_[*plane_id].pixel_blend_mode == value) {
        continue;
      }
      if (auto r = req.add_property(*plane_id, *prop_id, value); r.has_value()) {
        ++report.properties_written;
        if (!test_only) {
          sticky_props_[*plane_id].pixel_blend_mode = value;
        }
      }
    }
  }

  // Wire each natively-placed layer's acquire fence to its plane. When the
  // plane advertises IN_FENCE_FD, hand the sync_file to KMS so the kernel waits
  // on the producer's render before scanning out; otherwise CPU-wait the fence
  // here so we never scan out a half-rendered buffer (correct on drivers without
  // IN_FENCE_FD). The SyncFence stays in the AcquiredBuffer and closes on buffer
  // release — the kernel does not take ownership of IN_FENCE_FD. The CPU-wait
  // fallback runs only on the real commit; TEST_ONLY never scans out.
  drm::expected<void, std::error_code> arm_layer_acquire_fences(
      const std::vector<AcquisitionSlot>& acquisitions, drm::AtomicRequest& req,
      CommitReport& report, bool test_only) {
    for (const auto& acq : acquisitions) {
      if (!acq.buffer.acquire_fence.has_value()) {
        continue;
      }
      const auto plane_id = acq.planes_layer->assigned_plane_id();
      if (!plane_id.has_value()) {
        continue;  // composited/dropped: the buffer's lifecycle owns the fence
      }
      const auto fence_fd = static_cast<std::uint64_t>(acq.buffer.acquire_fence->fd());
      const auto prop_id = props_.property_id(*plane_id, "IN_FENCE_FD");
      if (prop_id.has_value() && !props_.is_immutable(*plane_id, "IN_FENCE_FD").value_or(false)) {
        if (auto r = req.add_property(*plane_id, *prop_id, fence_fd); !r) {
          return r;
        }
        ++report.properties_written;
      } else if (!test_only) {
        if (auto r = acq.buffer.acquire_fence->wait(std::chrono::seconds(1)); !r) {
          drm::log_warn("scene::LayerScene: acquire-fence CPU wait failed: {}",
                        r.error().message());
        }
      }
    }
    return {};
  }

  // For every layer the allocator placed natively, write the plane's
  // `COLOR_ENCODING` and `COLOR_RANGE` properties to the layer's
  // override (when set) or to the scene's default of BT.709 + Limited.
  // Mirrors `arm_layer_plane_blend_defaults`: the properties are
  // process-sticky and a previous compositor's stale settings
  // shift colors on YUV layers (cyan / magenta tint), so re-emit on
  // every native plane each frame regardless of the format being
  // scanned out.
  void arm_layer_plane_color_props(const std::vector<AcquisitionSlot>& acquisitions,
                                   drm::AtomicRequest& req, CommitReport& report, bool test_only) {
    const auto crtc_index_opt = resolve_crtc_index();
    if (!crtc_index_opt.has_value()) {
      return;
    }
    for (const auto& acq : acquisitions) {
      // Fall back to the stream pin when the allocator didn't assign,
      // mirroring arm_layer_plane_blend_defaults.
      auto plane_id = acq.planes_layer->assigned_plane_id();
      if (!plane_id.has_value()) {
        plane_id = acq.stream_pinned_plane_id;
      }
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
      if (caps == nullptr) {
        continue;
      }

      const auto& display = acq.scene_layer->display();

      // Two enum properties live in sticky_props_; same skip-when-
      // unchanged + don't-promote-on-test_only shape as the blend
      // mode write above. `cached` is the per-plane snapshot slot
      // for whichever enum this call is writing.
      auto write_enum = [&](const char* prop_name, std::optional<std::uint64_t> value,
                            std::optional<std::uint64_t>& cached) {
        if (!value.has_value()) {
          return;
        }
        const auto prop_id = props_.property_id(*plane_id, prop_name);
        if (!prop_id) {
          return;
        }
        if (props_.is_immutable(*plane_id, prop_name).value_or(false)) {
          return;
        }
        if (cached == *value) {
          return;
        }
        if (auto r = req.add_property(*plane_id, *prop_id, *value); r.has_value()) {
          ++report.properties_written;
          if (!test_only) {
            cached = value;
          }
        }
      };

      if (caps->has_color_encoding) {
        const auto enc = display.color_encoding.value_or(drm::planes::ColorEncoding::BT_709);
        std::optional<std::uint64_t> raw;
        switch (enc) {
          case drm::planes::ColorEncoding::BT_601:
            raw = caps->color_encoding_bt601;
            break;
          case drm::planes::ColorEncoding::BT_709:
            raw = caps->color_encoding_bt709;
            break;
          case drm::planes::ColorEncoding::BT_2020:
            raw = caps->color_encoding_bt2020;
            break;
        }
        // If the requested entry is missing from this driver's enum
        // table, fall back to whichever named entry the driver does
        // expose. BT.709 is ubiquitous; BT.601 is the next-most.
        if (!raw.has_value()) {
          raw = caps->color_encoding_bt709.has_value() ? caps->color_encoding_bt709
                                                       : caps->color_encoding_bt601;
        }
        write_enum("COLOR_ENCODING", raw, sticky_props_[*plane_id].color_encoding);
      }

      if (caps->has_color_range) {
        const auto range = display.color_range.value_or(drm::planes::ColorRange::Limited);
        const auto raw = (range == drm::planes::ColorRange::Limited) ? caps->color_range_limited
                                                                     : caps->color_range_full;
        // Same fallback shape as encoding: Limited > Full when the
        // driver only exposes one named entry.
        std::optional<std::uint64_t> resolved = raw;
        if (!resolved.has_value()) {
          resolved = caps->color_range_limited;
        }
        if (!resolved.has_value()) {
          resolved = caps->color_range_full;
        }
        write_enum("COLOR_RANGE", resolved, sticky_props_[*plane_id].color_range);
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
  // write the connector's HDR_OUTPUT_METADATA
  // property. `effective_md` is the metadata to apply: either the
  // caller's manual set_output_metadata input (when hdr_user_set_)
  // or the auto-derived metadata from per-layer source_eotf
  // Returns true when the blob id changed and the
  // kernel therefore requires ALLOW_MODESET to validate (amdgpu /
  // i915 both reject HDR_OUTPUT_METADATA transitions without it).
  drm::expected<bool, std::error_code> inject_hdr_output_metadata(
      drm::AtomicRequest& req, const std::optional<drm::display::HdrSourceMetadata>& effective_md) {
    // Skip entirely when no caller ever set HDR, the auto-derive is
    // empty, AND we've never written a non-zero blob ourselves.
    // The last clause matters when the auto-derive previously
    // produced HDR but the layer dropped its source_eotf — we
    // need one more pass through hdr_cache_.set(nullopt) to write
    // blob_id=0 and clear the connector property.
    if (!hdr_user_set_ && !effective_md.has_value() && last_written_hdr_blob_id_ == 0) {
      return false;
    }
    const auto prop = props_.property_id(connector_id_, "HDR_OUTPUT_METADATA");
    if (!prop) {
      // Connector doesn't expose HDR_OUTPUT_METADATA (older kernel,
      // non-HDR sink). Silently swallow per the public API
      // contract — caller can probe connector capabilities up
      // front if it wants to gate.
      return false;
    }
    auto blob_id = hdr_cache_.set(*dev_, crtc_id_, effective_md);
    if (!blob_id) {
      return drm::unexpected<std::error_code>(blob_id.error());
    }
    if (auto r = req.add_property(connector_id_, *prop, *blob_id); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
    const bool needs_modeset = *blob_id != last_written_hdr_blob_id_;
    last_written_hdr_blob_id_ = *blob_id;
    return needs_modeset;
  }

  // write the connector's `Colorspace` property to the
  // enum value matching the auto-derived ColorPrimaries (per
  // derive_output_signaling). Falls back to `Default` when the
  // requested entry isn't on the connector's enum table -- the
  // amdgpu RDNA set, for instance, lacks DCI-P3 but has Default,
  // BT709_YCC, BT2020_RGB, BT2020_YCC, opRGB. Returns true when
  // the property value changed (caller ORs ALLOW_MODESET; the
  // kernel treats Colorspace transitions the same as
  // HDR_OUTPUT_METADATA transitions, since both reconfigure the
  // AVI / DRM InfoFrame).
  drm::expected<bool, std::error_code> inject_output_colorspace(
      drm::AtomicRequest& req, std::optional<drm::display::Colorspace> desired) {
    if (!desired.has_value() || !connector_caps_.has_colorspace) {
      return false;
    }
    auto value = connector_caps_.colorspace_value(*desired);
    if (!value.has_value()) {
      // Requested entry not advertised — fall back to Default.
      // Default is the kernel's "let driver pick" sentinel and is
      // present on every connector that exposes Colorspace at all.
      value = connector_caps_.colorspace_value(drm::display::Colorspace::Default);
      if (!value.has_value()) {
        return false;
      }
    }
    if (last_written_colorspace_value_.has_value() && *last_written_colorspace_value_ == *value) {
      return false;
    }
    const auto prop = props_.property_id(connector_id_, "Colorspace");
    if (!prop) {
      return false;
    }
    if (auto r = req.add_property(connector_id_, *prop, *value); !r) {
      return drm::unexpected<std::error_code>(r.error());
    }
    last_written_colorspace_value_ = value;
    return true;
  }

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

  // Placeholder composition layer that compose_unassigned arms when
  // unassigned layers fall back to the canvas.
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

  // Stream capability snapshot, taken at construction (and
  // preserved across rebind / resume — the capability describes the
  // driver and is invariant under connector/CRTC changes). Consumed
  // by add_layer to gate `BindingModel::DriverOwnsBinding` sources
  // and by the commit-path branch.
  StreamCapability stream_capability_;

  // True once probe_stream_mixing() has run since the most recent
  // create() / rebind() / session_resumed. Once true, subsequent
  // calls return the cached `stream_capability_.mixing` without
  // re-running the kernel-side TEST commit. Cleared on rebind and
  // session resume because the new CRTC / fresh fd would need its
  // own empirical confirmation.
  bool mixing_probe_ran_{false};

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
  // Per-frame layer-display-params view used by derive_output_signaling.
  // Held as a member so the per-frame heap allocation is paid once.
  std::vector<const drm::scene::DisplayParams*> scratch_layer_params_;
  // Per-frame "external plane reservations" list build_frame_into
  // assembles for the allocator. Lives only inside that call; cleared
  // + reserved at top.
  std::vector<std::uint32_t> scratch_reserved_planes_;
  // Caller-supplied plane ids the scene must NEVER disable: planes
  // armed by something outside this scene (e.g. a hardware/overlay
  // cursor managed by drm::cursor::Renderer). On a CRTC with no
  // dedicated cursor plane the cursor takes an overlay this scene
  // would otherwise treat as "unused" and disable on every commit,
  // fighting the cursor's own commits. build_frame_into folds these
  // into scratch_reserved_planes_ so the allocator's disable-unused
  // pass leaves them alone. Set via set_external_reserved_planes().
  std::vector<std::uint32_t> external_reserved_planes_;
  // Per-frame acquisitions list. build_frame_into populates it via
  // acquire_all and std::moves it into FrameBuildState::acquisitions
  // before returning. finalize_frame moves the (now-cleared but
  // capacity-retaining) vector back here so the next frame skips
  // the buffer allocation. After a failed build / dropped state the
  // capacity is lost — exception-safety insurance, not the steady
  // state.
  std::vector<AcquisitionSlot> scratch_acquisitions_;

  // Two-deep ring of in-flight acquisitions for the deferred-release
  // contract documented on `LayerBufferSource::release`: source
  // release() must not be called until the buffer has been replaced
  // on screen by a subsequent commit's flip. With NONBLOCK + page-flip-
  // event commits, the safe release point for buf_N is the kernel
  // commit at frame N+2 — by then buf_N's vblank has fired and buf_(N+1)
  // is on screen. Releasing earlier (e.g. immediately at finalize_frame
  // of commit N, the prior implementation) lets producers like
  // V4l2CameraSource QBUF the just-committed buffer back to the kernel
  // for re-capture while DRM is still scanning it out — visible as
  // tearing during motion on hardware whose producer driver doesn't
  // attach reservation fences (uvcvideo, most V4L2 capture drivers).
  // Test commits, failed real commits, on_session_paused, and rebind
  // bypass the ring and release immediately — no flip happened in
  // those paths, so the deferral isn't needed and could leak buffers.
  std::vector<AcquisitionSlot> prev_acquisitions_;
  std::vector<AcquisitionSlot> prev_prev_acquisitions_;

  // Per-plane "what we last actually committed" snapshot for the
  // properties that arm_layer_plane_blend_defaults /
  // arm_layer_plane_color_props re-emit defensively each frame
  // (pixel_blend_mode + COLOR_ENCODING + COLOR_RANGE — see
  // `reference_plane_property_stickiness.md`). The defensive write
  // was added to overwrite stale values left on the plane by a
  // previous compositor; once we've written our intended value, the
  // kernel preserves it across our subsequent commits, so the next
  // frame can suppress the write when the value matches. Mirrors the
  // allocator's `last_committed_` shape — cleared on session resume
  // / rebind (same lifecycle the allocator gets reset under), TEST
  // commits don't promote into the cache.
  struct PlaneStickyProps {
    std::optional<std::uint64_t> pixel_blend_mode;
    std::optional<std::uint64_t> color_encoding;
    std::optional<std::uint64_t> color_range;
  };
  std::unordered_map<std::uint32_t, PlaneStickyProps> sticky_props_;

  // HDR output metadata signaling. `desired_hdr_` carries the
  // most-recent set_output_metadata input (`nullopt` == "clear");
  // `hdr_user_set_` flips true on the first call so connectors that
  // never had HDR signaled don't accumulate redundant property
  // writes. The cache owns the kernel blobs; commit() pushes
  // pending destruction through `acknowledge_committed()` after a
  // successful real commit.
  drm::display::HdrMetadataCache hdr_cache_;
  std::optional<drm::display::HdrSourceMetadata> desired_hdr_;
  bool hdr_user_set_{false};
  // Set by `set_output_metadata` and cleared on a successful real
  // commit. Conservative pre-build signal for
  // `SceneSet::NarrowPolicy::AutoOnModeset`: over-includes (same-
  // content sets land here even though the build pass dedups them),
  // but never under-includes a real HDR transition.
  bool hdr_dirty_pending_{false};
  // Last HDR blob id we wrote to the connector property. Used to
  // detect changes that require ALLOW_MODESET. Cleared on session
  // loss / rebind alongside the cache.
  std::uint32_t last_written_hdr_blob_id_{0};

  // connector Colorspace property tracking. Cached
  // capabilities + last-written integer drive the same
  // dedup-and-modeset-on-change logic as HDR_OUTPUT_METADATA.
  drm::display::ConnectorCapabilities connector_caps_{};
  std::optional<std::uint64_t> last_written_colorspace_value_;
};

// ─────────────────────────────────────────────────────────────────────
// FrameBuildState — owns the per-frame state that flows from
// build_frame_into to finalize_frame. Defined after Impl so the
// AcquisitionSlot type (an Impl-private nested struct) is complete.
// ─────────────────────────────────────────────────────────────────────

class FrameBuildState {
 public:
  FrameBuildState() = default;
  FrameBuildState(const FrameBuildState&) = delete;
  FrameBuildState& operator=(const FrameBuildState&) = delete;
  FrameBuildState(FrameBuildState&&) noexcept = default;
  FrameBuildState& operator=(FrameBuildState&&) noexcept = default;
  ~FrameBuildState() = default;

  std::vector<LayerScene::Impl::AcquisitionSlot> acquisitions;
  std::uint32_t effective_flags{0};
  bool test_only{false};
  CommitReport report{};
};

void FrameBuildStateDeleter::operator()(FrameBuildState* state) const noexcept {
  if (state != nullptr && !state->acquisitions.empty()) {
    // Defensive: a caller that drops the state without finalize_frame
    // would leak the source-held buffer acquisitions. The first-class
    // paths (LayerScene::commit, SceneSet::commit) always finalize, so
    // this branch is exception-safety insurance.
    LayerScene::Impl::release_all(state->acquisitions);
  }
  delete state;
}

// ─────────────────────────────────────────────────────────────────────
// LayerScene::Impl::build_frame_into / finalize_frame
//
// The two halves of the old do_commit. Defined out-of-line so they can
// see FrameBuildState's complete type when constructing / consuming
// the FrameBuildPtr. Behavior matches the pre-split flow line for
// line; the only structural change is where `req.test()/commit()` sits
// (now in do_commit / SceneSet) and what flows across the boundary
// (a FrameBuildState rather than locals).
// ─────────────────────────────────────────────────────────────────────

drm::expected<FrameBuildPtr, std::error_code> LayerScene::Impl::build_frame_into(
    drm::AtomicRequest& req, std::uint32_t caller_flags, bool test_only) {
  CommitReport report;
  report.layers_total = layer_count();

  // Short-circuit while the seat is suspended. The kernel revokes
  // commit privileges before libseat delivers pause_cb, so commit()
  // starts returning EACCES some frames ahead of the notification. A
  // sticky flag here keeps us from burning frames in the allocator
  // between that first EACCES and the resume_cb that clears it.
  if (suspended_) {
    return FrameBuildPtr{};
  }

  // Establish stream-source plane pins before acquire_all runs. EGL
  // stream sources return EAGAIN from acquire() until their consumer
  // is wired to a plane (eglStreamConsumerOutputEXT), and that wiring
  // is what ensure_stream_layer_pins drives via the source's
  // bind_to_plane hook. Non-stream sources are untouched.
  ensure_stream_layer_pins();

  // Exclusive-mixing constraint enforcement. When the stream
  // capability says Exclusive (the driver doesn't permit FB-ID and
  // stream-consumer planes to coexist on one CRTC) and any layer
  // in the scene is stream-bound, force every non-stream layer
  // through the composition path so the kernel only sees the
  // stream plane + the canvas plane on this CRTC. Mixed-mode
  // capability skips the constraint — the allocator can place
  // FB-ID layers natively alongside the stream consumer plane.
  apply_exclusive_mixing_constraint();

  // Acquire every live layer's buffer up front. On any failure the
  // already-acquired buffers are handed back to their sources.
  // scratch_acquisitions_ carries capacity across frames (see member
  // doc); finalize_frame moves the cleared vector back to restore
  // the buffer for the next commit.
  scratch_acquisitions_.clear();
  scratch_acquisitions_.reserve(report.layers_total);
  auto& acquisitions = scratch_acquisitions_;
  if (auto r = acquire_all(acquisitions, report); !r) {
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
  // by the time build_frame_into runs.
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
  // External reservations: the composition canvas plane (if any)
  // plus every plane pinned to a DriverOwnsBinding source. The
  // allocator must leave all of these alone — the scene writes
  // their properties itself via compose_unassigned / the canvas-arm
  // path and arm_stream_layer_planes respectively.
  scratch_reserved_planes_.clear();
  scratch_reserved_planes_.reserve(1 + acquisitions.size() + external_reserved_planes_.size());
  if (last_canvas_plane_id_.has_value()) {
    scratch_reserved_planes_.push_back(*last_canvas_plane_id_);
  }
  for (const auto& acq : acquisitions) {
    if (acq.stream_pinned_plane_id.has_value()) {
      scratch_reserved_planes_.push_back(*acq.stream_pinned_plane_id);
    }
  }
  // Caller-armed planes (e.g. an overlay cursor managed outside this
  // scene): the allocator must not disable them in its disable-unused
  // pass — the external owner drives their properties itself.
  for (const auto id : external_reserved_planes_) {
    scratch_reserved_planes_.push_back(id);
  }
  const auto reserved_span = scratch_reserved_planes_.empty()
                                 ? drm::span<const std::uint32_t>{}
                                 : drm::span<const std::uint32_t>(scratch_reserved_planes_.data(),
                                                                  scratch_reserved_planes_.size());
  auto assigned =  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
      allocator_->apply(output_, req, effective_flags, reserved_span, test_only);
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
  // Stream layers are intentionally untouched in the scene's
  // atomic commit. Desktop NVIDIA (no EGL_NV_output_drm_atomic
  // extension) handles plane FB / CRTC binding inside
  // eglStreamConsumerOutputEXT and subsequent auto-acquire
  // events; if the scene writes CRTC_*/SRC_* without FB_ID the
  // kernel rejects the commit (EINVAL on an active plane with
  // no framebuffer). bookkeeping-only here keeps the dropped-
  // layers tally honest.
  count_stream_layers_assigned(acquisitions, report);

  arm_layer_plane_blend_defaults(acquisitions, req, report, test_only);
  arm_layer_plane_color_props(acquisitions, req, report, test_only);
  if (auto r = arm_layer_acquire_fences(acquisitions, req, report, test_only); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }

  // rescue unassigned layers via CPU composition before
  // counting them as dropped. compose_unassigned() updates
  // report.layers_composited and report.composition_buckets; the
  // dropped tally below is the residual that wasn't rescued
  // (no CPU mapping, no free plane, canvas alloc failed).
  compose_unassigned(acquisitions, req, report);

  // Subtract skipped layers from the residual: they're flow-controlled
  // (no new frame this vblank), not dropped, and the warning below
  // would be misleading otherwise.
  const auto accounted =
      report.layers_assigned + report.layers_composited + report.layers_skipped_no_frame;
  if (report.layers_total > accounted) {
    report.layers_unassigned = report.layers_total - accounted;
    // On a real commit a dropped layer means content the user won't see, so
    // warn. During a test_only dry run this is a placement *prediction* (the
    // caller inspects report.placements to decide a fallback path, e.g. routing
    // the frame through its own compositor); nothing is actually dropped, so
    // emitting it at warn every probed frame is misleading noise — log at debug.
    if (test_only) {
      drm::log_debug("scene::LayerScene: {} layer(s) would be dropped (test)",
                     report.layers_unassigned);
    } else {
      drm::log_warn("scene::LayerScene: {} layer(s) dropped this frame", report.layers_unassigned);
    }
  }

  // Per-layer placement readout. Always populates `report.placements`
  // so test() consumers (e.g. probe modes) can see the same shape a
  // real commit would land. Scene::Layer state is only updated on
  // the real-commit success branch in finalize_frame, after the
  // kernel commit lands — test() must not mutate observable layer
  // state.
  populate_report_placements(acquisitions, report);

  // auto-derive connector signaling from the live
  // layers' DisplayParams. Manual `set_output_metadata` overrides
  // the HDR half — the auto-derive is a sensible default for
  // callers that haven't bothered to populate mastering data.
  // Colorspace is always auto-derived (no manual override yet).
  scratch_layer_params_.clear();
  scratch_layer_params_.reserve(acquisitions.size());
  for (const auto& acq : acquisitions) {
    if (acq.scene_layer != nullptr) {
      scratch_layer_params_.push_back(&acq.scene_layer->display());
    }
  }
  const auto signaling = drm::scene::derive_output_signaling(
      drm::span<const drm::scene::DisplayParams* const>(scratch_layer_params_.data(),
                                                        scratch_layer_params_.size()),
      &connector_caps_);
  const auto& effective_hdr = hdr_user_set_ ? desired_hdr_ : signaling.hdr_metadata;
  // surface the auto-derive's downgrade in the report
  // so callers can see when their HDR layers got silently dropped
  // to SDR. Manual override bypasses the constraint check, so the
  // flag stays false on that path.
  report.hdr_downgraded_no_max_bpc = signaling.hdr_downgraded;

  // Colorspace first (modeset-needy properties on the same
  // connector should batch into one ALLOW_MODESET commit).
  auto cs_changed = inject_output_colorspace(req, signaling.colorspace);
  if (!cs_changed) {
    release_all(acquisitions);
    return drm::unexpected<std::error_code>(cs_changed.error());
  }
  if (*cs_changed) {
    effective_flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
  }

  // HDR_OUTPUT_METADATA. inject_hdr_output_metadata hashes the
  // metadata, dedups against the per-CRTC cache, and writes the
  // property. No-op when neither manual nor auto-derive is in
  // play, or when the connector doesn't expose the property.
  auto hdr_needs_modeset = inject_hdr_output_metadata(req, effective_hdr);
  if (!hdr_needs_modeset) {
    release_all(acquisitions);
    return drm::unexpected<std::error_code>(hdr_needs_modeset.error());
  }
  if (*hdr_needs_modeset) {
    effective_flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
  }

  FrameBuildPtr out(new FrameBuildState{});
  out->acquisitions = std::move(acquisitions);
  out->effective_flags = effective_flags;
  out->test_only = test_only;
  out->report = std::move(report);
  return out;
}

drm::expected<CommitReport, std::error_code> LayerScene::Impl::finalize_frame(
    FrameBuildPtr state, drm::expected<void, std::error_code> kernel_result) {
  CommitReport report = std::move(state->report);
  // EACCES from the kernel means the seat just lost master (compositor
  // foregrounded, libseat is about to fire pause_cb). Mirror the old
  // do_commit logic: stick suspended_ so subsequent calls short-circuit
  // until resume.
  if (!kernel_result) {
    if (kernel_result.error() == std::errc::permission_denied) {
      suspended_ = true;
    }
    release_all(state->acquisitions);
    // Return the cleared-but-capacity-retaining vector to the
    // per-Impl scratch. Mirror in the success path below.
    scratch_acquisitions_ = std::move(state->acquisitions);
    return drm::unexpected<std::error_code>(kernel_result.error());
  }

  if (!state->test_only) {
    // Commit succeeded: the connector property has switched to the
    // new HDR blob, so prior blobs in the cache's pending-destruction
    // queue are safe to release.
    hdr_cache_.acknowledge_committed();
    // Only real commits flip the scene past first-commit; tests don't.
    first_commit_ = false;
    // The user's set_output_metadata input (if any) has been
    // resolved by this commit's build pass — the cache caught a
    // same-blob noop or the kernel acknowledged a new blob id. Clear
    // the AutoOnModeset hint so the next commit isn't unnecessarily
    // split off as modeset-needing.
    hdr_dirty_pending_ = false;
    // Mark dirty layers clean for a successful commit, and record the
    // placement we just wrote so `Layer::last_assigned_plane_id()` /
    // `Layer::last_placement()` reflect this commit's outcome on
    // subsequent reads.
    for (auto& slot : slots_) {
      if (slot.alive && slot.scene_layer) {
        slot.scene_layer->mark_clean();
      }
    }
    record_layer_placements(state->acquisitions);
    output_.mark_clean();

    // Deferred release: rotate the in-flight ring. See
    // prev_acquisitions_ / prev_prev_acquisitions_ member docs for the
    // why. Sequence (state on entry → state on exit):
    //   prev_prev = buf_(N-2)  → released back to its source (now empty)
    //   prev      = buf_(N-1)  → moves to prev_prev (still in flight,
    //                            on screen until commit N+1's vblank)
    //   state     = buf_N      → moves to prev (just queued for next
    //                            vblank, will be on screen between
    //                            event N and event N+1)
    // The empty vector that was prev_prev becomes the new prev's
    // backing storage on its way through. The ex-state vector goes
    // back to scratch for next frame's build to fill.
    release_all(prev_prev_acquisitions_);
    std::swap(prev_prev_acquisitions_, prev_acquisitions_);
    std::swap(prev_acquisitions_, state->acquisitions);
    scratch_acquisitions_ = std::move(state->acquisitions);
    return report;
  }

  // Test-only commits and (above) failed real commits release
  // immediately — no flip happened, so nothing in the kernel's
  // scanout pipeline is referencing these buffers and deferral
  // would just stall the source's buffer ring. Mirrors the
  // failure path early-return above.
  release_all(state->acquisitions);
  scratch_acquisitions_ = std::move(state->acquisitions);
  return report;
}

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

Layer* LayerScene::find_by_identity_tag(void* tag) noexcept {
  return impl_->find_by_identity_tag(tag);
}

const Layer* LayerScene::find_by_identity_tag(void* tag) const noexcept {
  return impl_->find_by_identity_tag(tag);
}

std::size_t LayerScene::layer_count() const noexcept {
  return impl_->layer_count();
}

const StreamCapability& LayerScene::stream_capability() const noexcept {
  return impl_->stream_capability();
}

std::vector<std::uint64_t> LayerScene::candidate_modifiers(std::uint32_t drm_format) const {
  return impl_->candidate_modifiers(drm_format);
}

drm::expected<StreamMixingMode, std::error_code> LayerScene::probe_stream_mixing() {
  return impl_->probe_stream_mixing();
}

void LayerScene::prepare_stream_layers() {
  impl_->ensure_stream_layer_pins();
}

drm::expected<CommitReport, std::error_code> LayerScene::test() {
  return impl_->do_commit(0, /*test_only=*/true, /*user_data=*/nullptr);
}

drm::expected<CommitReport, std::error_code> LayerScene::commit(std::uint32_t flags,
                                                                void* user_data) {
  return impl_->do_commit(flags, /*test_only=*/false, user_data);
}

drm::expected<FrameBuildPtr, std::error_code> LayerScene::build_frame_into(
    drm::AtomicRequest& req, std::uint32_t caller_flags, bool test_only) {
  return impl_->build_frame_into(req, caller_flags, test_only);
}

drm::expected<CommitReport, std::error_code> LayerScene::finalize_frame(
    FrameBuildPtr state, drm::expected<void, std::error_code> kernel_result) {
  return impl_->finalize_frame(std::move(state), kernel_result);
}

std::uint32_t LayerScene::effective_flags_of(const FrameBuildState& state) noexcept {
  return state.effective_flags;
}

bool LayerScene::would_request_modeset() const noexcept {
  return impl_->would_request_modeset();
}

void LayerScene::set_output_metadata(const std::optional<drm::display::HdrSourceMetadata>& src) {
  impl_->set_output_metadata(src);
}

void LayerScene::set_force_full_property_writes(bool force) noexcept {
  impl_->set_force_full_property_writes(force);
}

void LayerScene::set_external_reserved_planes(drm::span<const std::uint32_t> planes) {
  impl_->set_external_reserved_planes(planes);
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
