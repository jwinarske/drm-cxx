// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#include "page_flip.hpp"
#include "../core/device.hpp"

namespace drm {

PageFlip::PageFlip(const Device& dev) : drm_fd_(dev.fd()) {}

PageFlip::~PageFlip() = default;

void PageFlip::set_handler(Handler handler) {
  handler_ = std::move(handler);
}

std::expected<void, std::error_code>
PageFlip::dispatch([[maybe_unused]] int timeout_ms) {
  return std::unexpected(std::make_error_code(std::errc::not_supported));
}

} // namespace drm
