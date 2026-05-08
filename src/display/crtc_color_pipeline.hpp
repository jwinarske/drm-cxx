// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// crtc_color_pipeline.hpp — RAII-managed CRTC color pipeline.
//
// Wraps the kernel's three CRTC-side color-pipeline blob
// properties (DEGAMMA_LUT, CTM, GAMMA_LUT). Callers pick a built-in
// curve (PQ → linear, linear → PQ, BT.2020 → BT.709, …) or supply
// custom LUT / matrix data; the wrapper builds the property blobs,
// owns their kernel handles, and writes the property values into a
// caller-supplied `AtomicRequest`.
//
// Property-blob lifecycle is per-stage: setting a new DEGAMMA blob
// destroys the prior one, etc. The pattern matches the existing
// MODE_ID handling in `modeset.cpp` — kernel ref-counting keeps the
// previously-applied blob alive internally until the property
// switches over, so eager destroy-on-replace is safe.

#pragma once

#include "crtc_capabilities.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm/drm_mode.h>

#include <cstdint>
#include <system_error>

namespace drm {
class Device;
class AtomicRequest;
}  // namespace drm

namespace drm::display {

class CrtcColorPipeline {
 public:
  /// Build a pipeline bound to `crtc_id`. Probes `CrtcCapabilities`
  /// during construction; returns `errc::operation_not_supported`
  /// when the CRTC exposes none of DEGAMMA / CTM / GAMMA. Drivers
  /// that expose a partial set (vkms with GAMMA only) succeed; the
  /// pipeline rejects calls targeting absent stages.
  [[nodiscard]] static drm::expected<CrtcColorPipeline, std::error_code> create(
      const drm::Device& dev, std::uint32_t crtc_id);

  CrtcColorPipeline() noexcept = default;
  ~CrtcColorPipeline();

  CrtcColorPipeline(CrtcColorPipeline&& other) noexcept;
  CrtcColorPipeline& operator=(CrtcColorPipeline&& other) noexcept;
  CrtcColorPipeline(const CrtcColorPipeline&) = delete;
  CrtcColorPipeline& operator=(const CrtcColorPipeline&) = delete;

  // ── Built-in shapes ──────────────────────────────────────────────

  /// Set every available stage to identity. `errc::operation_not_supported`
  /// if the CRTC has none of the three properties.
  drm::expected<void, std::error_code> set_identity();

  /// DEGAMMA: SMPTE ST 2084 PQ EOTF (encoded → linear).
  drm::expected<void, std::error_code> set_pq_to_linear();

  /// DEGAMMA: ITU-R BT.2100 HLG OETF^-1 (encoded → scene-linear).
  drm::expected<void, std::error_code> set_hlg_to_linear();

  /// CTM: BT.2020 → BT.709 RGB (linear-light). Requires a DEGAMMA
  /// stage that produces linear values.
  drm::expected<void, std::error_code> set_bt2020_to_bt709();

  /// GAMMA: linear → SMPTE ST 2084 PQ OETF.
  drm::expected<void, std::error_code> set_linear_to_pq();

  // ── Custom shapes ────────────────────────────────────────────────

  /// DEGAMMA / GAMMA / CTM custom data. The LUT spans must match
  /// the corresponding `CrtcCapabilities::*_lut_size` exactly;
  /// passing a different size returns `errc::invalid_argument`.
  drm::expected<void, std::error_code> set_custom_degamma(drm::span<const drm_color_lut> lut);
  drm::expected<void, std::error_code> set_custom_gamma(drm::span<const drm_color_lut> lut);
  drm::expected<void, std::error_code> set_custom_ctm(const drm_color_ctm& ctm);

  // ── Apply / lifecycle ────────────────────────────────────────────

  /// Write the staged blob ids onto the connector / CRTC properties
  /// in `req`. Properties that haven't been set this session are
  /// left out of the request (the kernel preserves whatever value
  /// the property currently has). The atomic commit / test is the
  /// caller's responsibility.
  drm::expected<void, std::error_code> apply(drm::AtomicRequest& req) const;

  /// Forget every staged blob without calling
  /// `drmModeDestroyPropertyBlob`. Use on session loss; the kernel
  /// reclaims the blobs when the originating fd closes.
  void clear_for_session_loss() noexcept;

  /// Read the cached capability table.
  [[nodiscard]] const CrtcCapabilities& capabilities() const noexcept { return caps_; }

  /// Currently-staged blob ids. Zero when the corresponding stage
  /// hasn't been set (or was reset). Useful for tests + diagnostics.
  [[nodiscard]] std::uint32_t degamma_blob_id() const noexcept { return degamma_blob_; }
  [[nodiscard]] std::uint32_t ctm_blob_id() const noexcept { return ctm_blob_; }
  [[nodiscard]] std::uint32_t gamma_blob_id() const noexcept { return gamma_blob_; }

 private:
  CrtcColorPipeline(int fd, std::uint32_t crtc_id, CrtcCapabilities caps) noexcept;

  void destroy_blob(std::uint32_t& blob_id) const noexcept;
  drm::expected<void, std::error_code> replace_lut_blob(std::uint32_t& slot,
                                                        drm::span<const drm_color_lut> lut);
  drm::expected<void, std::error_code> replace_ctm_blob(std::uint32_t& slot,
                                                        const drm_color_ctm& ctm);

  int fd_{-1};
  std::uint32_t crtc_id_{0};
  CrtcCapabilities caps_{};
  std::uint32_t degamma_blob_{0};
  std::uint32_t ctm_blob_{0};
  std::uint32_t gamma_blob_{0};
};

}  // namespace drm::display
