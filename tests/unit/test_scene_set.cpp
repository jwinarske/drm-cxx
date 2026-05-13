// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::scene::SceneSet covering the contract visible
// without a live KMS device:
//   - Empty SceneSet construction + no-op commit/test.
//   - scene_count / scene(index) bounds behavior.
//
// Multi-CRTC kernel commit coverage belongs in a hardware-backed
// integration test (planned: configfs-spawned vkms with two virtual
// outputs, or a bare-TTY amdgpu run mirroring multi_crtc_probe).

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/scene_set.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

namespace {

// /dev/null wrapped as a non-owning Device. SceneSet::create only
// stores the reference, and the empty-scene-set commit path returns
// before touching the fd — so no DRM ioctl ever reaches the null fd.
class NullFd {
 public:
  NullFd() noexcept : fd_(::open("/dev/null", O_RDWR | O_CLOEXEC)) {}
  NullFd(const NullFd&) = delete;
  NullFd& operator=(const NullFd&) = delete;
  ~NullFd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  [[nodiscard]] int fd() const noexcept { return fd_; }

 private:
  int fd_{-1};
};

}  // namespace

TEST(SceneSetCtor, EmptyConstructionSucceeds) {
  const NullFd nfd;
  ASSERT_GE(nfd.fd(), 0) << "/dev/null open failed: " << std::strerror(errno);
  auto dev = drm::Device::from_fd(nfd.fd());
  auto set = drm::scene::SceneSet::create(dev, {});
  ASSERT_TRUE(set.has_value()) << set.error().message();
  EXPECT_EQ((*set)->scene_count(), 0U);
  EXPECT_EQ((*set)->scene(0), nullptr);
}

TEST(SceneSetCommit, EmptySetCommitReturnsEmptyReports) {
  const NullFd nfd;
  ASSERT_GE(nfd.fd(), 0);
  auto dev = drm::Device::from_fd(nfd.fd());
  auto set = drm::scene::SceneSet::create(dev, {});
  ASSERT_TRUE(set.has_value());

  auto reports = (*set)->commit();
  ASSERT_TRUE(reports.has_value()) << reports.error().message();
  EXPECT_TRUE(reports->empty());
}

TEST(SceneSetCommit, EmptySetTestReturnsEmptyReports) {
  const NullFd nfd;
  ASSERT_GE(nfd.fd(), 0);
  auto dev = drm::Device::from_fd(nfd.fd());
  auto set = drm::scene::SceneSet::create(dev, {});
  ASSERT_TRUE(set.has_value());

  auto reports = (*set)->test();
  ASSERT_TRUE(reports.has_value()) << reports.error().message();
  EXPECT_TRUE(reports->empty());
}

TEST(SceneSetAccess, OutOfRangeIndexReturnsNull) {
  const NullFd nfd;
  ASSERT_GE(nfd.fd(), 0);
  auto dev = drm::Device::from_fd(nfd.fd());
  auto set = drm::scene::SceneSet::create(dev, {});
  ASSERT_TRUE(set.has_value());
  EXPECT_EQ((*set)->scene(0), nullptr);
  EXPECT_EQ((*set)->scene(42), nullptr);
}
