// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// dmabuf_slot.hpp — the per-buffer KMS import/teardown primitive shared by the
// externally-allocated dma-buf sources (ExternalDmaBufSource, ExternalDmaBufRing,
// and the forthcoming dynamic pool). Each of those wraps caller-supplied dma-buf
// plane fds as a scanout framebuffer; the fd-dup, PRIME import, AddFB2, and the
// RmFB/GEM_CLOSE teardown are identical between them and live here once.
//
// A DmaBufSlot owns duped plane fds + GEM handles + one cached fb_id for a single
// buffer. Ownership/lifetime is the caller's: it decides how many slots exist
// (one, a fixed ring, or a dynamic map) and when they die. These free functions
// only touch the slot handed to them.

#pragma once

#include "../buffer_source.hpp"  // ExternalPlaneInfo, SourceFormat

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <system_error>

namespace drm::scene::detail {

inline constexpr std::size_t k_max_planes = 4;
// DRM_FORMAT_MOD_INVALID — no explicit modifier; import takes the legacy AddFB2
// path so drivers without the modifiers capability accept it.
inline constexpr std::uint64_t k_mod_invalid = (1ULL << 56U) - 1U;

// One imported plane: the duped fd, its GEM handle on the current device, and
// the layout the producer advertised.
struct PlaneRecord {
  int duped_fd{-1};
  std::uint32_t gem_handle{0};
  std::uint32_t offset{0};
  std::uint32_t pitch{0};
};

// One buffer's planes (1..k_max_planes) plus its cached KMS fb_id and modifier.
struct DmaBufSlot {
  std::array<PlaneRecord, k_max_planes> planes{};
  std::size_t plane_count{0};
  std::uint32_t fb_id{0};
  std::uint64_t modifier{0};
};

// F_DUPFD_CLOEXEC each caller plane fd into `slot.planes[*].duped_fd` (with its
// offset/pitch), so the slot's lifetime is independent of the caller's fds.
// `slot.plane_count` is bumped after each success, so a mid-loop failure leaves
// the caller's teardown able to close exactly the fds already duped. `planes`
// must be pre-validated (non-empty, <= k_max_planes, each fd >= 0, pitch != 0).
[[nodiscard]] drm::expected<void, std::error_code> dup_planes(
    DmaBufSlot& slot, drm::span<const ExternalPlaneInfo> planes) noexcept;

// PRIME-import each duped fd into a GEM handle on `fd`, then
// drmModeAddFB2WithModifiers into `slot.fb_id`. `fmt` supplies width/height/
// fourcc; `slot.modifier` selects the modifier vs. legacy AddFB2 path. The
// kernel dedups identical fds into one handle, so shared-fd planes are correct.
[[nodiscard]] drm::expected<void, std::error_code> import_slot(int fd, DmaBufSlot& slot,
                                                               const SourceFormat& fmt) noexcept;

// RmFB + GEM_CLOSE for the slot; zeroes fb_id and every gem_handle. Keeps the
// duped fds (a session resume re-imports them). Idempotent; no-op when fd < 0.
void teardown_slot(int fd, DmaBufSlot& slot) noexcept;

// Close every duped fd and reset plane_count. Terminal cleanup (destructor path).
void close_slot_fds(DmaBufSlot& slot) noexcept;

}  // namespace drm::scene::detail
