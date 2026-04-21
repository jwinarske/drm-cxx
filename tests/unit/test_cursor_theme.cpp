// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Exercises drm::cursor::Theme without requiring a system XCursor
// installation. Every test builds a throwaway theme tree under a
// unique temp dir, feeds it to discover_with_paths(), and asserts on
// resolve() outcomes. No KMS / no libxcursor load — resolve() only
// checks for file existence, so empty "dummy" files are enough.

#include "cursor/theme.hpp"

#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;

class ThemeTest : public ::testing::Test {
 protected:
  // NOLINTNEXTLINE(cppcoreguidelines-non-private-member-variables-in-classes)
  fs::path root;

  void SetUp() override {
    // Per-process + per-test counter so parallel ctest runs don't
    // collide on the same temp dir. (ctest may fan out several tests
    // simultaneously; each invocation of this binary has one pid.)
    static std::atomic<int> counter{0};
    const auto suffix = std::to_string(::getpid()) + "-" + std::to_string(counter.fetch_add(1));
    root = fs::temp_directory_path() / fs::path("drm-cxx-theme-test-" + suffix);
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  // Create <root>/<name>/cursors/. Optional `inherits` vector triggers
  // an index.theme write with a matching Inherits= line.
  fs::path make_theme(const std::string& name, const std::vector<std::string>& inherits = {}) {
    const auto theme_dir = root / name;
    fs::create_directories(theme_dir / "cursors");
    if (!inherits.empty()) {
      std::ofstream out(theme_dir / "index.theme");
      out << "[Icon Theme]\n";
      out << "Name=" << name << "\n";
      out << "Inherits=";
      for (std::size_t i = 0; i < inherits.size(); ++i) {
        if (i > 0) {
          out << ",";
        }
        out << inherits[i];
      }
      out << "\n";
    }
    return theme_dir;
  }

  // Empty placeholder — resolve() only stats it.
  static void touch_cursor(const fs::path& theme_dir, const std::string& name) {
    std::ofstream(theme_dir / "cursors" / name) << "";
  }

  // Wrap a single search path as a span for discover_with_paths().
  [[nodiscard]] drm::expected<drm::cursor::Theme, std::error_code> discover() const {
    const std::vector<fs::path> paths{root};
    return drm::cursor::Theme::discover_with_paths(
        drm::span<const fs::path>(paths.data(), paths.size()));
  }
};

}  // namespace

TEST_F(ThemeTest, DiscoverSucceedsWithAtLeastOneTheme) {
  make_theme("foo");
  auto theme = discover();
  ASSERT_TRUE(theme.has_value());
}

TEST_F(ThemeTest, DiscoverFailsWhenNoThemesPresent) {
  // root exists but is empty — no theme dirs under it.
  auto theme = discover();
  EXPECT_FALSE(theme.has_value());
}

TEST_F(ThemeTest, ResolveFindsCursorInRequestedTheme) {
  const auto foo = make_theme("foo");
  touch_cursor(foo, "pointer");
  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  auto r = theme->resolve("pointer", "foo");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->theme_name, "foo");
  EXPECT_EQ(r->source.filename(), "pointer");
}

TEST_F(ThemeTest, ResolveMissingCursorReportsError) {
  make_theme("foo");  // no cursor files
  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  auto r = theme->resolve("pointer", "foo");
  EXPECT_FALSE(r.has_value());
}

TEST_F(ThemeTest, ResolveWalksInheritsChain) {
  const auto parent = make_theme("parent");
  touch_cursor(parent, "pointer");
  make_theme("child", {"parent"});  // child has no cursors; inherits parent

  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  auto r = theme->resolve("pointer", "child");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->theme_name, "parent");
  // Chain records the walk order: child was tried first, then parent.
  ASSERT_GE(r->chain.size(), 2U);
  EXPECT_EQ(r->chain.front(), "child");
  EXPECT_NE(std::find(r->chain.begin(), r->chain.end(), "parent"), r->chain.end());
}

TEST_F(ThemeTest, ResolveTransitivelyWalksInherits) {
  const auto a = make_theme("a");
  touch_cursor(a, "pointer");
  make_theme("b", {"a"});
  make_theme("c", {"b"});

  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  auto r = theme->resolve("pointer", "c");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->theme_name, "a");
}

TEST_F(ThemeTest, ResolveAliasFindsLegacyName) {
  // Theme ships the X11 legacy name; caller asks for the modern CSS name.
  const auto theme_dir = make_theme("legacy");
  touch_cursor(theme_dir, "hand2");

  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  auto r = theme->resolve("pointer", "legacy");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->source.filename(), "hand2");
}

TEST_F(ThemeTest, ResolveAliasFindsModernName) {
  // Reverse direction — theme ships the CSS name, caller asks for the
  // legacy X11 name. Both land on the same equivalence class.
  const auto theme_dir = make_theme("modern");
  touch_cursor(theme_dir, "e-resize");

  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  auto r = theme->resolve("right_side", "modern");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->source.filename(), "e-resize");
}

TEST_F(ThemeTest, ResolveFallsBackThroughIndexedThemesOnChainMiss) {
  // Requested theme + its inherits chain have no matching cursor, but
  // another indexed theme does. resolve() should fall through.
  make_theme("requested");                 // empty
  const auto other = make_theme("other");  // has pointer
  touch_cursor(other, "pointer");

  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  auto r = theme->resolve("pointer", "requested");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->theme_name, "other");
}

TEST_F(ThemeTest, ResolveRejectsPathTraversalCursorName) {
  make_theme("foo");

  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  for (const auto* name : {"../etc/passwd", "..", ".", "with/slash", "back\\slash", ""}) {
    auto r = theme->resolve(name, "foo");
    EXPECT_FALSE(r.has_value()) << "expected rejection for: '" << name << "'";
  }
}

TEST_F(ThemeTest, InheritsParserAcceptsSemicolonSeparator) {
  // Spec says comma; some themes in the wild use semicolons anyway.
  const auto parent1 = make_theme("p1");
  touch_cursor(parent1, "pointer");
  make_theme("p2");
  const auto child = root / "sc-child";
  fs::create_directories(child / "cursors");
  {
    std::ofstream out(child / "index.theme");
    out << "[Icon Theme]\nInherits=p1;p2\n";
  }

  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  auto r = theme->resolve("pointer", "sc-child");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->theme_name, "p1");
}

TEST_F(ThemeTest, InheritsParserIgnoresCommentsAndOtherSections) {
  const auto parent = make_theme("pp");
  touch_cursor(parent, "pointer");
  const auto child = root / "commented-child";
  fs::create_directories(child / "cursors");
  {
    std::ofstream out(child / "index.theme");
    // Leading comment lines + a non-Icon-Theme section before Icon Theme
    // block should all be skipped by the parser.
    out << "# comment\n";
    out << "[Some Other Section]\n";
    out << "Inherits=bogus\n";
    out << "[Icon Theme]\n";
    out << "Name=commented-child\n";
    out << "Inherits=pp\n";
  }

  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  auto r = theme->resolve("pointer", "commented-child");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->theme_name, "pp");
}

TEST_F(ThemeTest, ResolveMemoizesAcrossRepeatedCalls) {
  // Caching contract: a resolved (name, preferred_theme) pair is
  // memoized for the Theme's lifetime, so a follow-up resolve with
  // the same inputs returns the original result even if the backing
  // cursor file disappears between calls. Callers who need a fresh
  // read are expected to construct a new Theme, which is cheap.
  const auto foo = make_theme("foo");
  touch_cursor(foo, "pointer");

  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  auto first = theme->resolve("pointer", "foo");
  ASSERT_TRUE(first.has_value());
  const auto first_source = first->source;

  std::error_code ec;
  fs::remove(foo / "cursors" / "pointer", ec);
  ASSERT_FALSE(ec);

  auto second = theme->resolve("pointer", "foo");
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->theme_name, "foo");
  EXPECT_EQ(second->source, first_source);
}

TEST_F(ThemeTest, ResolveMemoizesNegativeResults) {
  // Negative caching: a failed lookup should also be remembered so a
  // compositor that probes for an absent shape doesn't re-walk the
  // chain on every request. Verified by creating the cursor after the
  // first (failing) resolve — the cached failure wins over the
  // now-present file.
  const auto foo = make_theme("foo");
  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  auto first = theme->resolve("pointer", "foo");
  EXPECT_FALSE(first.has_value());

  touch_cursor(foo, "pointer");
  auto second = theme->resolve("pointer", "foo");
  EXPECT_FALSE(second.has_value());
}

TEST_F(ThemeTest, ResolveChainReportsDedupedWalkOrder) {
  // Diamond inheritance: c inherits a and b; a inherits z; b inherits z.
  // z should appear once in the chain regardless of the two paths to it.
  const auto z = make_theme("zz");
  touch_cursor(z, "pointer");
  make_theme("aa", {"zz"});
  make_theme("bb", {"zz"});
  make_theme("cc", {"aa", "bb"});

  auto theme = discover();
  ASSERT_TRUE(theme.has_value());

  auto r = theme->resolve("pointer", "cc");
  ASSERT_TRUE(r.has_value());
  const int zz_count =
      static_cast<int>(std::count(r->chain.begin(), r->chain.end(), std::string("zz")));
  EXPECT_EQ(zz_count, 1);
}
