// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "hdr_metadata.hpp"

#include "../core/device.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <drm/drm_mode.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <vector>

namespace drm::display {

namespace {

/// CTA-861.3 §6.9.1.5: color primaries are encoded as 16-bit unsigned
/// fixed-point, 0.00002 step, 0xC350 == 1.0.
constexpr std::uint16_t cie_to_u16(float v) noexcept {
  if (!(v >= 0.0F)) {
    return 0;
  }
  const float scaled = std::round(v * 50000.0F);
  if (scaled >= 65535.0F) {
    return 0xFFFFU;
  }
  return static_cast<std::uint16_t>(scaled);
}

/// Static metadata descriptor type 1 (CTA-861.3 §6.9.1). Kernel calls
/// it `HDMI_STATIC_METADATA_TYPE1` in `<linux/hdmi.h>`, which is not
/// part of the userspace UAPI surface — replicate the constant.
constexpr std::uint32_t k_hdmi_static_metadata_type1 = 0;

/// FNV-1a 64-bit. Stable, well-distributed, no third-party dependency.
std::uint64_t fnv1a_64(const std::uint8_t* data, std::size_t len) noexcept {
  constexpr std::uint64_t k_offset_basis = 0xCBF29CE484222325ULL;
  constexpr std::uint64_t k_prime = 0x100000001B3ULL;
  std::uint64_t h = k_offset_basis;
  for (std::size_t i = 0; i < len; ++i) {
    h ^= static_cast<std::uint64_t>(data[i]);
    h *= k_prime;
  }
  return h;
}

}  // namespace

std::vector<std::uint8_t> serialize_hdr_metadata(const HdrSourceMetadata& src) noexcept {
  hdr_output_metadata out{};
  out.metadata_type = k_hdmi_static_metadata_type1;

  auto& info = out.hdmi_metadata_type1;
  info.eotf = static_cast<std::uint8_t>(src.eotf);
  info.metadata_type = static_cast<std::uint8_t>(k_hdmi_static_metadata_type1);

  // CTA-861.3 §6.9.1.5: primaries are written in green / blue / red
  // order. ColorimetryInfo names them by color, so reorder here.
  const auto& p = src.display_primaries;
  info.display_primaries[0].x = cie_to_u16(p.green.x);
  info.display_primaries[0].y = cie_to_u16(p.green.y);
  info.display_primaries[1].x = cie_to_u16(p.blue.x);
  info.display_primaries[1].y = cie_to_u16(p.blue.y);
  info.display_primaries[2].x = cie_to_u16(p.red.x);
  info.display_primaries[2].y = cie_to_u16(p.red.y);
  info.white_point.x = cie_to_u16(p.white.x);
  info.white_point.y = cie_to_u16(p.white.y);

  info.max_display_mastering_luminance = src.max_display_mastering_luminance;
  info.min_display_mastering_luminance = src.min_display_mastering_luminance;
  info.max_cll = src.max_content_light_level;
  info.max_fall = src.max_frame_average_light_level;

  std::vector<std::uint8_t> bytes(sizeof(hdr_output_metadata));
  std::memcpy(bytes.data(), &out, sizeof(hdr_output_metadata));
  return bytes;
}

std::uint64_t hdr_metadata_hash(const HdrSourceMetadata& src) noexcept {
  const auto bytes = serialize_hdr_metadata(src);
  return fnv1a_64(bytes.data(), bytes.size());
}

HdrMetadataBlob::HdrMetadataBlob(int fd, std::uint32_t blob_id, std::uint64_t hash) noexcept
    : fd_(fd), blob_id_(blob_id), content_hash_(hash) {}

HdrMetadataBlob::~HdrMetadataBlob() {
  reset();
}

HdrMetadataBlob::HdrMetadataBlob(HdrMetadataBlob&& other) noexcept
    : fd_(other.fd_), blob_id_(other.blob_id_), content_hash_(other.content_hash_) {
  other.fd_ = -1;
  other.blob_id_ = 0;
  other.content_hash_ = 0;
}

HdrMetadataBlob& HdrMetadataBlob::operator=(HdrMetadataBlob&& other) noexcept {
  if (this != &other) {
    reset();
    fd_ = other.fd_;
    blob_id_ = other.blob_id_;
    content_hash_ = other.content_hash_;
    other.fd_ = -1;
    other.blob_id_ = 0;
    other.content_hash_ = 0;
  }
  return *this;
}

void HdrMetadataBlob::reset() noexcept {
  if (blob_id_ != 0 && fd_ >= 0) {
    drmModeDestroyPropertyBlob(fd_, blob_id_);
  }
  fd_ = -1;
  blob_id_ = 0;
  content_hash_ = 0;
}

HdrMetadataBlob HdrMetadataBlob::synthesize_for_test(std::uint32_t synthetic_blob_id,
                                                     std::uint64_t hash) noexcept {
  // fd_ == -1 keeps `reset()` from calling drmModeDestroyPropertyBlob.
  return {-1, synthetic_blob_id, hash};
}

void HdrMetadataBlob::forget() noexcept {
  fd_ = -1;
  blob_id_ = 0;
  content_hash_ = 0;
}

drm::expected<HdrMetadataBlob, std::error_code> HdrMetadataBlob::create(
    const drm::Device& dev, const HdrSourceMetadata& src) {
  const auto bytes = serialize_hdr_metadata(src);
  std::uint32_t blob_id = 0;
  if (drmModeCreatePropertyBlob(dev.fd(), bytes.data(), bytes.size(), &blob_id) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  return HdrMetadataBlob(dev.fd(), blob_id, fnv1a_64(bytes.data(), bytes.size()));
}

}  // namespace drm::display
