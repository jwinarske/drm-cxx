// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Regression coverage for LayerScene's deferred-release contract on
// LayerBufferSource (`buffer_source.hpp:140` — release happens "after
// page-flip completion", not at commit-ioctl-return time).
//
// What it proves:
//   1. The first two real commits acquire from the source but do NOT
//      call release on it. Releasing the just-committed buffer
//      immediately would let producers (V4L2, GBM ring) start
//      overwriting a buffer the kernel is still scanning out — visible
//      as tearing during motion on hardware whose producer driver
//      doesn't attach reservation fences (uvcvideo, most V4L2 capture
//      drivers).
//   2. From the third commit on, each real commit releases exactly the
//      acquisition from two commits ago. Two-deep deferral is the
//      shortest interval that keeps the previously-on-screen buffer
//      held until the *next* commit's vblank has replaced it.
//   3. Destroying the scene releases every still-held in-flight
//      acquisition. No leaks across teardown.
//   4. Test commits (`scene->test()`) are NOT subject to deferred
//      release — they release immediately, since no kernel flip
//      occurred for the in-flight ring to gate on.
//
// The fake source wraps a real DumbBufferSource so the kernel commit
// can succeed; the wrapper just intercepts acquire/release and writes
// a per-call event into a shared transcript that the assertions read.
//
// Self-skips when VKMS isn't loaded — same pattern as the other
// tests/integration files.

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/modeset/atomic.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using drm::Device;
using drm::scene::AcquiredBuffer;
using drm::scene::BindingModel;
using drm::scene::DumbBufferSource;
using drm::scene::LayerBufferSource;
using drm::scene::LayerDesc;
using drm::scene::LayerScene;
using drm::scene::SourceFormat;

namespace {

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

struct ActiveCrtc {
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  drmModeModeInfo mode{};
};

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

// Per-acquisition counter encoded into AcquiredBuffer.opaque so the
// transcript can pair release calls with the specific acquire that
// produced them. `+1` so the first acquire (counter=0) doesn't collide
// with a default-constructed (nullptr) opaque.
inline void* encode_id(std::uint64_t id) noexcept {
  // NOLINTNEXTLINE(performance-no-int-to-ptr) — opaque cookie, not a real address.
  return reinterpret_cast<void*>(static_cast<std::uintptr_t>(id) + 1U);
}
inline std::uint64_t decode_id(void* opaque) noexcept {
  const auto raw = reinterpret_cast<std::uintptr_t>(opaque);
  return raw == 0 ? 0 : static_cast<std::uint64_t>(raw - 1U);
}

// Wraps a real DumbBufferSource so the kernel commit succeeds while
// recording acquire/release in a shared transcript. The wrapped
// DumbBufferSource handles all the boilerplate (FB allocation, format
// reporting, session hooks); we only intercept the two methods whose
// timing this test asserts.
class TrackingSource : public LayerBufferSource {
 public:
  struct Event {
    enum class Kind : std::uint8_t { Acquire, Release };
    Kind kind;
    std::uint64_t id;  // unique per acquire; release matches by id
  };

  // `destroyed`, when set, is flipped true in the destructor so tests can assert
  // *when* the scene tears the source down. It is a shared_ptr, not a transcript
  // write, because the scene may destroy the source (retire-ring drain, teardown)
  // after the test's transcript vector has gone out of scope.
  TrackingSource(std::unique_ptr<DumbBufferSource> inner, std::vector<Event>& transcript,
                 std::shared_ptr<bool> destroyed = nullptr)
      : inner_(std::move(inner)), transcript_(transcript), destroyed_(std::move(destroyed)) {}

  ~TrackingSource() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  drm::expected<AcquiredBuffer, std::error_code> acquire() override {
    auto r = inner_->acquire();
    if (!r) {
      return r;
    }
    const std::uint64_t id = next_id_++;
    transcript_.push_back({Event::Kind::Acquire, id});
    AcquiredBuffer out;
    out.fb_id = r->fb_id;
    out.opaque = encode_id(id);
    return out;
  }

  void release(AcquiredBuffer acquired) noexcept override {
    transcript_.push_back({Event::Kind::Release, decode_id(acquired.opaque)});
    // Forward to inner with the inner's own opaque (nullptr — the
    // wrapped DumbBufferSource ignores the opaque since its release
    // is a no-op anyway).
    AcquiredBuffer inner_buf;
    inner_buf.fb_id = acquired.fb_id;
    inner_buf.opaque = nullptr;
    inner_->release(std::move(inner_buf));
  }

  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return inner_->binding_model();
  }

  [[nodiscard]] SourceFormat format() const noexcept override { return inner_->format(); }

  drm::expected<drm::BufferMapping, std::error_code> map(drm::MapAccess access) override {
    return inner_->map(access);
  }

  void on_session_paused() noexcept override { inner_->on_session_paused(); }

  drm::expected<void, std::error_code> on_session_resumed(const drm::Device& new_dev) override {
    return inner_->on_session_resumed(new_dev);
  }

 private:
  std::unique_ptr<DumbBufferSource> inner_;
  std::vector<Event>& transcript_;
  std::shared_ptr<bool> destroyed_;
  std::uint64_t next_id_{0};
};

struct SceneFixture {
  std::unique_ptr<Device> dev;
  ActiveCrtc active;
  std::unique_ptr<LayerScene> scene;
};

drm::expected<SceneFixture, std::error_code> open_vkms_scene(const std::string& node) {
  auto dev_r = Device::open(node);
  if (!dev_r) {
    return drm::unexpected<std::error_code>(dev_r.error());
  }
  auto dev = std::make_unique<Device>(std::move(*dev_r));
  if (auto r = dev->enable_universal_planes(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  if (auto r = dev->enable_atomic(); !r) {
    return drm::unexpected<std::error_code>(r.error());
  }
  auto active_r = pick_crtc(dev->fd());
  if (!active_r) {
    return drm::unexpected<std::error_code>(active_r.error());
  }
  LayerScene::Config cfg;
  cfg.crtc_id = active_r->crtc_id;
  cfg.connector_id = active_r->connector_id;
  cfg.mode = active_r->mode;
  auto scene_r = LayerScene::create(*dev, cfg);
  if (!scene_r) {
    return drm::unexpected<std::error_code>(scene_r.error());
  }
  return SceneFixture{std::move(dev), *active_r, std::move(*scene_r)};
}

void cleanup_crtc(int fd, std::uint32_t crtc_id) {
  drmModeSetCrtc(fd, crtc_id, 0, 0, 0, nullptr, 0, nullptr);
}

// Counts releases in the transcript. The deferred-release contract
// makes the per-commit release count predictable: 0, 0, 1, 1, 1, ...
// (commit N releases acquire N-2's id, for N >= 2).
std::vector<std::uint64_t> released_ids(const std::vector<TrackingSource::Event>& transcript) {
  std::vector<std::uint64_t> out;
  for (const auto& e : transcript) {
    if (e.kind == TrackingSource::Event::Kind::Release) {
      out.push_back(e.id);
    }
  }
  return out;
}

std::size_t count_acquires(const std::vector<TrackingSource::Event>& transcript) {
  std::size_t n = 0;
  for (const auto& e : transcript) {
    if (e.kind == TrackingSource::Event::Kind::Acquire) {
      ++n;
    }
  }
  return n;
}

// Outstanding leases = buffers acquired from the source but not yet released
// back to it. The deferred-release contract keeps this at 1-2 across steady
// commits; a commit *failure* must drive it straight back to 0.
std::size_t outstanding_leases(const std::vector<TrackingSource::Event>& transcript) {
  return count_acquires(transcript) - released_ids(transcript).size();
}

}  // namespace

TEST(LayerSceneReleaseVkms, DefersReleaseTwoCommitsDeep) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded — `sudo modprobe vkms enable_overlay=1` "
                    "to enable this test";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;

  std::vector<TrackingSource::Event> transcript;
  auto inner = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(inner.has_value()) << inner.error().message();

  LayerDesc layer;
  layer.source = std::make_unique<TrackingSource>(std::move(*inner), transcript);
  layer.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.zpos = 1;
  ASSERT_TRUE(fx.scene->add_layer(std::move(layer)).has_value());

  // First commit: acquire #0; release MUST NOT happen yet (the kernel
  // hasn't even started scanning out the just-committed FB — releasing
  // it now would let a producer overwrite it mid-scanout).
  auto c1 = fx.scene->commit();
  ASSERT_TRUE(c1.has_value()) << c1.error().message();
  EXPECT_EQ(count_acquires(transcript), 1U) << "commit 1 should acquire one buffer";
  EXPECT_TRUE(released_ids(transcript).empty())
      << "commit 1 must NOT release the just-committed buffer (deferred-release contract)";

  // Second commit: acquire #1; the in-flight ring (depth 2) is now
  // full but no release has fired yet — the buffer from commit 1 is
  // still on screen (will be replaced at commit 2's vblank, which
  // userspace observes via a page-flip event before issuing commit 3).
  auto c2 = fx.scene->commit();
  ASSERT_TRUE(c2.has_value()) << c2.error().message();
  EXPECT_EQ(count_acquires(transcript), 2U) << "commit 2 should acquire one more buffer";
  EXPECT_TRUE(released_ids(transcript).empty())
      << "commit 2 must NOT release any buffer yet — both prior frames are still in flight";

  // Third commit: acquire #2; NOW commit 1's acquisition is releasable
  // (its FB has been off-screen since commit 2's vblank). Exactly one
  // release, identifying acquire id 0.
  auto c3 = fx.scene->commit();
  ASSERT_TRUE(c3.has_value()) << c3.error().message();
  EXPECT_EQ(count_acquires(transcript), 3U);
  ASSERT_EQ(released_ids(transcript).size(), 1U)
      << "commit 3 should release exactly one buffer (commit 1's acquisition)";
  EXPECT_EQ(released_ids(transcript).back(), 0U)
      << "commit 3's release must identify acquire id 0 (FIFO order)";

  // Fourth commit: same pattern — releases acquire id 1.
  auto c4 = fx.scene->commit();
  ASSERT_TRUE(c4.has_value()) << c4.error().message();
  EXPECT_EQ(count_acquires(transcript), 4U);
  ASSERT_EQ(released_ids(transcript).size(), 2U);
  EXPECT_EQ(released_ids(transcript).back(), 1U) << "commit 4 must release acquire id 1";

  // Fifth commit: releases acquire id 2.
  auto c5 = fx.scene->commit();
  ASSERT_TRUE(c5.has_value()) << c5.error().message();
  EXPECT_EQ(count_acquires(transcript), 5U);
  ASSERT_EQ(released_ids(transcript).size(), 3U);
  EXPECT_EQ(released_ids(transcript).back(), 2U) << "commit 5 must release acquire id 2";

  // Tear down the scene; the deferred-release ring still holds the
  // last two acquisitions (ids 3 and 4). The Impl destructor must
  // drain them — verify by checking the transcript after reset.
  fx.scene.reset();
  auto final_releases = released_ids(transcript);
  EXPECT_EQ(final_releases.size(), 5U)
      << "scene destruction must release every still-held in-flight acquisition";
  // Order: 3 then 4 (matches release_pending_acquisitions's
  // prev_prev-then-prev draining order).
  if (final_releases.size() == 5U) {
    EXPECT_EQ(final_releases.at(3), 3U) << "destructor releases id 3 (the prev_prev slot) first";
    EXPECT_EQ(final_releases.at(4), 4U) << "destructor releases id 4 (the prev slot) second";
  }

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

TEST(LayerSceneReleaseVkms, TestCommitsReleaseImmediately) {
  const auto node = find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;

  std::vector<TrackingSource::Event> transcript;
  auto inner = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(inner.has_value()) << inner.error().message();

  LayerDesc layer;
  layer.source = std::make_unique<TrackingSource>(std::move(*inner), transcript);
  layer.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.zpos = 1;
  ASSERT_TRUE(fx.scene->add_layer(std::move(layer)).has_value());

  // Test commits don't queue a real flip — there's no in-flight
  // scanout to gate the release on, so they MUST release immediately.
  // Holding buffers across test commits would just stall the source's
  // ring (visible as EAGAIN-loop in V4L2-style pull-mode sources).
  // NOLINTNEXTLINE(misc-confusable-identifiers)
  auto t1 = fx.scene->test();
  ASSERT_TRUE(t1.has_value()) << t1.error().message();
  EXPECT_EQ(count_acquires(transcript), 1U);
  EXPECT_EQ(released_ids(transcript).size(), 1U)
      << "test() must release immediately — no flip happened";

  auto t2 = fx.scene->test();
  ASSERT_TRUE(t2.has_value()) << t2.error().message();
  EXPECT_EQ(count_acquires(transcript), 2U);
  EXPECT_EQ(released_ids(transcript).size(), 2U)
      << "every test() releases immediately, regardless of count";

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

// A layer removed while its buffers are still in flight must not have its source
// destroyed synchronously (the old behavior released on-scan buffers early and
// tore the layer down out from under the deferred-release ring). Instead the
// source is retired: its buffers drain through the ring with their release
// fences, and it is destroyed only afterward.
TEST(LayerSceneReleaseVkms, RemoveLayerDefersSourceRetirement) {
  const char* env = std::getenv("DRM_CXX_TEST_CARD");
  const std::optional<std::string> node =
      (env != nullptr && *env != '\0') ? std::optional<std::string>(env) : find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded (or set DRM_CXX_TEST_CARD to a modeset card)";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;

  std::vector<TrackingSource::Event> transcript;
  auto destroyed = std::make_shared<bool>(false);

  auto inner = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(inner.has_value()) << inner.error().message();
  LayerDesc layer;
  layer.source = std::make_unique<TrackingSource>(std::move(*inner), transcript, destroyed);
  layer.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.zpos = 1;
  auto handle_r = fx.scene->add_layer(std::move(layer));
  ASSERT_TRUE(handle_r.has_value()) << handle_r.error().message();

  // Two commits: acquisitions 0 and 1 are in flight, neither released yet.
  ASSERT_TRUE(fx.scene->commit().has_value());
  ASSERT_TRUE(fx.scene->commit().has_value());
  ASSERT_TRUE(released_ids(transcript).empty());

  // Remove the layer while both buffers are still on the deferred-release ring.
  fx.scene->remove_layer(*handle_r);
  // The regression guard: removal must NOT synchronously release the on-scan
  // buffers, nor destroy the source.
  EXPECT_TRUE(released_ids(transcript).empty())
      << "remove_layer must defer, not synchronously release, in-flight buffers";
  EXPECT_FALSE(*destroyed) << "the source must outlive its in-flight buffers";

  // The retired source's buffers drain through the ring on the next commits.
  ASSERT_TRUE(fx.scene->commit().has_value());  // releases id 0
  EXPECT_EQ(released_ids(transcript).size(), 1U);
  EXPECT_FALSE(*destroyed);
  ASSERT_TRUE(fx.scene->commit().has_value());  // releases id 1
  EXPECT_EQ(released_ids(transcript).size(), 2U);
  EXPECT_FALSE(*destroyed) << "destroyed only after every buffer has drained, not before";

  // A further commit rotates the retire ring far enough to destroy the source —
  // strictly after both releases.
  ASSERT_TRUE(fx.scene->commit().has_value());
  EXPECT_TRUE(*destroyed) << "the retired source must eventually be destroyed";
  EXPECT_EQ(released_ids(transcript).size(), 2U);

  fx.scene.reset();  // drain teardown releases while the transcript is alive
  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

// replace_source keeps the layer (handle, geometry, plane) and swaps only the
// source. The displaced source's still-in-flight buffers must release to IT (not
// the replacement), deferred, and it must be destroyed only afterward.
TEST(LayerSceneReleaseVkms, ReplaceSourceRetiresOldToProducer) {
  const char* env = std::getenv("DRM_CXX_TEST_CARD");
  const std::optional<std::string> node =
      (env != nullptr && *env != '\0') ? std::optional<std::string>(env) : find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded (or set DRM_CXX_TEST_CARD to a modeset card)";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;
  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;

  std::vector<TrackingSource::Event> t_old;  // the source we replace
  std::vector<TrackingSource::Event> t_new;  // the replacement
  auto old_destroyed = std::make_shared<bool>(false);

  auto inner_old = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(inner_old.has_value()) << inner_old.error().message();
  LayerDesc layer;
  layer.source = std::make_unique<TrackingSource>(std::move(*inner_old), t_old, old_destroyed);
  layer.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.zpos = 1;
  auto handle_r = fx.scene->add_layer(std::move(layer));
  ASSERT_TRUE(handle_r.has_value()) << handle_r.error().message();

  // Two commits: the old source has acquisitions 0 and 1 in flight.
  ASSERT_TRUE(fx.scene->commit().has_value());
  ASSERT_TRUE(fx.scene->commit().has_value());
  ASSERT_TRUE(released_ids(t_old).empty());

  // Swap the source. The old source's in-flight buffers are still on the ring.
  auto inner_new = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(inner_new.has_value()) << inner_new.error().message();
  auto rep = fx.scene->replace_source(
      *handle_r, std::make_unique<TrackingSource>(std::move(*inner_new), t_new));
  ASSERT_TRUE(rep.has_value()) << rep.error().message();
  // No synchronous release or destroy of the displaced source.
  EXPECT_TRUE(released_ids(t_old).empty())
      << "replace_source must defer, not synchronously release, in-flight buffers";
  EXPECT_FALSE(*old_destroyed);

  // Next commits acquire from the NEW source; the OLD source's buffers drain to
  // IT (not the replacement).
  ASSERT_TRUE(fx.scene->commit().has_value());  // acquires new #0, releases old #0
  EXPECT_EQ(count_acquires(t_new), 1U) << "the new source must be acquired going forward";
  EXPECT_EQ(released_ids(t_old).size(), 1U);
  EXPECT_TRUE(released_ids(t_new).empty()) << "the old buffers must not release to the new source";

  ASSERT_TRUE(fx.scene->commit().has_value());  // releases old #1
  EXPECT_EQ(released_ids(t_old).size(), 2U);
  EXPECT_TRUE(released_ids(t_new).empty());
  EXPECT_FALSE(*old_destroyed) << "destroyed only after both its buffers have drained";

  // A further commit rotates the source-retire ring enough to destroy the old
  // source — strictly after its releases.
  ASSERT_TRUE(fx.scene->commit().has_value());
  EXPECT_TRUE(*old_destroyed) << "the retired source must eventually be destroyed";

  // Destroy the scene while the transcripts are still alive: teardown drains the
  // new source's in-flight buffers back to it, which records into t_new.
  fx.scene.reset();
  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

TEST(LayerSceneReleaseVkms, ReplaceSourceRejectsBadArgs) {
  const char* env = std::getenv("DRM_CXX_TEST_CARD");
  const std::optional<std::string> node =
      (env != nullptr && *env != '\0') ? std::optional<std::string>(env) : find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded (or set DRM_CXX_TEST_CARD to a modeset card)";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;
  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;

  std::vector<TrackingSource::Event> transcript;
  auto inner = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(inner.has_value());
  LayerDesc layer;
  layer.source = std::make_unique<TrackingSource>(std::move(*inner), transcript);
  layer.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.zpos = 1;
  auto handle_r = fx.scene->add_layer(std::move(layer));
  ASSERT_TRUE(handle_r.has_value());

  // Null source is rejected.
  EXPECT_FALSE(fx.scene->replace_source(*handle_r, nullptr).has_value());

  // A stale handle (its layer was removed) is rejected, not a crash.
  auto inner2 = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(inner2.has_value());
  fx.scene->remove_layer(*handle_r);
  auto rep = fx.scene->replace_source(
      *handle_r, std::make_unique<TrackingSource>(std::move(*inner2), transcript));
  EXPECT_FALSE(rep.has_value());
  EXPECT_EQ(rep.error(), std::make_error_code(std::errc::invalid_argument));

  fx.scene.reset();  // drain teardown releases while the transcript is alive
  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

// A failed real commit must release exactly the frame's own acquisitions
// immediately — no flip is in flight to gate their release on — while leaving
// the buffers legitimately in flight from earlier successful commits untouched,
// and a non-EACCES failure must leave the scene paintable. Consumer self-healing
// designs (a source ring reusing buffers after a rejected commit) now depend on
// this, so pin it against silent regression: after steady-state presentation,
// N consecutive injected commit failures each return the source's outstanding-
// lease count to the pre-failure baseline, and commit N+1 still presents.
//
// The failure is injected without a fault-injecting driver via the two-phase
// commit API: build_frame_into() acquires from the source and appends the
// property writes, then finalize_frame() is handed a simulated kernel error
// in place of a real req.commit().
TEST(LayerSceneReleaseVkms, CommitFailureReleasesAcquisitions) {
  const char* env = std::getenv("DRM_CXX_TEST_CARD");
  const std::optional<std::string> node =
      (env != nullptr && *env != '\0') ? std::optional<std::string>(env) : find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded (or set DRM_CXX_TEST_CARD to a modeset card)";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;

  std::vector<TrackingSource::Event> transcript;
  auto inner = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(inner.has_value()) << inner.error().message();

  LayerDesc layer;
  layer.source = std::make_unique<TrackingSource>(std::move(*inner), transcript);
  layer.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.zpos = 1;
  ASSERT_TRUE(fx.scene->add_layer(std::move(layer)).has_value());

  // Establish steady state: two real commits so the mode is set and the
  // deferred-release ring holds two in-flight buffers. This mirrors a
  // producer ring that has been presenting frames before a commit is
  // rejected — the case the self-healing contract exists for.
  ASSERT_TRUE(fx.scene->commit().has_value());
  ASSERT_TRUE(fx.scene->commit().has_value());
  const std::size_t baseline = outstanding_leases(transcript);
  ASSERT_EQ(baseline, 2U) << "two commits should leave two buffers deferred in flight";

  // Inject N consecutive commit failures. Each build acquires one buffer;
  // the failed finalize must release exactly that buffer (no flip is in
  // flight to gate it on) and leave the two legitimately in-flight buffers
  // untouched — so outstanding leases return to the pre-failure baseline.
  constexpr int k_failures = 3;
  for (int i = 0; i < k_failures; ++i) {
    const std::size_t acquires_before = count_acquires(transcript);

    drm::AtomicRequest req(*fx.dev);
    ASSERT_TRUE(req.valid());
    auto build = fx.scene->build_frame_into(req, 0, /*test_only=*/false);
    ASSERT_TRUE(build.has_value()) << build.error().message();
    ASSERT_TRUE(*build) << "scene unexpectedly suspended before failure " << i;
    ASSERT_EQ(count_acquires(transcript), acquires_before + 1)
        << "build_frame_into should acquire exactly one buffer on failure " << i;

    // EINVAL, not EACCES: a transient rejection that must NOT suspend the scene.
    auto fin = fx.scene->finalize_frame(
        std::move(*build),
        drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument)));
    EXPECT_FALSE(fin.has_value()) << "commit failure " << i << " should propagate";
    EXPECT_EQ(fin.error(), std::make_error_code(std::errc::invalid_argument));

    EXPECT_EQ(outstanding_leases(transcript), baseline)
        << "commit failure " << i
        << " must release its own acquisition immediately and touch nothing else";
  }

  // Frame N+1: the ring is still paintable — a real commit succeeds.
  auto ok = fx.scene->commit();
  ASSERT_TRUE(ok.has_value()) << "ring must be paintable after " << k_failures
                              << " commit failures: " << ok.error().message();

  fx.scene.reset();  // drain the in-flight ring while the transcript is alive
  EXPECT_EQ(outstanding_leases(transcript), 0U)
      << "scene destruction must release every still-held in-flight acquisition";

  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}

// drain() must land the page-flip event armed by the last real commit so the
// scene can be torn down without leaving an event queued against a buffer it
// is about to release (the classic RmFB-on-in-flight-FB hazard). After drain()
// returns, the flip has been dispatched and the kernel's event queue is empty.
TEST(LayerSceneReleaseVkms, DrainLandsPendingFlipBeforeTeardown) {
  const char* env = std::getenv("DRM_CXX_TEST_CARD");
  const std::optional<std::string> node =
      (env != nullptr && *env != '\0') ? std::optional<std::string>(env) : find_vkms_node();
  if (!node) {
    GTEST_SKIP() << "VKMS not loaded (or set DRM_CXX_TEST_CARD to a modeset card)";
  }
  auto fx_r = open_vkms_scene(*node);
  ASSERT_TRUE(fx_r.has_value()) << fx_r.error().message();
  auto& fx = *fx_r;

  const auto fb_w = fx.active.mode.hdisplay;
  const auto fb_h = fx.active.mode.vdisplay;

  std::vector<TrackingSource::Event> transcript;
  auto inner = DumbBufferSource::create(*fx.dev, fb_w, fb_h, DRM_FORMAT_ARGB8888);
  ASSERT_TRUE(inner.has_value()) << inner.error().message();

  LayerDesc layer;
  layer.source = std::make_unique<TrackingSource>(std::move(*inner), transcript);
  layer.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  layer.display.zpos = 1;
  ASSERT_TRUE(fx.scene->add_layer(std::move(layer)).has_value());

  drm::PageFlip pf(*fx.dev);
  int flips_seen = 0;
  pf.set_handler(
      [&](std::uint32_t /*crtc*/, std::uint64_t /*seq*/, std::uint64_t /*ts*/) { ++flips_seen; });

  // Establish the mode with a plain commit, then a flip commit that arms the
  // page-flip event (user_data routes the completion back through pf).
  ASSERT_TRUE(fx.scene->commit().has_value());
  auto flip = fx.scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &pf);
  ASSERT_TRUE(flip.has_value()) << flip.error().message();

  // drain() lands exactly that event.
  auto d = fx.scene->drain(pf, 1000);
  ASSERT_TRUE(d.has_value()) << d.error().message();
  EXPECT_EQ(flips_seen, 1) << "drain should have dispatched the pending flip event";

  // No event left in the queue: a non-blocking dispatch now times out.
  auto extra = pf.dispatch(0);
  EXPECT_FALSE(extra.has_value());
  EXPECT_EQ(extra.error(), std::make_error_code(std::errc::timed_out));

  // With nothing armed, drain() is an immediate no-op that never touches pf.
  auto again = fx.scene->drain(pf, 0);
  EXPECT_TRUE(again.has_value()) << again.error().message();
  EXPECT_EQ(flips_seen, 1);

  fx.scene.reset();  // safe: the in-flight flip already landed
  cleanup_crtc(fx.dev->fd(), fx.active.crtc_id);
}
