// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

namespace drm {
class Device;
}  // namespace drm

namespace drm::planes {

enum class DRMPlaneType : uint32_t {
  PRIMARY,
  OVERLAY,
  CURSOR,
};

struct PlaneCapabilities {
  uint32_t id{};
  uint32_t possible_crtcs{};
  DRMPlaneType type{DRMPlaneType::OVERLAY};
  std::vector<uint32_t> formats;
  std::optional<uint64_t> zpos_min;
  std::optional<uint64_t> zpos_max;
  bool supports_rotation{false};
  bool supports_scaling{false};
  uint32_t cursor_max_w{};
  uint32_t cursor_max_h{};

  [[nodiscard]] bool supports_format(uint32_t fmt) const;
  [[nodiscard]] bool compatible_with_crtc(uint32_t crtc_index) const;
};

class PlaneRegistry {
 public:
  static std::expected<PlaneRegistry, std::error_code> enumerate(const Device& dev);

  [[nodiscard]] std::span<const PlaneCapabilities> all() const noexcept;

  [[nodiscard]] std::vector<const PlaneCapabilities*> for_crtc(uint32_t crtc_index) const;

 private:
  std::vector<PlaneCapabilities> planes_;
};

}  // namespace drm::planes
