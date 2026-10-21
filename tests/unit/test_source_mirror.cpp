// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// detail::SourceMirror against an in-memory ring: one acquire per frame
// shared by every consumer, buffers returned once all consumers are done,
// session hooks once per cycle.

#include "scene/buffer_source.hpp"
#include "scene/detail/source_mirror.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/sync/fence.hpp>

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

using drm::scene::AcquiredBuffer;
using drm::scene::BindingModel;
using drm::scene::LayerBufferSource;
using drm::scene::SourceFormat;
using drm::scene::detail::SourceMirror;

namespace {

// Each acquire hands out the next fb id, 1, 2, 3, ... and a one-rect damage
// list tagged with it. Records what comes back.
class FakeRing : public LayerBufferSource {
 public:
  drm::expected<AcquiredBuffer, std::error_code> acquire() override {
    if (empty) {
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    ++acquires;
    AcquiredBuffer b;
    b.fb_id = next_fb++;
    b.damage.push_back({0, 0, b.fb_id, 1});
    if (fence_fd >= 0) {
      if (auto f = drm::sync::SyncFence::import_fd(fence_fd)) {
        b.acquire_fence = std::move(*f);
      }
    }
    return b;
  }
  void release(AcquiredBuffer acquired) noexcept override { released.push_back(acquired.fb_id); }
  void on_retired() noexcept override { ++retired; }
  [[nodiscard]] BindingModel binding_model() const noexcept override {
    return BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] SourceFormat format() const noexcept override { return {}; }
  void on_session_paused() noexcept override { ++paused; }
  drm::expected<void, std::error_code> on_session_resumed(const drm::Device& /*dev*/) override {
    ++resumed;
    return {};
  }

  bool empty{false};
  int fence_fd{-1};
  std::uint32_t next_fb{1};
  int acquires{0};
  int retired{0};
  int paused{0};
  int resumed{0};
  std::vector<std::uint32_t> released;
};

struct Rig {
  std::shared_ptr<FakeRing> ring = std::make_shared<FakeRing>();
  std::shared_ptr<SourceMirror> mirror = SourceMirror::create(ring);
  std::unique_ptr<LayerBufferSource> a = mirror->make_consumer();
  std::unique_ptr<LayerBufferSource> b = mirror->make_consumer();
};

AcquiredBuffer take(LayerBufferSource& c) {
  auto r = c.acquire();
  EXPECT_TRUE(r.has_value());
  return r ? std::move(*r) : AcquiredBuffer{};
}

}  // namespace

TEST(SourceMirror, ConsumersShareOneAcquirePerFrame) {
  Rig r;
  auto a1 = take(*r.a);
  auto b1 = take(*r.b);
  EXPECT_EQ(a1.fb_id, 1U);
  EXPECT_EQ(b1.fb_id, 1U);
  EXPECT_EQ(r.ring->acquires, 1);
  auto a2 = take(*r.a);
  auto b2 = take(*r.b);
  EXPECT_EQ(a2.fb_id, 2U);
  EXPECT_EQ(b2.fb_id, 2U);
  EXPECT_EQ(r.ring->acquires, 2);
}

TEST(SourceMirror, BufferReturnsOnceEveryConsumerIsDoneAndItIsReplaced) {
  Rig r;
  auto a1 = take(*r.a);
  auto b1 = take(*r.b);
  r.a->release(std::move(a1));
  r.b->release(std::move(b1));
  EXPECT_TRUE(r.ring->released.empty()) << "frame 1 is still the newest";
  auto a2 = take(*r.a);
  EXPECT_EQ(r.ring->released, std::vector<std::uint32_t>{1});

  auto b2 = take(*r.b);
  auto a3 = take(*r.a);
  r.a->release(std::move(a2));
  EXPECT_EQ(r.ring->released.size(), 1U) << "b still shows frame 2";
  r.b->release(std::move(b2));
  EXPECT_EQ(r.ring->released, (std::vector<std::uint32_t>{1, 2}));
}

TEST(SourceMirror, AConsumerCommittingAloneAdvancesAndTheOtherSkips) {
  Rig r;
  for (std::uint32_t fb = 1; fb <= 3; ++fb) {
    auto a = take(*r.a);
    EXPECT_EQ(a.fb_id, fb);
    r.a->release(std::move(a));
  }
  auto b = take(*r.b);
  EXPECT_EQ(b.fb_id, 3U) << "b takes the newest frame";
  EXPECT_TRUE(b.damage.empty()) << "b missed frames 1-2: full damage";
}

TEST(SourceMirror, DamagePassesOnlyToAConsumerThatShowedThePreviousFrame) {
  Rig r;
  auto a1 = take(*r.a);
  auto b1 = take(*r.b);
  auto a2 = take(*r.a);
  auto b2 = take(*r.b);
  ASSERT_EQ(a2.damage.size(), 1U);
  ASSERT_EQ(b2.damage.size(), 1U);
  EXPECT_EQ(b2.damage[0].w, 2U);
}

TEST(SourceMirror, TryAgainOnlyWhenTheConsumerHasTheNewestFrame) {
  Rig r;
  auto a1 = take(*r.a);
  r.ring->empty = true;
  auto b1 = r.b->acquire();
  ASSERT_TRUE(b1.has_value()) << "b has not shown frame 1 yet";
  EXPECT_EQ(b1->fb_id, 1U);
  auto a2 = r.a->acquire();
  ASSERT_FALSE(a2.has_value());
  EXPECT_EQ(a2.error(), std::make_error_code(std::errc::resource_unavailable_try_again));
  EXPECT_TRUE(r.a->has_fresh_content()) << "the ring's own answer";
}

TEST(SourceMirror, FreshContentIsTheNewestFrameNotYetShown) {
  Rig r;
  auto a1 = take(*r.a);
  EXPECT_TRUE(r.b->has_fresh_content());
}

TEST(SourceMirror, EachConsumerGetsItsOwnAcquireFence) {
  std::array<int, 2> fds{};
  ASSERT_EQ(::pipe(fds.data()), 0);
  Rig r;
  r.ring->fence_fd = fds[0];
  auto a1 = take(*r.a);
  auto b1 = take(*r.b);
  ASSERT_TRUE(a1.acquire_fence.has_value());
  ASSERT_TRUE(b1.acquire_fence.has_value());
  EXPECT_NE(a1.acquire_fence->fd(), b1.acquire_fence->fd());
  ::close(fds[0]);
  ::close(fds[1]);
}

TEST(SourceMirror, SessionHooksReachTheSourceOncePerCycle) {
  Rig r;
  auto a1 = take(*r.a);
  r.a->release(std::move(a1));
  r.a->on_session_paused();
  r.b->on_session_paused();
  EXPECT_EQ(r.ring->paused, 1);
  EXPECT_EQ(r.ring->released, std::vector<std::uint32_t>{1}) << "the old session's frame goes back";
  const auto dev = drm::Device::from_fd(-1);
  EXPECT_TRUE(r.a->on_session_resumed(dev).has_value());
  EXPECT_TRUE(r.b->on_session_resumed(dev).has_value());
  EXPECT_EQ(r.ring->resumed, 1);
  auto b2 = take(*r.b);
  EXPECT_EQ(b2.fb_id, 2U) << "no frame from before the pause";
}

TEST(SourceMirror, SourceRetiresWhenEveryConsumerHas) {
  Rig r;
  auto a1 = take(*r.a);
  r.a->release(std::move(a1));
  r.a->on_retired();
  EXPECT_EQ(r.ring->retired, 0);
  r.b->on_retired();
  EXPECT_EQ(r.ring->retired, 1);
  EXPECT_EQ(r.ring->released, std::vector<std::uint32_t>{1});
  r.a.reset();
  r.b.reset();
  r.mirror.reset();
  EXPECT_EQ(r.ring->retired, 1) << "retired once";
}

TEST(SourceMirror, DestroyingTheConsumersReturnsWhatIsLeft) {
  Rig r;
  auto a1 = take(*r.a);
  r.a->release(std::move(a1));
  r.a.reset();
  r.b.reset();
  r.mirror.reset();
  EXPECT_EQ(r.ring->released, std::vector<std::uint32_t>{1});
  EXPECT_EQ(r.ring->retired, 1);
}
