// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// drm-cxx/fmt/format_mod.hpp
//
// Format-modifier / hardware-compression support for the native plane allocator.
//
// Design contract:
//   * IN_FORMATS prunes the bipartite graph   (necessary, NOT sufficient).
//   * The atomic TEST_ONLY commit is ground truth for what a plane can scan out.
//   * BandwidthClass is a COST input to placement scoring, never a correctness gate.
//
// This keeps every vendor (AFBC: Arm/Rockchip/MediaTek, AMD DCC, Qualcomm UBWC,
// NVIDIA block-linear, Broadcom tiling) on one code path: a (fourcc, modifier)
// pair flows from the producer's GBM allocation, through IN_FORMATS eligibility,
// to a probed-and-memoized test commit.
//
// C++17: uses drm::expected / drm::span from detail/, hand-written comparison
// operators (no <=>), and iterator-form std::algorithms (no std::ranges). Matches
// the rest of the tree, which builds on both the c++17 and c++23 CI legs.

#include <drm-cxx/detail/expected.hpp>  // drm::expected, drm::unexpected
#include <drm-cxx/detail/span.hpp>      // drm::span

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <gbm.h>
#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

// Fallbacks for headers predating the ARM type-field encoding.
#ifndef DRM_FORMAT_MOD_ARM_TYPE_AFBC
#define DRM_FORMAT_MOD_ARM_TYPE_AFBC 0x00
#endif
#ifndef DRM_FORMAT_MOD_ARM_TYPE_AFRC
#define DRM_FORMAT_MOD_ARM_TYPE_AFRC 0x02
#endif
#ifndef DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED
#define DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED fourcc_mod_code(ARM, 1)
#endif

namespace drm::fmt {

// ---------------------------------------------------------------------------
// Modifier: strong wrapper over the 64-bit DRM format modifier.
// ---------------------------------------------------------------------------
struct Modifier {
  std::uint64_t value = DRM_FORMAT_MOD_LINEAR;

  [[nodiscard]] constexpr std::uint8_t vendor() const noexcept {
    return static_cast<std::uint8_t>(value >> 56);  // fourcc_mod_get_vendor()
  }
  [[nodiscard]] constexpr bool is_linear() const noexcept { return value == DRM_FORMAT_MOD_LINEAR; }
  [[nodiscard]] constexpr bool is_invalid() const noexcept {
    return value == DRM_FORMAT_MOD_INVALID;
  }

  friend constexpr bool operator==(Modifier a, Modifier b) noexcept { return a.value == b.value; }
  friend constexpr bool operator!=(Modifier a, Modifier b) noexcept { return a.value != b.value; }
  friend constexpr bool operator<(Modifier a, Modifier b) noexcept { return a.value < b.value; }
};

// What the modifier does to *bus traffic*. Best-effort classification by vendor
// + bit inspection; used only for allocator scoring. Correctness never depends
// on this being right.
enum class BandwidthClass : std::uint8_t {
  Linear,       // raw bytes, poor DRAM locality
  Tiling,       // same byte count, better locality: Broadcom UIF/SAND/T,
                // ARM 16x16 interleaved, NVIDIA block-linear w/ compression == 0
  Compression,  // fewer bytes on the bus, data-dependent: AFBC/AFRC, AMD DCC,
                // Qualcomm UBWC, NVIDIA block-linear w/ compression != 0
};

[[nodiscard]] BandwidthClass classify(Modifier m) noexcept;

// Human-readable, e.g. "ARM_AFBC(16x16|SPARSE|YTR)", "QCOM_COMPRESSED(UBWC)".
// For logging only.
[[nodiscard]] std::string describe(Modifier m);

// Display-plane rotation. 90/270 transpose the scanout fetch order; 0/180 don't.
enum class Rotation : std::uint8_t { Rotate0, Rotate90, Rotate180, Rotate270 };

// Can a buffer with modifier m be scanned out under rotation r? A 90/270 rotation
// transposes the fetch order, which the display engine cannot follow for a LINEAR
// buffer (a rotated fetch needs a tiled layout) or for an AMD DCC metadata surface;
// every other layout (plain tiling, AFBC, ...) is left to the atomic TEST_ONLY
// commit to confirm. 0/180 never transpose, so all modifiers pass. This is a
// pre-filter for the negotiator only -- correctness still rests on the commit.
[[nodiscard]] bool rotation_compatible(Modifier m, Rotation r) noexcept;

// ---------------------------------------------------------------------------
// FormatTable: parsed, queryable view of a plane's IN_FORMATS blob.
// ---------------------------------------------------------------------------
struct FormatMod {
  std::uint32_t fourcc = 0;
  Modifier modifier{};

  friend bool operator==(const FormatMod& a, const FormatMod& b) noexcept {
    return a.fourcc == b.fourcc && a.modifier == b.modifier;
  }
  friend bool operator<(const FormatMod& a, const FormatMod& b) noexcept {
    return a.fourcc != b.fourcc ? a.fourcc < b.fourcc : a.modifier < b.modifier;
  }
};

class FormatTable {
 public:
  // Reads the IN_FORMATS blob off the plane and decodes every (fourcc, modifier)
  // pair. Returns a no_such_file error if the plane has no IN_FORMATS (very old
  // kernels) -- the caller then falls back to "formats[] implies LINEAR only".
  static drm::expected<FormatTable, std::error_code> from_plane(int drm_fd, std::uint32_t plane_id);

  // Parse a raw IN_FORMATS blob payload. Separated from the DRM fetch so the
  // parser is unit-testable against a synthetic blob (see tests/fmt). Malformed
  // input yields an empty table rather than UB.
  static FormatTable from_blob(const void* data, std::size_t size);

  [[nodiscard]] bool supports(std::uint32_t fourcc, Modifier m) const noexcept;
  [[nodiscard]] drm::span<const Modifier> modifiers_for(std::uint32_t fourcc) const noexcept;
  [[nodiscard]] drm::span<const FormatMod> all() const noexcept {
    return {pairs_.data(), pairs_.size()};
  }

 private:
  std::vector<FormatMod> pairs_;  // sorted by (fourcc, modifier); backs all()/supports()
  std::vector<Modifier> mods_;    // modifiers grouped contiguously by fourcc
  struct Group {                  // index into mods_, sorted by fourcc
    std::uint32_t fourcc = 0;
    std::uint32_t first = 0;  // offset into mods_
    std::uint32_t count = 0;
  };
  std::vector<Group> groups_;
};

// ---------------------------------------------------------------------------
// ScanoutBuffer: a GBM bo allocated against a modifier candidate list and
// imported as a KMS framebuffer. Owns both; move-only RAII.
//
// Handles the multi-plane case transparently: AFBC keeps its header in the same
// object, YUV-with-modifier and UBWC metadata surface as extra GBM planes. We
// forward whatever GBM reports straight into AddFB2WithModifiers, so we never
// special-case a vendor's auxiliary surface.
//
// Two construction paths:
//   create()        -- allocate fresh on a GBM device, SAME node as scanout.
//                      Integrated GPU+display (AMD, RPi) or a Mesa kmsro gbm.
//   import_dmabuf() -- wrap a dma-buf the *producer* already rendered and make
//                      it a scanout FB on a possibly DIFFERENT display node
//                      (Panfrost->Rockchip/MediaTek, PowerVR->tidss/rcar-du, or
//                      the compositor path for client buffers). The producer
//                      dictates the modifier; the display plane's IN_FORMATS +
//                      the test commit decide whether it scans out.
//
// LIFETIME: the returned buffer borrows the inputs -- create()'s `gbm` device
// (its bo lives on it) and both paths' DRM fd (the FB is registered on it). The
// `gbm_device` and the fd MUST outlive the ScanoutBuffer; destroying either
// first makes the destructor's gbm_bo_destroy()/drmModeRmFB() act on freed
// state (use-after-free / EBADF). Scope the ScanoutBuffer inside them.
// ---------------------------------------------------------------------------
class ScanoutBuffer {
 public:
  struct Desc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fourcc = 0;
    // Candidates, most-preferred first. GBM picks the best it can satisfy; ask
    // modifier() afterward. Pass a single LINEAR entry to force uncompressed.
    drm::span<const Modifier> modifiers;
    std::uint32_t gbm_flags = GBM_BO_USE_SCANOUT;
  };

  static drm::expected<ScanoutBuffer, std::error_code> create(gbm_device* gbm, int drm_fd,
                                                              const Desc& d);

  // Cross-device import. The caller must already have intersected producer
  // capabilities with the display plane's FormatTable; the test commit is the
  // final arbiter.
  struct ImportDesc {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t fourcc = 0;
    Modifier modifier{};  // exact layout of the imported buffer, not a candidate
    struct Plane {
      int dmabuf_fd = -1;  // borrowed: the FB takes its own ref; caller closes
      std::uint32_t stride = 0;
      std::uint32_t offset = 0;
    };
    drm::span<const Plane> planes;  // 1 for AFBC RGB; >1 for YUV / UBWC metadata
  };

  static drm::expected<ScanoutBuffer, std::error_code> import_dmabuf(int drm_scanout_fd,
                                                                     const ImportDesc& d);

  [[nodiscard]] std::uint32_t fb_id() const noexcept { return fb_id_; }
  [[nodiscard]] Modifier modifier() const noexcept { return chosen_; }
  [[nodiscard]] unsigned plane_count() const noexcept { return planes_; }
  [[nodiscard]] gbm_bo* bo() const noexcept { return bo_; }

  ScanoutBuffer(ScanoutBuffer&& /*o*/) noexcept;
  ScanoutBuffer& operator=(ScanoutBuffer&& /*o*/) noexcept;
  ScanoutBuffer(const ScanoutBuffer&) = delete;
  ScanoutBuffer& operator=(const ScanoutBuffer&) = delete;
  ~ScanoutBuffer();

 private:
  ScanoutBuffer() = default;
  gbm_bo* bo_ = nullptr;  // null after import_dmabuf(): no GBM ownership, only the FB
  int drm_fd_ = -1;
  std::uint32_t fb_id_ = 0;
  unsigned planes_ = 1;
  Modifier chosen_{};
};

// ---------------------------------------------------------------------------
// Allocator integration.
// ---------------------------------------------------------------------------

// Edge eligibility for the Hopcroft-Karp pre-solve: pure IN_FORMATS check.
[[nodiscard]] inline bool layer_fits_plane(const FormatTable& plane_formats, std::uint32_t fourcc,
                                           Modifier m) noexcept {
  return plane_formats.supports(fourcc, m);
}

// Memoizes atomic TEST_ONLY verdicts so an IN_FORMATS that advertises a modifier
// the hardware can't actually program (historically some pre-6.16 MediaTek OVL
// instances) costs one probe, not one glitch per frame. Keyed on
// (crtc, plane, fourcc, modifier).
class ModifierProbeCache {
 public:
  enum class Verdict : std::uint8_t { Unknown, Ok, Rejected };

  [[nodiscard]] Verdict lookup(std::uint32_t crtc, std::uint32_t plane, std::uint32_t fourcc,
                               Modifier m) const noexcept;
  void record(std::uint32_t crtc, std::uint32_t plane, std::uint32_t fourcc, Modifier m, bool ok);
  void invalidate_plane(std::uint32_t plane) noexcept;  // on hotplug/modeset

 private:
  struct Key {
    std::uint32_t crtc = 0;
    std::uint32_t plane = 0;
    std::uint32_t fourcc = 0;
    std::uint64_t modifier = 0;
    friend bool operator==(const Key& a, const Key& b) noexcept {
      return a.crtc == b.crtc && a.plane == b.plane && a.fourcc == b.fourcc &&
             a.modifier == b.modifier;
    }
  };
  std::vector<std::pair<Key, bool>> entries_;  // small; linear scan
};

// Approximate per-frame scanout READ in bytes for the allocator's content-
// priority / spatial-split cost. Compression is data-dependent, so we score with
// a conservative worst-case ratio: Linear/Tiling => full bytes, Compression =>
// full bytes * compressed_ratio.
//
// CONTRACT: `cls` MUST be classify(ScanoutBuffer::modifier()) -- the layout the
// display engine actually fetches -- NEVER a producer-internal compression state.
// On render-offload SoCs (PowerVR->tidss/rcar-du) the GPU may use PVRIC to cut
// its own DRAM traffic, but the display reads a decompressed buffer; crediting
// that here double-counts a saving on a bus this allocator does not manage.
[[nodiscard]] std::uint64_t scanout_cost_bytes(std::uint32_t w, std::uint32_t h,
                                               std::uint32_t fourcc, BandwidthClass cls,
                                               float compressed_ratio = 0.6F) noexcept;

// ===========================================================================
// Inline implementations
// ===========================================================================

inline BandwidthClass classify(Modifier m) noexcept {
  if (m.is_linear() || m.is_invalid()) {
    return BandwidthClass::Linear;
  }
  switch (m.vendor()) {
    case DRM_FORMAT_MOD_VENDOR_ARM: {
      // Legacy 16x16 interleaved (type field 0, value 1) is tiling, not AFBC.
      if (m.value == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED) {
        return BandwidthClass::Tiling;
      }
      const std::uint64_t type = (m.value >> 52) & 0xf;
      // AFBC (type 0) and AFRC (type 2) are true compression; MISC (type 1) tiling.
      return (type == DRM_FORMAT_MOD_ARM_TYPE_AFBC || type == DRM_FORMAT_MOD_ARM_TYPE_AFRC)
                 ? BandwidthClass::Compression
                 : BandwidthClass::Tiling;
    }
    case DRM_FORMAT_MOD_VENDOR_AMD:
#ifdef AMD_FMT_MOD
      // Canonical DCC accessor: nonzero "displayable DCC" => compression.
      return (AMD_FMT_MOD_GET(DCC, m.value) != 0U) ? BandwidthClass::Compression
                                                   : BandwidthClass::Tiling;
#else
      // Fallback for a drm_fourcc.h without the AMD_FMT_MOD macros: approximate
      // DCC bit (shift varies by GFX generation).
      return (((m.value >> 13) & 0x1) != 0U) ? BandwidthClass::Compression : BandwidthClass::Tiling;
#endif
    case DRM_FORMAT_MOD_VENDOR_QCOM:
      return BandwidthClass::Compression;  // QCOM_COMPRESSED == UBWC
    case DRM_FORMAT_MOD_VENDOR_NVIDIA:
      // Block-linear compression field (approximate; validate per target).
      return (((m.value >> 23) & 0x3) != 0U) ? BandwidthClass::Compression : BandwidthClass::Tiling;
    case DRM_FORMAT_MOD_VENDOR_BROADCOM:  // UIF/SAND/T: locality only, no savings
    default:
      // Unknown non-linear: assume tiled, not free.
      return BandwidthClass::Tiling;
  }
}

inline bool rotation_compatible(Modifier m, Rotation r) noexcept {
  if (r != Rotation::Rotate90 && r != Rotation::Rotate270) {
    return true;  // 0/180 keep the scanout walk order: any modifier is fine
  }
  if (m.is_linear()) {
    return false;  // a rotated fetch needs a tiled layout, not raw bytes
  }
  if (m.vendor() == DRM_FORMAT_MOD_VENDOR_AMD) {
    // AMD DCC metadata can't be walked transposed (same DCC bit classify() uses).
#ifdef AMD_FMT_MOD
    if (AMD_FMT_MOD_GET(DCC, m.value) != 0U) {
      return false;
    }
#else
    if (((m.value >> 13) & 0x1) != 0U) {
      return false;
    }
#endif
  }
  return true;
}

inline std::uint64_t scanout_cost_bytes(std::uint32_t w, std::uint32_t h, std::uint32_t /*fourcc*/,
                                        BandwidthClass cls, float compressed_ratio) noexcept {
  // bytes-per-pixel: simplified to the common 32bpp scanout case. Replace with a
  // fourcc->bpp lookup before trusting the cost model for placement decisions.
  const std::uint64_t bpp = 4;
  const std::uint64_t raw = static_cast<std::uint64_t>(w) * h * bpp;
  return cls == BandwidthClass::Compression
             ? static_cast<std::uint64_t>(static_cast<double>(raw) * compressed_ratio)
             : raw;  // Linear and Tiling move the same number of bytes
}

}  // namespace drm::fmt