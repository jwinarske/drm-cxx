// SPDX-FileCopyrightText: (c) 2026 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "vaapi_h264_encoder.hpp"

#include <drm-cxx/detail/format.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <system_error>
#include <va/va.h>
#include <va/va_enc_h264.h>
#include <vector>

namespace drm::examples::camera {

namespace {

constexpr unsigned int k_va_invalid = 0xffffffffU;  // VA_INVALID_ID / VA_INVALID_SURFACE

// H.264 coding constants used across the SPS/PPS writer and the param buffers.
constexpr int k_profile_idc = 66;               // Baseline (constrained via constraint_set flags)
constexpr int k_level_idc = 42;                 // 4.2 — covers every common webcam size up to 1080p
constexpr int k_log2_max_frame_num_minus4 = 8;  // max_frame_num = 4096
constexpr int k_log2_max_poc_lsb_minus4 = 8;    // max_pic_order_cnt_lsb = 4096
constexpr std::uint32_t k_poc_lsb_mask = (1U << (k_log2_max_poc_lsb_minus4 + 4)) - 1U;

// NOLINTNEXTLINE(bugprone-exception-escape) — a throw from println just logs
void log_va(const char* step, VAStatus status) noexcept {
  drm::println(stderr, "[vaapi_h264_encoder] {}: {} (0x{:x})", step, vaErrorStr(status),
               static_cast<unsigned>(status));
}

// ── Annex-B bit writer for the SPS / PPS RBSP ────────────────────────────
//
// Writes a raw byte stream (start code + NAL header + RBSP), MSB-first,
// with Exp-Golomb helpers and start-code-emulation prevention on the RBSP.
class NalWriter {
 public:
  void start_code_and_header(std::uint8_t nal_ref_idc, std::uint8_t nal_unit_type) {
    out_.push_back(0x00);
    out_.push_back(0x00);
    out_.push_back(0x00);
    out_.push_back(0x01);
    // NAL header byte is not part of the emulation-prevented RBSP.
    out_.push_back(static_cast<std::uint8_t>((nal_ref_idc << 5U) | nal_unit_type));
    rbsp_start_ = out_.size();
  }

  void u(int bits, std::uint32_t value) {
    for (int i = bits - 1; i >= 0; --i) {
      put_bit((value >> static_cast<unsigned>(i)) & 1U);
    }
  }
  void ue(std::uint32_t value) {
    // Exp-Golomb: leadingZeros = floor(log2(value+1)); code = value+1.
    const std::uint32_t v = value + 1U;
    int bits = 0;
    while ((v >> static_cast<unsigned>(bits)) != 0U) {
      ++bits;
    }
    const int leading = bits - 1;
    u(leading, 0);
    u(bits, v);
  }
  void se(std::int32_t value) {
    if (value == 0) {
      ue(0);
    } else if (value > 0) {
      ue(static_cast<std::uint32_t>((2 * value) - 1));
    } else {
      ue(static_cast<std::uint32_t>(-2 * value));
    }
  }

  // rbsp_trailing_bits + flush, then splice emulation-prevention bytes into
  // the RBSP region (everything after the NAL header byte). For complete NALs
  // (SPS / PPS) supplied with has_emulation_bytes = 1.
  void finish() {
    put_bit(1);  // rbsp_stop_one_bit
    while (bit_count_ != 0) {
      put_bit(0);  // align to a byte
    }
    insert_emulation_prevention();
  }

  // Number of bits written so far (byte-aligned prefix + the partial byte).
  [[nodiscard]] unsigned int bit_length() const noexcept {
    return static_cast<unsigned int>((out_.size() * 8) + static_cast<std::size_t>(bit_count_));
  }

  // For a packed *slice header*: no rbsp_trailing, no emulation prevention
  // (the driver appends slice_data and does emulation prevention over the
  // combined NAL). Flush the partial byte, zero-padded; the exact used bit
  // count comes from bit_length() *before* this call.
  void flush_partial() {
    if (bit_count_ != 0) {
      out_.push_back(static_cast<std::uint8_t>(cur_ << static_cast<unsigned>(8 - bit_count_)));
      cur_ = 0;
      bit_count_ = 0;
    }
  }

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return out_; }

 private:
  void put_bit(std::uint32_t b) {
    cur_ = static_cast<std::uint8_t>((cur_ << 1U) | (b & 1U));
    if (++bit_count_ == 8) {
      out_.push_back(cur_);
      cur_ = 0;
      bit_count_ = 0;
    }
  }

  void insert_emulation_prevention() {
    std::vector<std::uint8_t> ep(out_.begin(),
                                 out_.begin() + static_cast<std::ptrdiff_t>(rbsp_start_));
    int zeros = 0;
    for (std::size_t i = rbsp_start_; i < out_.size(); ++i) {
      const std::uint8_t byte = out_[i];
      if (zeros >= 2 && byte <= 0x03) {
        ep.push_back(0x03);
        zeros = 0;
      }
      ep.push_back(byte);
      zeros = (byte == 0x00) ? zeros + 1 : 0;
    }
    out_.swap(ep);
  }

  std::vector<std::uint8_t> out_;
  std::size_t rbsp_start_ = 0;
  std::uint8_t cur_ = 0;
  int bit_count_ = 0;
};

std::vector<std::uint8_t> build_sps(std::uint32_t width, std::uint32_t height, std::uint32_t mbw,
                                    std::uint32_t mbh) {
  NalWriter w;
  w.start_code_and_header(3, 7);  // nal_ref_idc=3, nal_unit_type=7 (SPS)
  w.u(8, k_profile_idc);
  // constraint_set0..2 = 1 (constrained baseline), 3..5 = 0, reserved 2 bits = 0.
  w.u(8, 0xE0U);
  w.u(8, k_level_idc);
  w.ue(0);  // seq_parameter_set_id
  w.ue(k_log2_max_frame_num_minus4);
  w.ue(0);  // pic_order_cnt_type
  w.ue(k_log2_max_poc_lsb_minus4);
  w.ue(1);        // max_num_ref_frames
  w.u(1, 0);      // gaps_in_frame_num_value_allowed_flag
  w.ue(mbw - 1);  // pic_width_in_mbs_minus1
  w.ue(mbh - 1);  // pic_height_in_map_units_minus1
  w.u(1, 1);      // frame_mbs_only_flag
  w.u(1, 1);      // direct_8x8_inference_flag
  const std::uint32_t crop_r = ((mbw * 16U) - width) / 2U;
  const std::uint32_t crop_b = ((mbh * 16U) - height) / 2U;
  const bool crop = (crop_r != 0U) || (crop_b != 0U);
  w.u(1, crop ? 1U : 0U);  // frame_cropping_flag
  if (crop) {
    w.ue(0);       // frame_crop_left_offset
    w.ue(crop_r);  // frame_crop_right_offset (CropUnitX = 2 for 4:2:0)
    w.ue(0);       // frame_crop_top_offset
    w.ue(crop_b);  // frame_crop_bottom_offset
  }
  w.u(1, 0);  // vui_parameters_present_flag
  w.finish();
  return w.bytes();
}

std::vector<std::uint8_t> build_pps(std::uint32_t qp) {
  NalWriter w;
  w.start_code_and_header(3, 8);             // nal_unit_type=8 (PPS)
  w.ue(0);                                   // pic_parameter_set_id
  w.ue(0);                                   // seq_parameter_set_id
  w.u(1, 0);                                 // entropy_coding_mode_flag (0 = CAVLC, baseline)
  w.u(1, 0);                                 // bottom_field_pic_order_in_frame_present_flag
  w.ue(0);                                   // num_slice_groups_minus1
  w.ue(0);                                   // num_ref_idx_l0_default_active_minus1
  w.ue(0);                                   // num_ref_idx_l1_default_active_minus1
  w.u(1, 0);                                 // weighted_pred_flag
  w.u(2, 0);                                 // weighted_bipred_idc
  w.se(static_cast<std::int32_t>(qp) - 26);  // pic_init_qp_minus26
  w.se(0);                                   // pic_init_qs_minus26
  w.se(0);                                   // chroma_qp_index_offset
  w.u(1, 1);                                 // deblocking_filter_control_present_flag
  w.u(1, 0);                                 // constrained_intra_pred_flag
  w.u(1, 0);                                 // redundant_pic_cnt_present_flag
  w.finish();
  return w.bytes();
}

// A packed slice header: the NAL up to (not including) slice_data. `bit_length`
// is the exact number of header bits; the driver resumes from there with the
// encoded macroblocks and does emulation prevention over the whole NAL.
struct PackedSlice {
  std::vector<std::uint8_t> data;
  unsigned int bit_length = 0;
};

PackedSlice build_slice_header(bool is_idr, std::uint16_t frame_num, std::uint32_t poc_lsb,
                               std::uint32_t idr_pic_id) {
  NalWriter w;
  w.start_code_and_header(1, is_idr ? 5 : 1);  // nal_ref_idc=1, type 5 (IDR) / 1 (non-IDR)
  w.ue(0);                                     // first_mb_in_slice
  w.ue(is_idr ? 7 : 5);                        // slice_type (7 = I, 5 = P — "all slices" variants)
  w.ue(0);                                     // pic_parameter_set_id
  w.u(k_log2_max_frame_num_minus4 + 4, frame_num);
  if (is_idr) {
    w.ue(idr_pic_id);
  }
  w.u(k_log2_max_poc_lsb_minus4 + 4, poc_lsb);  // pic_order_cnt_lsb
  if (!is_idr) {
    w.u(1, 0);  // num_ref_idx_active_override_flag (use PPS default of 1)
    w.u(1, 0);  // ref_pic_list_modification_flag_l0
  }
  if (is_idr) {
    w.u(1, 0);  // no_output_of_prior_pics_flag
    w.u(1, 0);  // long_term_reference_flag
  } else {
    w.u(1, 0);  // adaptive_ref_pic_marking_mode_flag
  }
  w.se(0);  // slice_qp_delta
  w.ue(0);  // disable_deblocking_filter_idc (PPS deblock control present)
  w.se(0);  // slice_alpha_c0_offset_div2
  w.se(0);  // slice_beta_offset_div2
  const unsigned int bl = w.bit_length();
  w.flush_partial();
  return {w.bytes(), bl};
}

}  // namespace

std::unique_ptr<VaapiH264Encoder> VaapiH264Encoder::create(void* va_display, const Config& cfg,
                                                           std::error_code* ec) {
  auto set_ec = [&](std::errc e) {
    if (ec != nullptr) {
      *ec = std::make_error_code(e);
    }
  };
  if (va_display == nullptr || cfg.width == 0 || cfg.height == 0) {
    set_ec(std::errc::invalid_argument);
    return nullptr;
  }
  auto* disp = static_cast<VADisplay>(va_display);

  auto enc = std::unique_ptr<VaapiH264Encoder>(new VaapiH264Encoder());
  enc->va_display_ = va_display;
  enc->width_ = cfg.width;
  enc->height_ = cfg.height;
  enc->fps_ = cfg.fps != 0 ? cfg.fps : 30;
  enc->gop_ = cfg.gop != 0 ? cfg.gop : enc->fps_ * 2;
  enc->qp_ = cfg.qp <= 51 ? cfg.qp : 26;

  // Config: constrained-baseline encode with constant-QP rate control, and
  // app-supplied packed SPS/PPS (the radeonsi driver won't emit them itself).
  std::array<VAConfigAttrib, 2> cfg_attrs{};
  cfg_attrs[0].type = VAConfigAttribRateControl;
  cfg_attrs[0].value = VA_RC_CQP;
  cfg_attrs[1].type = VAConfigAttribEncPackedHeaders;
  cfg_attrs[1].value =
      VA_ENC_PACKED_HEADER_SEQUENCE | VA_ENC_PACKED_HEADER_PICTURE | VA_ENC_PACKED_HEADER_SLICE;
  if (const VAStatus s = vaCreateConfig(
          disp, VAProfileH264ConstrainedBaseline, VAEntrypointEncSlice, cfg_attrs.data(),
          static_cast<int>(cfg_attrs.size()), reinterpret_cast<VAConfigID*>(&enc->va_config_));
      s != VA_STATUS_SUCCESS) {
    log_va("vaCreateConfig", s);
    set_ec(std::errc::not_supported);
    return nullptr;
  }

  // Reconstructed/reference pool (2) + an upload surface for the CPU path.
  std::array<VASurfaceID, 3> surfaces{};
  if (const VAStatus s =
          vaCreateSurfaces(disp, VA_RT_FORMAT_YUV420, cfg.width, cfg.height, surfaces.data(),
                           static_cast<unsigned>(surfaces.size()), nullptr, 0);
      s != VA_STATUS_SUCCESS) {
    log_va("vaCreateSurfaces", s);
    set_ec(std::errc::not_supported);
    enc->destroy_state();
    return nullptr;
  }
  enc->va_recon_[0] = surfaces[0];
  enc->va_recon_[1] = surfaces[1];
  enc->va_input_surface_ = surfaces[2];

  if (const VAStatus s = vaCreateContext(
          disp, static_cast<VAConfigID>(enc->va_config_), static_cast<int>(cfg.width),
          static_cast<int>(cfg.height), VA_PROGRESSIVE, surfaces.data(),
          static_cast<int>(surfaces.size()), reinterpret_cast<VAContextID*>(&enc->va_context_));
      s != VA_STATUS_SUCCESS) {
    log_va("vaCreateContext", s);
    set_ec(std::errc::not_supported);
    enc->destroy_state();
    return nullptr;
  }

  // Coded output buffer, reused each frame. Sized generously (worst-case an
  // IDR is well under the uncompressed frame size).
  const unsigned int coded_size = (cfg.width * cfg.height * 3U / 2U) + (1U << 16U);
  if (const VAStatus s = vaCreateBuffer(disp, static_cast<VAContextID>(enc->va_context_),
                                        VAEncCodedBufferType, coded_size, 1, nullptr,
                                        reinterpret_cast<VABufferID*>(&enc->va_coded_buffer_));
      s != VA_STATUS_SUCCESS) {
    log_va("vaCreateBuffer(coded)", s);
    set_ec(std::errc::not_supported);
    enc->destroy_state();
    return nullptr;
  }

  return enc;
}

bool VaapiH264Encoder::encode_surface(unsigned int nv12_surface,
                                      std::vector<std::uint8_t>& out) noexcept {
  return encode_source(nv12_surface, out);
}

bool VaapiH264Encoder::encode_nv12(const std::uint8_t* y, const std::uint8_t* uv,
                                   std::uint32_t y_stride, std::uint32_t uv_stride,
                                   std::vector<std::uint8_t>& out) noexcept {
  if (y == nullptr || uv == nullptr) {
    return false;
  }
  auto* disp = static_cast<VADisplay>(va_display_);

  // Derive an NV12 VAImage for the input surface and copy the planes in.
  VAImage img{};
  if (const VAStatus s = vaDeriveImage(disp, static_cast<VASurfaceID>(va_input_surface_), &img);
      s != VA_STATUS_SUCCESS) {
    log_va("vaDeriveImage(input)", s);
    return false;
  }
  void* base = nullptr;
  if (const VAStatus s = vaMapBuffer(disp, img.buf, &base); s != VA_STATUS_SUCCESS) {
    log_va("vaMapBuffer(input)", s);
    vaDestroyImage(disp, img.image_id);
    return false;
  }
  auto* dst = static_cast<std::uint8_t*>(base);
  std::uint8_t* dst_y = dst + img.offsets[0];
  std::uint8_t* dst_uv = dst + img.offsets[1];
  for (std::uint32_t r = 0; r < height_; ++r) {
    std::memcpy(dst_y + (static_cast<std::size_t>(r) * img.pitches[0]),
                y + (static_cast<std::size_t>(r) * y_stride), width_);
  }
  for (std::uint32_t r = 0; r < height_ / 2U; ++r) {
    std::memcpy(dst_uv + (static_cast<std::size_t>(r) * img.pitches[1]),
                uv + (static_cast<std::size_t>(r) * uv_stride), width_);
  }
  vaUnmapBuffer(disp, img.buf);
  vaDestroyImage(disp, img.image_id);

  return encode_source(va_input_surface_, out);
}

bool VaapiH264Encoder::encode_source(unsigned int src_surface,
                                     std::vector<std::uint8_t>& out) noexcept {
  auto* disp = static_cast<VADisplay>(va_display_);
  const auto ctx = static_cast<VAContextID>(va_context_);
  const std::uint32_t mbw = (width_ + 15U) / 16U;
  const std::uint32_t mbh = (height_ + 15U) / 16U;
  const bool is_idr = (frame_in_gop_ == 0);
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index) — a 2-entry pool, index in {0,1}
  const auto recon = static_cast<VASurfaceID>(va_recon_[cur_recon_]);
  const auto ref = static_cast<VASurfaceID>(va_recon_[1 - cur_recon_]);
  // NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

  // Buffers created for this frame, destroyed after vaEndPicture. Worst case
  // (IDR): seq + packed SPS (param+data) + packed PPS (param+data) + pic +
  // packed slice (param+data) + slice = 10.
  std::array<VABufferID, 12> bufs{};
  std::size_t nbuf = 0;
  bool ok = true;
  auto make_buf = [&](VABufferType type, unsigned int size, void* data) {
    if (!ok) {
      return;
    }
    VABufferID id = VA_INVALID_ID;
    if (const VAStatus s = vaCreateBuffer(disp, ctx, type, size, 1, data, &id);
        s != VA_STATUS_SUCCESS) {
      log_va("vaCreateBuffer", s);
      ok = false;
      return;
    }
    bufs.at(nbuf++) = id;
  };

  // Sequence parameters (only rendered on an IDR).
  VAEncSequenceParameterBufferH264 seq{};
  seq.seq_parameter_set_id = 0;
  seq.level_idc = k_level_idc;
  seq.intra_period = gop_;
  seq.intra_idr_period = gop_;
  seq.ip_period = 1;
  seq.max_num_ref_frames = 1;
  seq.picture_width_in_mbs = mbw;
  seq.picture_height_in_mbs = mbh;
  seq.seq_fields.bits.chroma_format_idc = 1;  // 4:2:0
  seq.seq_fields.bits.frame_mbs_only_flag = 1;
  seq.seq_fields.bits.direct_8x8_inference_flag = 1;
  seq.seq_fields.bits.log2_max_frame_num_minus4 = k_log2_max_frame_num_minus4;
  seq.seq_fields.bits.pic_order_cnt_type = 0;
  seq.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = k_log2_max_poc_lsb_minus4;
  if (((mbw * 16U) != width_) || ((mbh * 16U) != height_)) {
    seq.frame_cropping_flag = 1;
    seq.frame_crop_right_offset = ((mbw * 16U) - width_) / 2U;
    seq.frame_crop_bottom_offset = ((mbh * 16U) - height_) / 2U;
  }

  // Picture parameters.
  VAEncPictureParameterBufferH264 pic{};
  pic.CurrPic.picture_id = recon;
  pic.CurrPic.frame_idx = frame_num_;
  pic.CurrPic.flags = 0;
  pic.CurrPic.TopFieldOrderCnt = poc_;
  pic.CurrPic.BottomFieldOrderCnt = poc_;
  for (auto& r : pic.ReferenceFrames) {
    r.picture_id = VA_INVALID_SURFACE;
    r.flags = VA_PICTURE_H264_INVALID;
  }
  if (!is_idr && have_ref_) {
    pic.ReferenceFrames[0].picture_id = ref;
    pic.ReferenceFrames[0].frame_idx = (frame_num_ == 0) ? 0 : frame_num_ - 1;
    pic.ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    pic.ReferenceFrames[0].TopFieldOrderCnt = poc_ - 2;
    pic.ReferenceFrames[0].BottomFieldOrderCnt = poc_ - 2;
  }
  pic.coded_buf = static_cast<VABufferID>(va_coded_buffer_);
  pic.pic_parameter_set_id = 0;
  pic.seq_parameter_set_id = 0;
  pic.frame_num = frame_num_;
  pic.pic_init_qp = qp_;
  pic.num_ref_idx_l0_active_minus1 = 0;
  pic.num_ref_idx_l1_active_minus1 = 0;
  pic.pic_fields.bits.idr_pic_flag = is_idr ? 1U : 0U;
  pic.pic_fields.bits.reference_pic_flag = 1;
  pic.pic_fields.bits.entropy_coding_mode_flag = 0;
  pic.pic_fields.bits.deblocking_filter_control_present_flag = 1;
  if (is_idr) {
    pic.pic_fields.bits.idr_pic_flag = 1;
  }

  // Slice parameters — one slice covering the whole frame.
  VAEncSliceParameterBufferH264 slice{};
  slice.macroblock_address = 0;
  slice.num_macroblocks = mbw * mbh;
  slice.slice_type = is_idr ? 2U : 0U;  // 2 = I, 0 = P
  slice.pic_parameter_set_id = 0;
  slice.idr_pic_id = static_cast<std::uint16_t>(idr_pic_id_);
  slice.pic_order_cnt_lsb =
      static_cast<std::uint16_t>(static_cast<std::uint32_t>(poc_) & k_poc_lsb_mask);
  slice.num_ref_idx_l0_active_minus1 = 0;
  slice.slice_qp_delta = 0;
  slice.disable_deblocking_filter_idc = 0;
  for (auto& r : slice.RefPicList0) {
    r.picture_id = VA_INVALID_SURFACE;
    r.flags = VA_PICTURE_H264_INVALID;
  }
  if (!is_idr && have_ref_) {
    slice.RefPicList0[0].picture_id = ref;
    slice.RefPicList0[0].frame_idx = (frame_num_ == 0) ? 0 : frame_num_ - 1;
    slice.RefPicList0[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    slice.RefPicList0[0].TopFieldOrderCnt = poc_ - 2;
    slice.RefPicList0[0].BottomFieldOrderCnt = poc_ - 2;
  }

  if (const VAStatus s = vaBeginPicture(disp, ctx, static_cast<VASurfaceID>(src_surface));
      s != VA_STATUS_SUCCESS) {
    log_va("vaBeginPicture", s);
    return false;
  }

  if (is_idr) {
    make_buf(VAEncSequenceParameterBufferType, sizeof(seq), &seq);
    // Packed SPS.
    const std::vector<std::uint8_t> sps = build_sps(width_, height_, mbw, mbh);
    VAEncPackedHeaderParameterBuffer ph_sps{};
    ph_sps.type = VAEncPackedHeaderSequence;
    ph_sps.bit_length = static_cast<unsigned>(sps.size() * 8);
    ph_sps.has_emulation_bytes = 1;
    make_buf(VAEncPackedHeaderParameterBufferType, sizeof(ph_sps), &ph_sps);
    make_buf(
        VAEncPackedHeaderDataBufferType, static_cast<unsigned>(sps.size()),
        const_cast<std::uint8_t*>(sps.data()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    // Packed PPS.
    const std::vector<std::uint8_t> pps = build_pps(qp_);
    VAEncPackedHeaderParameterBuffer ph_pps{};
    ph_pps.type = VAEncPackedHeaderPicture;
    ph_pps.bit_length = static_cast<unsigned>(pps.size() * 8);
    ph_pps.has_emulation_bytes = 1;
    make_buf(VAEncPackedHeaderParameterBufferType, sizeof(ph_pps), &ph_pps);
    make_buf(
        VAEncPackedHeaderDataBufferType, static_cast<unsigned>(pps.size()),
        const_cast<std::uint8_t*>(pps.data()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  }
  make_buf(VAEncPictureParameterBufferType, sizeof(pic), &pic);
  // Packed slice header — radeonsi emits no bitstream headers itself, so the
  // app supplies the slice NAL header up to slice_data (the driver appends the
  // encoded macroblocks and does emulation prevention over the whole NAL).
  const PackedSlice sh = build_slice_header(
      is_idr, frame_num_, static_cast<std::uint32_t>(poc_) & k_poc_lsb_mask, idr_pic_id_);
  VAEncPackedHeaderParameterBuffer ph_slice{};
  ph_slice.type = VAEncPackedHeaderSlice;
  ph_slice.bit_length = sh.bit_length;
  ph_slice.has_emulation_bytes = 0;
  make_buf(VAEncPackedHeaderParameterBufferType, sizeof(ph_slice), &ph_slice);
  make_buf(
      VAEncPackedHeaderDataBufferType, static_cast<unsigned>(sh.data.size()),
      const_cast<std::uint8_t*>(sh.data.data()));  // NOLINT(cppcoreguidelines-pro-type-const-cast)
  make_buf(VAEncSliceParameterBufferType, sizeof(slice), &slice);

  if (ok) {
    if (const VAStatus s = vaRenderPicture(disp, ctx, bufs.data(), static_cast<int>(nbuf));
        s != VA_STATUS_SUCCESS) {
      log_va("vaRenderPicture", s);
      ok = false;
    }
  }
  if (const VAStatus s = vaEndPicture(disp, ctx); s != VA_STATUS_SUCCESS) {
    log_va("vaEndPicture", s);
    ok = false;
  }
  for (std::size_t i = 0; i < nbuf; ++i) {
    vaDestroyBuffer(disp, bufs.at(i));
  }
  if (!ok) {
    return false;
  }

  if (const VAStatus s = vaSyncSurface(disp, static_cast<VASurfaceID>(src_surface));
      s != VA_STATUS_SUCCESS) {
    log_va("vaSyncSurface", s);
    return false;
  }

  // Extract the coded bitstream (a segment chain) into `out`.
  void* seg_ptr = nullptr;
  if (const VAStatus s = vaMapBuffer(disp, static_cast<VABufferID>(va_coded_buffer_), &seg_ptr);
      s != VA_STATUS_SUCCESS) {
    log_va("vaMapBuffer(coded)", s);
    return false;
  }
  for (auto* seg = static_cast<VACodedBufferSegment*>(seg_ptr); seg != nullptr;
       seg = static_cast<VACodedBufferSegment*>(seg->next)) {
    const auto* p = static_cast<const std::uint8_t*>(seg->buf);
    out.insert(out.end(), p, p + seg->size);
  }
  vaUnmapBuffer(disp, static_cast<VABufferID>(va_coded_buffer_));

  // Advance GOP / reference state.
  if (is_idr) {
    frame_num_ = 0;
    poc_ = 0;
    idr_pic_id_ = (idr_pic_id_ + 1U) & 0xffffU;
  }
  frame_num_ = static_cast<std::uint16_t>((frame_num_ + 1U) & 0xffffU);
  poc_ += 2;
  have_ref_ = true;
  cur_recon_ = 1 - cur_recon_;
  frame_in_gop_ = (frame_in_gop_ + 1U) % gop_;
  return true;
}

void VaapiH264Encoder::destroy_state() noexcept {
  if (va_display_ == nullptr) {
    return;
  }
  auto* disp = static_cast<VADisplay>(va_display_);
  if (va_coded_buffer_ != k_va_invalid) {
    vaDestroyBuffer(disp, static_cast<VABufferID>(va_coded_buffer_));
    va_coded_buffer_ = k_va_invalid;
  }
  if (va_context_ != k_va_invalid) {
    vaDestroyContext(disp, static_cast<VAContextID>(va_context_));
    va_context_ = k_va_invalid;
  }
  for (auto& s : va_recon_) {
    if (s != k_va_invalid) {
      auto id = static_cast<VASurfaceID>(s);
      vaDestroySurfaces(disp, &id, 1);
      s = k_va_invalid;
    }
  }
  if (va_input_surface_ != k_va_invalid) {
    auto id = static_cast<VASurfaceID>(va_input_surface_);
    vaDestroySurfaces(disp, &id, 1);
    va_input_surface_ = k_va_invalid;
  }
  if (va_config_ != k_va_invalid) {
    vaDestroyConfig(disp, static_cast<VAConfigID>(va_config_));
    va_config_ = k_va_invalid;
  }
}

VaapiH264Encoder::~VaapiH264Encoder() {
  destroy_state();
}

}  // namespace drm::examples::camera
