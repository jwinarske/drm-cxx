// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "crtc_color_pipeline.hpp"

#include "../core/device.hpp"
#include "../core/property_store.hpp"
#include "color_pipeline_curves.hpp"
#include "crtc_capabilities.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/atomic.hpp>

#include <drm/drm_mode.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <system_error>
#include <vector>

namespace drm::display {

namespace {

drm::expected<std::uint32_t, std::error_code> create_blob(int fd, const void* data,
                                                          std::size_t size_bytes) {
  std::uint32_t blob_id = 0;
  if (drmModeCreatePropertyBlob(fd, data, size_bytes, &blob_id) != 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::system_category()));
  }
  return blob_id;
}

}  // namespace

CrtcColorPipeline::CrtcColorPipeline(int fd, std::uint32_t crtc_id, CrtcCapabilities caps) noexcept
    : fd_(fd), crtc_id_(crtc_id), caps_(caps) {}

CrtcColorPipeline::~CrtcColorPipeline() {
  destroy_blob(degamma_blob_);
  destroy_blob(ctm_blob_);
  destroy_blob(gamma_blob_);
}

CrtcColorPipeline::CrtcColorPipeline(CrtcColorPipeline&& other) noexcept
    : fd_(other.fd_),
      crtc_id_(other.crtc_id_),
      caps_(other.caps_),
      degamma_blob_(other.degamma_blob_),
      ctm_blob_(other.ctm_blob_),
      gamma_blob_(other.gamma_blob_),
      degamma_prop_(other.degamma_prop_),
      ctm_prop_(other.ctm_prop_),
      gamma_prop_(other.gamma_prop_) {
  other.fd_ = -1;
  other.crtc_id_ = 0;
  other.degamma_blob_ = 0;
  other.ctm_blob_ = 0;
  other.gamma_blob_ = 0;
}

CrtcColorPipeline& CrtcColorPipeline::operator=(CrtcColorPipeline&& other) noexcept {
  if (this != &other) {
    destroy_blob(degamma_blob_);
    destroy_blob(ctm_blob_);
    destroy_blob(gamma_blob_);
    fd_ = other.fd_;
    crtc_id_ = other.crtc_id_;
    caps_ = other.caps_;
    degamma_blob_ = other.degamma_blob_;
    ctm_blob_ = other.ctm_blob_;
    gamma_blob_ = other.gamma_blob_;
    degamma_prop_ = other.degamma_prop_;
    ctm_prop_ = other.ctm_prop_;
    gamma_prop_ = other.gamma_prop_;
    other.fd_ = -1;
    other.crtc_id_ = 0;
    other.degamma_blob_ = 0;
    other.ctm_blob_ = 0;
    other.gamma_blob_ = 0;
  }
  return *this;
}

void CrtcColorPipeline::destroy_blob(std::uint32_t& blob_id) const noexcept {
  if (blob_id != 0 && fd_ >= 0) {
    drmModeDestroyPropertyBlob(fd_, blob_id);
  }
  blob_id = 0;
}

drm::expected<CrtcColorPipeline, std::error_code> CrtcColorPipeline::create(
    const drm::Device& dev, const std::uint32_t crtc_id) {
  auto caps = probe_crtc_capabilities(dev, crtc_id);
  if (!caps) {
    return drm::unexpected<std::error_code>(caps.error());
  }
  if (!caps->has_degamma_lut && !caps->has_ctm && !caps->has_gamma_lut) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_supported));
  }
  CrtcColorPipeline pipeline(dev.fd(), crtc_id, *caps);
  // Resolve the stage property ids once: they cannot change for the life of
  // the CRTC, and re-reading the property set cost ~10 ioctls per commit.
  drm::PropertyStore props;
  if (auto r = props.cache_properties(dev.fd(), crtc_id, DRM_MODE_OBJECT_CRTC); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  pipeline.degamma_prop_ = props.property_id(crtc_id, "DEGAMMA_LUT").value_or(0);
  pipeline.ctm_prop_ = props.property_id(crtc_id, "CTM").value_or(0);
  pipeline.gamma_prop_ = props.property_id(crtc_id, "GAMMA_LUT").value_or(0);
  return pipeline;
}

drm::expected<void, std::error_code> CrtcColorPipeline::replace_lut_blob(
    std::uint32_t& slot, const drm::span<const drm_color_lut> lut) {
  auto blob = create_blob(fd_, lut.data(), lut.size_bytes());
  if (!blob) {
    return drm::unexpected<std::error_code>(blob.error());
  }
  destroy_blob(slot);
  slot = *blob;
  return {};
}

drm::expected<void, std::error_code> CrtcColorPipeline::replace_ctm_blob(std::uint32_t& slot,
                                                                         const drm_color_ctm& ctm) {
  auto blob = create_blob(fd_, &ctm, sizeof(ctm));
  if (!blob) {
    return drm::unexpected<std::error_code>(blob.error());
  }
  destroy_blob(slot);
  slot = *blob;
  return {};
}

// ── Built-in stages ────────────────────────────────────────────────

drm::expected<void, std::error_code> CrtcColorPipeline::set_identity() {
  if (!caps_.has_degamma_lut && !caps_.has_ctm && !caps_.has_gamma_lut) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_supported));
  }
  if (caps_.has_degamma_lut) {
    std::vector<drm_color_lut> lut(caps_.degamma_lut_size);
    build_identity_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
    if (auto r =
            replace_lut_blob(degamma_blob_, drm::span<const drm_color_lut>(lut.data(), lut.size()));
        !r) {
      return r;
    }
  }
  if (caps_.has_ctm) {
    const auto ctm = build_identity_ctm();
    if (auto r = replace_ctm_blob(ctm_blob_, ctm); !r) {
      return r;
    }
  }
  if (caps_.has_gamma_lut) {
    std::vector<drm_color_lut> lut(caps_.gamma_lut_size);
    build_identity_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
    if (auto r =
            replace_lut_blob(gamma_blob_, drm::span<const drm_color_lut>(lut.data(), lut.size()));
        !r) {
      return r;
    }
  }
  return {};
}

drm::expected<void, std::error_code> CrtcColorPipeline::set_pq_to_linear() {
  if (!caps_.has_degamma_lut) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_supported));
  }
  std::vector<drm_color_lut> lut(caps_.degamma_lut_size);
  build_pq_eotf_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
  return replace_lut_blob(degamma_blob_, drm::span<const drm_color_lut>(lut.data(), lut.size()));
}

drm::expected<void, std::error_code> CrtcColorPipeline::set_hlg_to_linear() {
  if (!caps_.has_degamma_lut) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_supported));
  }
  std::vector<drm_color_lut> lut(caps_.degamma_lut_size);
  build_hlg_oetf_inverse_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
  return replace_lut_blob(degamma_blob_, drm::span<const drm_color_lut>(lut.data(), lut.size()));
}

drm::expected<void, std::error_code> CrtcColorPipeline::set_bt2020_to_bt709() {
  if (!caps_.has_ctm) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_supported));
  }
  const auto ctm = build_bt2020_to_bt709_ctm();
  return replace_ctm_blob(ctm_blob_, ctm);
}

drm::expected<void, std::error_code> CrtcColorPipeline::set_linear_to_pq() {
  if (!caps_.has_gamma_lut) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_supported));
  }
  std::vector<drm_color_lut> lut(caps_.gamma_lut_size);
  build_pq_oetf_lut(drm::span<drm_color_lut>(lut.data(), lut.size()));
  return replace_lut_blob(gamma_blob_, drm::span<const drm_color_lut>(lut.data(), lut.size()));
}

// ── Custom stages ──────────────────────────────────────────────────

drm::expected<void, std::error_code> CrtcColorPipeline::set_custom_degamma(
    const drm::span<const drm_color_lut> lut) {
  if (!caps_.has_degamma_lut) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_supported));
  }
  if (lut.size() != caps_.degamma_lut_size) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  return replace_lut_blob(degamma_blob_, lut);
}

drm::expected<void, std::error_code> CrtcColorPipeline::set_custom_gamma(
    const drm::span<const drm_color_lut> lut) {
  if (!caps_.has_gamma_lut) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_supported));
  }
  if (lut.size() != caps_.gamma_lut_size) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  return replace_lut_blob(gamma_blob_, lut);
}

drm::expected<void, std::error_code> CrtcColorPipeline::set_custom_ctm(const drm_color_ctm& ctm) {
  if (!caps_.has_ctm) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_supported));
  }
  return replace_ctm_blob(ctm_blob_, ctm);
}

// ── Apply / lifecycle ──────────────────────────────────────────────

drm::expected<void, std::error_code> CrtcColorPipeline::apply(drm::AtomicRequest& req) const {
  // A set stage writes through the property id create() resolved; a CRTC that
  // lacks the property never got a blob for it (the setters check caps_).
  auto write = [&](std::uint32_t prop, std::uint64_t v) -> drm::expected<void, std::error_code> {
    if (prop == 0) {
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::operation_not_supported));
    }
    return req.add_property(crtc_id_, prop, v);
  };
  if (degamma_blob_ != 0) {
    if (auto r = write(degamma_prop_, degamma_blob_); !r) {
      return r;
    }
  }
  if (ctm_blob_ != 0) {
    if (auto r = write(ctm_prop_, ctm_blob_); !r) {
      return r;
    }
  }
  if (gamma_blob_ != 0) {
    if (auto r = write(gamma_prop_, gamma_blob_); !r) {
      return r;
    }
  }
  return {};
}

void CrtcColorPipeline::clear_for_session_loss() noexcept {
  // Forget without destroy: kernel reclaims blobs on fd close.
  fd_ = -1;
  degamma_blob_ = 0;
  ctm_blob_ = 0;
  gamma_blob_ = 0;
}

}  // namespace drm::display
