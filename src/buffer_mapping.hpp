// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// buffer_mapping.hpp — unified scoped CPU mapping for drm::dumb::Buffer
// and drm::gbm::Buffer (and any future buffer type that exposes linear
// CPU pixel storage).
//
// What problem this solves: every consumer that paints into a scanout
// buffer has historically reached for the buffer's eager `data()`
// accessor — which works for dumb buffers (kernel-coherent mappings)
// but is the wrong shape for GBM, where `gbm_bo_map` is meant to be
// scoped (mesa drivers do cache-coherence work at map / unmap, and a
// pinned mapping defeats it). `BufferMapping` consolidates the read /
// write surface behind a move-only RAII guard:
//
//   * Construction — the buffer's `map()` factory does whatever the
//     backend needs (mmap of the dumb-ioctl offset, gbm_bo_map for the
//     transfer mode the caller asked for).
//   * pixels() / stride() / width() / height() — view onto the linear
//     pixel storage. Always mutable; const-ness is on the guard, not
//     the span.
//   * Destruction — runs the backend's unmap path. For dumb buffers
//     this is a no-op (the kernel's cache-coherent mapping is held by
//     the buffer for its lifetime, not by the guard); for GBM buffers
//     this calls gbm_bo_unmap and discards the staging buffer.
//
// The guard owns no DRM/GBM state beyond the unmap callback — it never
// outlives the buffer it was acquired from. Move-only so a guard hands
// out its unmap responsibility cleanly when it crosses scopes.
//
// MapAccess is informational at the buffer level (every backend's
// underlying call accepts a transfer mode), but it propagates to the
// guard so future cache-flush hooks have the access mode to switch on.
// Read / Write / ReadWrite map onto GBM_BO_TRANSFER_READ /
// GBM_BO_TRANSFER_WRITE / GBM_BO_TRANSFER_READ_WRITE one-for-one;
// dumb buffers ignore the value.

#pragma once

#include <drm-cxx/detail/span.hpp>

#include <cstddef>
#include <cstdint>

namespace drm {

/// Intent of a CPU mapping. Backends that distinguish read-only vs
/// read-write traffic (GBM via `gbm_bo_map`'s transfer flags) honour
/// this; backends that don't (dumb buffers) ignore it. Picking the
/// narrowest mode that fits is always correct: a Read mapping is
/// cheaper to set up on tiled / cached buffers than a ReadWrite one,
/// because the staging buffer doesn't have to be flushed back on unmap.
enum class MapAccess : std::uint8_t {
  Read = 0,
  Write = 1,
  ReadWrite = 2,
};

/// Move-only RAII view over a buffer's CPU-linear pixel storage.
/// Constructed by `drm::dumb::Buffer::map()` / `drm::gbm::Buffer::map()`
/// (or any backend that exposes the same surface). On destruction the
/// guard calls back into the backend's unmap path — for dumb buffers
/// this is a no-op, for GBM buffers this is `gbm_bo_unmap`.
///
/// The guard never outlives the buffer it was acquired from. Lifetime
/// is the consumer's responsibility: holding a mapping past the
/// buffer's destructor is a use-after-free; `forget()`-ing the buffer
/// while a mapping is alive is the same. In practice the scene's
/// per-frame pattern (acquire mapping → paint → drop mapping → commit)
/// keeps the guard's lifetime well short of anything else.
class BufferMapping {
 public:
  /// Type-erased unmap function. Called from the guard's destructor
  /// and on move-assignment. Receives the `ctx` pointer the guard was
  /// constructed with; the backend stashes whatever cookie its unmap
  /// path needs there (a `gbm_bo_unmap` cookie pair, etc.).
  using Unmapper = void (*)(void* ctx) noexcept;

  /// Empty mapping — `pixels()` is empty, destructor is a no-op.
  /// The buffer-side factory uses this for the failure path so the
  /// returned `expected` carries a default-constructed Mapping when
  /// the caller didn't ask for one.
  BufferMapping() noexcept = default;

  /// Owning constructor used by backends. `pixels` points at
  /// `size_bytes` of linear ARGB-or-similar storage; the backend
  /// promises the storage is valid until `unmap(ctx)` runs. Width and
  /// height are the buffer's dimensions; consumers usually want
  /// `stride` for row arithmetic and `width`/`height` for clipping.
  BufferMapping(std::uint8_t* pixels, std::size_t size_bytes, std::uint32_t stride,
                std::uint32_t width, std::uint32_t height, MapAccess access, Unmapper unmap,
                void* ctx) noexcept
      : pixels_(pixels),
        size_bytes_(size_bytes),
        stride_(stride),
        width_(width),
        height_(height),
        access_(access),
        unmap_(unmap),
        ctx_(ctx) {}

  ~BufferMapping() {
    if (unmap_ != nullptr) {
      unmap_(ctx_);
    }
  }

  BufferMapping(const BufferMapping&) = delete;
  BufferMapping& operator=(const BufferMapping&) = delete;

  BufferMapping(BufferMapping&& other) noexcept
      : pixels_(other.pixels_),
        size_bytes_(other.size_bytes_),
        stride_(other.stride_),
        width_(other.width_),
        height_(other.height_),
        access_(other.access_),
        unmap_(other.unmap_),
        ctx_(other.ctx_) {
    other.pixels_ = nullptr;
    other.size_bytes_ = 0;
    other.stride_ = 0;
    other.width_ = 0;
    other.height_ = 0;
    other.unmap_ = nullptr;
    other.ctx_ = nullptr;
  }

  BufferMapping& operator=(BufferMapping&& other) noexcept {
    if (this != &other) {
      if (unmap_ != nullptr) {
        unmap_(ctx_);
      }
      pixels_ = other.pixels_;
      size_bytes_ = other.size_bytes_;
      stride_ = other.stride_;
      width_ = other.width_;
      height_ = other.height_;
      access_ = other.access_;
      unmap_ = other.unmap_;
      ctx_ = other.ctx_;
      other.pixels_ = nullptr;
      other.size_bytes_ = 0;
      other.stride_ = 0;
      other.width_ = 0;
      other.height_ = 0;
      other.unmap_ = nullptr;
      other.ctx_ = nullptr;
    }
    return *this;
  }

  [[nodiscard]] bool empty() const noexcept { return pixels_ == nullptr || size_bytes_ == 0; }

  /// Linear pixel storage. Always mutable; consumers that want a
  /// read-only view convert to `drm::span<const std::uint8_t>` at the
  /// call site. The first row starts at `pixels().data()`, subsequent
  /// rows are `stride()` bytes apart.
  [[nodiscard]] drm::span<std::uint8_t> pixels() const noexcept { return {pixels_, size_bytes_}; }

  /// Bytes per row. The kernel / GBM allocator may pad this above
  /// `width * bytes_per_pixel`; honour it instead of recomputing.
  [[nodiscard]] std::uint32_t stride() const noexcept { return stride_; }

  /// Buffer width in pixels.
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }

  /// Buffer height in pixels.
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }

  /// Access mode the mapping was acquired with. Most consumers ignore
  /// this; backends that maintain dirty state across map cycles can
  /// switch on it.
  [[nodiscard]] MapAccess access() const noexcept { return access_; }

 private:
  std::uint8_t* pixels_{nullptr};
  std::size_t size_bytes_{0};
  std::uint32_t stride_{0};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
  MapAccess access_{MapAccess::ReadWrite};
  Unmapper unmap_{nullptr};
  void* ctx_{nullptr};
};

}  // namespace drm
