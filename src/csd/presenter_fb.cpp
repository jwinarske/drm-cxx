// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "presenter_fb.hpp"

#include "../scene/composite_canvas.hpp"
#include "../scene/composition_target.hpp"
#include "damage.hpp"
#include "presenter.hpp"
#include "surface.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>
#include <drm-cxx/detail/span.hpp>

#include <drm_fourcc.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <linux/fb.h>
#include <memory>
#include <optional>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace drm::csd {

namespace {

drm::unexpected<std::error_code> err(std::errc e) {
  return drm::unexpected<std::error_code>(std::make_error_code(e));
}

}  // namespace

// ── Pure helpers ───────────────────────────────────────────────────────

std::optional<std::uint32_t> fb_fourcc_for(std::uint32_t bpp, std::uint32_t red_offset,
                                           std::uint32_t blue_offset, std::uint32_t transp_length) {
  if (bpp == 32U) {
    const bool has_alpha = transp_length != 0U;
    // red above blue => memory order B,G,R,(A) => DRM *RGB8888.
    if (red_offset > blue_offset) {
      return has_alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
    }
    // red below blue => memory order R,G,B,(A) => DRM *BGR8888.
    return has_alpha ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_XBGR8888;
  }
  if (bpp == 16U) {
    return red_offset > blue_offset ? DRM_FORMAT_RGB565 : DRM_FORMAT_BGR565;
  }
  return std::nullopt;
}

void compose_into_framebuffer(drm::span<std::uint8_t> fb, std::uint32_t fb_stride,
                              std::uint32_t fb_w, std::uint32_t fb_h, std::uint32_t fb_fourcc,
                              drm::span<std::uint8_t> shadow, drm::span<const FbBlitItem> items,
                              const DamageRect& damage) noexcept {
  const std::size_t shadow_stride = static_cast<std::size_t>(fb_w) * 4U;
  if (shadow.size() < shadow_stride * fb_h || damage.empty()) {
    return;
  }

  // Clear only the damage region, then SRC_OVER-blend each item clipped to
  // it (bottom-to-top). Everything outside keeps the persistent shadow.
  drm::scene::CompositeCanvas::clear_into(
      shadow, static_cast<std::uint32_t>(shadow_stride), fb_w, fb_h, damage.x, damage.y,
      static_cast<std::int32_t>(damage.w), static_cast<std::int32_t>(damage.h));
  for (const auto& item : items) {
    const DamageRect d = intersect_rect(item.x, item.y, item.width, item.height, damage);
    if (d.empty()) {
      continue;
    }
    drm::scene::CompositeSrc src;
    src.pixels = item.pixels;
    src.src_stride_bytes = item.stride;
    src.src_width = item.width;
    src.src_height = item.height;
    src.drm_fourcc = item.fourcc;
    const drm::scene::CompositeRect src_rect{d.x - item.x, d.y - item.y, d.w, d.h};
    const drm::scene::CompositeRect dst_rect{d.x, d.y, d.w, d.h};
    drm::scene::CompositeCanvas::blend_into(shadow, static_cast<std::uint32_t>(shadow_stride), fb_w,
                                            fb_h, src, src_rect, dst_rect);
  }

  // Convert only the damaged scanlines of the shadow into the fb's layout
  // (fb line stride and bpp differ from the ARGB shadow's). The fb is
  // single-buffered, so a converted row is immediately live.
  const std::uint32_t y_end = damage.y + damage.h;
  for (std::uint32_t row = damage.y; row < y_end; ++row) {
    std::uint8_t* dst = fb.data() + (static_cast<std::size_t>(row) * fb_stride);
    const std::uint8_t* srow = shadow.data() + (static_cast<std::size_t>(row) * shadow_stride);
    drm::scene::CompositeCanvas::convert_row(dst, srow, static_cast<std::int32_t>(fb_w), fb_fourcc);
  }
}

// ── FramebufferPresenter ───────────────────────────────────────────────

drm::expected<std::unique_ptr<FramebufferPresenter>, std::error_code> FramebufferPresenter::create(
    const char* fb_path) {
  const int fd = ::open(fb_path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    return drm::unexpected<std::error_code>(std::error_code(errno, std::generic_category()));
  }
  // Own the fd from here on so any early return closes it.
  auto self = std::unique_ptr<FramebufferPresenter>(new FramebufferPresenter());
  self->fd_ = fd;

  fb_var_screeninfo var{};
  fb_fix_screeninfo fix{};
  if (::ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 || ::ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
    return err(std::errc::io_error);
  }

  const auto fourcc =
      fb_fourcc_for(var.bits_per_pixel, var.red.offset, var.blue.offset, var.transp.length);
  if (!fourcc) {
    return err(std::errc::not_supported);
  }

  void* map = ::mmap(nullptr, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    return err(std::errc::io_error);
  }

  self->fb_ = static_cast<std::uint8_t*>(map);
  self->map_len_ = fix.smem_len;
  self->width_ = var.xres;
  self->height_ = var.yres;
  self->stride_ = fix.line_length;
  self->fourcc_ = *fourcc;
  self->shadow_.assign(static_cast<std::size_t>(var.xres) * var.yres * 4U, 0U);
  return self;
}

FramebufferPresenter::~FramebufferPresenter() {
  if (fb_ != nullptr) {
    ::munmap(fb_, map_len_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

drm::expected<void, std::error_code> FramebufferPresenter::apply(
    drm::span<const SurfaceRef> surfaces, drm::AtomicRequest& /*req*/) {
  // Snapshot this frame's slots and diff against last frame's. The fb is a
  // single buffer, so the region to re-blit is just this frame's damage (no
  // double-buffer union). Everything outside keeps the shadow's / fb's prior
  // pixels — the incremental blit.
  std::vector<DamageSlot> cur;
  cur.reserve(surfaces.size());
  for (const auto& ref : surfaces) {
    DamageSlot s;
    if (ref.surface != nullptr && !ref.surface->empty()) {
      s.armed = true;
      s.x = ref.x;
      s.y = ref.y;
      s.w = ref.surface->width();
      s.h = ref.surface->height();
      s.gen = ref.surface->content_generation();
    }
    cur.push_back(s);
  }
  const DamageRect damage =
      compute_damage(drm::span<const DamageSlot>(prev_slots_.data(), prev_slots_.size()),
                     drm::span<const DamageSlot>(cur.data(), cur.size()), width_, height_);

  // Map + build blit items only for decorations overlapping the damage — a
  // decoration outside it keeps its persistent shadow pixels. Hold the CPU
  // mappings alive for the whole compose pass (items reference their spans).
  std::vector<drm::BufferMapping> maps;
  std::vector<FbBlitItem> items;
  maps.reserve(surfaces.size());
  items.reserve(surfaces.size());
  if (!damage.empty()) {
    for (const auto& ref : surfaces) {
      const auto* surface = ref.surface;
      if (surface == nullptr || surface->empty()) {
        continue;
      }
      if (intersect_rect(ref.x, ref.y, surface->width(), surface->height(), damage).empty()) {
        continue;
      }
      // See presenter_composite.cpp: SurfaceRef holds a const Surface*, but a
      // read-only map is non-const only because the GBM backend stages a
      // scratch buffer; MapAccess::Read never mutates the decoration.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      auto map = const_cast<Surface*>(surface)->paint(drm::MapAccess::Read);
      if (!map) {
        return drm::unexpected<std::error_code>(map.error());
      }
      FbBlitItem item;
      item.pixels = map->pixels();
      item.stride = map->stride();
      item.width = surface->width();
      item.height = surface->height();
      item.x = ref.x;
      item.y = ref.y;
      item.fourcc = surface->format();
      items.push_back(item);
      maps.push_back(std::move(*map));
    }
  }

  compose_into_framebuffer(drm::span<std::uint8_t>(fb_, map_len_), stride_, width_, height_,
                           fourcc_, drm::span<std::uint8_t>(shadow_.data(), shadow_.size()),
                           drm::span<const FbBlitItem>(items.data(), items.size()), damage);
  prev_slots_ = std::move(cur);
  return {};
}

}  // namespace drm::csd
