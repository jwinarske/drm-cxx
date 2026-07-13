// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "vaapi_jpeg_decoder.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/external_dma_buf_source.hpp>

#include <drm_fourcc.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <system_error>
#include <unistd.h>
#include <va/va.h>
#include <va/va_dec_jpeg.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>
#include <vector>

namespace drm::examples::camera {

namespace {

// ── JPEG header parsing ──────────────────────────────────────────────
//
// Baseline-only (SOF0, 8-bit sample precision). Up to 4 components and
// 4 quantisation tables; AC/DC Huffman tables indexed in the standard
// JPEG layout. UVC and AMD/Intel MJPEG webcam streams universally fit
// this subset.

constexpr std::uint8_t k_marker_byte = 0xFF;
constexpr std::uint8_t k_marker_soi = 0xD8;
constexpr std::uint8_t k_marker_eoi = 0xD9;
constexpr std::uint8_t k_marker_sof0 = 0xC0;
constexpr std::uint8_t k_marker_dht = 0xC4;
constexpr std::uint8_t k_marker_dqt = 0xDB;
constexpr std::uint8_t k_marker_dri = 0xDD;
constexpr std::uint8_t k_marker_sos = 0xDA;

constexpr std::size_t k_max_components = 4;
constexpr std::size_t k_quant_table_size = 64;
constexpr std::size_t k_max_quant_tables = 4;
constexpr std::size_t k_huffman_slots = 4;  // DC0, DC1, AC0, AC1
constexpr std::size_t k_huffman_lengths = 16;
constexpr std::size_t k_huffman_max_values = 256;

struct Component {
  std::uint8_t id = 0;
  std::uint8_t h_sampling = 1;
  std::uint8_t v_sampling = 1;
  std::uint8_t qt_selector = 0;
};

struct ScanComponent {
  std::uint8_t id = 0;
  std::uint8_t dc_table = 0;
  std::uint8_t ac_table = 0;
};

struct HuffmanTable {
  bool valid = false;
  std::array<std::uint8_t, k_huffman_lengths> num_codes{};
  std::array<std::uint8_t, k_huffman_max_values> values{};
  std::uint16_t value_count = 0;
};

struct ParsedJpeg {
  std::uint16_t width = 0;
  std::uint16_t height = 0;
  std::array<std::uint8_t, k_max_quant_tables * k_quant_table_size> quant_tables{};
  std::array<bool, k_max_quant_tables> quant_valid{};
  std::array<Component, k_max_components> sof_components{};
  std::uint8_t sof_component_count = 0;
  std::array<ScanComponent, k_max_components> scan_components{};
  std::uint8_t scan_component_count = 0;
  std::array<HuffmanTable, k_huffman_slots> huffman{};
  std::uint16_t restart_interval = 0;
  const std::uint8_t* scan_data = nullptr;
  std::size_t scan_size = 0;
};

[[nodiscard]] std::uint16_t read_u16_be(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8U) | p[1]);
}

// Walk one JPEG until SOS. Returns false on malformed input. Fills
// every field of `out` the VA-API decoder needs.
[[nodiscard]] bool parse_jpeg(const std::uint8_t* data, std::size_t size, ParsedJpeg& out) {
  if (size < 4 || data[0] != k_marker_byte || data[1] != k_marker_soi) {
    return false;
  }
  std::size_t i = 2;
  while (i + 1 < size) {
    if (data[i] != k_marker_byte) {
      return false;
    }
    // Skip fill bytes (a run of 0xFF between segments is legal padding).
    while (i + 1 < size && data[i + 1] == k_marker_byte) {
      ++i;
    }
    if (i + 1 >= size) {
      return false;
    }
    const std::uint8_t marker = data[i + 1];
    i += 2;
    if (marker == k_marker_soi || marker == k_marker_eoi) {
      continue;
    }
    if (i + 2 > size) {
      return false;
    }
    const std::uint16_t seg_len = read_u16_be(data + i);
    if (seg_len < 2 || i + seg_len > size) {
      return false;
    }
    const std::uint8_t* seg = data + i + 2;
    const std::size_t seg_body = seg_len - 2U;
    if (marker == k_marker_sof0) {
      if (seg_body < 6) {
        return false;
      }
      // bits-per-sample, height, width, components
      if (seg[0] != 8) {
        return false;
      }
      out.height = read_u16_be(seg + 1);
      out.width = read_u16_be(seg + 3);
      const std::uint8_t nc = seg[5];
      if (nc == 0 || nc > k_max_components || seg_body < 6U + (std::size_t{nc} * 3U)) {
        return false;
      }
      out.sof_component_count = nc;
      for (std::size_t c = 0; c < nc; ++c) {
        const std::uint8_t* p = seg + 6 + (c * 3U);
        auto& comp = out.sof_components.at(c);
        comp.id = p[0];
        comp.h_sampling = (p[1] >> 4U) & 0x0FU;
        comp.v_sampling = p[1] & 0x0FU;
        comp.qt_selector = p[2];
      }
    } else if (marker == k_marker_dqt) {
      std::size_t off = 0;
      while (off < seg_body) {
        const std::uint8_t tq_byte = seg[off];
        const std::uint8_t precision = (tq_byte >> 4U) & 0x0FU;
        const std::uint8_t id = tq_byte & 0x0FU;
        if (precision != 0 || id >= k_max_quant_tables || off + 1 + k_quant_table_size > seg_body) {
          return false;  // baseline = 8-bit only
        }
        std::memcpy(out.quant_tables.data() + (std::size_t{id} * k_quant_table_size), seg + off + 1,
                    k_quant_table_size);
        out.quant_valid.at(id) = true;
        off += 1 + k_quant_table_size;
      }
    } else if (marker == k_marker_dht) {
      std::size_t off = 0;
      while (off + 17 <= seg_body) {
        const std::uint8_t tc_th = seg[off];
        const std::uint8_t tc = (tc_th >> 4U) & 0x0FU;  // 0=DC, 1=AC
        const std::uint8_t th = tc_th & 0x0FU;          // table id 0..1
        if (tc > 1 || th > 1) {
          return false;
        }
        const std::size_t slot = (std::size_t{tc} * 2U) + th;
        auto& t = out.huffman.at(slot);
        std::memcpy(t.num_codes.data(), seg + off + 1, k_huffman_lengths);
        std::uint16_t total = 0;
        for (const std::uint8_t v : t.num_codes) {
          total = static_cast<std::uint16_t>(total + v);
        }
        if (total > k_huffman_max_values || off + 17U + total > seg_body) {
          return false;
        }
        std::memcpy(t.values.data(), seg + off + 17, total);
        t.value_count = total;
        t.valid = true;
        off += 17U + total;
      }
    } else if (marker == k_marker_dri) {
      if (seg_body < 2) {
        return false;
      }
      out.restart_interval = read_u16_be(seg);
    } else if (marker == k_marker_sos) {
      if (seg_body < 1) {
        return false;
      }
      const std::uint8_t ns = seg[0];
      if (ns == 0 || ns > k_max_components || seg_body < 1U + (std::size_t{ns} * 2U) + 3U) {
        return false;
      }
      out.scan_component_count = ns;
      for (std::size_t c = 0; c < ns; ++c) {
        const std::uint8_t* p = seg + 1 + (c * 2U);
        auto& comp = out.scan_components.at(c);
        comp.id = p[0];
        comp.dc_table = (p[1] >> 4U) & 0x0FU;
        comp.ac_table = p[1] & 0x0FU;
      }
      // Entropy-coded data begins immediately after the SOS segment.
      const std::size_t scan_start = i + seg_len;
      if (scan_start >= size) {
        return false;
      }
      out.scan_data = data + scan_start;
      // Trim a trailing EOI marker if present.
      std::size_t scan_end = size;
      if (size >= 2 && data[size - 2] == k_marker_byte && data[size - 1] == k_marker_eoi) {
        scan_end = size - 2;
      }
      out.scan_size = scan_end - scan_start;
      return true;
    }
    i += seg_len;
  }
  return false;
}

// ── VA-API helpers ───────────────────────────────────────────────────

void log_va(const char* step, VAStatus status) noexcept try {
  drm::println(stderr, "[vaapi_jpeg_decoder] {}: {} (0x{:x})", step, vaErrorStr(status),
               static_cast<unsigned>(status));
} catch (...) {
  // Best-effort diagnostic; swallow any formatting failure in this noexcept helper.
  return;
}

// RAII for a VABufferID. vaCreateBuffer + vaDestroyBuffer pairs.
class VaBuffer {
 public:
  VaBuffer(VADisplay disp, VAContextID ctx, VABufferType type, unsigned int size, void* data)
      : disp_(disp) {
    if (const VAStatus s = vaCreateBuffer(disp, ctx, type, size, 1, data, &id_);
        s != VA_STATUS_SUCCESS) {
      log_va("vaCreateBuffer", s);
      id_ = VA_INVALID_ID;
    }
  }
  VaBuffer(const VaBuffer&) = delete;
  VaBuffer& operator=(const VaBuffer&) = delete;
  VaBuffer(VaBuffer&&) = delete;
  VaBuffer& operator=(VaBuffer&&) = delete;
  ~VaBuffer() {
    if (id_ != VA_INVALID_ID) {
      vaDestroyBuffer(disp_, id_);
    }
  }
  [[nodiscard]] VABufferID id() const noexcept { return id_; }
  [[nodiscard]] bool valid() const noexcept { return id_ != VA_INVALID_ID; }

 private:
  VADisplay disp_;
  VABufferID id_ = VA_INVALID_ID;
};

}  // namespace

void* VaapiJpegDecoder::open_display(int drm_fd, std::error_code* ec) noexcept {
  auto set_ec = [&](std::errc e) {
    if (ec != nullptr) {
      *ec = std::make_error_code(e);
    }
  };
  VADisplay disp = vaGetDisplayDRM(drm_fd);
  if (disp == nullptr) {
    set_ec(std::errc::no_such_device);
    return nullptr;
  }
  int major = 0;
  int minor = 0;
  if (const VAStatus s = vaInitialize(disp, &major, &minor); s != VA_STATUS_SUCCESS) {
    log_va("vaInitialize", s);
    set_ec(std::errc::io_error);
    return nullptr;
  }
  return disp;
}

void VaapiJpegDecoder::close_display(void* va_display) noexcept {
  if (va_display != nullptr) {
    vaTerminate(static_cast<VADisplay>(va_display));
  }
}

std::unique_ptr<VaapiJpegDecoder> VaapiJpegDecoder::create(void* va_display, std::uint32_t width,
                                                           std::uint32_t height, Sampling sampling,
                                                           std::error_code* ec) {
  auto set_ec = [&](std::errc e) {
    if (ec != nullptr) {
      *ec = std::make_error_code(e);
    }
  };
  if (sampling != Sampling::Yuv420 && sampling != Sampling::Yuv422) {
    set_ec(std::errc::invalid_argument);
    return nullptr;
  }
  if (va_display == nullptr) {
    set_ec(std::errc::invalid_argument);
    return nullptr;
  }

  auto dec = std::unique_ptr<VaapiJpegDecoder>(new VaapiJpegDecoder());
  dec->width_ = width;
  dec->height_ = height;
  dec->configured_sampling_ = sampling;
  dec->va_display_ = va_display;
  auto* disp = static_cast<VADisplay>(va_display);

  // Probe JPEG support before allocating anything.
  const int max_entrypoints = vaMaxNumEntrypoints(disp);
  std::vector<VAEntrypoint> entrypoints(static_cast<std::size_t>(max_entrypoints));
  int num_entry = 0;
  if (const VAStatus s =
          vaQueryConfigEntrypoints(disp, VAProfileJPEGBaseline, entrypoints.data(), &num_entry);
      s != VA_STATUS_SUCCESS) {
    log_va("vaQueryConfigEntrypoints(JPEGBaseline)", s);
    set_ec(std::errc::not_supported);
    return nullptr;
  }
  bool has_vld = false;
  for (int k = 0; k < num_entry; ++k) {
    if (entrypoints.at(static_cast<std::size_t>(k)) == VAEntrypointVLD) {
      has_vld = true;
      break;
    }
  }
  if (!has_vld) {
    set_ec(std::errc::not_supported);
    return nullptr;
  }

  // JPEG config: RT format matches the decode-side sub-sampling.
  // VCN's `radeon_dec_jpeg_check_format` compares the JPEG's
  // sampling-factor-derived expected pipe_format against the decode
  // surface's pipe_format; for 4:2:0 input we want a YUV420 decode
  // surface (NV12), for 4:2:2 input we want a YUV422 decode surface
  // (YUY2).
  const std::uint32_t jpeg_rt_format =
      (sampling == Sampling::Yuv422) ? VA_RT_FORMAT_YUV422 : VA_RT_FORMAT_YUV420;
  VAConfigAttrib attrib{};
  attrib.type = VAConfigAttribRTFormat;
  attrib.value = jpeg_rt_format;
  if (const VAStatus s = vaCreateConfig(disp, VAProfileJPEGBaseline, VAEntrypointVLD, &attrib, 1,
                                        &dec->va_jpeg_config_);
      s != VA_STATUS_SUCCESS) {
    log_va("vaCreateConfig(JPEG)", s);
    set_ec(std::errc::io_error);
    return nullptr;
  }

  // Output surface: always NV12. amdgpu DC and i915 DC both list NV12
  // as a scanout-capable plane format; YUYV is not on amdgpu's plane
  // format list, so we cannot scan out the decode surface directly in
  // 4:2:2 mode.
  VASurfaceAttrib out_attr{};
  out_attr.type = VASurfaceAttribPixelFormat;
  out_attr.flags = VA_SURFACE_ATTRIB_SETTABLE;
  out_attr.value.type = VAGenericValueTypeInteger;
  out_attr.value.value.i = VA_FOURCC_NV12;
  if (const VAStatus s = vaCreateSurfaces(disp, VA_RT_FORMAT_YUV420, width, height,
                                          &dec->va_output_surface_, 1, &out_attr, 1);
      s != VA_STATUS_SUCCESS) {
    log_va("vaCreateSurfaces(NV12 output)", s);
    set_ec(std::errc::io_error);
    return nullptr;
  }

  // Decode surface: same as output for 4:2:0; separate YUYV for 4:2:2.
  // We don't hint LINEAR on either — VCN's hardware writes into its
  // native tiled layout and a LINEAR hint causes the decode to fail
  // with "Decode format check failed". The exported NV12 output is
  // tagged with whatever modifier radeonsi picks; ExternalDmaBufSource
  // forwards it verbatim to drmModeAddFB2WithModifiers.
  unsigned int decode_target = dec->va_output_surface_;
  if (sampling == Sampling::Yuv422) {
    VASurfaceAttrib dec_attr{};
    dec_attr.type = VASurfaceAttribPixelFormat;
    dec_attr.flags = VA_SURFACE_ATTRIB_SETTABLE;
    dec_attr.value.type = VAGenericValueTypeInteger;
    dec_attr.value.value.i = VA_FOURCC_YUY2;
    if (const VAStatus s = vaCreateSurfaces(disp, VA_RT_FORMAT_YUV422, width, height,
                                            &dec->va_decode_surface_, 1, &dec_attr, 1);
        s != VA_STATUS_SUCCESS) {
      log_va("vaCreateSurfaces(YUY2 decode)", s);
      set_ec(std::errc::io_error);
      return nullptr;
    }
    decode_target = dec->va_decode_surface_;
  }

  if (const VAStatus s = vaCreateContext(disp, dec->va_jpeg_config_, static_cast<int>(width),
                                         static_cast<int>(height), VA_PROGRESSIVE, &decode_target,
                                         1, &dec->va_jpeg_context_);
      s != VA_STATUS_SUCCESS) {
    log_va("vaCreateContext(JPEG)", s);
    set_ec(std::errc::io_error);
    return nullptr;
  }

  // VP pipeline for 4:2:2 → NV12 chroma conversion. Default pipeline
  // params (no filters, full-surface copy) drive a hardware-accelerated
  // YUYV → NV12 downsample on every frame.
  if (sampling == Sampling::Yuv422) {
    if (const VAStatus s = vaCreateConfig(disp, VAProfileNone, VAEntrypointVideoProc, nullptr, 0,
                                          &dec->va_vp_config_);
        s != VA_STATUS_SUCCESS) {
      log_va("vaCreateConfig(VP)", s);
      set_ec(std::errc::io_error);
      return nullptr;
    }
    if (const VAStatus s = vaCreateContext(disp, dec->va_vp_config_, static_cast<int>(width),
                                           static_cast<int>(height), 0, &dec->va_output_surface_, 1,
                                           &dec->va_vp_context_);
        s != VA_STATUS_SUCCESS) {
      log_va("vaCreateContext(VP)", s);
      set_ec(std::errc::io_error);
      return nullptr;
    }
  }

  // Note: an attempted "clear surface to BT.601 black via vaPutImage"
  // step was removed — radeonsi appears to rebind the surface to a
  // freshly-allocated BO on vaPutImage, after which our exported
  // dma-buf still references the OLD (now-stale) BO that the VP
  // never writes to. End result: green-on-screen forever. The caller
  // sidesteps the issue by adding the layer at alpha=0 and bumping
  // back to 0xFFFF once decode_into_surface has written a real frame
  // (see configure_slot in main.cpp; the same first-frame gate covers
  // the libyuv tiers, whose XRGB DumbBufferSource is zero-filled and
  // would otherwise flash black for one to two frames).

  // Export the OUTPUT (NV12) surface as a dma-buf with separate-layers
  // so each plane has its own (offset, pitch); ExternalDmaBufSource
  // consumes that shape directly.
  VADRMPRIMESurfaceDescriptor desc{};
  if (const VAStatus s = vaExportSurfaceHandle(
          disp, dec->va_output_surface_, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
          VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &desc);
      s != VA_STATUS_SUCCESS) {
    log_va("vaExportSurfaceHandle", s);
    set_ec(std::errc::io_error);
    return nullptr;
  }
  if (desc.fourcc != VA_FOURCC_NV12 || desc.num_objects < 1 || desc.num_layers < 2) {
    drm::println(stderr,
                 "[vaapi_jpeg_decoder] vaExportSurfaceHandle: unexpected layout "
                 "(fourcc=0x{:x} objects={} layers={})",
                 desc.fourcc, desc.num_objects, desc.num_layers);
    // desc.objects / desc.layers are C arrays inside the VAAPI POD
    // descriptor — `.at()` isn't available. Bounds are validated above
    // (num_objects >= 1, num_layers >= 2). Suppress the bounds-check
    // lint narrowly for the C-API access.
    // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
    for (std::uint32_t k = 0; k < desc.num_objects; ++k) {
      ::close(desc.objects[k].fd);
    }
    // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
    set_ec(std::errc::not_supported);
    return nullptr;
  }
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
  dec->exported_fd_ = desc.objects[0].fd;
  // Close any aux dma-buf fds the driver handed back (we route every
  // plane through `objects[0]`; that's what desc.layers[*].object_index
  // resolves to when num_objects == 1, which is the radeonsi / iHD
  // common case for NV12).
  for (std::uint32_t k = 1; k < desc.num_objects; ++k) {
    ::close(desc.objects[k].fd);
  }
  dec->exported_modifier_ = desc.objects[0].drm_format_modifier;
  dec->exported_offset_y_ = desc.layers[0].offset[0];
  dec->exported_pitch_y_ = desc.layers[0].pitch[0];
  dec->exported_offset_uv_ = desc.layers[1].offset[0];
  dec->exported_pitch_uv_ = desc.layers[1].pitch[0];
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
  drm::println(stderr,
               "[vaapi_jpeg_decoder] surface ready: sampling={} {}x{} mod=0x{:016x} "
               "pitch_y={} pitch_uv={} offset_y={} offset_uv={}",
               sampling == Sampling::Yuv422 ? "4:2:2+VP" : "4:2:0", dec->width_, dec->height_,
               dec->exported_modifier_, dec->exported_pitch_y_, dec->exported_pitch_uv_,
               dec->exported_offset_y_, dec->exported_offset_uv_);

  return dec;
}

VaapiJpegDecoder::~VaapiJpegDecoder() {
  destroy_state();
}

void VaapiJpegDecoder::destroy_state() noexcept {
  if (exported_fd_ >= 0) {
    ::close(exported_fd_);
    exported_fd_ = -1;
  }
  // VADisplay is borrowed (owned by the caller via open_display /
  // close_display) so destroy_state never calls vaTerminate here —
  // doing so would brick the parent slot's display, and libva does
  // not allow re-init within the same process anyway.
  if (va_display_ != nullptr) {
    auto* disp = static_cast<VADisplay>(va_display_);
    // Tear down contexts before their referenced surfaces: VAAPI
    // implementations may pin the surfaces under the active context.
    if (va_vp_context_ != VA_INVALID_ID) {
      vaDestroyContext(disp, va_vp_context_);
      va_vp_context_ = VA_INVALID_ID;
    }
    if (va_jpeg_context_ != VA_INVALID_ID) {
      vaDestroyContext(disp, va_jpeg_context_);
      va_jpeg_context_ = VA_INVALID_ID;
    }
    if (va_decode_surface_ != VA_INVALID_ID) {
      vaDestroySurfaces(disp, &va_decode_surface_, 1);
      va_decode_surface_ = VA_INVALID_ID;
    }
    if (va_output_surface_ != VA_INVALID_ID) {
      vaDestroySurfaces(disp, &va_output_surface_, 1);
      va_output_surface_ = VA_INVALID_ID;
    }
    if (va_vp_config_ != VA_INVALID_ID) {
      vaDestroyConfig(disp, va_vp_config_);
      va_vp_config_ = VA_INVALID_ID;
    }
    if (va_jpeg_config_ != VA_INVALID_ID) {
      vaDestroyConfig(disp, va_jpeg_config_);
      va_jpeg_config_ = VA_INVALID_ID;
    }
    va_display_ = nullptr;
  }
}

bool VaapiJpegDecoder::decode_into_surface(const std::uint8_t* jpeg, std::size_t jpeg_size) noexcept
    try {
  if (va_display_ == nullptr || va_jpeg_context_ == VA_INVALID_ID) {
    return false;
  }
  ParsedJpeg parsed;
  if (!parse_jpeg(jpeg, jpeg_size, parsed)) {
    detected_sampling_ = Sampling::Unknown;
    return false;
  }
  if (parsed.width != width_ || parsed.height != height_) {
    drm::println(stderr,
                 "[vaapi_jpeg_decoder] JPEG dimensions {}x{} differ from surface {}x{}; skipping",
                 parsed.width, parsed.height, width_, height_);
    detected_sampling_ = Sampling::Unknown;
    return false;
  }
  if (parsed.sof_component_count == 0 || parsed.scan_size == 0) {
    detected_sampling_ = Sampling::Unknown;
    return false;
  }

  // Classify the JPEG's chroma sub-sampling from the Y component's
  // horizontal/vertical sampling factors (UVC always puts Y first):
  //   (2,2) → 4:2:0
  //   (2,1) → 4:2:2
  //   anything else → unknown (4:4:4 / grayscale / weird) — VCN may
  //   accept some of these too, but we don't have surface allocations
  //   for them so we drop the frame.
  const auto& y_comp = parsed.sof_components.at(0);
  if (y_comp.h_sampling == 2 && y_comp.v_sampling == 2) {
    detected_sampling_ = Sampling::Yuv420;
  } else if (y_comp.h_sampling == 2 && y_comp.v_sampling == 1) {
    detected_sampling_ = Sampling::Yuv422;
  } else {
    detected_sampling_ = Sampling::Unknown;
    static bool warned_unsupported = false;
    if (!warned_unsupported) {
      warned_unsupported = true;
      drm::println(stderr,
                   "[vaapi_jpeg_decoder] camera emits unsupported MJPEG sampling (Y h={} v={}); "
                   "frames will be dropped",
                   static_cast<unsigned>(y_comp.h_sampling),
                   static_cast<unsigned>(y_comp.v_sampling));
    }
    return false;
  }
  if (detected_sampling_ != configured_sampling_) {
    // Caller can read detected_sampling() and rebuild with the right
    // mode; silent return — no per-frame log spam.
    return false;
  }

  auto* disp = static_cast<VADisplay>(va_display_);
  const unsigned int decode_target =
      (va_decode_surface_ != VA_INVALID_ID) ? va_decode_surface_ : va_output_surface_;

  // pic.components / iq.{load_,}quantiser_table / ht.huffman_table /
  // sp.components are C arrays inside the VAAPI POD structs — `.at()`
  // isn't available and the bounds are validated against component_count
  // / k_max_quant_tables / 2 above. Suppress the bounds-check lint
  // narrowly for these C-API accesses.
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)

  // Picture parameters.
  VAPictureParameterBufferJPEGBaseline pic{};
  pic.picture_width = parsed.width;
  pic.picture_height = parsed.height;
  pic.num_components = parsed.sof_component_count;
  for (std::size_t c = 0; c < parsed.sof_component_count; ++c) {
    const auto& sof_c = parsed.sof_components.at(c);
    pic.components[c].component_id = sof_c.id;
    pic.components[c].h_sampling_factor = sof_c.h_sampling;
    pic.components[c].v_sampling_factor = sof_c.v_sampling;
    pic.components[c].quantiser_table_selector = sof_c.qt_selector;
  }

  // Quant tables: VAAPI takes a fixed 4-table struct; mark which are
  // loaded so unused entries don't affect the decode.
  VAIQMatrixBufferJPEGBaseline iq{};
  for (std::size_t t = 0; t < k_max_quant_tables; ++t) {
    iq.load_quantiser_table[t] = parsed.quant_valid.at(t) ? 1U : 0U;
    if (parsed.quant_valid.at(t)) {
      std::memcpy(iq.quantiser_table[t], parsed.quant_tables.data() + (t * k_quant_table_size),
                  k_quant_table_size);
    }
  }

  // Huffman tables: 2 DC + 2 AC slots, mirroring DHT layout.
  VAHuffmanTableBufferJPEGBaseline ht{};
  for (std::size_t t = 0; t < 2; ++t) {
    const auto& dc = parsed.huffman.at(t);      // DC class
    const auto& ac = parsed.huffman.at(2 + t);  // AC class
    ht.load_huffman_table[t] = (dc.valid || ac.valid) ? 1U : 0U;
    if (dc.valid) {
      std::memcpy(ht.huffman_table[t].num_dc_codes, dc.num_codes.data(), k_huffman_lengths);
      std::memcpy(ht.huffman_table[t].dc_values, dc.values.data(), dc.value_count);
    }
    if (ac.valid) {
      std::memcpy(ht.huffman_table[t].num_ac_codes, ac.num_codes.data(), k_huffman_lengths);
      std::memcpy(ht.huffman_table[t].ac_values, ac.values.data(), ac.value_count);
    }
  }

  // Slice parameters describe the lone scan (baseline = single scan).
  VASliceParameterBufferJPEGBaseline sp{};
  sp.slice_data_size = static_cast<unsigned int>(parsed.scan_size);
  sp.slice_data_offset = 0;
  sp.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
  sp.slice_horizontal_position = 0;
  sp.slice_vertical_position = 0;
  sp.num_components = parsed.scan_component_count;
  for (std::size_t c = 0; c < parsed.scan_component_count; ++c) {
    const auto& scan_c = parsed.scan_components.at(c);
    sp.components[c].component_selector = scan_c.id;
    sp.components[c].dc_table_selector = scan_c.dc_table;
    sp.components[c].ac_table_selector = scan_c.ac_table;
  }
  sp.restart_interval = parsed.restart_interval;
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)
  // Total MCUs in the frame. For 4:2:0 YUV (h=2, v=2 on Y, 1/1 on Cb/Cr)
  // the picture is split into ceil(W/16) * ceil(H/16) MCUs. For other
  // chroma layouts (4:2:2 or grayscale), the MCU width / height in
  // pixels differs. Compute from the maximum sampling factor across
  // every component.
  std::uint8_t mcu_w_blocks = 1;
  std::uint8_t mcu_h_blocks = 1;
  for (std::size_t c = 0; c < parsed.sof_component_count; ++c) {
    const auto& comp = parsed.sof_components.at(c);
    mcu_w_blocks = std::max(mcu_w_blocks, comp.h_sampling);
    mcu_h_blocks = std::max(mcu_h_blocks, comp.v_sampling);
  }
  const std::uint32_t mcu_w = static_cast<std::uint32_t>(mcu_w_blocks) * 8U;
  const std::uint32_t mcu_h = static_cast<std::uint32_t>(mcu_h_blocks) * 8U;
  const std::uint32_t mcus_x = (parsed.width + mcu_w - 1) / mcu_w;
  const std::uint32_t mcus_y = (parsed.height + mcu_h - 1) / mcu_h;
  sp.num_mcus = mcus_x * mcus_y;

  if (const VAStatus s = vaBeginPicture(disp, va_jpeg_context_, decode_target);
      s != VA_STATUS_SUCCESS) {
    log_va("vaBeginPicture(JPEG)", s);
    return false;
  }

  const VaBuffer pic_buf(disp, va_jpeg_context_, VAPictureParameterBufferType, sizeof(pic), &pic);
  const VaBuffer iq_buf(disp, va_jpeg_context_, VAIQMatrixBufferType, sizeof(iq), &iq);
  const VaBuffer ht_buf(disp, va_jpeg_context_, VAHuffmanTableBufferType, sizeof(ht), &ht);
  const VaBuffer sp_buf(disp, va_jpeg_context_, VASliceParameterBufferType, sizeof(sp), &sp);
  // Slice data buffer wants a non-const pointer per VA-API; the buffer
  // is copied internally so the cast is safe.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  void* scan_ptr = const_cast<void*>(static_cast<const void*>(parsed.scan_data));
  const VaBuffer scan_buf(disp, va_jpeg_context_, VASliceDataBufferType,
                          static_cast<unsigned int>(parsed.scan_size), scan_ptr);
  if (!pic_buf.valid() || !iq_buf.valid() || !ht_buf.valid() || !sp_buf.valid() ||
      !scan_buf.valid()) {
    vaEndPicture(disp, va_jpeg_context_);
    return false;
  }

  std::array<VABufferID, 5> ids{pic_buf.id(), iq_buf.id(), ht_buf.id(), sp_buf.id(), scan_buf.id()};
  if (const VAStatus s = vaRenderPicture(disp, va_jpeg_context_, ids.data(), ids.size());
      s != VA_STATUS_SUCCESS) {
    log_va("vaRenderPicture(JPEG)", s);
    vaEndPicture(disp, va_jpeg_context_);
    return false;
  }
  if (const VAStatus s = vaEndPicture(disp, va_jpeg_context_); s != VA_STATUS_SUCCESS) {
    log_va("vaEndPicture(JPEG)", s);
    return false;
  }

  // For 4:2:0 decode == output, scanning out from the decode surface
  // is direct and we only need to sync that one surface. For 4:2:2 the
  // decode surface is YUYV; we run the VP pipeline to convert into
  // the NV12 output surface, and sync that.
  if (configured_sampling_ == Sampling::Yuv422) {
    if (const VAStatus s = vaSyncSurface(disp, va_decode_surface_); s != VA_STATUS_SUCCESS) {
      log_va("vaSyncSurface(decode)", s);
      return false;
    }
    VAProcPipelineParameterBuffer pp{};
    pp.surface = va_decode_surface_;
    pp.surface_region = nullptr;
    pp.surface_color_standard = VAProcColorStandardNone;
    pp.output_region = nullptr;
    pp.output_background_color = 0;
    pp.output_color_standard = VAProcColorStandardNone;
    pp.pipeline_flags = 0;
    pp.filter_flags = 0;
    pp.filters = nullptr;
    pp.num_filters = 0;
    pp.forward_references = nullptr;
    pp.num_forward_references = 0;
    pp.backward_references = nullptr;
    pp.num_backward_references = 0;
    if (const VAStatus s = vaBeginPicture(disp, va_vp_context_, va_output_surface_);
        s != VA_STATUS_SUCCESS) {
      log_va("vaBeginPicture(VP)", s);
      return false;
    }
    const VaBuffer pp_buf(disp, va_vp_context_, VAProcPipelineParameterBufferType, sizeof(pp), &pp);
    if (!pp_buf.valid()) {
      vaEndPicture(disp, va_vp_context_);
      return false;
    }
    VABufferID pp_id = pp_buf.id();
    if (const VAStatus s = vaRenderPicture(disp, va_vp_context_, &pp_id, 1);
        s != VA_STATUS_SUCCESS) {
      log_va("vaRenderPicture(VP)", s);
      vaEndPicture(disp, va_vp_context_);
      return false;
    }
    if (const VAStatus s = vaEndPicture(disp, va_vp_context_); s != VA_STATUS_SUCCESS) {
      log_va("vaEndPicture(VP)", s);
      return false;
    }
  }
  if (const VAStatus s = vaSyncSurface(disp, va_output_surface_); s != VA_STATUS_SUCCESS) {
    log_va("vaSyncSurface(output)", s);
    return false;
  }
  return true;
} catch (...) {
  return false;
}

drm::expected<std::unique_ptr<drm::scene::ExternalDmaBufSource>, std::error_code>
VaapiJpegDecoder::make_source(const drm::Device& dev) const {
  if (exported_fd_ < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  const std::array<drm::scene::ExternalPlaneInfo, 2> planes{
      drm::scene::ExternalPlaneInfo{exported_fd_, exported_offset_y_, exported_pitch_y_},
      drm::scene::ExternalPlaneInfo{exported_fd_, exported_offset_uv_, exported_pitch_uv_},
  };
  return drm::scene::ExternalDmaBufSource::create(dev, width_, height_, DRM_FORMAT_NV12,
                                                  exported_modifier_, planes);
}

}  // namespace drm::examples::camera
