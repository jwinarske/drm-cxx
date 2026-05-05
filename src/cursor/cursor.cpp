// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "cursor.hpp"

#include "detail/expected.hpp"
#include "detail/span.hpp"
#include "theme.hpp"

#include <X11/Xcursor/Xcursor.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::cursor {

struct Cursor::Impl {
  // One contiguous allocation backs every frame's pixels. Frame::pixels
  // are spans into this storage; if this vector ever reallocated the
  // spans would dangle, so it is sized up-front via resize() and never
  // appended to afterward.
  std::vector<std::uint32_t> pixel_storage;
  std::vector<Frame> frames;
  std::chrono::milliseconds cycle{0};
};

Cursor::Cursor(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Cursor::Cursor(Cursor&&) noexcept = default;
Cursor& Cursor::operator=(Cursor&&) noexcept = default;
Cursor::~Cursor() = default;

drm::expected<Cursor, std::error_code> Cursor::load(const ThemeResolution& resolved,
                                                    std::uint32_t requested_size) {
  // 0 is a foot-gun: libxcursor's 0 path falls back to 24 px, which is
  // tiny against typical KMS cursor planes (64 px on i915, 256 px on
  // amdgpu DC). Substitute a default that lines up with the smallest
  // commonly-supported plane size so the sprite reaches the buffer
  // edges instead of floating in the middle.
  constexpr std::uint32_t k_default_size = 64;
  const int xcursor_size = static_cast<int>(requested_size != 0 ? requested_size : k_default_size);

  // XcursorFilenameLoadImages selects the on-disk size closest to the
  // requested one (XCursor files are multi-resolution packs); we do not
  // get to pick between >= vs. <= — libxcursor returns "best match".
  XcursorImages* imgs = XcursorFilenameLoadImages(resolved.source.c_str(), xcursor_size);
  if (imgs == nullptr || imgs->nimage == 0) {
    if (imgs != nullptr) {
      XcursorImagesDestroy(imgs);
    }
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::no_message));
  }

  // Bound checks before any allocation sizing: .cursor files come
  // from attacker-controlled locations (user theme dirs, XCURSOR_PATH),
  // and a crafted file can advertise wildly large dimensions or frame
  // counts. 512px is already larger than any real-world cursor; 256
  // frames is well beyond the ~50 that real animated cursors ship
  // with. Rejecting here turns a pathological file into a load error
  // instead of a process-wide OOM.
  constexpr int k_max_frames = 256;
  constexpr std::uint32_t k_max_dim = 512;
  if (imgs->nimage < 0 || imgs->nimage > k_max_frames) {
    XcursorImagesDestroy(imgs);
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::file_too_large));
  }
  for (int i = 0; i < imgs->nimage; ++i) {
    const XcursorImage* src = imgs->images[i];
    if (src == nullptr || src->width > k_max_dim || src->height > k_max_dim) {
      XcursorImagesDestroy(imgs);
      return drm::unexpected<std::error_code>(std::make_error_code(std::errc::file_too_large));
    }
  }

  auto impl = std::make_unique<Impl>();
  impl->frames.reserve(static_cast<std::size_t>(imgs->nimage));

  // Pass 1: compute the total pixel count so the storage vector can be
  // sized once. Appending per-frame would risk reallocation mid-loop,
  // which would dangle every span we had already built.
  std::size_t total_pixels = 0;
  for (int i = 0; i < imgs->nimage; ++i) {
    const XcursorImage* src = imgs->images[i];
    total_pixels += static_cast<std::size_t>(src->width) * src->height;
  }
  impl->pixel_storage.resize(total_pixels);

  // Pass 2: copy pixels at their offset and record each Frame's span.
  std::size_t offset = 0;
  for (int i = 0; i < imgs->nimage; ++i) {
    const XcursorImage* src = imgs->images[i];
    const std::size_t n = static_cast<std::size_t>(src->width) * src->height;

    std::memcpy(impl->pixel_storage.data() + offset, src->pixels, n * sizeof(std::uint32_t));

    // XcursorImage::delay is a core field on every libxcursor >= 1.1.0
    // (shipped since 2003); the example still guards it for the
    // hypothetical pre-1.1 system header, so we do the same to keep
    // the gate set aligned across the library + example builds.
#ifdef HAS_XCURSOR_DELAY
    const std::uint32_t delay_ms = src->delay;
#else
    const std::uint32_t delay_ms = 0;
#endif

    impl->frames.push_back(Frame{
        drm::span<const std::uint32_t>(impl->pixel_storage.data() + offset, n),
        src->width,
        src->height,
        static_cast<int>(src->xhot),
        static_cast<int>(src->yhot),
        std::chrono::milliseconds(delay_ms),
    });
    impl->cycle += impl->frames.back().delay;
    offset += n;
  }

  XcursorImagesDestroy(imgs);

  return Cursor(std::move(impl));
}

drm::expected<Cursor, std::error_code> Cursor::load(const Theme& theme,
                                                    std::string_view cursor_name,
                                                    std::string_view preferred_theme,
                                                    std::uint32_t requested_size) {
  auto resolved = theme.resolve(cursor_name, preferred_theme);
  if (!resolved) {
    return drm::unexpected<std::error_code>(resolved.error());
  }
  return load(*resolved, requested_size);
}

const Frame& Cursor::first() const noexcept {
  return impl_->frames.front();
}

const Frame& Cursor::at(std::size_t index) const noexcept {
  return impl_->frames[index];
}

std::size_t Cursor::frame_count() const noexcept {
  return impl_->frames.size();
}

bool Cursor::animated() const noexcept {
  return impl_->frames.size() > 1 && impl_->cycle.count() > 0;
}

std::chrono::milliseconds Cursor::cycle() const noexcept {
  return impl_->cycle;
}

const Frame& Cursor::frame_at(std::chrono::milliseconds elapsed) const noexcept {
  // Static cursor, or every frame advertised zero delay (some themes
  // ship multi-frame packs with delay=0 on every frame — treat them as
  // static; otherwise we'd loop through instantly and flicker).
  if (impl_->frames.size() == 1 || impl_->cycle.count() == 0) {
    return impl_->frames.front();
  }
  const auto t = std::chrono::milliseconds(elapsed.count() % impl_->cycle.count());
  std::chrono::milliseconds acc{0};
  for (const auto& f : impl_->frames) {
    acc += f.delay;
    if (t < acc) {
      return f;
    }
  }
  return impl_->frames.back();
}

}  // namespace drm::cursor
