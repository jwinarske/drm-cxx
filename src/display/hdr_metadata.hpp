// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// hdr_metadata.hpp — source-side HDR static metadata.
//
// `HdrSourceMetadata` is a friendly description of what content the
// source is emitting (transfer function, mastering display primaries,
// luminance levels, MaxCLL / MaxFALL); `HdrMetadataBlob` is a RAII
// wrapper around the kernel property blob the connector's
// `HDR_OUTPUT_METADATA` property points at. The blob is what drives
// the HDR InfoFrame the source sends to the sink per CTA-861.3 / SMPTE
// ST 2086.
//
// Rationale: callers describe their content; the library serializes
// to the kernel UAPI struct (`struct hdr_output_metadata`), creates +
// destroys the property blob, and exposes a stable content hash so a
// per-CRTC cache can skip redundant blob churn.

#pragma once

#include "connector_info.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <cstdint>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

namespace drm::display {

/// Source-side electro-optical transfer function. Integer values match
/// the kernel's `enum hdmi_eotf` (kernel-internal, replicated here so
/// the userspace mapping is stable).
enum class TransferFunction : std::uint8_t {
  TraditionalGammaSdr = 0,  // BT.1886 / sRGB
  TraditionalGammaHdr = 1,
  SmpteSt2084Pq = 2,  // HDR10 / HDR10+ (PQ)
  Bt2100Hlg = 3,      // HLG
};

/// Source-side HDR static metadata. Describes the *content* the source
/// is emitting: transfer function, mastering display primaries +
/// luminance, content light levels. Serialised to the kernel's
/// `struct hdr_output_metadata` (`HDMI_STATIC_METADATA_TYPE1`) by
/// `HdrMetadataBlob::create`.
///
/// Luminance / MaxCLL / MaxFALL units follow CTA-861.3:
///   * `max_display_mastering_luminance`: cd/m² (1 cd/m² steps)
///   * `min_display_mastering_luminance`: 0.0001 cd/m² steps
///   * `max_content_light_level`: cd/m² (1 cd/m² steps)
///   * `max_frame_average_light_level`: cd/m² (1 cd/m² steps)
struct HdrSourceMetadata {
  TransferFunction eotf{TransferFunction::TraditionalGammaSdr};

  /// Mastering display primaries + white point. Ordering follows
  /// `ColorimetryInfo` (red / green / blue / white); the serializer
  /// reorders to CTA-861.3 §6.9.1.5 (green, blue, red) on the way
  /// into the kernel struct. All coordinates are CIE 1931 xy
  /// (display-referred); the spec encodes them as 16-bit unsigned
  /// in 0.00002 steps (0xC350 == 1.0).
  ColorimetryInfo display_primaries;

  std::uint16_t max_display_mastering_luminance{};
  std::uint16_t min_display_mastering_luminance{};
  std::uint16_t max_content_light_level{};
  std::uint16_t max_frame_average_light_level{};
};

/// Serialise an `HdrSourceMetadata` to the kernel's
/// `struct hdr_output_metadata` UAPI layout. Returns the raw bytes
/// suitable for `drmModeCreatePropertyBlob`. Display primaries are
/// reordered to CTA-861.3 §6.9.1.5 (green, blue, red); float xy
/// coordinates are converted to 16-bit unsigned 0.00002-steps and
/// clamped to `[0, 0xFFFF]`.
[[nodiscard]] std::vector<std::uint8_t> serialize_hdr_metadata(
    const HdrSourceMetadata& src) noexcept;

/// Stable 64-bit FNV-1a hash over the serialized representation.
/// Identical content yields identical hash; any single-field change
/// yields a different hash. Drives blob diff/replace decisions in the
/// per-CRTC cache so unchanged metadata doesn't churn the
/// kernel blob every frame.
[[nodiscard]] std::uint64_t hdr_metadata_hash(const HdrSourceMetadata& src) noexcept;

/// RAII wrapper around a kernel property blob holding a serialized
/// `struct hdr_output_metadata`. Destruction calls
/// `drmModeDestroyPropertyBlob`; move-only.
///
/// Callers point the connector's `HDR_OUTPUT_METADATA` property at
/// `blob_id()`; setting that property to `0` instead clears HDR
/// signaling on the connector (kernel returns to SDR mode).
class HdrMetadataBlob {
 public:
  /// Build a property blob for `src`. Calls `drmModeCreatePropertyBlob`
  /// with the serialized bytes; returns `errc::*` on ioctl failure.
  static drm::expected<HdrMetadataBlob, std::error_code> create(const drm::Device& dev,
                                                                const HdrSourceMetadata& src);

  HdrMetadataBlob() noexcept = default;
  ~HdrMetadataBlob();

  HdrMetadataBlob(HdrMetadataBlob&& other) noexcept;
  HdrMetadataBlob& operator=(HdrMetadataBlob&& other) noexcept;
  HdrMetadataBlob(const HdrMetadataBlob&) = delete;
  HdrMetadataBlob& operator=(const HdrMetadataBlob&) = delete;

  /// Kernel property-blob id. `0` when the blob is empty / moved-from.
  [[nodiscard]] std::uint32_t blob_id() const noexcept { return blob_id_; }

  /// Stable hash of the source metadata that produced this blob.
  /// Same content → same hash, irrespective of when the blob was
  /// created.
  [[nodiscard]] std::uint64_t content_hash() const noexcept { return content_hash_; }

  /// True when the wrapper owns a live kernel blob.
  [[nodiscard]] explicit operator bool() const noexcept { return blob_id_ != 0; }

  /// Test-only: build a wrapper around a synthetic blob id without
  /// calling libdrm. The returned wrapper's destructor is a no-op
  /// (`fd_ == -1`), so the kernel is never invoked. Use only in unit
  /// tests for higher-level helpers (e.g. `HdrMetadataCache`) that
  /// need a `HdrMetadataBlob` value but shouldn't depend on a real
  /// DRM fd.
  [[nodiscard]] static HdrMetadataBlob synthesize_for_test(std::uint32_t synthetic_blob_id,
                                                           std::uint64_t hash) noexcept;

  /// Drop ownership without calling `drmModeDestroyPropertyBlob`.
  /// After this call the wrapper is empty (`bool(*this) == false`).
  /// The kernel reclaims property blobs implicitly when the
  /// originating DRM fd closes, so a session-loss path that
  /// abandons the fd should `forget()` outstanding blobs rather
  /// than destroying them — destruction would issue ioctls against
  /// an already-closed fd.
  void forget() noexcept;

 private:
  HdrMetadataBlob(int fd, std::uint32_t blob_id, std::uint64_t hash) noexcept;
  void reset() noexcept;

  int fd_{-1};
  std::uint32_t blob_id_{0};
  std::uint64_t content_hash_{0};
};

}  // namespace drm::display
