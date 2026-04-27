// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Unit tests for drm::BufferMapping. Drives the move-only RAII contract
// against an in-test unmap counter — the real backends (dumb, gbm)
// produce mappings whose unmap path is exercised by integration tests.

#include "buffer_mapping.hpp"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <utility>

namespace {

// In-test backend: a counter the unmap callback bumps so move /
// destruction semantics can be observed without a live DRM fd.
struct UnmapTrap {
  int destroyed{0};
};

drm::BufferMapping make_trap_mapping(std::uint8_t* pixels, std::size_t bytes, std::uint32_t stride,
                                     std::uint32_t width, std::uint32_t height,
                                     drm::MapAccess access, UnmapTrap* trap) {
  return {pixels,
          bytes,
          stride,
          width,
          height,
          access,
          [](void* ctx) noexcept { ++static_cast<UnmapTrap*>(ctx)->destroyed; },
          trap};
}

}  // namespace

TEST(BufferMapping, DefaultIsEmptyAndDestructorIsNoOp) {
  const drm::BufferMapping m;
  EXPECT_TRUE(m.empty());
  EXPECT_EQ(m.stride(), 0U);
  EXPECT_EQ(m.width(), 0U);
  EXPECT_EQ(m.height(), 0U);
  EXPECT_TRUE(m.pixels().empty());
  // Falls out of scope without invoking any unmap function (none was
  // registered).
}

TEST(BufferMapping, DestructorCallsUnmapOnce) {
  alignas(4) std::uint8_t storage[16]{};
  UnmapTrap trap;
  {
    auto m =
        make_trap_mapping(storage, sizeof(storage), 8U, 2U, 2U, drm::MapAccess::ReadWrite, &trap);
    EXPECT_FALSE(m.empty());
    EXPECT_EQ(m.stride(), 8U);
    EXPECT_EQ(m.width(), 2U);
    EXPECT_EQ(m.height(), 2U);
    EXPECT_EQ(m.access(), drm::MapAccess::ReadWrite);
    EXPECT_EQ(trap.destroyed, 0);
  }
  EXPECT_EQ(trap.destroyed, 1);
}

TEST(BufferMapping, MoveConstructTransfersUnmapOwnership) {
  alignas(4) std::uint8_t storage[16]{};
  UnmapTrap trap;

  auto src = make_trap_mapping(storage, sizeof(storage), 8U, 2U, 2U, drm::MapAccess::Write, &trap);
  drm::BufferMapping dst(std::move(src));

  // Source is now empty; destruction should NOT call the unmap.
  // NOLINTNEXTLINE(bugprone-use-after-move) — checking the contract
  EXPECT_TRUE(src.empty());
  EXPECT_FALSE(dst.empty());
  EXPECT_EQ(trap.destroyed, 0);

  // Destroy dst — unmap should fire exactly once.
  {
    const drm::BufferMapping consumed = std::move(dst);
  }
  EXPECT_EQ(trap.destroyed, 1);
}

TEST(BufferMapping, MoveAssignDestroysExistingTarget) {
  alignas(4) std::uint8_t storage_a[16]{};
  alignas(4) std::uint8_t storage_b[16]{};
  UnmapTrap trap_a;
  UnmapTrap trap_b;

  auto a =
      make_trap_mapping(storage_a, sizeof(storage_a), 8U, 2U, 2U, drm::MapAccess::Read, &trap_a);
  auto b =
      make_trap_mapping(storage_b, sizeof(storage_b), 8U, 2U, 2U, drm::MapAccess::Write, &trap_b);

  // Overwrite a with b — a's existing unmap fires, b moves in.
  a = std::move(b);
  EXPECT_EQ(trap_a.destroyed, 1);
  EXPECT_EQ(trap_b.destroyed, 0);
  // NOLINTNEXTLINE(bugprone-use-after-move) — checking the contract
  EXPECT_TRUE(b.empty());
  EXPECT_FALSE(a.empty());
  EXPECT_EQ(a.access(), drm::MapAccess::Write);

  // a (carrying b's payload) goes out of scope — b's unmap fires.
  {
    const drm::BufferMapping consumed = std::move(a);
  }
  EXPECT_EQ(trap_b.destroyed, 1);
}

TEST(BufferMapping, SelfMoveAssignmentIsHarmless) {
  alignas(4) std::uint8_t storage[16]{};
  UnmapTrap trap;
  auto m = make_trap_mapping(storage, sizeof(storage), 8U, 2U, 2U, drm::MapAccess::Read, &trap);

  // Self-move must not unmap the live mapping out from under itself.
  // The literal `m = std::move(m)` form is what `BufferMapping::operator=`'s
  // `if (this != &other)` guard exists for — drive that branch by routing
  // the move through a reference so neither `-Wself-move` nor clang-tidy's
  // self-move check fires on the syntactic shape.
  auto& self = m;
  m = std::move(self);
  EXPECT_EQ(trap.destroyed, 0);
  EXPECT_FALSE(m.empty());
}
