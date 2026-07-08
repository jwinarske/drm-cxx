// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// tests/integration/test_explicit_sync_fence_gl.cpp
//
// Validates the IN_FENCE_FD explicit-sync path against a REAL GPU + atomic KMS
// driver. A drm::present::GlScanoutProducer renders a GL frame; the frame's
// EGL_ANDROID_native_fence becomes a genuine acquire fence, which the scene arms
// as the plane's IN_FENCE_FD (CommitReport::in_fences_armed) so the display
// engine — not the CPU — waits on the producer before sampling the buffer. The
// same commit's OUT_FENCE_PTR is checked for the return trip.
//
// This is the coverage vkms cannot give: vkms has no GPU, so it produces no real
// acquire fence, and software EGL (llvmpipe) yields none either. The test
// therefore self-skips wherever no real GPU fence is produced (no GPU EGL, no
// EGL_ANDROID_native_fence, or no KMS card), and only asserts the KMS-side fence
// path when a genuine acquire fence actually exists. Validated live on a
// Raspberry Pi 4 (vc4 atomic KMS + v3d GPU).
//
// Self-skips cleanly on CI/hosts without a real GPU. Not parallel: it modesets.

#include <gtest/gtest.h>

#if DRM_CXX_HAS_EGL

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/present/gl_scanout_producer.hpp>
#include <drm-cxx/present/scanout_backend.hpp>
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <drm_fourcc.h>
#include <xf86drm.h>

#include <GLES2/gl2.h>
#include <fcntl.h>
#include <optional>
#include <string>
#include <unistd.h>

namespace {

// First KMS card that isn't vkms. vkms has no GPU, so a GL producer on it would
// render through llvmpipe and never yield a real acquire fence — not the target
// of this test. Any real atomic-KMS card (vc4, amdgpu, i915, …) is a candidate;
// GlScanoutProducer::create() self-fails where EGL can't come up.
[[nodiscard]] std::optional<std::string> find_gpu_kms_node() noexcept {
  for (int idx = 0; idx < 8; ++idx) {
    std::string path = "/dev/dri/card" + std::to_string(idx);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr ver = drmGetVersion(fd);
    const bool is_vkms = (ver != nullptr) && (ver->name != nullptr) &&
                         std::string(ver->name, ver->name_len) == "vkms";
    const bool usable = (ver != nullptr);
    if (ver != nullptr) {
      drmFreeVersion(ver);
    }
    ::close(fd);
    if (usable && !is_vkms) {
      return path;
    }
  }
  return std::nullopt;
}

}  // namespace

// Renders + presents two frames through a GL producer on a real card. The first
// present is the modeset; the second is the steady-state flip whose report and
// OUT_FENCE we assert. If a genuine GPU acquire fence is produced, it MUST have
// been armed as the plane's IN_FENCE_FD (KMS-side wait), not the CPU fallback.
TEST(ExplicitSyncFenceGl, InFenceArmedFromRealGpuFence) {
  const auto path = find_gpu_kms_node();
  if (!path.has_value()) {
    GTEST_SKIP() << "no non-vkms KMS card present";
  }

  auto dev = drm::Device::open(*path);
  ASSERT_TRUE(dev.has_value()) << "Device::open: " << dev.error().message();

  auto producer = drm::present::GlScanoutProducer::create(*dev);
  if (!producer.has_value()) {
    GTEST_SKIP() << "no GPU EGL on " << *path << " (" << producer.error().message()
                 << "); explicit-sync IN_FENCE needs a real GPU fence, skipping";
  }

  drm::present::ScanoutBackend::Config cfg;
  cfg.fourcc = DRM_FORMAT_ARGB8888;
  auto backend = drm::present::ScanoutBackend::create(*dev, **producer, cfg);
  ASSERT_TRUE(backend.has_value()) << "ScanoutBackend::create: " << backend.error().message();

  const auto& target = (*backend)->target();
  ASSERT_GT(target.mode.hdisplay, 0);

  drm::scene::CommitReport last{};
  drm::sync::SyncFence out_fence;
  for (int frame = 0; frame < 2; ++frame) {
    auto cur = (*producer)->make_current();
    ASSERT_TRUE(cur.has_value()) << "make_current: " << cur.error().message();
    glViewport(0, 0, target.mode.hdisplay, target.mode.vdisplay);
    glClearColor(0.2F, 0.4F, 0.6F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    auto swap = (*producer)->swap_buffers();
    ASSERT_TRUE(swap.has_value()) << "swap_buffers: " << swap.error().message();

    drm::sync::SyncFence frame_fence;
    auto report = (*backend)->present(0, &frame_fence);
    ASSERT_TRUE(report.has_value()) << "present: " << report.error().message();
    last = *report;
    out_fence = std::move(frame_fence);
  }

  const std::size_t fences = last.in_fences_armed + last.in_fence_cpu_waits;
  if (fences == 0) {
    GTEST_SKIP() << "GL produced no acquire fence (no EGL_ANDROID_native_fence "
                    "on this stack); nothing to assert about the KMS fence path";
  }

  // A real acquire fence existed this frame: it must have gone to the plane's
  // IN_FENCE_FD (display-engine wait), not degraded to a CPU stall.
  EXPECT_GT(last.in_fences_armed, 0U)
      << "acquire fence produced but not armed as IN_FENCE_FD — the present path "
         "fell back to a CPU wait; this plane/driver can't do KMS-side sync";
  EXPECT_EQ(last.in_fence_cpu_waits, 0U);

  // The return trip: a flip on a CRTC advertising OUT_FENCE_PTR delivers a
  // sync_file that signals at scanout (vc4/amdgpu/i915 do).
  EXPECT_TRUE(out_fence.valid())
      << "OUT_FENCE not delivered on a flip; expected on any CRTC with OUT_FENCE_PTR";
}

#else  // DRM_CXX_HAS_EGL

TEST(ExplicitSyncFenceGl, InFenceArmedFromRealGpuFence) {
  GTEST_SKIP() << "built without EGL (DRM_CXX_HAS_EGL=0); GL producer unavailable";
}

#endif  // DRM_CXX_HAS_EGL
