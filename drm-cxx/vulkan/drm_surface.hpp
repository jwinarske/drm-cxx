// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <expected>
#include <system_error>

namespace drm::vulkan {

class DrmSurface {
public:
  ~DrmSurface();
  DrmSurface(DrmSurface&&) noexcept;
  DrmSurface& operator=(DrmSurface&&) noexcept;
  DrmSurface(const DrmSurface&) = delete;
  DrmSurface& operator=(const DrmSurface&) = delete;

private:
  DrmSurface() = default;
};

} // namespace drm::vulkan
