// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Standalone smoke test for the drm::cursor theme resolver + cursor
// loader. Loads a named cursor through the library's public API and
// prints its metadata. No display required.
//
// Usage: xcursor_smoke [name] [theme] [size]
//   name   — cursor name (default "default")
//   theme  — theme name (empty string lets the resolver pick via
//            $XCURSOR_THEME or "default"; default "Adwaita")
//   size   — pixel size (default 64)
//
// Exit codes: 0 on success, 1 on load failure.

#include "cursor/cursor.hpp"
#include "cursor/theme.hpp"
#include "drm-cxx/detail/format.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
  const char* name = argc > 1 ? argv[1] : "default";
  const char* theme_hint = argc > 2 ? argv[2] : "Adwaita";
  const int size = argc > 3 ? std::atoi(argv[3]) : 64;

  auto theme = drm::cursor::Theme::discover();
  if (!theme) {
    drm::println(stderr, "xcursor_smoke: no XCursor themes found ({})", theme.error().message());
    return 1;
  }

  auto resolved = theme->resolve(name, theme_hint);
  if (!resolved) {
    drm::println(stderr, "xcursor_smoke: failed to resolve '{}' (hint '{}'): {}", name, theme_hint,
                 resolved.error().message());
    return 1;
  }
  drm::println("resolved  : {} in theme '{}'", resolved->source.string(), resolved->theme_name);
  std::string chain;
  for (std::size_t i = 0; i < resolved->chain.size(); ++i) {
    if (i > 0) {
      chain += " -> ";
    }
    chain += resolved->chain[i];
  }
  drm::println("chain     : {}", chain);

  auto cursor = drm::cursor::Cursor::load(*resolved, static_cast<std::uint32_t>(size));
  if (!cursor) {
    drm::println(stderr, "xcursor_smoke: Cursor::load failed: {}", cursor.error().message());
    return 1;
  }

  const auto& f = cursor->first();
  drm::println("name      : {}", name);
  drm::println("theme hint: {}", theme_hint);
  drm::println("req_size  : {}", size);
  drm::println("frames    : {}", cursor->frame_count());
  drm::println("first.w   : {}", f.width);
  drm::println("first.h   : {}", f.height);
  drm::println("first.xhot: {}", f.xhot);
  drm::println("first.yhot: {}", f.yhot);
  drm::println("cycle_ms  : {}", cursor->cycle().count());
  drm::println("animated  : {}", cursor->animated() ? "yes" : "no");

  if (!cursor->animated()) {
    return 0;
  }

  // Animated path: verify every frame parsed and that frame_at() walks
  // them correctly across one full cycle.
  drm::println("\n-- per-frame --");
  drm::println("idx  w x h    hotspot   delay_ms");
  for (std::size_t i = 0; i < cursor->frame_count(); ++i) {
    const auto& fi = cursor->at(i);
    drm::println("{:3}  {} x {}   ({:2}, {:2})   {}", i, fi.width, fi.height, fi.xhot, fi.yhot,
                 fi.delay.count());
  }

  drm::println("\n-- frame_at() sampling --");
  const auto cycle = cursor->cycle();
  const std::chrono::milliseconds samples[] = {
      std::chrono::milliseconds{0},         cycle / 4, cycle / 2,           (cycle * 3) / 4,
      cycle - std::chrono::milliseconds{1}, cycle,     cycle + (cycle / 3), cycle * 2,
  };
  for (const auto t : samples) {
    const auto& at = cursor->frame_at(t);
    // Identify the frame by pointer arithmetic against frame 0 — no
    // index accessor is needed because Frame structs live in a stable
    // vector inside Cursor's Impl.
    const auto idx = static_cast<std::size_t>(&at - &cursor->at(0));
    drm::println("t={:<8} -> frame {:3} (delay {})", t.count(), idx, at.delay.count());
  }
  return 0;
}
