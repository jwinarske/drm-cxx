// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Exercises drm::cursor::Cursor against synthesized XCursor files.
// Writing the files byte-for-byte (format lifted from libxcursor's
// file.c) avoids depending on a system icon theme and lets us probe
// edge cases the spec permits but real themes rarely ship — oversized
// frames, all-zero-delay animations, etc.

#include "cursor/cursor.hpp"
#include "cursor/theme.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <ios>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

// XCursor file-format constants lifted from libxcursor/include/X11/Xcursor/Xcursor.h.
constexpr std::uint32_t k_xc_file_header_size = 16;
constexpr std::uint32_t k_xc_file_version = 0x10000U;
constexpr std::uint32_t k_xc_toc_entry_size = 12;
constexpr std::uint32_t k_xc_image_type = 0xfffd0002U;
constexpr std::uint32_t k_xc_image_header_size = 36;
constexpr std::uint32_t k_xc_image_version = 1;

struct FrameSpec {
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t xhot;
  std::uint32_t yhot;
  std::uint32_t delay;       // ms; 0 means static frame in the XCursor sense
  std::uint32_t color_argb;  // solid fill — we don't test pixel content here
};

// Write a minimal-but-valid XCursor file at `path` with the given frames.
// All frames are assumed to share the same nominal size (their `width`),
// which is the common case for animated cursors.
void write_xcursor_file(const fs::path& path, const std::vector<FrameSpec>& frames) {
  std::vector<std::uint8_t> bytes;
  const auto push_u32 = [&](std::uint32_t v) {
    bytes.push_back(static_cast<std::uint8_t>(v & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xffU));
  };

  // File header.
  bytes.insert(bytes.end(), {'X', 'c', 'u', 'r'});
  push_u32(k_xc_file_header_size);
  push_u32(k_xc_file_version);
  push_u32(static_cast<std::uint32_t>(frames.size()));

  // TOC. Positions accumulate as we virtually lay out the image chunks.
  std::uint32_t pos =
      k_xc_file_header_size + (static_cast<std::uint32_t>(frames.size()) * k_xc_toc_entry_size);
  for (const auto& f : frames) {
    push_u32(k_xc_image_type);
    push_u32(f.width);  // subtype = nominal size
    push_u32(pos);
    pos += k_xc_image_header_size + (f.width * f.height * 4U);
  }

  // Image chunks in TOC order.
  for (const auto& f : frames) {
    push_u32(k_xc_image_header_size);
    push_u32(k_xc_image_type);
    push_u32(f.width);
    push_u32(k_xc_image_version);
    push_u32(f.width);
    push_u32(f.height);
    push_u32(f.xhot);
    push_u32(f.yhot);
    push_u32(f.delay);
    for (std::uint32_t i = 0; i < f.width * f.height; ++i) {
      push_u32(f.color_argb);
    }
  }

  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

class CursorTest : public ::testing::Test {
 protected:
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  fs::path root;

  void SetUp() override {
    static std::atomic<int> counter{0};
    const auto suffix = std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1));
    root = fs::temp_directory_path() / fs::path("drm-cxx-cursor-test-" + suffix);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  fs::path write(const std::vector<FrameSpec>& frames, const std::string& name = "test.cur") {
    const auto p = root / name;
    write_xcursor_file(p, frames);
    return p;
  }

  // Wrap a raw filesystem path as a ThemeResolution so we can drive
  // Cursor::load directly without plumbing through Theme.
  static drm::cursor::ThemeResolution resolution_for(const fs::path& p) {
    return drm::cursor::ThemeResolution{"synthetic", p, {"synthetic"}};
  }
};

}  // namespace

TEST_F(CursorTest, LoadStaticCursorExtractsMetadata) {
  const auto p = write({
      FrameSpec{32, 32, 4, 2, 0, 0xffff00ffU},
  });
  auto c = drm::cursor::Cursor::load(resolution_for(p), 32);
  ASSERT_TRUE(c.has_value());

  EXPECT_EQ(c->frame_count(), 1U);
  EXPECT_FALSE(c->animated());
  EXPECT_EQ(c->cycle(), std::chrono::milliseconds{0});
  EXPECT_EQ(c->first().width, 32U);
  EXPECT_EQ(c->first().height, 32U);
  EXPECT_EQ(c->first().xhot, 4);
  EXPECT_EQ(c->first().yhot, 2);
  EXPECT_EQ(c->first().pixels.size(), 32U * 32U);
}

TEST_F(CursorTest, FrameAtOnStaticCursorAlwaysReturnsFirst) {
  const auto p = write({
      FrameSpec{16, 16, 0, 0, 0, 0xff00ff00U},
  });
  auto c = drm::cursor::Cursor::load(resolution_for(p), 16);
  ASSERT_TRUE(c.has_value());

  const auto& f0 = c->first();
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{0}), &f0);
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{9999}), &f0);
}

TEST_F(CursorTest, AnimatedCursorReportsCycleAndFrameCount) {
  const auto p = write({
      FrameSpec{16, 16, 8, 8, 100, 0xff0000ffU},
      FrameSpec{16, 16, 8, 8, 100, 0xff00ff00U},
      FrameSpec{16, 16, 8, 8, 100, 0xffff0000U},
  });
  auto c = drm::cursor::Cursor::load(resolution_for(p), 16);
  ASSERT_TRUE(c.has_value());

  EXPECT_EQ(c->frame_count(), 3U);
  EXPECT_TRUE(c->animated());
  EXPECT_EQ(c->cycle(), std::chrono::milliseconds{300});
}

TEST_F(CursorTest, FrameAtWalksFramesAccumulatingDelays) {
  const auto p = write({
      FrameSpec{16, 16, 0, 0, 100, 0xff0000ffU},  // frame 0: [0, 100)
      FrameSpec{16, 16, 0, 0, 100, 0xff00ff00U},  // frame 1: [100, 200)
      FrameSpec{16, 16, 0, 0, 100, 0xffff0000U},  // frame 2: [200, 300)
  });
  auto c = drm::cursor::Cursor::load(resolution_for(p), 16);
  ASSERT_TRUE(c.has_value());

  const auto& f0 = c->at(0);
  const auto& f1 = c->at(1);
  const auto& f2 = c->at(2);

  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{0}), &f0);
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{50}), &f0);
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{99}), &f0);
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{100}), &f1);
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{199}), &f1);
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{200}), &f2);
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{299}), &f2);
}

TEST_F(CursorTest, FrameAtWrapsModuloCycle) {
  const auto p = write({
      FrameSpec{16, 16, 0, 0, 100, 0xff000000U},
      FrameSpec{16, 16, 0, 0, 100, 0xff111111U},
  });
  auto c = drm::cursor::Cursor::load(resolution_for(p), 16);
  ASSERT_TRUE(c.has_value());

  const auto& f0 = c->at(0);
  const auto& f1 = c->at(1);
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{0}), &f0);
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{200}), &f0);  // full cycle wrap
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{250}), &f0);
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{350}), &f1);     // 350 % 200 = 150
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{10'000}), &f0);  // 10000 % 200 = 0
}

TEST_F(CursorTest, AllZeroDelayFramesTreatedAsStatic) {
  // Three frames, every delay=0 — cycle length is zero, so frame_at()
  // must short-circuit to first() rather than divide-by-zero.
  const auto p = write({
      FrameSpec{16, 16, 0, 0, 0, 0xff000000U},
      FrameSpec{16, 16, 0, 0, 0, 0xff111111U},
      FrameSpec{16, 16, 0, 0, 0, 0xff222222U},
  });
  auto c = drm::cursor::Cursor::load(resolution_for(p), 16);
  ASSERT_TRUE(c.has_value());

  // animated() requires a non-zero cycle so this cursor is "static".
  EXPECT_FALSE(c->animated());
  EXPECT_EQ(c->frame_count(), 3U);
  EXPECT_EQ(c->cycle(), std::chrono::milliseconds{0});
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{0}), &c->first());
  EXPECT_EQ(&c->frame_at(std::chrono::milliseconds{1000}), &c->first());
}

TEST_F(CursorTest, OversizedFrameIsRejected) {
  // 1024px > the 512px cap the loader enforces as defense against
  // malformed / malicious theme files.
  const auto p = write({
      FrameSpec{1024, 1024, 0, 0, 0, 0xffffffffU},
  });
  auto c = drm::cursor::Cursor::load(resolution_for(p), 1024);
  ASSERT_FALSE(c.has_value());
}

TEST_F(CursorTest, MissingFileReportsError) {
  auto c = drm::cursor::Cursor::load(resolution_for(root / "does-not-exist.cur"), 32);
  ASSERT_FALSE(c.has_value());
}

TEST_F(CursorTest, FramesAreContiguouslyBacked) {
  // Impl guarantees all frames share one vector<uint32_t>. Verify the
  // Frame::pixels spans line up end-to-end in memory so the caller can
  // rely on that layout (it's load-bearing for anyone zero-copying
  // into a GPU upload path).
  const auto p = write({
      FrameSpec{8, 8, 0, 0, 50, 0xff0000ffU},
      FrameSpec{8, 8, 0, 0, 50, 0xff00ff00U},
  });
  auto c = drm::cursor::Cursor::load(resolution_for(p), 8);
  ASSERT_TRUE(c.has_value());

  const auto& f0 = c->at(0);
  const auto& f1 = c->at(1);
  EXPECT_EQ(f0.pixels.data() + f0.pixels.size(), f1.pixels.data());
}
