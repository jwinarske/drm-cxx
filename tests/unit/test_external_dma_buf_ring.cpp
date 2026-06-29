// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene::ExternalDmaBufRing. Covers the contract visible
// without a live KMS device: argument + per-slot structural validation, plane
// bounds, graceful failure on an invalid device fd, and the modifier
// validation against a synthetic FormatTable.
//
// The prime-import + drmModeAddFB2WithModifiers round-trip and the slot FSM
// (submit -> acquire rotation, idle hold-last-frame, release leave-scanout
// signal) live in an integration test against vgem/vkms and are intentionally
// not driven from this TU.

#include "core/device.hpp"

#include <drm-cxx/fmt/format_mod.hpp>
#include <drm-cxx/scene/external_dma_buf_ring.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <system_error>

namespace {

constexpr std::uint32_t k_w = 320;
constexpr std::uint32_t k_h = 240;

drm::scene::ExternalPlaneInfo plane(int fd, std::uint32_t pitch = k_w * 4) {
  drm::scene::ExternalPlaneInfo p;
  p.fd = fd;
  p.offset = 0;
  p.pitch = pitch;
  return p;
}

// One LINEAR slot whose single plane carries `fd`.
drm::scene::ExternalSlotDesc one_slot(const std::array<drm::scene::ExternalPlaneInfo, 1>& planes,
                                      std::uint64_t modifier = DRM_FORMAT_MOD_LINEAR) {
  drm::scene::ExternalSlotDesc s;
  s.modifier = modifier;
  s.planes = drm::span<const drm::scene::ExternalPlaneInfo>(planes.data(), planes.size());
  return s;
}

}  // namespace

// ── Argument validation — runs entirely against an invalid Device ──────

TEST(SceneExternalDmaBufRing, RejectsZeroWidth) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 1> const planes{plane(0)};
  std::array<drm::scene::ExternalSlotDesc, 1> slots{one_slot(planes)};
  auto r = drm::scene::ExternalDmaBufRing::create(
      dev, /*width=*/0, k_h, DRM_FORMAT_ARGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufRing, RejectsZeroFourcc) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 1> const planes{plane(0)};
  std::array<drm::scene::ExternalSlotDesc, 1> slots{one_slot(planes)};
  auto r = drm::scene::ExternalDmaBufRing::create(
      dev, k_w, k_h, /*drm_format=*/0,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufRing, RejectsEmptySlots) {
  auto dev = drm::Device::from_fd(-1);
  auto r = drm::scene::ExternalDmaBufRing::create(dev, k_w, k_h, DRM_FORMAT_ARGB8888,
                                                  drm::span<const drm::scene::ExternalSlotDesc>());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufRing, RejectsSlotWithNoPlanes) {
  auto dev = drm::Device::from_fd(-1);
  drm::scene::ExternalSlotDesc const empty;  // planes default-empty
  std::array<drm::scene::ExternalSlotDesc, 1> slots{empty};
  auto r = drm::scene::ExternalDmaBufRing::create(
      dev, k_w, k_h, DRM_FORMAT_ARGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufRing, RejectsTooManyPlanes) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 5> planes{plane(0), plane(0), plane(0), plane(0),
                                                      plane(0)};
  drm::scene::ExternalSlotDesc s;
  s.planes = drm::span<const drm::scene::ExternalPlaneInfo>(planes.data(), planes.size());
  std::array<drm::scene::ExternalSlotDesc, 1> slots{s};
  auto r = drm::scene::ExternalDmaBufRing::create(
      dev, k_w, k_h, DRM_FORMAT_ARGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufRing, RejectsZeroPitch) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 1> const planes{plane(0, /*pitch=*/0)};
  std::array<drm::scene::ExternalSlotDesc, 1> slots{one_slot(planes)};
  auto r = drm::scene::ExternalDmaBufRing::create(
      dev, k_w, k_h, DRM_FORMAT_ARGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneExternalDmaBufRing, RejectsNegativePlaneFd) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 1> const planes{plane(-1)};
  std::array<drm::scene::ExternalSlotDesc, 1> slots{one_slot(planes)};
  auto r = drm::scene::ExternalDmaBufRing::create(
      dev, k_w, k_h, DRM_FORMAT_ARGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::invalid_argument));
}

// Structurally valid but the device fd is dead: validation passes, then the fd
// guard rejects before any ioctl.
TEST(SceneExternalDmaBufRing, RejectsDeadDeviceFd) {
  auto dev = drm::Device::from_fd(-1);
  std::array<drm::scene::ExternalPlaneInfo, 1> const planes{plane(0)};
  std::array<drm::scene::ExternalSlotDesc, 1> slots{one_slot(planes)};
  auto r = drm::scene::ExternalDmaBufRing::create(
      dev, k_w, k_h, DRM_FORMAT_ARGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::bad_file_descriptor));
}

// ── modifier validation against a synthetic IN_FORMATS table ────────

namespace {

// Build a FormatTable advertising exactly one (fourcc, modifier) pair via the
// raw drm_format_modifier_blob layout that FormatTable::from_blob parses.
drm::fmt::FormatTable table_with(std::uint32_t fourcc, std::uint64_t modifier) {
  struct Blob {
    drm_format_modifier_blob header;
    std::uint32_t format;     // formats[0]
    std::uint32_t pad;        // 8-byte align the modifier array
    drm_format_modifier mod;  // modifiers[0]
  } blob{};
  blob.header.version = FORMAT_BLOB_CURRENT;
  blob.header.count_formats = 1;
  blob.header.formats_offset = offsetof(Blob, format);
  blob.header.count_modifiers = 1;
  blob.header.modifiers_offset = offsetof(Blob, mod);
  blob.format = fourcc;
  blob.mod.formats = 1;  // bitmask: formats[0]
  blob.mod.offset = 0;
  blob.mod.modifier = modifier;
  return drm::fmt::FormatTable::from_blob(&blob, sizeof(blob));
}

}  // namespace

TEST(SceneExternalDmaBufRing, RejectsModifierNotInFormatTable) {
  auto dev = drm::Device::from_fd(-1);
  const auto table = table_with(DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR);
  std::array<drm::scene::ExternalPlaneInfo, 1> const planes{plane(0)};
  // Slot asks for an AFBC modifier the table does not advertise.
  std::array<drm::scene::ExternalSlotDesc, 1> slots{one_slot(planes, fourcc_mod_code(ARM, 1))};
  drm::scene::ExternalDmaBufRing::Options opts;
  opts.validate_against = &table;
  auto r = drm::scene::ExternalDmaBufRing::create(
      dev, k_w, k_h, DRM_FORMAT_ARGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()), opts);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::not_supported));
}

TEST(SceneExternalDmaBufRing, ModifierInFormatTablePassesValidation) {
  auto dev = drm::Device::from_fd(-1);
  const auto table = table_with(DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR);
  std::array<drm::scene::ExternalPlaneInfo, 1> const planes{plane(0)};
  std::array<drm::scene::ExternalSlotDesc, 1> slots{one_slot(planes, DRM_FORMAT_MOD_LINEAR)};
  drm::scene::ExternalDmaBufRing::Options opts;
  opts.validate_against = &table;
  auto r = drm::scene::ExternalDmaBufRing::create(
      dev, k_w, k_h, DRM_FORMAT_ARGB8888,
      drm::span<const drm::scene::ExternalSlotDesc>(slots.data(), slots.size()), opts);
  // Modifier validation passed; the dead fd (-1) is the next guard hit.
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), std::make_error_code(std::errc::bad_file_descriptor));
}
