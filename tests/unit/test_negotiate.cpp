// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tests/unit/test_negotiate.cpp
//
// Host unit tests for drm::present::negotiate. Self-contained CHECK harness
// (same style as test_format_mod): non-zero exit on failure.

#include <drm-cxx/present/negotiate.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace present = drm::present;
namespace fmt = drm::fmt;

static int g_fail = 0;
#define CHECK(x)                                                        \
  do {                                                                  \
    if (!(x)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++g_fail;                                                         \
    }                                                                   \
  } while (0)

namespace {

const fmt::Modifier kLin{DRM_FORMAT_MOD_LINEAR};
const fmt::Modifier kAfbc{DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16)};
const fmt::Modifier kTiled{DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED};

void test_intersect_and_rank() {
  // Producer lists linear-first; the negotiator must still rank the AFBC
  // (compression) ahead of the tiled, ahead of the linear.
  const std::vector<fmt::Modifier> producer{kLin, kTiled, kAfbc};
  const std::vector<fmt::Modifier> plane{kAfbc, kLin, kTiled};
  const auto r = present::negotiate(producer, plane);
  CHECK(r.size() == 3);
  CHECK(r[0] == kAfbc);   // Compression first
  CHECK(r[1] == kTiled);  // Tiling
  CHECK(r[2] == kLin);    // Linear last
}

void test_intersection_only() {
  // Only modifiers in BOTH sets survive.
  const std::vector<fmt::Modifier> producer{kAfbc, kLin};
  const std::vector<fmt::Modifier> plane{kLin, kTiled};
  const auto r = present::negotiate(producer, plane);
  CHECK(r.size() == 1);
  CHECK(r[0] == kLin);
}

void test_empty_overlap() {
  const std::vector<fmt::Modifier> producer{kAfbc};
  const std::vector<fmt::Modifier> plane{kTiled, kLin};
  CHECK(present::negotiate(producer, plane).empty());
  // Empty inputs are well-defined too.
  CHECK(present::negotiate({}, plane).empty());
  CHECK(present::negotiate(producer, {}).empty());
}

void test_dedup() {
  // A producer that lists a modifier twice yields it once.
  const std::vector<fmt::Modifier> producer{kAfbc, kAfbc};
  const std::vector<fmt::Modifier> plane{kAfbc};
  CHECK(present::negotiate(producer, plane).size() == 1);
}

void test_rotation_filter() {
  using R = fmt::Rotation;
  const std::vector<fmt::Modifier> both{kLin, kAfbc, kTiled};

  // 0 degrees: full intersection survives.
  CHECK(present::negotiate(both, both, R::Rotate0).size() == 3);

  // 90 degrees: LINEAR drops out (a rotated fetch needs tiling); AFBC + tiled stay.
  const auto r90 = present::negotiate(both, both, R::Rotate90);
  CHECK(r90.size() == 2);
  for (const fmt::Modifier m : r90) {
    CHECK(m != kLin);
  }
  CHECK(present::negotiate(both, both, R::Rotate270).size() == 2);

#ifdef AMD_FMT_MOD
  // AMD DCC is filtered out under 90/270, kept at 0.
  const fmt::Modifier amd_dcc{
      AMD_FMT_MOD | AMD_FMT_MOD_SET(TILE_VERSION, AMD_FMT_MOD_TILE_VER_GFX9) |
      AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX9_64K_S_X) | AMD_FMT_MOD_SET(DCC, 1)};
  const std::vector<fmt::Modifier> amd{amd_dcc};
  CHECK(present::negotiate(amd, amd, R::Rotate0).size() == 1);
  CHECK(present::negotiate(amd, amd, R::Rotate90).empty());
#endif
}

// Build a minimal IN_FORMATS blob (one fourcc, two modifiers) to cover the
// FormatTable overload's forwarding.
std::vector<std::uint8_t> make_blob() {
  const std::uint32_t formats[1] = {DRM_FORMAT_XRGB8888};
  drm_format_modifier mods[2] = {};
  mods[0].formats = 0b1ULL;
  mods[0].offset = 0;
  mods[0].modifier = DRM_FORMAT_MOD_LINEAR;
  mods[1].formats = 0b1ULL;
  mods[1].offset = 0;
  mods[1].modifier = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16);

  drm_format_modifier_blob h = {};
  h.version = 1;
  h.count_formats = 1;
  h.formats_offset = sizeof(h);
  h.count_modifiers = 2;
  h.modifiers_offset = sizeof(h) + sizeof(formats);

  std::vector<std::uint8_t> buf(h.modifiers_offset + sizeof(mods));
  std::memcpy(buf.data(), &h, sizeof(h));
  std::memcpy(buf.data() + h.formats_offset, formats, sizeof(formats));
  std::memcpy(buf.data() + h.modifiers_offset, mods, sizeof(mods));
  return buf;
}

void test_format_table_overload() {
  const auto blob = make_blob();
  const fmt::FormatTable t = fmt::FormatTable::from_blob(blob.data(), blob.size());
  const std::vector<fmt::Modifier> producer{kAfbc, kLin};
  const auto r = present::negotiate(producer, t, DRM_FORMAT_XRGB8888);
  CHECK(r.size() == 2);
  CHECK(r[0] == kAfbc);  // compression ranked first
  CHECK(r[1] == kLin);
  // A fourcc the table doesn't carry yields nothing.
  CHECK(present::negotiate(producer, t, DRM_FORMAT_RGB565).empty());
}

}  // namespace

int main() {
  test_intersect_and_rank();
  test_intersection_only();
  test_empty_overlap();
  test_dedup();
  test_rotation_filter();
  test_format_table_overload();

  if (g_fail) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fail);
    return 1;
  }
  std::puts("all negotiate tests passed");
  return 0;
}
