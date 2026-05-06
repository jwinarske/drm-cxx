// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// gst_appsink_source.hpp — LayerBufferSource that pulls decoded frames
// from a caller-owned GStreamer `appsink` element and exposes the most
// recent one as a scanout-ready KMS framebuffer.
//
// Motivating consumer: video playback, hardware-decoded camera feeds,
// and any pipeline a user already has in GStreamer that can be
// terminated with `appsink name=sink`. The user owns the upstream
// pipeline (filesrc / camera / decoder / parser); drm-cxx owns the
// appsink-to-scene bridge.
//
// Buffer paths:
//   * **DMABUF zero-copy (primary)** — when the appsink hands back a
//     `GstSample` whose memory is `GstDmaBufMemory`, the source pulls
//     each plane's fd via `gst_dmabuf_memory_get_fd`, looks up
//     per-plane offsets/strides from `GstVideoMeta`, runs the same
//     `drmPrimeFDToHandle` + `drmModeAddFB2WithModifiers` import as
//     `ExternalDmaBufSource`, and caches one fb_id per source-buffer
//     pointer so a small ring of GStreamer buffers becomes a small
//     pool of stable KMS FBs. Suitable for `v4l2h264dec`, `vah264dec`,
//     `nvh264dec` and most embedded hardware decoder elements.
//   * **System-memory memcpy (fallback)** — when the sample is plain
//     `GstMemory` (software decode on x86, mismatched plugin set), the
//     source maps it READ-ONLY, memcpy's into a per-source dumb buffer,
//     and surfaces that. Costs the bandwidth of one full frame per
//     acquire; tolerable on desktop, painful on weak ARM. Detected at
//     first sample.
//
// Latest-frame-wins drop semantics: the source configures the appsink
// with `drop = true`, `max-buffers = 1`, `sync = false` so a slow
// consumer cannot stall the decoder. `acquire()` returns the most
// recent buffer the appsink has parked; older ones are dropped by
// GStreamer before they reach us.
//
// Threading: `acquire()` polls via `gst_app_sink_try_pull_sample` from
// whatever thread the scene drives commits on; GStreamer's appsink
// internally serialises against its streaming thread, so no mutex is
// needed in this TU. No GLib main loop is required either — the source
// neither connects nor dispatches signals. Bus messages (errors, EOS)
// are pumped via `drive()` from the caller's loop.
//
// Format negotiation: caps come from the appsink's negotiated stream;
// the source resolves them to a DRM FourCC + LINEAR modifier on first
// sample and caches the result. Format change mid-stream is handled
// transparently: the source tears down its FB cache + sysmem fallback,
// re-resolves the format from the new caps, and re-imports the new
// sample. Consumers that cached `format()` must re-query it after a
// caps change — `acquire()` reflects the new dimensions immediately
// on the post-change call, but the layer's display rect (in
// `LayerDesc`) is the caller's responsibility to update.
//
// Fence import: producer-specific elements may attach a sync_file fd
// to the GstBuffer via custom meta; the GStreamer ecosystem has no
// universal extractor for this, so the caller supplies one through
// `GstAppsinkConfig::fence_extractor`. When present, its return value
// (an fd to a sync_file) is dup'd and surfaced as
// `AcquiredBuffer::acquire_fence_fd`. Default: no-op (no fence —
// produces fence_fd == -1, which is what V4L2 → drm-cxx scanout
// already runs without).
//
// Appsink ownership: the caller builds the pipeline and passes the
// appsink `GstElement*` in. The source ref's the element on `create`
// and unref's it on destruction; the caller is free to release their
// own reference any time after `create` returns.

#pragma once

#include "buffer_source.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <system_error>

// Forward-declare the few GStreamer typedefs we need in the public
// surface, so this header stays GStreamer-free for consumers who don't
// link against it. Match GStreamer's own naming verbatim so the
// typedef collapses to the same type when <gst/gst.h> is in scope —
// the leading-underscore tag would otherwise be a reserved identifier
// per [global.names], so silence the lint narrowly.
extern "C" {
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming)
struct _GstElement;
using GstElement = struct _GstElement;
// NOLINTNEXTLINE(bugprone-reserved-identifier,readability-identifier-naming)
struct _GstSample;
using GstSample = struct _GstSample;
}

namespace drm {
class Device;
}  // namespace drm

namespace drm::scene {

/// Configuration for `GstAppsinkSource::create`. Default values match
/// the latest-frame-wins drop semantics described in the file comment;
/// override only when a caller has unusual buffering needs.
struct GstAppsinkConfig {
  /// Maximum number of stable KMS FB IDs the source caches. Each entry
  /// holds a duped DMABUF fd, a GEM handle, and an FB ID. Should be
  /// >= the upstream decoder's CAPTURE-buffer ring size to amortize
  /// the import cost; smaller values still work but every recycle of
  /// a not-cached buffer pays one drmModeAddFB2 round-trip.
  std::uint32_t fb_cache_size{8};

  /// Configure the appsink for latest-frame-wins drop on construction.
  /// When false the caller is expected to have configured the appsink
  /// themselves (e.g. via `g_object_set` before `create`); the source
  /// leaves the relevant properties alone.
  bool configure_drop_oldest{true};

  /// Optional callback the source uses to pull a sync_file fd off a
  /// freshly-acquired sample. Producer-specific (e.g. a Vulkan-render
  /// pipeline element that stashes its OUT_FENCE in a custom GstMeta);
  /// when null the source surfaces `acquire_fence_fd = -1`. Returning
  /// `-1` from the callback is also treated as "no fence." The source
  /// dups the returned fd via `F_DUPFD_CLOEXEC`, so the callback may
  /// return a borrowed fd whose lifetime is bound to the GstSample.
  std::function<int(GstSample*)> fence_extractor;
};

/// `LayerBufferSource` that bridges a GStreamer `appsink` element to
/// a KMS scanout-ready framebuffer. See file comment for the full
/// contract.
class GstAppsinkSource : public LayerBufferSource {
 public:
  /// Bind to `appsink` and prepare the import path against `dev`.
  ///
  /// Argument validation runs before any GStreamer call:
  ///   * `appsink` non-null,
  ///   * `cfg.fb_cache_size` non-zero.
  ///
  /// On success the appsink's "new-sample" signal is wired into the
  /// source. The source dups the underlying DRM fd from `dev` so its
  /// lifetime is independent of the caller's `Device` object, and
  /// `on_session_resumed` swaps to a new fd cleanly.
  ///
  /// In a build without GStreamer support (`DRM_CXX_HAS_GSTREAMER=0`)
  /// every call returns `errc::function_not_supported`. The header
  /// compiles either way, so users with a GStreamer-less build can
  /// still see the API surface and feature-test against it.
  [[nodiscard]] static drm::expected<std::unique_ptr<GstAppsinkSource>, std::error_code> create(
      const drm::Device& dev, GstElement* appsink, const GstAppsinkConfig& cfg = {});

  GstAppsinkSource(const GstAppsinkSource&) = delete;
  GstAppsinkSource& operator=(const GstAppsinkSource&) = delete;
  GstAppsinkSource(GstAppsinkSource&&) = delete;
  GstAppsinkSource& operator=(GstAppsinkSource&&) = delete;
  ~GstAppsinkSource() override;

  // ── Caller API: bus pumping ─────────────────────────────────────────

  /// Pump the appsink's pipeline bus once, non-blocking. Surfaces
  /// pipeline errors (decoder failure, source EOF, format change) as
  /// distinct error codes:
  ///   * `errc::operation_canceled` — caps changed mid-stream;
  ///   * `errc::no_message_available` — pipeline reached EOS;
  ///   * `errc::io_error` — element posted an ERROR message.
  /// Returns success when no message is pending. Callers usually run
  /// this once per frame from their main loop.
  drm::expected<void, std::error_code> drive() noexcept;

  // ── LayerBufferSource ────────────────────────────────────────────────

  [[nodiscard]] drm::expected<AcquiredBuffer, std::error_code> acquire() override;
  void release(AcquiredBuffer acquired) noexcept override;
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override;

  // map() inherits the base default. DMABUF samples are not generally
  // CPU-mappable in a useful way (vendor strides, possibly carveout
  // memory). System-memory samples *could* be exposed as a CPU
  // mapping, but the asymmetry would surprise the composition fallback;
  // safer to declare the source uncompositable across both modes.

  void on_session_paused() noexcept override;
  [[nodiscard]] drm::expected<void, std::error_code> on_session_resumed(
      const drm::Device& new_dev) override;

  // Implementation detail; defined in the .cpp. Forward-declared at
  // public scope so the .cpp's anon-namespace helpers can name the
  // type, but consumers cannot construct or copy one — the API surface
  // remains pimpl from the outside.
  struct Impl;

 private:
  GstAppsinkSource();

  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::scene
