// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Integration smoke test for drm::capture::snapshot() against the
// kernel's virtual KMS driver (VKMS).
//
// Preconditions:
//   - VKMS module loaded:  sudo modprobe vkms enable_overlay=1
//   - read/write access to /dev/dri/card* (a fresh open() on the VKMS
//     node makes the test the DRM master for that device, since VKMS
//     is a dedicated virtual device with no compositor attached).
//
// If VKMS is not loaded the test self-skips via GTEST_SKIP() so the
// unit-test suite stays green on developer machines that haven't
// modprobed vkms. CI runners should load it explicitly.
//
// Pattern under test: a 4-quadrant 0xFFRRGGBB pattern (red / green /
// blue / white) is written into a dumb framebuffer, pinned as the
// primary plane via drmModeSetCrtc, and snapshot() is expected to
// return an Image whose quadrant centres match the source colours
// byte-for-byte.

#include <drm-cxx/capture/snapshot.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;
using drm::Device;
using drm::capture::Image;
using drm::capture::snapshot;

namespace {

// Locate /dev/dri/cardN for the VKMS driver, if present.
std::optional<std::string> find_vkms_node() {
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator("/dev/dri", ec)) {
    const auto& p = entry.path();
    const std::string name = p.filename().string();
    if (name.rfind("card", 0) != 0) {
      continue;
    }
    const int fd = ::open(p.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr v = drmGetVersion(fd);
    const bool is_vkms =
        (v != nullptr) && (v->name != nullptr) && (std::strcmp(v->name, "vkms") == 0);
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

// RAII wrapper: dumb-buffer + AddFB2 + mmap. Destructor unwinds in
// reverse order. ok() reports whether construction succeeded.
class DumbFb {
 public:
  DumbFb(int fd, std::uint32_t w, std::uint32_t h) : fd_(fd) {
    drm_mode_create_dumb cd{};
    cd.width = w;
    cd.height = h;
    cd.bpp = 32;
    if (::ioctl(fd_, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0) {
      return;
    }
    handle_ = cd.handle;
    pitch_ = cd.pitch;
    size_ = cd.size;

    std::uint32_t handles[4] = {handle_, 0, 0, 0};
    std::uint32_t pitches[4] = {pitch_, 0, 0, 0};
    std::uint32_t offsets[4] = {0, 0, 0, 0};
    if (drmModeAddFB2(fd_, w, h, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &fb_id_, 0) != 0) {
      return;
    }

    drm_mode_map_dumb md{};
    md.handle = handle_;
    if (::ioctl(fd_, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0) {
      return;
    }
    void* p = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_,
                     static_cast<off_t>(md.offset));
    if (p == MAP_FAILED) {
      return;
    }
    map_ = p;
  }

  ~DumbFb() {
    if (map_ != nullptr) {
      ::munmap(map_, size_);
    }
    if (fb_id_ != 0) {
      drmModeRmFB(fd_, fb_id_);
    }
    if (handle_ != 0) {
      drm_mode_destroy_dumb dd{};
      dd.handle = handle_;
      ::ioctl(fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }
  }

  DumbFb(const DumbFb&) = delete;
  DumbFb& operator=(const DumbFb&) = delete;
  DumbFb(DumbFb&&) = delete;
  DumbFb& operator=(DumbFb&&) = delete;

  [[nodiscard]] bool ok() const noexcept { return map_ != nullptr && fb_id_ != 0; }
  [[nodiscard]] std::uint32_t fb_id() const noexcept { return fb_id_; }
  [[nodiscard]] std::uint32_t pitch() const noexcept { return pitch_; }
  [[nodiscard]] std::uint32_t* pixels() noexcept { return static_cast<std::uint32_t*>(map_); }

 private:
  int fd_{-1};
  std::uint32_t handle_{0};
  std::uint32_t pitch_{0};
  std::uint32_t fb_id_{0};
  std::size_t size_{0};
  void* map_{nullptr};
};

struct ActiveCrtc {
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  drmModeModeInfo mode{};
};

// Pick the first connected connector with modes, and the first CRTC
// that at least one of its encoders can drive. Returns via
// drm::expected rather than std::optional so the test's downstream
// `.value()` uses don't trip bugprone-unchecked-optional-access —
// clang-tidy tracks that check for std::optional only.
drm::expected<ActiveCrtc, std::error_code> pick_crtc(int fd) {
  auto* res = drmModeGetResources(fd);
  if (res == nullptr) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_such_device));
  }

  std::optional<ActiveCrtc> found;

  for (int i = 0; i < res->count_connectors && !found.has_value(); ++i) {
    auto* conn = drmModeGetConnector(fd, res->connectors[i]);
    if (conn == nullptr) {
      continue;
    }
    if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0) {
      for (int e = 0; e < conn->count_encoders && !found.has_value(); ++e) {
        auto* enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (enc == nullptr) {
          continue;
        }
        for (int c = 0; c < res->count_crtcs; ++c) {
          if ((enc->possible_crtcs & (1U << static_cast<unsigned>(c))) != 0) {
            ActiveCrtc out;
            out.connector_id = conn->connector_id;
            out.mode = conn->modes[0];
            out.crtc_id = res->crtcs[c];
            found = out;
            break;
          }
        }
        drmModeFreeEncoder(enc);
      }
    }
    drmModeFreeConnector(conn);
  }
  drmModeFreeResources(res);
  if (!found.has_value()) {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::no_such_device_or_address));
  }
  return *found;
}

}  // namespace

TEST(CaptureVkms, RoundTripPrimaryPlane) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded — `sudo modprobe vkms enable_overlay=1` "
                    "to enable this test";
  }

  auto dev_r = Device::open(*node);
  ASSERT_TRUE(dev_r.has_value()) << dev_r.error().message();
  auto& dev = *dev_r;

  ASSERT_TRUE(dev.enable_universal_planes().has_value());
  // snapshot() reads atomic-only plane properties (CRTC_X/Y/W/H, zpos),
  // which the kernel only exposes to clients that have opted into the
  // atomic API. Without this, drmModeObjectGetProperties returns only
  // the legacy subset and every plane gets skipped.
  ASSERT_TRUE(dev.enable_atomic().has_value());

  const auto active_r = pick_crtc(dev.fd());
  ASSERT_TRUE(active_r.has_value())
      << "VKMS present but exposes no connected connector with a CRTC — "
         "module probe may have failed: "
      << active_r.error().message();
  const auto& active = *active_r;

  const std::uint32_t w = active.mode.hdisplay;
  const std::uint32_t h = active.mode.vdisplay;
  ASSERT_GE(w, 16U);
  ASSERT_GE(h, 16U);

  DumbFb fb(dev.fd(), w, h);
  ASSERT_TRUE(fb.ok()) << "dumb buffer allocation failed — errno=" << std::strerror(errno);

  // 4-quadrant pattern. ARGB8888 on LE → uint32 byte order is B,G,R,A
  // in memory, which round-trips cleanly because PRGB32 shares the
  // layout and alpha=0xFF means no premultiplication scaling.
  const std::uint32_t pitch_px = fb.pitch() / 4;
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const bool right = x >= w / 2;
      const bool bottom = y >= h / 2;
      std::uint32_t px = 0;
      if (!right && !bottom) {
        px = 0xFFFF0000U;  // red
      } else if (right && !bottom) {
        px = 0xFF00FF00U;  // green
      } else if (!right && bottom) {
        px = 0xFF0000FFU;  // blue
      } else {
        px = 0xFFFFFFFFU;  // white
      }
      fb.pixels()[(y * pitch_px) + x] = px;
    }
  }

  // Activate the mode with our FB as primary. SetCrtc is legacy, but
  // on atomic-capable drivers like VKMS the kernel reflects this into
  // the primary plane's FB_ID property — exactly what snapshot()
  // reads back via drmModeGetPlane.
  std::uint32_t conn_id = active.connector_id;
  drmModeModeInfo mode = active.mode;
  ASSERT_EQ(drmModeSetCrtc(dev.fd(), active.crtc_id, fb.fb_id(), 0, 0, &conn_id, 1, &mode), 0)
      << "drmModeSetCrtc: " << std::strerror(errno);

  // Diagnostic: confirm that legacy SetCrtc was reflected into the
  // atomic plane state snapshot() depends on. On drivers that don't
  // round-trip the binding, plane->crtc_id or plane->fb_id will
  // remain zero even though the CRTC shows the mode as valid.
  {
    auto* crtc_after = drmModeGetCrtc(dev.fd(), active.crtc_id);
    ASSERT_NE(crtc_after, nullptr);
    EXPECT_NE(crtc_after->mode_valid, 0U)
        << "SetCrtc returned 0 but drmModeGetCrtc reports mode_valid=0";
    EXPECT_EQ(crtc_after->buffer_id, fb.fb_id())
        << "CRTC's primary fb_id does not match what we bound";
    drmModeFreeCrtc(crtc_after);

    auto* pres = drmModeGetPlaneResources(dev.fd());
    ASSERT_NE(pres, nullptr);
    bool plane_bound = false;
    for (std::uint32_t i = 0; i < pres->count_planes; ++i) {
      auto* pl = drmModeGetPlane(dev.fd(), pres->planes[i]);
      if (pl != nullptr && pl->crtc_id == active.crtc_id && pl->fb_id == fb.fb_id()) {
        plane_bound = true;
      }
      if (pl != nullptr) {
        drmModeFreePlane(pl);
      }
    }
    EXPECT_TRUE(plane_bound) << "No plane reports (crtc_id=" << active.crtc_id
                             << ", fb_id=" << fb.fb_id() << ") after SetCrtc";
    drmModeFreePlaneResources(pres);
  }

  auto img_r = snapshot(dev, active.crtc_id);
  // Unbind before assertions so a test failure doesn't leak an active FB.
  drmModeSetCrtc(dev.fd(), active.crtc_id, 0, 0, 0, nullptr, 0, nullptr);

  ASSERT_TRUE(img_r.has_value()) << img_r.error().message();
  const Image img = std::move(*img_r);
  ASSERT_EQ(img.width(), w);
  ASSERT_EQ(img.height(), h);

  auto at = [&](std::uint32_t x, std::uint32_t y) { return img.pixels()[(y * img.width()) + x]; };

  EXPECT_EQ(at(w / 4, h / 4), 0xFFFF0000U) << "top-left should be red";
  EXPECT_EQ(at((3 * w) / 4, h / 4), 0xFF00FF00U) << "top-right should be green";
  EXPECT_EQ(at(w / 4, (3 * h) / 4), 0xFF0000FFU) << "bottom-left should be blue";
  EXPECT_EQ(at((3 * w) / 4, (3 * h) / 4), 0xFFFFFFFFU) << "bottom-right should be white";
}
