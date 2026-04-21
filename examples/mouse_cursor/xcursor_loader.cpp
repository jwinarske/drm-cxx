// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "xcursor_loader.hpp"

#include <X11/Xcursor/Xcursor.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

std::optional<LoadedCursor> LoadedCursor::load(const char* name, const char* theme, int size) {
  XcursorImages* imgs = XcursorLibraryLoadImages(name, theme, size);
  if (imgs == nullptr || imgs->nimage == 0) {
    if (imgs != nullptr) {
      XcursorImagesDestroy(imgs);
    }
    return std::nullopt;
  }

  LoadedCursor out;
  out.frames_.reserve(static_cast<std::size_t>(imgs->nimage));
  uint32_t cycle = 0;

  for (int i = 0; i < imgs->nimage; ++i) {
    const XcursorImage* src = imgs->images[i];
    CursorFrame f;
    f.width = src->width;
    f.height = src->height;
    f.xhot = static_cast<int>(src->xhot);
    f.yhot = static_cast<int>(src->yhot);
#ifdef HAS_XCURSOR_DELAY
    f.delay_ms = src->delay;
#else
    f.delay_ms = 0;
#endif
    const std::size_t n = static_cast<std::size_t>(src->width) * src->height;
    f.pixels.assign(src->pixels, src->pixels + n);
    cycle += f.delay_ms;
    out.frames_.push_back(std::move(f));
  }

  out.cycle_ms_ = cycle;
  XcursorImagesDestroy(imgs);
  return out;
}

const CursorFrame& LoadedCursor::frame_at(uint64_t now_ms) const {
  if (frames_.size() == 1 || cycle_ms_ == 0) {
    return frames_.front();
  }
  auto t = static_cast<uint32_t>(now_ms % cycle_ms_);
  uint32_t acc = 0;
  for (const auto& f : frames_) {
    acc += f.delay_ms;
    if (t < acc) {
      return f;
    }
  }
  return frames_.back();
}
