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
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/scene/buffer_source.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/scene_set.hpp>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <memory>
#include <system_error>
#include <unistd.h>

namespace {

// LayerBufferSource subclass used only for SceneSetLayerSpec
// construction in validation-gate tests. SceneSet::add_layer rejects
// invalid specs before touching the underlying source's vtable, so
// none of the overrides are ever called; CHECK-failing in them would
// catch any accidental progress past the validation gate.
class FakeSource : public drm::scene::LayerBufferSource {
 public:
  drm::expected<drm::scene::AcquiredBuffer, std::error_code> acquire() override {
    return drm::unexpected<std::error_code>(
        std::make_error_code(std::errc::operation_not_supported));
  }
  void release(drm::scene::AcquiredBuffer /*acquired*/) noexcept override {}
  [[nodiscard]] drm::scene::BindingModel binding_model() const noexcept override {
    return drm::scene::BindingModel::SceneSubmitsFbId;
  }
  [[nodiscard]] drm::scene::SourceFormat format() const noexcept override { return {}; }
};

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

// ─── add_layer / remove_layer validation ───────────────────────────
//
// The full kernel path is exercised by the bare-TTY multi_crtc_probe
// `--scene-test --mirror` smoke; these unit tests cover the validation
// gate that runs before any LayerScene::add_layer call.

TEST(SceneSetLayerSpec, AddLayerRejectsNullSource) {
  const NullFd nfd;
  ASSERT_GE(nfd.fd(), 0);
  auto dev = drm::Device::from_fd(nfd.fd());
  auto set = drm::scene::SceneSet::create(dev, {});
  ASSERT_TRUE(set.has_value());

  drm::scene::SceneSetLayerSpec spec;
  spec.source = nullptr;
  spec.targets.push_back({.scene_index = 0, .display = {}, .force_composited = false});

  auto h = (*set)->add_layer(spec);
  ASSERT_FALSE(h.has_value());
  EXPECT_EQ(h.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneSetLayerSpec, AddLayerRejectsEmptyTargets) {
  const NullFd nfd;
  ASSERT_GE(nfd.fd(), 0);
  auto dev = drm::Device::from_fd(nfd.fd());
  auto set = drm::scene::SceneSet::create(dev, {});
  ASSERT_TRUE(set.has_value());

  drm::scene::SceneSetLayerSpec spec;
  spec.source = std::make_shared<FakeSource>();
  // targets left empty intentionally.

  auto h = (*set)->add_layer(spec);
  ASSERT_FALSE(h.has_value());
  EXPECT_EQ(h.error(), std::make_error_code(std::errc::invalid_argument));
}

TEST(SceneSetLayerSpec, AddLayerRejectsOutOfRangeSceneIndex) {
  const NullFd nfd;
  ASSERT_GE(nfd.fd(), 0);
  auto dev = drm::Device::from_fd(nfd.fd());
  auto set = drm::scene::SceneSet::create(dev, {});  // zero scenes
  ASSERT_TRUE(set.has_value());

  drm::scene::SceneSetLayerSpec spec;
  spec.source = std::make_shared<FakeSource>();
  // Index 0 is out of range against an empty scene list.
  spec.targets.push_back({.scene_index = 0, .display = {}, .force_composited = false});

  auto h = (*set)->add_layer(spec);
  ASSERT_FALSE(h.has_value());
  EXPECT_EQ(h.error(), std::make_error_code(std::errc::invalid_argument));
}

// ─── add_scene / remove_scene validation ───────────────────────────
//
// The full hotplug round-trip (real LayerScene constructed against a
// vkms output, attached/detached, set commit observed) lives in the
// vkms integration test. These unit tests cover the validation gate
// only — LayerScene cannot be constructed without a real DRM device.

TEST(SceneSetAddScene, RejectsNull) {
  const NullFd nfd;
  ASSERT_GE(nfd.fd(), 0);
  auto dev = drm::Device::from_fd(nfd.fd());
  auto set = drm::scene::SceneSet::create(dev, {});
  ASSERT_TRUE(set.has_value());

  auto idx = (*set)->add_scene(std::unique_ptr<drm::scene::LayerScene>{});
  ASSERT_FALSE(idx.has_value());
  EXPECT_EQ(idx.error(), std::make_error_code(std::errc::invalid_argument));
  EXPECT_EQ((*set)->scene_count(), 0U);
}

TEST(SceneSetRemoveScene, OutOfRangeIsNoOp) {
  const NullFd nfd;
  ASSERT_GE(nfd.fd(), 0);
  auto dev = drm::Device::from_fd(nfd.fd());
  auto set = drm::scene::SceneSet::create(dev, {});
  ASSERT_TRUE(set.has_value());

  (*set)->remove_scene(0);   // empty set
  (*set)->remove_scene(42);  // far past the end
  EXPECT_EQ((*set)->scene_count(), 0U);
}

TEST(SceneSetCommit, AllNullScenesSkipsKernelCommit) {
  // After every scene has been removed, scenes_ holds only holes; the
  // narrow per-CRTC fallback must return per-scene zero reports without
  // touching the kernel. With the test fd backed by /dev/null an actual
  // ioctl would surface as ENOTTY — a successful empty CommitReport
  // vector confirms we bypassed it.
  const NullFd nfd;
  ASSERT_GE(nfd.fd(), 0);
  auto dev = drm::Device::from_fd(nfd.fd());
  auto set = drm::scene::SceneSet::create(dev, {});
  ASSERT_TRUE(set.has_value());

  auto reports = (*set)->commit();
  ASSERT_TRUE(reports.has_value()) << reports.error().message();
  EXPECT_TRUE(reports->empty());
}

// ─── NarrowPolicy partition planner ────────────────────────────────
//
// Pure-logic tests for drm::scene::detail::partition_for_policy. They
// exercise the three policies (Combined / AutoOnModeset / PerCrtc)
// over hand-built slot vectors without needing a real LayerScene.

namespace {

using SlotVec = std::vector<drm::scene::detail::SceneSlotState>;

drm::scene::detail::SceneSlotState slot_engaged(bool wants_modeset) noexcept {
  return {.is_hole = false, .wants_modeset = wants_modeset};
}
drm::scene::detail::SceneSlotState slot_hole() noexcept {
  return {.is_hole = true, .wants_modeset = false};
}

}  // namespace

TEST(SceneSetPartition, EmptySlotsYieldsNoGroups) {
  EXPECT_TRUE(
      drm::scene::detail::partition_for_policy({}, drm::scene::NarrowPolicy::Combined).empty());
  EXPECT_TRUE(drm::scene::detail::partition_for_policy({}, drm::scene::NarrowPolicy::AutoOnModeset)
                  .empty());
  EXPECT_TRUE(
      drm::scene::detail::partition_for_policy({}, drm::scene::NarrowPolicy::PerCrtc).empty());
}

TEST(SceneSetPartition, AllHolesYieldsNoGroups) {
  const SlotVec slots{slot_hole(), slot_hole(), slot_hole()};
  EXPECT_TRUE(
      drm::scene::detail::partition_for_policy(slots, drm::scene::NarrowPolicy::Combined).empty());
  EXPECT_TRUE(
      drm::scene::detail::partition_for_policy(slots, drm::scene::NarrowPolicy::AutoOnModeset)
          .empty());
  EXPECT_TRUE(
      drm::scene::detail::partition_for_policy(slots, drm::scene::NarrowPolicy::PerCrtc).empty());
}

TEST(SceneSetPartition, CombinedAlwaysOneGroupWithEveryNonHoleSlot) {
  const SlotVec slots{slot_engaged(true), slot_hole(), slot_engaged(false), slot_engaged(true)};
  const auto groups =
      drm::scene::detail::partition_for_policy(slots, drm::scene::NarrowPolicy::Combined);
  ASSERT_EQ(groups.size(), 1U);
  EXPECT_EQ(groups[0], (std::vector<std::size_t>{0, 2, 3}));
}

TEST(SceneSetPartition, PerCrtcOneGroupPerNonHoleSlot) {
  const SlotVec slots{slot_engaged(false), slot_hole(), slot_engaged(true), slot_engaged(false)};
  const auto groups =
      drm::scene::detail::partition_for_policy(slots, drm::scene::NarrowPolicy::PerCrtc);
  ASSERT_EQ(groups.size(), 3U);
  EXPECT_EQ(groups[0], (std::vector<std::size_t>{0}));
  EXPECT_EQ(groups[1], (std::vector<std::size_t>{2}));
  EXPECT_EQ(groups[2], (std::vector<std::size_t>{3}));
}

TEST(SceneSetPartition, AutoOnModesetUniformAllSteadyYieldsOneGroup) {
  const SlotVec slots{slot_engaged(false), slot_engaged(false), slot_engaged(false)};
  const auto groups =
      drm::scene::detail::partition_for_policy(slots, drm::scene::NarrowPolicy::AutoOnModeset);
  ASSERT_EQ(groups.size(), 1U);
  EXPECT_EQ(groups[0], (std::vector<std::size_t>{0, 1, 2}));
}

TEST(SceneSetPartition, AutoOnModesetUniformAllModesetYieldsOneGroup) {
  const SlotVec slots{slot_engaged(true), slot_engaged(true)};
  const auto groups =
      drm::scene::detail::partition_for_policy(slots, drm::scene::NarrowPolicy::AutoOnModeset);
  ASSERT_EQ(groups.size(), 1U);
  EXPECT_EQ(groups[0], (std::vector<std::size_t>{0, 1}));
}

TEST(SceneSetPartition, AutoOnModesetMixedSplitsWithModesetFirst) {
  // Slot 1 + 3 want modeset; 0 + 2 are steady. Expect two groups,
  // modeset-needing one first, each carrying its scenes' indices in
  // ascending order.
  const SlotVec slots{slot_engaged(false), slot_engaged(true), slot_engaged(false),
                      slot_engaged(true)};
  const auto groups =
      drm::scene::detail::partition_for_policy(slots, drm::scene::NarrowPolicy::AutoOnModeset);
  ASSERT_EQ(groups.size(), 2U);
  EXPECT_EQ(groups[0], (std::vector<std::size_t>{1, 3}));
  EXPECT_EQ(groups[1], (std::vector<std::size_t>{0, 2}));
}

TEST(SceneSetPartition, AutoOnModesetIgnoresHolesInModesetClassification) {
  // Hole at slot 1 should not contribute to either group; the
  // surrounding slots still classify correctly.
  const SlotVec slots{slot_engaged(true), slot_hole(), slot_engaged(false)};
  const auto groups =
      drm::scene::detail::partition_for_policy(slots, drm::scene::NarrowPolicy::AutoOnModeset);
  ASSERT_EQ(groups.size(), 2U);
  EXPECT_EQ(groups[0], (std::vector<std::size_t>{0}));
  EXPECT_EQ(groups[1], (std::vector<std::size_t>{2}));
}

TEST(SceneSetRemoveLayer, StaleHandleIsNoOp) {
  const NullFd nfd;
  ASSERT_GE(nfd.fd(), 0);
  auto dev = drm::Device::from_fd(nfd.fd());
  auto set = drm::scene::SceneSet::create(dev, {});
  ASSERT_TRUE(set.has_value());

  // Default-constructed handle has id=0 → silent no-op.
  const drm::scene::SetLayerHandle stale{};
  EXPECT_FALSE(stale.valid());
  (*set)->remove_layer(stale);

  // Non-default-but-never-issued handle — also a no-op.
  const drm::scene::SetLayerHandle never_issued{.id = 99, .generation = 1};
  (*set)->remove_layer(never_issued);
}
