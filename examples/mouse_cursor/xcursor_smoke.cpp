// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Standalone smoke test for the XCursor theme loader.
// Loads a named cursor from an installed theme and prints its metadata.
//
// Usage: xcursor_smoke [name] [theme] [size]
//   name   — cursor name (default "default")
//   theme  — theme name (default "Adwaita")
//   size   — pixel size (default 64)
//
// Exit codes: 0 on success, 1 if the cursor couldn't be loaded.

#include "xcursor_loader.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
  const char* name = argc > 1 ? argv[1] : "default";
  const char* theme = argc > 2 ? argv[2] : "Adwaita";
  const int size = argc > 3 ? std::atoi(argv[3]) : 64;

  auto cursor = LoadedCursor::load(name, theme, size);
  if (!cursor) {
    std::fprintf(stderr,
                 "xcursor_smoke: failed to load '%s' from theme '%s' at "
                 "size %d\n",
                 name, theme, size);
    return 1;
  }

  const CursorFrame& f = cursor->first();
  std::printf("name      : %s\n", name);
  std::printf("theme     : %s\n", theme);
  std::printf("req_size  : %d\n", size);
  std::printf("frames    : %zu\n", cursor->frame_count());
  std::printf("first.w   : %u\n", f.width);
  std::printf("first.h   : %u\n", f.height);
  std::printf("first.xhot: %d\n", f.xhot);
  std::printf("first.yhot: %d\n", f.yhot);
  std::printf("cycle_ms  : %u\n", cursor->cycle_ms());
  std::printf("animated  : %s\n", cursor->animated() ? "yes" : "no");

  if (!cursor->animated()) {
    return 0;
  }

  // Animated path: verify every frame parsed and that frame_at() walks
  // them correctly across one full cycle.
  std::printf("\n-- per-frame --\n");
  std::printf("idx  w x h    hotspot   delay_ms\n");
  for (std::size_t i = 0; i < cursor->frame_count(); ++i) {
    const CursorFrame& fi = cursor->frame_at_index(i);
    std::printf("%3zu  %u x %u   (%2d, %2d)   %u\n", i, fi.width, fi.height, fi.xhot, fi.yhot,
                fi.delay_ms);
  }

  std::printf("\n-- frame_at() sampling --\n");
  const uint64_t cycle = cursor->cycle_ms();
  const uint64_t samples[] = {0,         cycle / 4, cycle / 2,           (3 * cycle) / 4,
                              cycle - 1, cycle,     cycle + (cycle / 3), cycle * 2};
  for (const uint64_t t : samples) {
    const CursorFrame& at = cursor->frame_at(t);
    const auto idx = static_cast<std::size_t>(&at - &cursor->frame_at_index(0));
    std::printf("t=%-8lu -> frame %3zu (delay %u)\n", static_cast<unsigned long>(t), idx,
                at.delay_ms);
  }
  return 0;
}
