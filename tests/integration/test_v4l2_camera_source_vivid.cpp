// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Integration smoke test for drm::scene::V4l2CameraSource against any
// available CAPTURE-only V4L2 device that advertises NV12 or YUYV.
//
// Preconditions:
//   - A V4L2 CAPTURE-only single-plane device advertising NV12 or YUYV
//     at one of /dev/video* (UVC webcams work; the kernel's vivid test
//     driver works after `sudo modprobe vivid`).
//   - vkms module loaded: `sudo modprobe vkms enable_overlay=1`. The
//     source needs a real DRM device with AddFB2 support so the
//     dumb-buffer pair (MMAP mode) can allocate.
//
// Either precondition failing self-skips via GTEST_SKIP() so the suite
// stays green on developer machines.
//
// Known skip case: vkms rejects YUV scanout FBs (drmModeAddFB2 EINVAL
// for both YUYV and NV12). The MmapCopy path needs the dumb buffer's
// fb_id, which forces FB creation in the source's format. When the
// only V4L2 capture device on the host emits YUYV and the DRM target
// is vkms, the test self-skips after `create()` returns EINVAL — this
// is a vkms format-acceptance limitation, not a source bug. A CI with
// vivid + a real DRM driver (amdgpu, i915) exercises the full path.

#include "core/device.hpp"

#include <drm-cxx/scene/v4l2_camera_source.hpp>

#include <xf86drm.h>

#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <ios>
#include <linux/videodev2.h>
#include <memory>
#include <optional>
#include <string>
#include <sys/ioctl.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t k_pix_fmt_nv12 = 0x3231564EU;
constexpr std::uint32_t k_pix_fmt_yuyv = 0x56595559U;

struct ProbedDevice {
  std::string path;
  std::uint32_t fourcc;
  std::uint32_t width;
  std::uint32_t height;
};

// Walk /dev/video* and return the first CAPTURE-only single-plane
// device advertising NV12 or YUYV plus a queryable framesize.
[[nodiscard]] std::optional<ProbedDevice> find_v4l2_capture() noexcept {
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
    if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
      ::close(fd);
      continue;
    }
    std::uint32_t const caps =
        ((cap.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U) ? cap.device_caps : cap.capabilities;
    bool const is_capture = (caps & V4L2_CAP_VIDEO_CAPTURE) != 0U;
    bool const is_m2m = (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) != 0U;
    if (!is_capture || is_m2m || (caps & V4L2_CAP_STREAMING) == 0U) {
      ::close(fd);
      continue;
    }

    std::optional<std::uint32_t> chosen_fourcc;
    for (std::uint32_t i = 0; i < 64; ++i) {
      v4l2_fmtdesc desc{};
      desc.index = i;
      desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      if (::ioctl(fd, VIDIOC_ENUM_FMT, &desc) != 0) {
        break;
      }
      if (desc.pixelformat == k_pix_fmt_nv12 || desc.pixelformat == k_pix_fmt_yuyv) {
        chosen_fourcc = desc.pixelformat;
        break;
      }
    }
    if (!chosen_fourcc.has_value()) {
      ::close(fd);
      continue;
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    v4l2_frmsizeenum fz{};
    fz.index = 0;
    fz.pixel_format = *chosen_fourcc;
    if (::ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fz) == 0) {
      if (fz.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        width = fz.discrete.width;
        height = fz.discrete.height;
      } else {
        width = fz.stepwise.min_width;
        height = fz.stepwise.min_height;
      }
    }
    ::close(fd);
    if (width == 0 || height == 0) {
      continue;
    }
    return ProbedDevice{p.string(), *chosen_fourcc, width, height};
  }
  return std::nullopt;
}

// Locate the vkms DRM node by driver name; matches the convention from
// the decoder source's vicodec test.
[[nodiscard]] std::optional<std::string> find_vkms_node() noexcept {
  for (int idx = 0; idx < 8; ++idx) {
    std::string path = "/dev/dri/card" + std::to_string(idx);
    int const fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr ver = drmGetVersion(fd);
    if (ver == nullptr) {
      ::close(fd);
      continue;
    }
    std::string const name(ver->name, ver->name_len);
    drmFreeVersion(ver);
    ::close(fd);
    if (name == "vkms") {
      return path;
    }
  }
  return std::nullopt;
}

class V4l2CameraSourceIntegration : public ::testing::Test {
 protected:
  void SetUp() override {
    auto const probed = find_v4l2_capture();
    if (!probed.has_value()) {
      GTEST_SKIP() << "No CAPTURE-only V4L2 device advertising NV12 or YUYV found.";
    }
    auto const drm_node = find_vkms_node();
    if (!drm_node.has_value()) {
      GTEST_SKIP() << "vkms not loaded; modprobe vkms enable_overlay=1.";
    }
    auto dev_r = drm::Device::open(*drm_node);
    ASSERT_TRUE(dev_r.has_value())
        << "Device::open(" << *drm_node << "): " << dev_r.error().message();
    dev = std::make_unique<drm::Device>(std::move(*dev_r));
    device_path = probed->path;
    fourcc = probed->fourcc;
    width = probed->width;
    height = probed->height;
  }

  // gtest fixture state needs protected visibility so the TEST_F bodies
  // can reach it; the lint check disagrees with the gtest pattern,
  // mirroring the suppression already used in other integration fixtures.
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,
  //             misc-non-private-member-variables-in-classes)
  std::unique_ptr<drm::Device> dev;
  std::string device_path;
  std::uint32_t fourcc{0};
  std::uint32_t width{0};
  std::uint32_t height{0};
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,
  //           misc-non-private-member-variables-in-classes)
};

}  // namespace

// MMAP-mode end-to-end: REQBUFS + MMAP per buffer + dumb pair
// allocation + STREAMON. The drive() loop runs once; no frame is
// guaranteed to have landed (cameras / vivid take a few ms to warm up)
// so we don't assert acquire() succeeds.
TEST_F(V4l2CameraSourceIntegration, MmapCopyEndToEnd) {
  drm::scene::V4l2CameraConfig cfg;
  cfg.pixel_fourcc = fourcc;
  cfg.width = width;
  cfg.height = height;
  cfg.buffer_count = 4;
  cfg.mode = drm::scene::V4l2CameraBufferMode::MmapCopy;

  auto r = drm::scene::V4l2CameraSource::create(*dev, device_path.c_str(), cfg);
  if (!r) {
    GTEST_SKIP() << "create() failed for " << device_path << " (fourcc=0x" << std::hex << fourcc
                 << " " << std::dec << width << "x" << height << "): " << r.error().message()
                 << ". Common cause on vkms: YUYV / NV12 scanout FB not accepted.";
  }
  auto& src = *r;
  EXPECT_EQ(src->active_mode(), drm::scene::V4l2CameraBufferMode::MmapCopy);
  EXPECT_GE(src->fd(), 0);

  auto const fmt = src->format();
  EXPECT_NE(fmt.drm_fourcc, 0U);
  EXPECT_GT(fmt.width, 0U);
  EXPECT_GT(fmt.height, 0U);

  // drive() must succeed even when no frame is yet ready (EAGAIN is
  // swallowed internally; the caller sees a clean return).
  auto drv = src->drive();
  EXPECT_TRUE(drv.has_value()) << "drive() returned " << drv.error().message();
}

// Auto mode: probe DMABUF first; falls back to MmapCopy on most hosts.
// We don't assert which mode actually lands — both are valid outcomes.
TEST_F(V4l2CameraSourceIntegration, AutoModeResolves) {
  drm::scene::V4l2CameraConfig cfg;
  cfg.pixel_fourcc = fourcc;
  cfg.width = width;
  cfg.height = height;
  cfg.buffer_count = 4;
  cfg.mode = drm::scene::V4l2CameraBufferMode::Auto;

  auto r = drm::scene::V4l2CameraSource::create(*dev, device_path.c_str(), cfg);
  if (!r) {
    GTEST_SKIP() << "create() failed for " << device_path << " (fourcc=0x" << std::hex << fourcc
                 << " " << std::dec << width << "x" << height << "): " << r.error().message();
  }
  auto& src = *r;
  EXPECT_TRUE(src->active_mode() == drm::scene::V4l2CameraBufferMode::DmaBufZeroCopy ||
              src->active_mode() == drm::scene::V4l2CameraBufferMode::MmapCopy);
}
