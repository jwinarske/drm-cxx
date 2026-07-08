// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tests/fmt/test_format_mod.cpp
//
// Self-contained unit tests for drm::fmt. No framework dependency: returns
// non-zero on failure so it works as a meson test() / CTest add_test() target.
// Port to the project's harness (Catch2/doctest) if preferred.

#include <drm-cxx/fmt/format_mod.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// Build a synthetic IN_FORMATS blob:
//   formats:   [ XRGB8888, ARGB8888 ]
//   modifiers: LINEAR over both (mask 0b11), AFBC(16x16) over XRGB only (0b01)
std::vector<std::uint8_t> make_blob() {
  const std::uint32_t formats[2] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};

  drm_format_modifier mods[2] = {};
  mods[0].formats = 0b11ull;
  mods[0].offset = 0;
  mods[0].modifier = DRM_FORMAT_MOD_LINEAR;
  mods[1].formats = 0b01ull;
  mods[1].offset = 0;
  mods[1].modifier = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16);

  drm_format_modifier_blob h = {};
  h.version = 1;
  h.flags = 0;
  h.count_formats = 2;
  h.formats_offset = sizeof(h);  // 24, 8-aligned
  h.count_modifiers = 2;
  h.modifiers_offset = sizeof(h) + sizeof(formats);  // 32, 8-aligned

  std::vector<std::uint8_t> buf(h.modifiers_offset + sizeof(mods));
  std::memcpy(buf.data(), &h, sizeof(h));
  std::memcpy(buf.data() + h.formats_offset, formats, sizeof(formats));
  std::memcpy(buf.data() + h.modifiers_offset, mods, sizeof(mods));
  return buf;
}

void test_format_table() {
  const auto blob = make_blob();
  const fmt::FormatTable t = fmt::FormatTable::from_blob(blob.data(), blob.size());

  // 3 pairs: (XRGB,LINEAR) (ARGB,LINEAR) (XRGB,AFBC)
  CHECK(t.all().size() == 3);

  const fmt::Modifier lin{DRM_FORMAT_MOD_LINEAR};
  const fmt::Modifier afbc{DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16)};

  CHECK(t.supports(DRM_FORMAT_XRGB8888, lin));
  CHECK(t.supports(DRM_FORMAT_ARGB8888, lin));
  CHECK(t.supports(DRM_FORMAT_XRGB8888, afbc));
  CHECK(!t.supports(DRM_FORMAT_ARGB8888, afbc));  // AFBC only listed for XRGB
  CHECK(!t.supports(DRM_FORMAT_RGB565, lin));     // absent fourcc

  CHECK(t.modifiers_for(DRM_FORMAT_XRGB8888).size() == 2);
  CHECK(t.modifiers_for(DRM_FORMAT_ARGB8888).size() == 1);
  CHECK(t.modifiers_for(DRM_FORMAT_RGB565).size() == 0);
}

void test_format_table_malformed() {
  // Truncated / empty input must yield an empty table, never UB.
  CHECK(fmt::FormatTable::from_blob(nullptr, 0).all().empty());
  std::uint8_t tiny[4] = {};
  CHECK(fmt::FormatTable::from_blob(tiny, sizeof(tiny)).all().empty());
}

void test_classify() {
  using BC = fmt::BandwidthClass;
  CHECK(fmt::classify(fmt::Modifier{DRM_FORMAT_MOD_LINEAR}) == BC::Linear);
  CHECK(fmt::classify(fmt::Modifier{DRM_FORMAT_MOD_INVALID}) == BC::Linear);

  // AFBC must classify as Compression (the original code checked the wrong type
  // field and would have called it Tiling -- regression guard).
  CHECK(fmt::classify(fmt::Modifier{DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16)}) ==
        BC::Compression);

  // Legacy ARM tiled interleaved is Tiling, not AFBC.
  CHECK(fmt::classify(fmt::Modifier{DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED}) == BC::Tiling);

  CHECK(fmt::classify(fmt::Modifier{DRM_FORMAT_MOD_QCOM_COMPRESSED}) == BC::Compression);
  CHECK(fmt::classify(fmt::Modifier{DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED}) == BC::Tiling);

  // NVIDIA block-linear: the compression field is bits 25:23 (3 bits). c=0 is
  // uncompressed (Tiling); c=1..4 are compressed. c=4 (CDE vertical, 0b100) is
  // the case a 2-bit mask dropped, calling real compression Tiling -- guard it.
  CHECK(fmt::classify(fmt::Modifier{DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(0, 0, 0, 0xfe, 0)}) ==
        BC::Tiling);
  CHECK(fmt::classify(fmt::Modifier{DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(1, 0, 0, 0xfe, 0)}) ==
        BC::Compression);
  CHECK(fmt::classify(fmt::Modifier{DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(4, 0, 0, 0xfe, 0)}) ==
        BC::Compression);
}

void test_cost() {
  using BC = fmt::BandwidthClass;
  const std::uint64_t lin = fmt::scanout_cost_bytes(1920, 1080, DRM_FORMAT_XRGB8888, BC::Linear);
  CHECK(lin == std::uint64_t(1920) * 1080 * 4);

  CHECK(fmt::scanout_cost_bytes(1920, 1080, DRM_FORMAT_XRGB8888, BC::Tiling) ==
        lin);  // tiling moves the same bytes

  const std::uint64_t half =
      fmt::scanout_cost_bytes(1920, 1080, DRM_FORMAT_XRGB8888, BC::Compression, 0.5f);
  CHECK(half == lin / 2);

  // Byte accounting is per-format, not a fixed 32bpp. Packed RGB565 is 2 B/px;
  // planar NV12 (4:2:0 8-bit) is 1.5 B/px; P010 (4:2:0 16-bit) is 3 B/px.
  CHECK(fmt::scanout_cost_bytes(1920, 1080, DRM_FORMAT_RGB565, BC::Linear) ==
        std::uint64_t(1920) * 1080 * 2);
  CHECK(fmt::scanout_cost_bytes(1920, 1080, DRM_FORMAT_NV12, BC::Linear) ==
        std::uint64_t(1920) * 1080 * 3 / 2);
  CHECK(fmt::scanout_cost_bytes(1920, 1080, DRM_FORMAT_P010, BC::Tiling) ==
        std::uint64_t(1920) * 1080 * 3);
}

bool contains(const std::string& hay, const char* needle) {
  return hay.find(needle) != std::string::npos;
}

void test_describe() {
  CHECK(fmt::describe(fmt::Modifier{DRM_FORMAT_MOD_LINEAR}) == "LINEAR");
  CHECK(fmt::describe(fmt::Modifier{DRM_FORMAT_MOD_INVALID}) == "INVALID");

  // AFBC: block size + flags must all surface.
  const std::string afbc = fmt::describe(fmt::Modifier{DRM_FORMAT_MOD_ARM_AFBC(
      AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 | AFBC_FORMAT_MOD_SPARSE | AFBC_FORMAT_MOD_YTR)});
  CHECK(contains(afbc, "AFBC"));
  CHECK(contains(afbc, "16x16"));
  CHECK(contains(afbc, "SPARSE"));
  CHECK(contains(afbc, "YTR"));

  // Broadcom layout codes (no #ifdef guard needed: VC4_T_TILED predates UIF).
  CHECK(fmt::describe(fmt::Modifier{DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED}) ==
        "BROADCOM(VC4_T_TILED)");
#ifdef DRM_FORMAT_MOD_BROADCOM_UIF
  CHECK(contains(fmt::describe(fmt::Modifier{DRM_FORMAT_MOD_BROADCOM_UIF}), "UIF"));
#endif

  CHECK(fmt::describe(fmt::Modifier{DRM_FORMAT_MOD_QCOM_COMPRESSED}) == "QCOM_COMPRESSED(UBWC)");

#ifdef AMD_FMT_MOD
  // Displayable DCC: describe surfaces dcc=1 and classify reports Compression;
  // the same tile layout with DCC=0 is Tiling.
  const std::uint64_t amd_dcc =
      AMD_FMT_MOD | AMD_FMT_MOD_SET(TILE_VERSION, AMD_FMT_MOD_TILE_VER_GFX9) |
      AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX9_64K_S_X) | AMD_FMT_MOD_SET(DCC, 1);
  const std::uint64_t amd_nodcc = AMD_FMT_MOD |
                                  AMD_FMT_MOD_SET(TILE_VERSION, AMD_FMT_MOD_TILE_VER_GFX9) |
                                  AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX9_64K_S_X);
  const std::string amd = fmt::describe(fmt::Modifier{amd_dcc});
  CHECK(contains(amd, "AMD"));
  CHECK(contains(amd, "GFX9"));
  CHECK(contains(amd, "dcc=1"));
  CHECK(fmt::classify(fmt::Modifier{amd_dcc}) == fmt::BandwidthClass::Compression);
  CHECK(fmt::classify(fmt::Modifier{amd_nodcc}) == fmt::BandwidthClass::Tiling);
#endif
}

void test_rotation_compatible() {
  using R = fmt::Rotation;
  const fmt::Modifier lin{DRM_FORMAT_MOD_LINEAR};
  const fmt::Modifier afbc{DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16)};
  const fmt::Modifier tiled{DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED};  // tiled, non-DCC

  // 0 / 180 never transpose the scanout walk: every modifier is compatible.
  CHECK(fmt::rotation_compatible(lin, R::Rotate0));
  CHECK(fmt::rotation_compatible(lin, R::Rotate180));
  CHECK(fmt::rotation_compatible(afbc, R::Rotate0));
  CHECK(fmt::rotation_compatible(tiled, R::Rotate180));

  // 90 / 270 transpose: LINEAR can't be fetched rotated; tiled and AFBC can
  // (AFBC is left to the commit -- the design contract's ground truth).
  CHECK(!fmt::rotation_compatible(lin, R::Rotate90));
  CHECK(!fmt::rotation_compatible(lin, R::Rotate270));
  CHECK(fmt::rotation_compatible(tiled, R::Rotate90));
  CHECK(fmt::rotation_compatible(tiled, R::Rotate270));
  CHECK(fmt::rotation_compatible(afbc, R::Rotate90));

#ifdef AMD_FMT_MOD
  // AMD DCC cannot scan out rotated; the same tile layout without DCC can.
  const fmt::Modifier amd_dcc{
      AMD_FMT_MOD | AMD_FMT_MOD_SET(TILE_VERSION, AMD_FMT_MOD_TILE_VER_GFX9) |
      AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX9_64K_S_X) | AMD_FMT_MOD_SET(DCC, 1)};
  const fmt::Modifier amd_nodcc{AMD_FMT_MOD |
                                AMD_FMT_MOD_SET(TILE_VERSION, AMD_FMT_MOD_TILE_VER_GFX9) |
                                AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX9_64K_S_X)};
  CHECK(fmt::rotation_compatible(amd_dcc, R::Rotate0));    // unrotated DCC: fine
  CHECK(!fmt::rotation_compatible(amd_dcc, R::Rotate90));  // rotated DCC: rejected
  CHECK(!fmt::rotation_compatible(amd_dcc, R::Rotate270));
  CHECK(fmt::rotation_compatible(amd_nodcc, R::Rotate90));  // tiled non-DCC: ok
#endif
}

void test_probe_cache() {
  using V = fmt::ModifierProbeCache::Verdict;
  fmt::ModifierProbeCache c;
  const fmt::Modifier afbc{DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16)};

  CHECK(c.lookup(1, 2, DRM_FORMAT_XRGB8888, afbc) == V::Unknown);
  c.record(1, 2, DRM_FORMAT_XRGB8888, afbc, false);
  CHECK(c.lookup(1, 2, DRM_FORMAT_XRGB8888, afbc) == V::Rejected);
  c.record(1, 2, DRM_FORMAT_XRGB8888, afbc, true);  // update in place
  CHECK(c.lookup(1, 2, DRM_FORMAT_XRGB8888, afbc) == V::Ok);
  // distinct key is independent
  CHECK(c.lookup(9, 2, DRM_FORMAT_XRGB8888, afbc) == V::Unknown);
  c.invalidate_plane(2);
  CHECK(c.lookup(1, 2, DRM_FORMAT_XRGB8888, afbc) == V::Unknown);
}

}  // namespace

int main() {
  test_format_table();
  test_format_table_malformed();
  test_classify();
  test_rotation_compatible();
  test_cost();
  test_describe();
  test_probe_cache();

  if (g_fail) {
    std::fprintf(stderr, "%d check(s) failed\n", g_fail);
    return 1;
  }
  std::puts("all format_mod tests passed");
  return 0;
}