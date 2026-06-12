// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// drm-cxx/fmt/format_mod.cpp
//
// Out-of-line implementations for format_mod.hpp. classify() and
// scanout_cost_bytes() are header-inline; everything else lives here.

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/fmt/format_mod.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// Provided by the build (meson + CMake gate). Default off so this also compiles
// against an older libgbm with no _2 variant.
#ifndef HAVE_GBM_BO_CREATE_WITH_MODIFIERS2
#define HAVE_GBM_BO_CREATE_WITH_MODIFIERS2 0
#endif

namespace drm::fmt {
namespace {

std::error_code errno_ec(int e) {
  return std::make_error_code(static_cast<std::errc>((e != 0) ? e : EINVAL));
}

// Find the blob id of a named immutable-blob property on an object.
drm::expected<std::uint32_t, std::error_code> find_blob_prop(int fd, std::uint32_t obj_id,
                                                             std::uint32_t obj_type,
                                                             const char* name) {
  drmModeObjectProperties* props = drmModeObjectGetProperties(fd, obj_id, obj_type);
  if (props == nullptr) {
    return drm::unexpected<std::error_code>(errno_ec(errno));
  }

  std::uint32_t blob_id = 0;
  bool found = false;
  for (std::uint32_t i = 0; i < props->count_props && !found; ++i) {
    drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
    if (p == nullptr) {
      continue;
    }
    if (std::strcmp(p->name, name) == 0) {
      blob_id = static_cast<std::uint32_t>(props->prop_values[i]);
      found = true;
    }
    drmModeFreeProperty(p);
  }
  drmModeFreeObjectProperties(props);

  if (!found || blob_id == 0) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_file_or_directory));
  }
  return blob_id;
}

}  // namespace

// ---------------------------------------------------------------------------
// FormatTable
// ---------------------------------------------------------------------------
FormatTable FormatTable::from_blob(const void* data, std::size_t size) {
  FormatTable t;
  if ((data == nullptr) || size < sizeof(drm_format_modifier_blob)) {
    return t;
  }

  const auto* base = static_cast<const std::uint8_t*>(data);
  const auto* h = reinterpret_cast<const drm_format_modifier_blob*>(base);

  // Bounds-check the two arrays before dereferencing (defends against a
  // truncated or hostile blob). Written in division form so the implied
  // count * element-size products cannot overflow size_t on a 32-bit target
  // and silently bypass the check.
  if (h->formats_offset > size || h->modifiers_offset > size ||
      h->count_formats > (size - h->formats_offset) / sizeof(std::uint32_t) ||
      h->count_modifiers > (size - h->modifiers_offset) / sizeof(drm_format_modifier)) {
    return t;
  }

  const auto* formats = reinterpret_cast<const std::uint32_t*>(base + h->formats_offset);
  const auto* mods = reinterpret_cast<const drm_format_modifier*>(base + h->modifiers_offset);

  for (std::uint32_t i = 0; i < h->count_modifiers; ++i) {
    const drm_format_modifier& m = mods[i];
    for (int b = 0; b < 64; ++b) {  // each set bit -> one fourcc
      if (((m.formats >> b) & 1ULL) == 0U) {
        continue;
      }
      const std::uint32_t idx = m.offset + static_cast<std::uint32_t>(b);
      if (idx >= h->count_formats) {
        continue;  // malformed blob guard
      }
      t.pairs_.push_back({formats[idx], Modifier{m.modifier}});
    }
  }

  // Sort + dedup (some kernels list a (fourcc, modifier) twice).
  std::sort(t.pairs_.begin(), t.pairs_.end());
  t.pairs_.erase(std::unique(t.pairs_.begin(), t.pairs_.end()), t.pairs_.end());

  // Build the contiguous per-fourcc modifier index that backs modifiers_for().
  for (std::size_t i = 0; i < t.pairs_.size();) {
    const std::uint32_t fourcc = t.pairs_[i].fourcc;
    Group g{fourcc, static_cast<std::uint32_t>(t.mods_.size()), 0};
    for (; i < t.pairs_.size() && t.pairs_[i].fourcc == fourcc; ++i) {
      t.mods_.push_back(t.pairs_[i].modifier);
      ++g.count;
    }
    t.groups_.push_back(g);
  }
  return t;
}

drm::expected<FormatTable, std::error_code> FormatTable::from_plane(int fd,
                                                                    std::uint32_t plane_id) {
  auto blob_id = find_blob_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE, "IN_FORMATS");
  if (!blob_id) {
    return drm::unexpected<std::error_code>(blob_id.error());
  }

  drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(fd, *blob_id);
  if (blob == nullptr) {
    return drm::unexpected<std::error_code>(errno_ec(errno));
  }

  FormatTable t = from_blob(blob->data, blob->length);
  drmModeFreePropertyBlob(blob);
  return t;
}

bool FormatTable::supports(std::uint32_t fourcc, Modifier m) const noexcept {
  return std::binary_search(pairs_.begin(), pairs_.end(), FormatMod{fourcc, m});
}

drm::span<const Modifier> FormatTable::modifiers_for(std::uint32_t fourcc) const noexcept {
  auto it = std::lower_bound(groups_.begin(), groups_.end(), fourcc,
                             [](const Group& g, std::uint32_t f) { return g.fourcc < f; });
  if (it == groups_.end() || it->fourcc != fourcc) {
    return {};
  }
  return {mods_.data() + it->first, it->count};
}

// ---------------------------------------------------------------------------
// ScanoutBuffer
// ---------------------------------------------------------------------------
namespace {
constexpr std::size_t k_max_planes = 4;

void gather_planes(gbm_bo* bo, unsigned n, std::uint64_t mod,
                   std::array<std::uint32_t, k_max_planes>& handles,
                   std::array<std::uint32_t, k_max_planes>& strides,
                   std::array<std::uint32_t, k_max_planes>& offsets,
                   std::array<std::uint64_t, k_max_planes>& mods) {
  for (std::size_t i = 0; i < n; ++i) {
    handles.at(i) = gbm_bo_get_handle_for_plane(bo, static_cast<int>(i)).u32;
    strides.at(i) = gbm_bo_get_stride_for_plane(bo, static_cast<int>(i));
    offsets.at(i) = gbm_bo_get_offset(bo, static_cast<int>(i));
    mods.at(i) = mod;
  }
}
}  // namespace

drm::expected<ScanoutBuffer, std::error_code> ScanoutBuffer::create(gbm_device* gbm, int drm_fd,
                                                                    const Desc& d) {
  std::vector<std::uint64_t> cand;
  cand.reserve(d.modifiers.size());
  for (Modifier const m : d.modifiers) {
    cand.push_back(m.value);
  }

#if HAVE_GBM_BO_CREATE_WITH_MODIFIERS2
  gbm_bo* bo = gbm_bo_create_with_modifiers2(gbm, d.width, d.height, d.fourcc, cand.data(),
                                             cand.size(), d.gbm_flags);
#else
  // Older libgbm: no flags-taking variant. The modifier list still constrains
  // layout; scanout-ability is enforced by AddFB2 + the atomic test commit.
  (void)d.gbm_flags;
  gbm_bo* bo =
      gbm_bo_create_with_modifiers(gbm, d.width, d.height, d.fourcc, cand.data(), cand.size());
#endif
  if (bo == nullptr) {
    return drm::unexpected<std::error_code>(errno_ec(errno));
  }

  const std::uint64_t chosen = gbm_bo_get_modifier(bo);
  const auto n = static_cast<unsigned>(gbm_bo_get_plane_count(bo));
  if (n == 0 || n > k_max_planes) {
    gbm_bo_destroy(bo);
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  std::array<std::uint32_t, k_max_planes> handles{};
  std::array<std::uint32_t, k_max_planes> strides{};
  std::array<std::uint32_t, k_max_planes> offsets{};
  std::array<std::uint64_t, k_max_planes> mods{};
  gather_planes(bo, n, chosen, handles, strides, offsets, mods);

  std::uint32_t fb_id = 0;
  const int ret = drmModeAddFB2WithModifiers(drm_fd, d.width, d.height, d.fourcc, handles.data(),
                                             strides.data(), offsets.data(), mods.data(), &fb_id,
                                             DRM_MODE_FB_MODIFIERS);
  if (ret != 0) {
    gbm_bo_destroy(bo);
    return drm::unexpected<std::error_code>(errno_ec(-ret));
  }

  ScanoutBuffer sb;
  sb.bo_ = bo;
  sb.drm_fd_ = drm_fd;
  sb.fb_id_ = fb_id;
  sb.planes_ = n;
  sb.chosen_ = Modifier{chosen};
  return sb;
}

drm::expected<ScanoutBuffer, std::error_code> ScanoutBuffer::import_dmabuf(int fd,
                                                                           const ImportDesc& d) {
  const auto n = static_cast<std::size_t>(d.planes.size());
  if (n == 0 || n > k_max_planes) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }

  std::array<std::uint32_t, k_max_planes> handles{};
  std::array<std::uint32_t, k_max_planes> strides{};
  std::array<std::uint32_t, k_max_planes> offsets{};
  std::array<std::uint64_t, k_max_planes> mods{};

  // PRIME-import each dma-buf fd to a GEM handle. A buffer shared across planes
  // (AFBC payload + header in one dma-buf) resolves to the SAME handle, so close
  // uniquely.
  std::array<std::uint32_t, k_max_planes> unique{};
  std::size_t unique_n = 0;
  auto note_unique = [&](std::uint32_t hnd) {
    for (std::size_t i = 0; i < unique_n; ++i) {
      if (unique.at(i) == hnd) {
        return;
      }
    }
    unique.at(unique_n++) = hnd;
  };

  std::error_code err;
  for (std::size_t i = 0; i < n; ++i) {
    std::uint32_t hnd = 0;
    if (drmPrimeFDToHandle(fd, d.planes[i].dmabuf_fd, &hnd) != 0) {
      err = errno_ec(errno);
      break;
    }
    handles.at(i) = hnd;
    strides.at(i) = d.planes[i].stride;
    offsets.at(i) = d.planes[i].offset;
    mods.at(i) = d.modifier.value;
    note_unique(hnd);
  }

  std::uint32_t fb_id = 0;
  if (!err) {
    if (d.modifier.value == DRM_FORMAT_MOD_INVALID) {
      // No explicit modifier — e.g. a LINEAR buffer from a GPU whose GBM only
      // renders LINEAR (PowerVR on StarFive). AddFB2WithModifiers with an
      // INVALID modifier is ill-formed, and minimal display drivers without
      // DRM_CAP_ADDFB2_MODIFIERS (e.g. starfive) return ENOSYS for the
      // modifier'd path regardless. Use plain AddFB2 so the import still lands.
      if (drmModeAddFB2(fd, d.width, d.height, d.fourcc, handles.data(), strides.data(),
                        offsets.data(), &fb_id, 0) != 0) {
        err = errno_ec(errno);
      }
    } else if (drmModeAddFB2WithModifiers(fd, d.width, d.height, d.fourcc, handles.data(),
                                          strides.data(), offsets.data(), mods.data(), &fb_id,
                                          DRM_MODE_FB_MODIFIERS) != 0) {
      err = errno_ec(errno);
    }
  }

  // The FB holds its own reference to the underlying GEM object(s); drop our
  // imported handles whether we succeeded or failed.
  for (std::size_t i = 0; i < unique_n; ++i) {
    drmCloseBufferHandle(fd, unique.at(i));
  }

  if (err) {
    return drm::unexpected<std::error_code>(err);
  }

  ScanoutBuffer sb;
  sb.bo_ = nullptr;  // imported: no GBM ownership, only the FB
  sb.drm_fd_ = fd;
  sb.fb_id_ = fb_id;
  sb.planes_ = n;
  sb.chosen_ = d.modifier;
  return sb;
}

ScanoutBuffer::ScanoutBuffer(ScanoutBuffer&& o) noexcept
    : bo_(o.bo_), drm_fd_(o.drm_fd_), fb_id_(o.fb_id_), planes_(o.planes_), chosen_(o.chosen_) {
  o.bo_ = nullptr;
  o.fb_id_ = 0;
  o.drm_fd_ = -1;
}

ScanoutBuffer& ScanoutBuffer::operator=(ScanoutBuffer&& o) noexcept {
  if (this != &o) {
    if ((fb_id_ != 0U) && drm_fd_ >= 0) {
      drmModeRmFB(drm_fd_, fb_id_);
    }
    if (bo_ != nullptr) {
      gbm_bo_destroy(bo_);
    }
    bo_ = o.bo_;
    drm_fd_ = o.drm_fd_;
    fb_id_ = o.fb_id_;
    planes_ = o.planes_;
    chosen_ = o.chosen_;
    o.bo_ = nullptr;
    o.fb_id_ = 0;
    o.drm_fd_ = -1;
  }
  return *this;
}

ScanoutBuffer::~ScanoutBuffer() {
  if ((fb_id_ != 0U) && drm_fd_ >= 0) {
    drmModeRmFB(drm_fd_, fb_id_);
  }
  if (bo_ != nullptr) {
    gbm_bo_destroy(bo_);
  }
}

// ---------------------------------------------------------------------------
// ModifierProbeCache
// ---------------------------------------------------------------------------
ModifierProbeCache::Verdict ModifierProbeCache::lookup(std::uint32_t crtc, std::uint32_t plane,
                                                       std::uint32_t fourcc,
                                                       Modifier m) const noexcept {
  const Key k{crtc, plane, fourcc, m.value};
  for (const auto& e : entries_) {
    if (e.first == k) {
      return e.second ? Verdict::Ok : Verdict::Rejected;
    }
  }
  return Verdict::Unknown;
}

void ModifierProbeCache::record(std::uint32_t crtc, std::uint32_t plane, std::uint32_t fourcc,
                                Modifier m, bool ok) {
  const Key k{crtc, plane, fourcc, m.value};
  for (auto& e : entries_) {
    if (e.first == k) {
      e.second = ok;
      return;
    }
  }
  entries_.emplace_back(k, ok);
}

void ModifierProbeCache::invalidate_plane(std::uint32_t plane) noexcept {
  entries_.erase(
      std::remove_if(entries_.begin(), entries_.end(),
                     [&](const std::pair<Key, bool>& e) { return e.first.plane == plane; }),
      entries_.end());
}

// ---------------------------------------------------------------------------
// describe()
//
// Best-effort human-readable decode, for logging only. Each vendor block is
// #ifdef-guarded on the macros it needs so the file still builds against an
// older drm_fourcc.h (the vendor falls back to "<NAME>(0xhex)").
// ---------------------------------------------------------------------------
namespace {

const char* vendor_name(std::uint8_t vendor) {
  switch (vendor) {
    case DRM_FORMAT_MOD_VENDOR_NONE:
      return "NONE";
    case DRM_FORMAT_MOD_VENDOR_INTEL:
      return "INTEL";
    case DRM_FORMAT_MOD_VENDOR_AMD:
      return "AMD";
    case DRM_FORMAT_MOD_VENDOR_NVIDIA:
      return "NVIDIA";
    case DRM_FORMAT_MOD_VENDOR_BROADCOM:
      return "BROADCOM";
    case DRM_FORMAT_MOD_VENDOR_ARM:
      return "ARM";
    case DRM_FORMAT_MOD_VENDOR_QCOM:
      return "QCOM";
#ifdef DRM_FORMAT_MOD_VENDOR_VIVANTE
    case DRM_FORMAT_MOD_VENDOR_VIVANTE:
      return "VIVANTE";
#endif
#ifdef DRM_FORMAT_MOD_VENDOR_AMLOGIC
    case DRM_FORMAT_MOD_VENDOR_AMLOGIC:
      return "AMLOGIC";
#endif
    default:
      return "?";
  }
}

std::string hex_suffix(std::uint64_t v) {
  std::array<char, 24> buf{};
  std::snprintf(buf.data(), buf.size(), "(0x%016llx)", static_cast<unsigned long long>(v));
  return {buf.data()};
}

#ifdef AMD_FMT_MOD
const char* amd_tile_version(std::uint64_t v) {
  switch (v) {
    case AMD_FMT_MOD_TILE_VER_GFX9:
      return "GFX9";
    case AMD_FMT_MOD_TILE_VER_GFX10:
      return "GFX10";
    case AMD_FMT_MOD_TILE_VER_GFX10_RBPLUS:
      return "GFX10_RBPLUS";
    case AMD_FMT_MOD_TILE_VER_GFX11:
      return "GFX11";
    default:
      return "GFX?";
  }
}

// AMD_FMT_MOD packs tile version, swizzle mode, and DCC (Delta Color
// Compression) state. DCC != 0 is "displayable DCC" -- the case classify()
// reports as Compression.
std::string amd_describe(std::uint64_t mod) {
  std::string s = "AMD(";
  s += amd_tile_version(AMD_FMT_MOD_GET(TILE_VERSION, mod));
  s += " tile";
  s += std::to_string(AMD_FMT_MOD_GET(TILE, mod));
  s += " dcc=";
  s += std::to_string(AMD_FMT_MOD_GET(DCC, mod));
  if (AMD_FMT_MOD_GET(DCC_RETILE, mod) != 0U) {
    s += " retile";
  }
  if (AMD_FMT_MOD_GET(DCC_PIPE_ALIGN, mod) != 0U) {
    s += " pipe_align";
  }
  s += ')';
  return s;
}
#endif  // AMD_FMT_MOD

// Decode an ARM modifier. AFBC (type field 0) is the lossless framebuffer
// compression Mali-fed displays (e.g. Rockchip VOP2) scan out; the superblock
// size + flags decide whether a producer's buffer is scannable compressed.
std::string arm_describe(std::uint64_t mod) {
  if (mod == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED) {
    return "ARM(16x16 interleaved)";
  }
  if (((mod >> 52) & 0xfULL) != DRM_FORMAT_MOD_ARM_TYPE_AFBC) {
    return "ARM" + hex_suffix(mod);  // AFRC / MISC -- not decoded
  }
  const std::uint64_t m = mod & 0x000fffffffffffffULL;
  std::string s = "ARM_AFBC(";
  switch (m & 0xf) {  // block-size field
    case AFBC_FORMAT_MOD_BLOCK_SIZE_16x16:
      s += "16x16";
      break;
    case AFBC_FORMAT_MOD_BLOCK_SIZE_32x8:
      s += "32x8";
      break;
    case AFBC_FORMAT_MOD_BLOCK_SIZE_64x4:
      s += "64x4";
      break;
#ifdef AFBC_FORMAT_MOD_BLOCK_SIZE_32x8_64x4
    case AFBC_FORMAT_MOD_BLOCK_SIZE_32x8_64x4:
      s += "32x8_64x4";
      break;
#endif
    default:
      s += "blk?";
      break;
  }
  if ((m & AFBC_FORMAT_MOD_YTR) != 0U) {
    s += "|YTR";
  }
  if ((m & AFBC_FORMAT_MOD_SPLIT) != 0U) {
    s += "|SPLIT";
  }
  if ((m & AFBC_FORMAT_MOD_SPARSE) != 0U) {
    s += "|SPARSE";
  }
#ifdef AFBC_FORMAT_MOD_CBR
  if ((m & AFBC_FORMAT_MOD_CBR) != 0U) {
    s += "|CBR";
  }
#endif
  if ((m & AFBC_FORMAT_MOD_TILED) != 0U) {
    s += "|TILED";
  }
#ifdef AFBC_FORMAT_MOD_SC
  if ((m & AFBC_FORMAT_MOD_SC) != 0U) {
    s += "|SC";
  }
#endif
#ifdef AFBC_FORMAT_MOD_DB
  if ((m & AFBC_FORMAT_MOD_DB) != 0U) {
    s += "|DB";
  }
#endif
#ifdef AFBC_FORMAT_MOD_BCH
  if ((m & AFBC_FORMAT_MOD_BCH) != 0U) {
    s += "|BCH";
  }
#endif
#ifdef AFBC_FORMAT_MOD_USM
  if ((m & AFBC_FORMAT_MOD_USM) != 0U) {
    s += "|USM";
  }
#endif
  s += ')';
  return s;
}

#ifdef DRM_FORMAT_MOD_BROADCOM_UIF
// Decode a Broadcom (Pi V3D / vc4) modifier. The low 8 bits are the layout
// code; SAND variants carry a column height in the next 48 bits. UIF is V3D's
// tiled render layout -- locality only, no byte savings (Broadcom has no
// AFBC-style compression), which is why classify() reports Broadcom as Tiling.
std::string broadcom_describe(std::uint64_t mod) {
  const std::uint64_t code = mod & 0xffULL;
  const std::uint64_t param = (mod >> 8) & ((1ULL << 48) - 1);
  switch (code) {
    case 1:
      return "BROADCOM(VC4_T_TILED)";
    case 2:
      return "BROADCOM(SAND32 col=" + std::to_string(param) + ")";
    case 3:
      return "BROADCOM(SAND64 col=" + std::to_string(param) + ")";
    case 4:
      return "BROADCOM(SAND128 col=" + std::to_string(param) + ")";
    case 5:
      return "BROADCOM(SAND256 col=" + std::to_string(param) + ")";
    case 6:
      return "BROADCOM(UIF)";
    default:
      return "BROADCOM" + hex_suffix(mod);
  }
}
#endif  // DRM_FORMAT_MOD_BROADCOM_UIF

}  // namespace

std::string describe(Modifier m) {
  if (m.is_linear()) {
    return "LINEAR";
  }
  if (m.is_invalid()) {
    return "INVALID";
  }
  const std::uint8_t vendor = m.vendor();
  switch (vendor) {
    case DRM_FORMAT_MOD_VENDOR_ARM:
      return arm_describe(m.value);
#ifdef AMD_FMT_MOD
    case DRM_FORMAT_MOD_VENDOR_AMD:
      return amd_describe(m.value);
#endif
    case DRM_FORMAT_MOD_VENDOR_QCOM:
      return "QCOM_COMPRESSED(UBWC)";
    case DRM_FORMAT_MOD_VENDOR_NVIDIA:
      return "NVIDIA_BLOCK_LINEAR";
#ifdef DRM_FORMAT_MOD_BROADCOM_UIF
    case DRM_FORMAT_MOD_VENDOR_BROADCOM:
      return broadcom_describe(m.value);
#endif
    default:
      return std::string(vendor_name(vendor)) + hex_suffix(m.value);
  }
}

}  // namespace drm::fmt