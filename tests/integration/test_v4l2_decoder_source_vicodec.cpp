// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Integration smoke test for drm::scene::V4l2DecoderSource against the
// kernel's V4L2 virtual codec test driver (vicodec).
//
// Preconditions:
//   - vicodec module loaded: `sudo modprobe vicodec`. Provides the
//     stateful FWHT codec endpoint we drive here, plus an encoder and
//     a stateless decoder that this test ignores.
//   - vkms module loaded: `sudo modprobe vkms enable_overlay=1`. The
//     V4l2DecoderSource needs a real DRM device with addFB2 support
//     so the per-CAPTURE-buffer prime-import + drmModeAddFB2 path can
//     run; vkms is the CI-friendly target.
//
// Either preconditions failing self-skips via GTEST_SKIP() so the
// integration suite stays green on developer machines.
//
// What this exercises:
//   * Probe + open of vicodec's stateful decoder via the source's
//     full create() path: validation, ::open(O_RDWR|O_NONBLOCK),
//     VIDIOC_QUERYCAP, M2M / MPLANE detection, S_FMT on both queues,
//     event subscription, REQBUFS + MMAP + EXPBUF on CAPTURE,
//     drmPrimeFDToHandle + drmModeAddFB2 per buffer, REQBUFS + MMAP
//     on OUTPUT, initial CAPTURE QBUF, STREAMON CAPTURE.
//   * format() returns the post-S_FMT echoed dimensions.
//   * fd() returns the V4L2 fd open for caller poll() loops.
//   * submit_bitstream + drive loop: every method completes without
//     surfacing an unexpected error. We don't validate that a frame
//     actually decoded -- crafting a valid FWHT bitstream from scratch
//     is out of scope for a plumbing test, and vicodec accepts the
//     QBUF regardless of payload contents (the decode failure surfaces
//     later or just sits waiting for more data).
//
// What this does NOT exercise:
//   * Successful FWHT decode round-trip with acquire() yielding a
//     frame. That requires either embedding a known-good FWHT sample
//     or running vicodec's encoder side first; both are deferred.
//     Hardware-validated separately on a real V4L2 stateful decoder
//     (rpivid / rkvdec / hantro / coda).
//   * SOURCE_CHANGE handling. vicodec doesn't fire it for arbitrary
//     bitstream input; exercising it requires the encoder path to
//     produce a frame at a different resolution.

#include "core/device.hpp"

#include <drm-cxx/scene/v4l2_decoder_source.hpp>

#include <xf86drm.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <linux/videodev2.h>
#include <memory>
#include <optional>
#include <string>
#include <sys/ioctl.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::chrono::milliseconds k_drive_total_budget{500};
constexpr std::chrono::milliseconds k_drive_poll_interval{20};

// V4L2_PIX_FMT_FWHT = v4l2_fourcc('F','W','H','T'). vicodec uses this
// as its codec format on the OUTPUT (stateful decoder input) side.
constexpr std::uint32_t k_fwht_fourcc = 0x54485746U;

// V4L2_PIX_FMT_NV12 = v4l2_fourcc('N','V','1','2'). vicodec advertises
// NV12 on CAPTURE; the source's derive_drm_plane_layout helper has a
// dedicated single-V4L2-plane -> 2-DRM-plane path for NV12, so this
// is the format that exercises the most of the source's plumbing.
constexpr std::uint32_t k_nv12_fourcc = 0x3231564EU;

// vicodec exposes three /dev/video* endpoints, all sharing the card
// name "vicodec": the stateful encoder, the stateful decoder, and
// the stateless decoder. We can't distinguish them from QUERYCAP
// alone -- need to enumerate OUTPUT-side formats and check for
// V4L2_PIX_FMT_FWHT. Only the stateful decoder advertises FWHT as
// an OUTPUT (input-to-decompression) format; the encoder has FWHT
// on CAPTURE, and the stateless decoder uses V4L2_PIX_FMT_FWHT_STATELESS
// (fourcc 'SFWH').
[[nodiscard]] bool fd_advertises_fwht_output(int fd, bool is_mplane) noexcept {
  std::uint32_t const type =
      is_mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
  for (std::uint32_t i = 0; i < 64; ++i) {
    v4l2_fmtdesc desc{};
    desc.index = i;
    desc.type = type;
    if (::ioctl(fd, VIDIOC_ENUM_FMT, &desc) < 0) {
      return false;
    }
    if (desc.pixelformat == k_fwht_fourcc) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool is_vicodec_stateful_decoder(int fd, const v4l2_capability& cap) noexcept {
  std::string const card(reinterpret_cast<const char*>(cap.card));  // NOLINT
  if (card.find("vicodec") == std::string::npos) {
    return false;
  }
  std::uint32_t const caps =
      ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) ? cap.device_caps : cap.capabilities;
  bool const is_mplane = (caps & V4L2_CAP_VIDEO_M2M_MPLANE) != 0U;
  bool const is_single = (caps & V4L2_CAP_VIDEO_M2M) != 0U;
  bool const streaming = (caps & V4L2_CAP_STREAMING) != 0U;
  if ((!is_mplane && !is_single) || !streaming) {
    return false;
  }
  return fd_advertises_fwht_output(fd, is_mplane);
}

[[nodiscard]] std::optional<std::string> find_vicodec_decoder() {
  std::error_code ec;
  for (auto const& entry : fs::directory_iterator("/dev", ec)) {
    auto const& p = entry.path();
    std::string const name = p.filename().string();
    if (name.rfind("video", 0) != 0) {
      continue;
    }
    int const fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
      continue;
    }
    v4l2_capability cap{};
    int const rc = ::ioctl(fd, VIDIOC_QUERYCAP, &cap);
    bool const matches = (rc == 0) && is_vicodec_stateful_decoder(fd, cap);
    ::close(fd);
    if (matches) {
      return p.string();
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> find_vkms_node() {
  std::error_code ec;
  for (auto const& entry : fs::directory_iterator("/dev/dri", ec)) {
    auto const& p = entry.path();
    std::string const name = p.filename().string();
    if (name.rfind("card", 0) != 0) {
      continue;
    }
    int const fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr v = drmGetVersion(fd);
    bool const is_vkms =
        (v != nullptr) && (v->name != nullptr) && std::strcmp(v->name, "vkms") == 0;
    if (v != nullptr) {
      drmFreeVersion(v);
    }
    ::close(fd);
    if (is_vkms) {
      return p.string();
    }
  }
  return std::nullopt;
}

drm::scene::V4l2DecoderConfig vicodec_config() noexcept {
  drm::scene::V4l2DecoderConfig cfg;
  cfg.codec_fourcc = k_fwht_fourcc;
  cfg.capture_fourcc = k_nv12_fourcc;
  cfg.coded_width = 320;
  cfg.coded_height = 240;
  cfg.output_buffer_count = 4;
  cfg.capture_buffer_count = 4;
  return cfg;
}

class VicodecFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    auto const v4l2_path = find_vicodec_decoder();
    if (!v4l2_path.has_value()) {
      GTEST_SKIP() << "vicodec stateful decoder not found "
                      "-- modprobe vicodec to enable";
    }
    auto const drm_node = find_vkms_node();
    if (!drm_node.has_value()) {
      GTEST_SKIP() << "vkms not loaded -- modprobe vkms enable_overlay=1 to enable";
    }
    auto dev_r = drm::Device::open(*drm_node);
    ASSERT_TRUE(dev_r.has_value())
        << "Device::open(" << *drm_node << "): " << dev_r.error().message();
    dev = std::make_unique<drm::Device>(std::move(*dev_r));
    decoder_path = *v4l2_path;
  }

  // gtest fixture state needs protected visibility so the TEST_F
  // bodies can reach it; the lint check disagrees with the gtest
  // pattern, mirroring the suppression already used in other
  // integration fixtures.
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,
  //             misc-non-private-member-variables-in-classes)
  std::unique_ptr<drm::Device> dev;
  std::string decoder_path;
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,
  //           misc-non-private-member-variables-in-classes)
};

}  // namespace

TEST_F(VicodecFixture, CreateNegotiatesFormat) {
  auto const cfg = vicodec_config();
  auto src_r = drm::scene::V4l2DecoderSource::create(*dev, decoder_path.c_str(), cfg);
  ASSERT_TRUE(src_r.has_value()) << "create: " << src_r.error().message();
  auto& src = *src_r;

  EXPECT_GE(src->fd(), 0);
  auto const fmt = src->format();
  EXPECT_EQ(fmt.drm_fourcc, k_nv12_fourcc);
  // vicodec rounds dimensions up to its block size; >= the requested
  // dims is the right invariant. Width / height of 0 would mean the
  // S_FMT echo wasn't captured.
  EXPECT_GE(fmt.width, cfg.coded_width);
  EXPECT_GE(fmt.height, cfg.coded_height);
  EXPECT_EQ(fmt.modifier, 0U);  // DRM_FORMAT_MOD_LINEAR
}

TEST_F(VicodecFixture, SubmitAndDriveDoesNotError) {
  auto const cfg = vicodec_config();
  auto src_r = drm::scene::V4l2DecoderSource::create(*dev, decoder_path.c_str(), cfg);
  ASSERT_TRUE(src_r.has_value()) << "create: " << src_r.error().message();
  auto& src = *src_r;

  // Submit a small zeroed bitstream chunk. vicodec's QBUF accepts the
  // payload regardless of FWHT validity; the actual decode either
  // produces a degenerate frame, fires SOURCE_CHANGE, or sits waiting
  // for more data. All three outcomes are acceptable for a plumbing
  // smoke test.
  std::vector<std::uint8_t> chunk(2048, 0);
  auto const submit_r = src->submit_bitstream(chunk, 0);
  ASSERT_TRUE(submit_r.has_value()) << "submit_bitstream: " << submit_r.error().message();

  // Spin drive() with a short interval. Allowed terminal states:
  //   * acquire() succeeds with non-zero fb_id (decode produced
  //     something usable),
  //   * drive() returns errc::operation_canceled (SOURCE_CHANGE
  //     fired -- expected for malformed bitstream),
  //   * drive() returns ok and acquire() returns
  //     resource_unavailable_try_again throughout the budget (the
  //     decoder is parked waiting for more data).
  // Any other drive() error is a real failure.
  using clock = std::chrono::steady_clock;
  auto const deadline = clock::now() + k_drive_total_budget;
  bool acquired = false;
  bool source_change = false;
  while (clock::now() < deadline && !acquired) {
    auto const drive_r = src->drive();
    if (!drive_r.has_value()) {
      // operation_canceled is the only acceptable error from drive().
      ASSERT_EQ(drive_r.error(), std::make_error_code(std::errc::operation_canceled))
          << "drive: " << drive_r.error().message();
      source_change = true;
      break;
    }
    auto acq_r = src->acquire();
    if (acq_r.has_value()) {
      EXPECT_NE(acq_r->fb_id, 0U);
      src->release(*acq_r);
      acquired = true;
      break;
    }
    // Acquire returning anything other than try_again here is a real
    // failure (busy / cancel / invalid).
    if (acq_r.error() != std::make_error_code(std::errc::resource_unavailable_try_again) &&
        acq_r.error() != std::make_error_code(std::errc::operation_canceled)) {
      FAIL() << "acquire: " << acq_r.error().message();
    }
    if (acq_r.error() == std::make_error_code(std::errc::operation_canceled)) {
      source_change = true;
      break;
    }
    std::this_thread::sleep_for(k_drive_poll_interval);
  }
  // The test is a plumbing smoke: passing means "no method surfaced
  // an unexpected error." Acquire success or source-change cancel
  // are interesting outcomes but neither is required. Annotate the
  // outcome so failing CI runs are easier to diagnose.
  if (!acquired && !source_change) {
    SUCCEED() << "decoder parked without producing a frame -- expected for "
                 "zero-payload bitstream against vicodec";
  }
}
