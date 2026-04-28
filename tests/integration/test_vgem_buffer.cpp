// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Integration tests for the buffer-only DRM surface (drm::dumb::Buffer +
// drm::BufferMapping + DMA-BUF prime export) against the kernel's virtual
// GEM driver (VGEM).
//
// Preconditions:
//   - VGEM module loaded:  sudo modprobe vgem
//   - read/write access to /dev/dri/card* exposing the vgem driver.
//
// VGEM exposes a buffer-only DRM device — it has GEM allocation,
// CREATE_DUMB / MAP_DUMB ioctls, DMA-BUF prime export/import, and the
// sync_file primitives, but no planes, no CRTCs, no connectors, no
// modesetting. That makes it the right target for shaking out the
// buffer-side surface (allocation lifecycle, mmap coherence, prime
// round-trips) without the modesetting machinery getting in the way.
// Display-side coverage lives in test_capture_vkms / test_layer_scene_*
// against VKMS.
//
// If VGEM isn't loaded the test self-skips via GTEST_SKIP() so the suite
// stays green on developer machines that haven't modprobed it.
//
// Note on capability probing: VGEM is *not* a DRIVER_MODESET driver, and
// the kernel's drm_getcap rejects every non-render-only capability query
// (including DRM_CAP_DUMB_BUFFER) with EOPNOTSUPP for non-MODESET drivers.
// CREATE_DUMB itself still works — VGEM registers a dumb_create handler
// regardless. So the fixture probes by *attempt*: a tiny dumb allocation
// is tried in SetUp; the suite skips on EOPNOTSUPP/ENOTTY (kernels where
// vgem genuinely lacks the path) and propagates anything else as a real
// failure.

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/dumb/buffer.hpp>

#include <drm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>  // NOLINT(misc-include-cleaner) — canonical home of SEEK_END
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <sys/ioctl.h>
#include <sys/types.h>  // NOLINT(misc-include-cleaner) — canonical home of off_t
#include <system_error>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;

namespace {

// Locate /dev/dri/cardN whose driver name is "vgem".
std::optional<std::string> find_vgem_node() {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/dev/dri", ec)) {
    const auto& p = entry.path();
    if (const std::string name = p.filename().string(); name.rfind("card", 0) != 0) {
      continue;
    }
    const int fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr v = drmGetVersion(fd);
    const bool is_vgem =
        (v != nullptr) && (v->name != nullptr) && (std::strcmp(v->name, "vgem") == 0);
    if (v != nullptr) {
      drmFreeVersion(v);
    }
    ::close(fd);
    if (is_vgem) {
      return p.string();
    }
  }
  return std::nullopt;
}

drm::dumb::Config buffer_only_config(std::uint32_t w = 64, std::uint32_t h = 64) {
  drm::dumb::Config cfg;
  cfg.width = w;
  cfg.height = h;
  cfg.drm_format = DRM_FORMAT_ARGB8888;
  cfg.bpp = 32;
  // VGEM has no modesetting, so drmModeAddFB2 would fail with EINVAL.
  // Buffer-only callers (cursor renderer's legacy SetCursor path,
  // foreign-source mocks) are the genuine consumers here anyway.
  cfg.add_fb = false;
  return cfg;
}

// Errno values that mean "this kernel/driver build doesn't carry a dumb
// path at all" — i.e. legitimate skip reasons. Anything else is a real
// failure that should propagate to the test report.
bool errc_means_no_dumb_path(const std::error_code& ec) {
  const int v = ec.value();
  return v == EOPNOTSUPP || v == ENOTTY || v == ENOSYS;
}

}  // namespace

class VgemBufferTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto node = find_vgem_node();
    if (!node) {
      GTEST_SKIP() << "VGEM not loaded — `sudo modprobe vgem` to enable this test";
    }
    node_ = std::move(*node);

    auto dev_r = drm::Device::open(node_);
    ASSERT_TRUE(dev_r.has_value()) << dev_r.error().message();
    dev_ = std::make_unique<drm::Device>(std::move(*dev_r));

    // Probe the dumb path with a throwaway 8x8 allocation. EOPNOTSUPP /
    // ENOTTY here mean the kernel lacks the dumb_create handler on this
    // vgem build (uncommon, but possible on stripped configs); skip the
    // suite. Any other error is a real failure.
    auto probe = drm::dumb::Buffer::create(*dev_, buffer_only_config(8, 8));
    if (!probe.has_value()) {
      if (errc_means_no_dumb_path(probe.error())) {
        GTEST_SKIP() << "vgem build lacks dumb-buffer path: " << probe.error().message();
      }
      FAIL() << "probe Buffer::create failed: " << probe.error().message();
    }
  }

  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  std::string node_;
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  std::unique_ptr<drm::Device> dev_;
};

TEST_F(VgemBufferTest, AllocateThenDestroyRoundTrips) {
  auto buf_r = drm::dumb::Buffer::create(*dev_, buffer_only_config(128, 64));
  ASSERT_TRUE(buf_r.has_value()) << buf_r.error().message();
  const auto& buf = *buf_r;

  EXPECT_FALSE(buf.empty());
  EXPECT_EQ(buf.width(), 128U);
  EXPECT_EQ(buf.height(), 64U);
  EXPECT_GE(buf.stride(), 128U * 4U);
  EXPECT_GE(buf.size_bytes(), static_cast<std::size_t>(buf.stride()) * 64U);
  EXPECT_NE(buf.handle(), 0U);
  EXPECT_EQ(buf.fb_id(), 0U);  // add_fb=false
  EXPECT_NE(buf.data(), nullptr);
  // Destructor handles cleanup.
}

TEST_F(VgemBufferTest, MmapIsKernelBackedAndReadable) {
  auto buf_r = drm::dumb::Buffer::create(*dev_, buffer_only_config(32, 32));
  ASSERT_TRUE(buf_r.has_value()) << buf_r.error().message();
  auto& buf = *buf_r;

  // CREATE_DUMB zeros pages on allocation; the dumb::Buffer factory also
  // memsets the mapping. Verify both halves of that contract before we
  // start writing patterns.
  for (std::size_t i = 0; i < buf.size_bytes(); ++i) {
    ASSERT_EQ(buf.data()[i], 0U);
  }

  // Write a per-byte pattern through data() and read it back. If the
  // mapping wasn't truly the kernel's page (e.g. a stray COW), the
  // readback would diverge from what we wrote.
  for (std::size_t i = 0; i < buf.size_bytes(); ++i) {
    buf.data()[i] = static_cast<std::uint8_t>(i & 0xFFU);
  }
  for (std::size_t i = 0; i < buf.size_bytes(); ++i) {
    ASSERT_EQ(buf.data()[i], static_cast<std::uint8_t>(i & 0xFFU));
  }
}

TEST_F(VgemBufferTest, ScopedMappingReflectsBufferStorage) {
  auto buf_r = drm::dumb::Buffer::create(*dev_, buffer_only_config(16, 8));
  ASSERT_TRUE(buf_r.has_value()) << buf_r.error().message();
  auto& buf = *buf_r;

  {
    auto mapping = buf.map(drm::MapAccess::ReadWrite);
    ASSERT_FALSE(mapping.empty());
    EXPECT_EQ(mapping.width(), buf.width());
    EXPECT_EQ(mapping.height(), buf.height());
    EXPECT_EQ(mapping.stride(), buf.stride());
    EXPECT_EQ(mapping.access(), drm::MapAccess::ReadWrite);

    // Write the canonical 0xDEADBEEF marker through mapping.pixels(),
    // then verify it's visible through Buffer::data() — proves the
    // BufferMapping is a view onto the same kernel-backed storage and
    // not a copy. Cast away const because pixels() returns a const-byte
    // span; the mapping was acquired ReadWrite.
    auto* pixels = const_cast<std::uint8_t*>(
        mapping.pixels().data());  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    pixels[0] = 0xDEU;
    pixels[1] = 0xADU;
    pixels[2] = 0xBEU;
    pixels[3] = 0xEFU;
  }

  // Mapping went out of scope. For dumb buffers the unmap is a no-op
  // (storage is held for the buffer's full lifetime), so the bytes are
  // still observable through buf.data().
  EXPECT_EQ(buf.data()[0], 0xDEU);
  EXPECT_EQ(buf.data()[1], 0xADU);
  EXPECT_EQ(buf.data()[2], 0xBEU);
  EXPECT_EQ(buf.data()[3], 0xEFU);
}

TEST_F(VgemBufferTest, PrimeExportProducesValidDmaBufFd) {
  auto buf_r = drm::dumb::Buffer::create(*dev_, buffer_only_config(64, 64));
  ASSERT_TRUE(buf_r.has_value()) << buf_r.error().message();
  auto& buf = *buf_r;

  int dmabuf_fd = -1;
  const int rc = drmPrimeHandleToFD(dev_->fd(), buf.handle(), DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd);
  ASSERT_EQ(rc, 0) << "drmPrimeHandleToFD failed: " << std::strerror(errno);
  ASSERT_GE(dmabuf_fd, 0);

  // DMA-BUF backing size — lseek(SEEK_END) is the documented way to ask.
  // Must equal the buffer's allocation (rounded up to a page if the
  // kernel padded it; >= covers both).
  // NOLINTNEXTLINE(misc-include-cleaner) — off_t arrives via <sys/types.h>
  const off_t size = ::lseek(dmabuf_fd, 0, SEEK_END);
  EXPECT_GE(size, 0);
  EXPECT_GE(static_cast<std::size_t>(size), buf.size_bytes());

  ::close(dmabuf_fd);
}

TEST_F(VgemBufferTest, PrimeImportAcrossSeparateFdsYieldsFreshHandle) {
  // The fixture's dev_ is the producer. Open a second independent fd
  // against the same node for the consumer side — separate fds get
  // separate GEM handle namespaces, exactly the producer/consumer split
  // a foreign-buffer source models (producer holds one fd, the scene's
  // Device holds another).
  auto consumer_r = drm::Device::open(node_);
  ASSERT_TRUE(consumer_r.has_value()) << consumer_r.error().message();
  const auto& consumer = *consumer_r;

  auto buf_r = drm::dumb::Buffer::create(*dev_, buffer_only_config(32, 32));
  ASSERT_TRUE(buf_r.has_value()) << buf_r.error().message();
  auto& buf = *buf_r;

  int dmabuf_fd = -1;
  ASSERT_EQ(drmPrimeHandleToFD(dev_->fd(), buf.handle(), DRM_CLOEXEC | DRM_RDWR, &dmabuf_fd), 0)
      << std::strerror(errno);
  ASSERT_GE(dmabuf_fd, 0);

  std::uint32_t imported_handle = 0;
  const int import_rc = drmPrimeFDToHandle(consumer.fd(), dmabuf_fd, &imported_handle);
  ::close(dmabuf_fd);

  ASSERT_EQ(import_rc, 0) << "drmPrimeFDToHandle failed: " << std::strerror(errno);
  EXPECT_NE(imported_handle, 0U);
  // The consumer's GEM namespace is independent of the producer's, so the
  // imported handle is allocated fresh; equality with buf.handle() would
  // be coincidental and is not a contract.

  // Release the consumer's reference. Without GEM_CLOSE on the consumer
  // fd the imported handle would leak until consumer fd close.
  drm_gem_close gc{};
  gc.handle = imported_handle;
  ::ioctl(consumer.fd(), DRM_IOCTL_GEM_CLOSE, &gc);
}