// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Integration smoke test for drm::scene::GstAppsinkSource against the
// kernel's virtual KMS driver (VKMS).
//
// Preconditions:
//   - VKMS module loaded:  sudo modprobe vkms enable_overlay=1
//   - read/write access to /dev/dri/card* (a fresh open() on the VKMS
//     node makes the test the DRM master for that device, since VKMS
//     is a dedicated virtual device with no compositor attached).
//   - GStreamer plugin set including `videotestsrc` and `appsink`
//     (gst-plugins-base ships both — installed on every distro that
//     ships GStreamer at all).
//
// If VKMS is not loaded the tests self-skip via GTEST_SKIP() so the
// integration suite stays green on developer machines that haven't
// modprobed vkms. CI runners should load it explicitly.
//
// What this exercises:
//   1. Sysmem-memcpy fallback path — `videotestsrc` produces system-
//      memory BGRx samples (no DMABUF), so the source allocates its
//      own dumb buffer and copies into it. Verifies format
//      negotiation, lazy buffer allocation, AddFB2 wiring, and
//      `acquire()` round-trip.
//   2. EOS surfacing — bounded-source pipeline reaches end-of-stream
//      and the source's `drive()` reports `no_message_available`.
//   3. Mid-stream caps change — a named capsfilter is reconfigured
//      while the pipeline is PLAYING, forcing videotestsrc to
//      renegotiate to a new resolution. The source must tear down
//      its FB cache + sysmem fallback, re-resolve format from the
//      new caps, and surface the new dimensions through `format()`.
//
// What this does NOT exercise:
//   - DMABUF zero-copy path. That requires a hardware decoder element
//     (v4l2h264dec, vah264dec, etc.) producing GstDmaBufMemory; CI
//     doesn't have one, and the sysmem path is the more vulnerable of
//     the two anyway. Hardware-validated separately.

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/scene/buffer_source.hpp>  // AcquiredBuffer
#include <drm-cxx/scene/commit_report.hpp>
#include <drm-cxx/scene/gst_appsink_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <glib-object.h>  // G_OBJECT / g_object_set — caps-change test
#include <gst/gst.h>
#include <gst/gstbin.h>
#include <gst/gstcaps.h>
#include <gst/gstelement.h>
#include <gst/gstobject.h>
#include <gst/gstparse.h>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;

namespace {

constexpr std::chrono::milliseconds k_acquire_timeout{2000};
constexpr std::chrono::milliseconds k_poll_interval{20};

struct ActiveCrtc {
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  drmModeModeInfo mode{};
};

// Pick the first connected connector with a usable mode and a CRTC it
// can drive. Mirrors the helper in test_layer_scene_*_vkms.
std::optional<ActiveCrtc> pick_crtc(int fd) {
  std::optional<ActiveCrtc> found;
  auto* res = drmModeGetResources(fd);
  if (res == nullptr) {
    return std::nullopt;
  }
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
  return found;
}

// Locate /dev/dri/cardN for the VKMS driver, if present. Same pattern
// as test_capture_vkms / test_layer_scene_*_vkms.
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
    const bool is_vkms = v != nullptr && v->name != nullptr && std::strcmp(v->name, "vkms") == 0;
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

class GstVkmsFixture : public ::testing::Test {
 public:
  static void SetUpTestSuite() { gst_init(nullptr, nullptr); }

 protected:
  void SetUp() override {
    const auto node = find_vkms_node();
    if (!node.has_value()) {
      GTEST_SKIP() << "vkms not loaded — skipping (modprobe vkms enable_overlay=1)";
    }
    auto dev_r = drm::Device::open(*node);
    ASSERT_TRUE(dev_r.has_value()) << "Device::open(" << *node << "): " << dev_r.error().message();
    dev = std::make_unique<drm::Device>(std::move(*dev_r));
  }

  void TearDown() override {
    if (pipeline != nullptr) {
      gst_element_set_state(pipeline, GST_STATE_NULL);
      gst_object_unref(pipeline);
      pipeline = nullptr;
    }
    dev.reset();
  }

  // Build a `videotestsrc num-buffers=N is-live=true ! capsfilter !
  // appsink name=sink` pipeline at the requested resolution and pull
  // the appsink element back out by name. Returns the appsink (already
  // ref'd by the bin; the GstAppsinkSource will take its own ref).
  GstElement* build_pipeline(int num_buffers, std::uint32_t width, std::uint32_t height) {
    const std::string desc = std::string("videotestsrc is-live=true num-buffers=") +
                             std::to_string(num_buffers) +
                             " ! video/x-raw,format=BGRx,width=" + std::to_string(width) +
                             ",height=" + std::to_string(height) + " ! appsink name=sink";
    // Pass nullptr for the GError out-param: GError lives in <glib.h>,
    // an umbrella the include-cleaner lint can't trace through. The
    // pipeline string is logged on failure so diagnostics are still
    // adequate.
    pipeline = gst_parse_launch(desc.c_str(), nullptr);
    if (pipeline == nullptr) {
      ADD_FAILURE() << "gst_parse_launch failed for: " << desc;
      return nullptr;
    }
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    EXPECT_NE(sink, nullptr);
    // gst_bin_get_by_name returned a fresh ref; the source will take
    // its own. Drop ours so we don't leak a ref past the pipeline.
    if (sink != nullptr) {
      gst_object_unref(sink);
    }
    return sink;
  }

  // gtest fixture convention: TEST_F bodies access these directly
  // through the fixture's `this` pointer, so they must be at protected
  // visibility (private would be unreachable from the test bodies).
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes,
  //             misc-non-private-member-variables-in-classes)
  std::unique_ptr<drm::Device> dev;
  GstElement* pipeline{nullptr};
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes,
  //           misc-non-private-member-variables-in-classes)
};

}  // namespace

TEST_F(GstVkmsFixture, AcquiresSysmemSampleEndToEnd) {
  GstElement* appsink = build_pipeline(/*num_buffers=*/100, /*width=*/320, /*height=*/240);
  ASSERT_NE(appsink, nullptr);

  auto src_r = drm::scene::GstAppsinkSource::create(*dev, appsink, {});
  ASSERT_TRUE(src_r.has_value()) << "GstAppsinkSource::create: " << src_r.error().message();
  auto src = std::move(*src_r);

  ASSERT_NE(gst_element_set_state(pipeline, GST_STATE_PLAYING), GST_STATE_CHANGE_FAILURE);

  // Poll until acquire() returns a real buffer (or until timeout).
  // Bus pump runs each iteration so EOS / errors surface promptly.
  const auto deadline = std::chrono::steady_clock::now() + k_acquire_timeout;
  drm::expected<drm::scene::AcquiredBuffer, std::error_code> acquired{
      drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::resource_unavailable_try_again))};
  while (std::chrono::steady_clock::now() < deadline) {
    auto drv = src->drive();
    if (!drv.has_value()) {
      ADD_FAILURE() << "drive(): " << drv.error().message();
      break;
    }
    acquired = src->acquire();
    if (acquired.has_value()) {
      break;
    }
    if (acquired.error() != std::make_error_code(std::errc::resource_unavailable_try_again)) {
      ADD_FAILURE() << "acquire(): " << acquired.error().message();
      break;
    }
    std::this_thread::sleep_for(k_poll_interval);
  }

  ASSERT_TRUE(acquired.has_value()) << "acquire() timed out without producing a sample";
  EXPECT_NE(acquired->fb_id, 0U) << "acquire() returned a zero fb_id";
  EXPECT_EQ(acquired->acquire_fence_fd, -1)
      << "no fence_extractor configured — fence fd should be -1";

  // Format should be populated after the first sample lands.
  const auto fmt = src->format();
  EXPECT_EQ(fmt.width, 320U);
  EXPECT_EQ(fmt.height, 240U);
  EXPECT_NE(fmt.drm_fourcc, 0U);

  // release() is a no-op for this source — exercise it anyway so the
  // contract test isn't a documentation lie.
  src->release(*acquired);
}

TEST_F(GstVkmsFixture, HandlesCapsChangeMidStream) {
  // Pipeline shape: a named capsfilter sits between videotestsrc and
  // appsink. We start at 320×240, wait for the source to lock format,
  // then poke the capsfilter's caps property to 640×480. videotestsrc
  // renegotiates upstream and the appsink sees the new caps; the
  // source's caps-change branch must tear down and re-resolve.
  const std::string desc =
      "videotestsrc is-live=true "
      "! capsfilter name=cf caps=video/x-raw,format=BGRx,width=320,height=240 "
      "! appsink name=sink";
  pipeline = gst_parse_launch(desc.c_str(), nullptr);
  ASSERT_NE(pipeline, nullptr) << "gst_parse_launch failed for: " << desc;

  GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
  ASSERT_NE(sink, nullptr);
  auto sink_holder =
      std::unique_ptr<GstElement, decltype(&gst_object_unref)>(sink, gst_object_unref);

  GstElement* cf = gst_bin_get_by_name(GST_BIN(pipeline), "cf");
  ASSERT_NE(cf, nullptr);
  auto cf_holder = std::unique_ptr<GstElement, decltype(&gst_object_unref)>(cf, gst_object_unref);

  auto src_r = drm::scene::GstAppsinkSource::create(*dev, sink, {});
  ASSERT_TRUE(src_r.has_value()) << "GstAppsinkSource::create: " << src_r.error().message();
  auto src = std::move(*src_r);

  ASSERT_NE(gst_element_set_state(pipeline, GST_STATE_PLAYING), GST_STATE_CHANGE_FAILURE);

  // Drain samples until format() reflects the requested dimensions.
  // Returns false on timeout. Each iteration runs drive() so EOS /
  // errors surface promptly, and acquire() so caps changes propagate.
  auto wait_for_format = [&](std::uint32_t want_w, std::uint32_t want_h) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + k_acquire_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      auto drv = src->drive();
      if (!drv.has_value()) {
        return false;
      }
      (void)src->acquire();
      const auto fmt = src->format();
      if (fmt.width == want_w && fmt.height == want_h) {
        return true;
      }
      std::this_thread::sleep_for(k_poll_interval);
    }
    return false;
  };

  // Phase 1: lock the initial caps.
  ASSERT_TRUE(wait_for_format(320, 240)) << "initial caps never latched";

  // Phase 2: reconfigure the capsfilter mid-stream. videotestsrc adapts
  // its output to whatever the capsfilter restricts to, so the appsink
  // sees the new resolution on the next sample.
  // NOLINTBEGIN(misc-include-cleaner)
  GstCaps* new_caps = gst_caps_from_string("video/x-raw,format=BGRx,width=640,height=480");
  ASSERT_NE(new_caps, nullptr);
  g_object_set(G_OBJECT(cf), "caps", new_caps, nullptr);
  gst_caps_unref(new_caps);
  // NOLINTEND(misc-include-cleaner)

  // Phase 3: source picks up the new caps. The first post-change
  // acquire() trips caps_match_cached, tears down DRM state, and
  // re-resolves; format() reflects 640×480 from then on.
  ASSERT_TRUE(wait_for_format(640, 480)) << "source did not pick up the caps change";
  EXPECT_EQ(src->format().width, 640U);
  EXPECT_EQ(src->format().height, 480U);
  EXPECT_NE(src->format().drm_fourcc, 0U);

  // Sanity: an explicit acquire() at the new caps should still produce
  // a valid fb_id (not just match dimensions on the next-frame path).
  auto acq = src->acquire();
  ASSERT_TRUE(acq.has_value()) << "post-caps-change acquire(): " << acq.error().message();
  EXPECT_NE(acq->fb_id, 0U);
}

TEST_F(GstVkmsFixture, SurfacesEosAfterBoundedStream) {
  GstElement* appsink = build_pipeline(/*num_buffers=*/2, /*width=*/160, /*height=*/120);
  ASSERT_NE(appsink, nullptr);

  auto src_r = drm::scene::GstAppsinkSource::create(*dev, appsink, {});
  ASSERT_TRUE(src_r.has_value()) << "GstAppsinkSource::create: " << src_r.error().message();
  auto src = std::move(*src_r);

  ASSERT_NE(gst_element_set_state(pipeline, GST_STATE_PLAYING), GST_STATE_CHANGE_FAILURE);

  // Drain the pipeline. After 2 buffers EOS is posted to the bus.
  // drive() should eventually surface no_message_available.
  bool saw_eos = false;
  const auto deadline = std::chrono::steady_clock::now() + k_acquire_timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto drv = src->drive();
    if (!drv.has_value()) {
      if (drv.error() == std::make_error_code(std::errc::no_message_available)) {
        saw_eos = true;
        break;
      }
      ADD_FAILURE() << "drive() unexpected error: " << drv.error().message();
      break;
    }
    // Pull samples to keep the appsink draining.
    (void)src->acquire();
    std::this_thread::sleep_for(k_poll_interval);
  }
  EXPECT_TRUE(saw_eos);
}

// LayerScene must treat EAGAIN from a source's acquire() as flow
// control: skip the layer for this commit, count it in
// layers_skipped_no_frame, and let the next commit pick the layer up
// once the source has a frame. This is the pre-preroll race the
// video_player example hit; before the fix, the first commit returned
// EAGAIN end-to-end and the example exited without ever drawing.
TEST_F(GstVkmsFixture, LayerSceneSkipsLayerWhenSourceReturnsEagain) {
  auto active = pick_crtc(dev->fd());
  if (!active.has_value()) {
    GTEST_SKIP() << "VKMS exposed no usable CRTC + connector";
  }
  ASSERT_TRUE(dev->enable_universal_planes().has_value());
  ASSERT_TRUE(dev->enable_atomic().has_value());

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = active->crtc_id;
  cfg.connector_id = active->connector_id;
  cfg.mode = active->mode;
  auto scene_r = drm::scene::LayerScene::create(*dev, cfg);
  ASSERT_TRUE(scene_r.has_value()) << "LayerScene::create: " << scene_r.error().message();
  auto scene = std::move(*scene_r);

  GstElement* appsink = build_pipeline(/*num_buffers=*/30,
                                       /*width=*/active->mode.hdisplay,
                                       /*height=*/active->mode.vdisplay);
  ASSERT_NE(appsink, nullptr);

  auto src_r = drm::scene::GstAppsinkSource::create(*dev, appsink, {});
  ASSERT_TRUE(src_r.has_value()) << "GstAppsinkSource::create: " << src_r.error().message();

  drm::scene::LayerDesc ldesc;
  ldesc.source = std::move(*src_r);
  ldesc.display.src_rect = drm::scene::Rect{0, 0, active->mode.hdisplay, active->mode.vdisplay};
  ldesc.display.dst_rect = drm::scene::Rect{0, 0, active->mode.hdisplay, active->mode.vdisplay};
  auto layer_r = scene->add_layer(std::move(ldesc));
  ASSERT_TRUE(layer_r.has_value()) << "add_layer: " << layer_r.error().message();

  // First commit BEFORE the pipeline transitions out of NULL. The
  // appsink has no streaming thread, no sample, and no preroll race to
  // win — acquire() deterministically returns EAGAIN. (An earlier
  // version of this test went PLAYING first and tried to commit before
  // the first sample arrived ~33 ms later; that races on fast hosts
  // where the commit beat the streaming thread by a hair, then loses
  // when the streaming thread wins instead.) The scenario this still
  // mirrors: video_player's first commit fires before the GStreamer
  // pipeline has produced anything; LayerScene must skip the layer
  // rather than fail the commit.
  auto first = scene->commit();
  ASSERT_TRUE(first.has_value()) << "first commit must succeed even with no sample ready: "
                                 << first.error().message();
  EXPECT_EQ(first->layers_total, 1U);
  EXPECT_EQ(first->layers_skipped_no_frame, 1U)
      << "GstAppsinkSource pre-preroll should be flow-controlled, not dropped";
  EXPECT_EQ(first->layers_assigned, 0U);
  EXPECT_EQ(first->layers_composited, 0U);
  EXPECT_EQ(first->layers_unassigned, 0U) << "skip is not a drop";

  // Now start the pipeline. Subsequent commits: as soon as a sample
  // lands, the source has an FB to hand out and the layer joins the
  // scene.
  ASSERT_NE(gst_element_set_state(pipeline, GST_STATE_PLAYING), GST_STATE_CHANGE_FAILURE);
  const auto deadline = std::chrono::steady_clock::now() + k_acquire_timeout;
  bool saw_assignment = false;
  while (std::chrono::steady_clock::now() < deadline) {
    auto rep = scene->commit();
    ASSERT_TRUE(rep.has_value()) << "subsequent commit: " << rep.error().message();
    if (rep->layers_assigned + rep->layers_composited >= 1U) {
      EXPECT_EQ(rep->layers_skipped_no_frame, 0U)
          << "once the source has a frame, EAGAIN should not recur";
      saw_assignment = true;
      break;
    }
    std::this_thread::sleep_for(k_poll_interval);
  }
  EXPECT_TRUE(saw_assignment) << "layer never picked up after pipeline started producing samples";
}
