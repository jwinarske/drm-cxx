// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "v4l2_decoder_source.hpp"

#include "buffer_source.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <memory>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>

namespace drm::scene {

namespace {

constexpr std::uint32_t k_min_buffers = 2U;
constexpr std::uint32_t k_max_buffers = 32U;

[[nodiscard]] std::error_code validate_config(const char* device_path,
                                              const V4l2DecoderConfig& cfg) noexcept {
  if (device_path == nullptr || std::strlen(device_path) == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.codec_fourcc == 0U || cfg.capture_fourcc == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.coded_width == 0U || cfg.coded_height == 0U) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.output_buffer_count < k_min_buffers || cfg.output_buffer_count > k_max_buffers) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  if (cfg.capture_buffer_count < k_min_buffers || cfg.capture_buffer_count > k_max_buffers) {
    return std::make_error_code(std::errc::invalid_argument);
  }
  return {};
}

// V4L2 ioctls can return EINTR if a signal interrupts the call before
// the kernel commits any state; the canonical idiom is to retry the
// ioctl until it succeeds, fails with a non-EINTR errno, or we give
// up. The retry budget is small because legitimate EINTR loops resolve
// in one or two iterations; a runaway here would be a kernel bug.
constexpr int k_max_ioctl_retries = 8;

[[nodiscard]] int xioctl(int fd, unsigned long request, void* arg) noexcept {
  for (int attempt = 0; attempt < k_max_ioctl_retries; ++attempt) {
    int const r = ::ioctl(fd, request, arg);
    if (r >= 0) {
      return 0;
    }
    if (errno != EINTR) {
      return errno;
    }
  }
  return EINTR;
}

// errno values from POSIX-defined system calls map 1:1 onto std::errc;
// using generic_category here keeps assertions of the form
// `EXPECT_EQ(r.error(), std::make_error_code(std::errc::foo))` working,
// which is the convention used elsewhere in the test suite.
[[nodiscard]] std::error_code make_errno(int err) noexcept {
  return {err, std::generic_category()};
}

[[nodiscard]] std::error_code subscribe_event(int fd, std::uint32_t event_type) noexcept {
  v4l2_event_subscription sub{};
  sub.type = event_type;
  if (int const e = xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub); e != 0) {
    return make_errno(e);
  }
  return {};
}

}  // namespace

// Pimpl carrying the full V4L2 + DRM-side state. Hidden so the public
// header doesn't need <linux/videodev2.h>. Step 1 populates v4l2_fd,
// is_mplane, and capture_format; later steps add the OUTPUT / CAPTURE
// buffer rings, the per-CAPTURE-buffer fb_id table, and the dup'd DRM
// fd that imports them.
struct V4l2DecoderSource::Impl {
  int v4l2_fd{-1};
  V4l2DecoderConfig cfg{};
  bool is_mplane{false};
  SourceFormat capture_format{};
};

V4l2DecoderSource::V4l2DecoderSource() : impl_(std::make_unique<Impl>()) {}

V4l2DecoderSource::~V4l2DecoderSource() {
  if (impl_ && impl_->v4l2_fd >= 0) {
    ::close(impl_->v4l2_fd);
    impl_->v4l2_fd = -1;
  }
}

drm::expected<std::unique_ptr<V4l2DecoderSource>, std::error_code> V4l2DecoderSource::create(
    const drm::Device& /*dev*/, const char* device_path, const V4l2DecoderConfig& cfg) {
  if (auto ec = validate_config(device_path, cfg); ec) {
    return drm::unexpected<std::error_code>(ec);
  }

  // O_NONBLOCK so drive() can drain DQBUF/DQEVENT without stalling the
  // caller's commit thread; CAPTURE EAGAIN is the in-band "no frame
  // ready" signal that LayerScene already understands as flow control.
  int const fd = ::open(device_path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    return drm::unexpected<std::error_code>(make_errno(errno));
  }

  auto fail = [fd](std::error_code ec) {
    ::close(fd);
    return drm::unexpected<std::error_code>(ec);
  };

  v4l2_capability cap{};
  if (int const e = xioctl(fd, VIDIOC_QUERYCAP, &cap); e != 0) {
    // ENOTTY here means the fd isn't a V4L2 device at all (e.g. caller
    // pointed at /dev/null). Surface as "not supported" so callers can
    // distinguish it from path-not-found.
    return fail(e == ENOTTY ? std::make_error_code(std::errc::not_supported) : make_errno(e));
  }

  // Per-device caps live in `device_caps` when V4L2_CAP_DEVICE_CAPS is
  // set in the overall `capabilities` mask; otherwise the union is
  // reported in `capabilities`. M2M decoders advertise either the
  // single-plane (V4L2_CAP_VIDEO_M2M) or multi-plane
  // (V4L2_CAP_VIDEO_M2M_MPLANE) flavor; both also need
  // V4L2_CAP_STREAMING because we use the mmap+QBUF path.
  std::uint32_t const device_caps =
      ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) ? cap.device_caps : cap.capabilities;
  bool const is_mplane = (device_caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0U;
  bool const is_single = (device_caps & V4L2_CAP_VIDEO_M2M) != 0U;
  if (!is_mplane && !is_single) {
    return fail(std::make_error_code(std::errc::not_supported));
  }
  if ((device_caps & V4L2_CAP_STREAMING) == 0U) {
    return fail(std::make_error_code(std::errc::not_supported));
  }

  // S_FMT on OUTPUT — the codec the decoder is being asked to consume.
  // The kernel echoes the negotiated values back; drivers may snap
  // sizeimage up to a hardware-imposed minimum, but the fourcc and
  // dimensions stay as requested for compliant decoders.
  v4l2_format out_fmt{};
  if (is_mplane) {
    out_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    out_fmt.fmt.pix_mp.pixelformat = cfg.codec_fourcc;
    out_fmt.fmt.pix_mp.width = cfg.coded_width;
    out_fmt.fmt.pix_mp.height = cfg.coded_height;
    out_fmt.fmt.pix_mp.num_planes = 1;
    if (cfg.output_buffer_size > 0) {
      out_fmt.fmt.pix_mp.plane_fmt[0].sizeimage =
          static_cast<std::uint32_t>(cfg.output_buffer_size);
    }
  } else {
    out_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    out_fmt.fmt.pix.pixelformat = cfg.codec_fourcc;
    out_fmt.fmt.pix.width = cfg.coded_width;
    out_fmt.fmt.pix.height = cfg.coded_height;
    if (cfg.output_buffer_size > 0) {
      out_fmt.fmt.pix.sizeimage = static_cast<std::uint32_t>(cfg.output_buffer_size);
    }
  }
  if (int const e = xioctl(fd, VIDIOC_S_FMT, &out_fmt); e != 0) {
    return fail(make_errno(e));
  }

  // S_FMT on CAPTURE — the decoded format the source will hand to the
  // scene. Kernel echoes back negotiated width/height and may inject a
  // bytesperline / sizeimage; we cache the post-S_FMT values into
  // capture_format so format() reflects what the decoder will actually
  // produce, not what the caller asked for.
  v4l2_format cap_fmt{};
  std::uint32_t cap_w = 0;
  std::uint32_t cap_h = 0;
  if (is_mplane) {
    cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    cap_fmt.fmt.pix_mp.pixelformat = cfg.capture_fourcc;
    cap_fmt.fmt.pix_mp.width = cfg.coded_width;
    cap_fmt.fmt.pix_mp.height = cfg.coded_height;
  } else {
    cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cap_fmt.fmt.pix.pixelformat = cfg.capture_fourcc;
    cap_fmt.fmt.pix.width = cfg.coded_width;
    cap_fmt.fmt.pix.height = cfg.coded_height;
  }
  if (int const e = xioctl(fd, VIDIOC_S_FMT, &cap_fmt); e != 0) {
    return fail(make_errno(e));
  }
  if (is_mplane) {
    cap_w = cap_fmt.fmt.pix_mp.width;
    cap_h = cap_fmt.fmt.pix_mp.height;
  } else {
    cap_w = cap_fmt.fmt.pix.width;
    cap_h = cap_fmt.fmt.pix.height;
  }

  // Subscribe to events the decoder fires asynchronously: SOURCE_CHANGE
  // when the bitstream's resolution differs from the OUTPUT-side
  // configuration (drive() surfaces it as operation_canceled so the
  // caller destroys + recreates), and EOS when the decoder has flushed
  // the last frame out of the CAPTURE queue.
  if (auto ec = subscribe_event(fd, V4L2_EVENT_SOURCE_CHANGE); ec) {
    return fail(ec);
  }
  if (auto ec = subscribe_event(fd, V4L2_EVENT_EOS); ec) {
    return fail(ec);
  }

  std::unique_ptr<V4l2DecoderSource> src(new V4l2DecoderSource());
  src->impl_->v4l2_fd = fd;
  src->impl_->cfg = cfg;
  src->impl_->is_mplane = is_mplane;
  // V4L2 surfaces in v1 are LINEAR (DRM_FORMAT_MOD_LINEAR == 0).
  src->impl_->capture_format = SourceFormat{cfg.capture_fourcc, 0, cap_w, cap_h};
  return src;
}

int V4l2DecoderSource::fd() const noexcept {
  return impl_ ? impl_->v4l2_fd : -1;
}

drm::expected<void, std::error_code> V4l2DecoderSource::drive() noexcept {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::function_not_supported));
}

drm::expected<void, std::error_code> V4l2DecoderSource::submit_bitstream(
    drm::span<const std::uint8_t> coded, std::uint64_t /*timestamp_ns*/) {
  if (!impl_ || coded.empty()) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::function_not_supported));
}

drm::expected<AcquiredBuffer, std::error_code> V4l2DecoderSource::acquire() {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::function_not_supported));
}

void V4l2DecoderSource::release(AcquiredBuffer /*acquired*/) noexcept {
  // No-op until the V4L2 CAPTURE re-queue path lands.
}

SourceFormat V4l2DecoderSource::format() const noexcept {
  return impl_ ? impl_->capture_format : SourceFormat{};
}

void V4l2DecoderSource::on_session_paused() noexcept {
  // No-op until DRM-side state (FB IDs, GEM handles) is held.
}

drm::expected<void, std::error_code> V4l2DecoderSource::on_session_resumed(
    const drm::Device& /*new_dev*/) {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  return {};
}

}  // namespace drm::scene