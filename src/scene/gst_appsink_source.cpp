// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "gst_appsink_source.hpp"

#include "buffer_source.hpp"

#include <drm-cxx/detail/expected.hpp>

#include <memory>
#include <system_error>

#if DRM_CXX_HAS_GSTREAMER
#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/dumb/buffer.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <fcntl.h>  // F_DUPFD_CLOEXEC

// Specific GStreamer subsystem headers — chosen per-symbol so tidy's
// misc-include-cleaner can trace usage. The `<gst/gst.h>` umbrella
// would also work but is opaque to the lint, which then fires at every
// call site instead of at the include line. GLib types and TRUE/FALSE
// macros are deliberately avoided in this TU (replaced with C++
// equivalents — `unsigned int`, `std::size_t`, plain `0`/`1`) so we
// don't need to include <glib.h>, which forbids direct include of its
// per-symbol sub-headers and is itself opaque to the lint.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/base/gstbasesink.h>
#include <gst/gst.h>  // gst_init / gst_is_initialized — only declared in the umbrella
#include <gst/gstbuffer.h>
#include <gst/gstbus.h>
#include <gst/gstcaps.h>
#include <gst/gstelement.h>
#include <gst/gstmemory.h>
#include <gst/gstmessage.h>
#include <gst/gstobject.h>
#include <gst/gstsample.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/video-info-dma.h>
#include <gst/video/video-info.h>
#include <list>
#include <optional>
#include <utility>
#endif

namespace drm::scene {

#if DRM_CXX_HAS_GSTREAMER

namespace {

constexpr std::size_t k_max_planes = 4;

/// One imported KMS framebuffer pinned to a dma-buf fd. Owns the GEM
/// handles and FB ID; teardown closes both. The dma-buf fd itself is
/// NOT owned — it belongs to the GstSample's GstMemory.
struct CachedFb {
  int dmabuf_fd{-1};                                  ///< cache key (not owned)
  std::array<std::uint32_t, k_max_planes> handles{};  ///< per-plane GEM handles
  std::size_t n_handles{0};
  std::uint32_t fb_id{0};
};

[[nodiscard]] std::error_code last_errno_or(std::errc fallback) noexcept {
  const int e = errno;
  return {e != 0 ? e : static_cast<int>(fallback), std::system_category()};
}

/// Per-plane offsets/strides resolved from a GstBuffer. Prefers the
/// per-buffer GstVideoMeta (added by elements that allocate the buffer
/// themselves) and falls back to the caps-derived GstVideoInfo.
struct PlaneLayout {
  std::array<std::uint64_t, k_max_planes> offsets{};
  std::array<std::uint32_t, k_max_planes> strides{};
  std::size_t n_planes{0};
};

[[nodiscard]] PlaneLayout resolve_plane_layout(GstBuffer* buf, const GstVideoInfo& info) noexcept {
  PlaneLayout out;
  const GstVideoMeta* vmeta = gst_buffer_get_video_meta(buf);
  if (vmeta != nullptr && vmeta->n_planes <= k_max_planes) {
    out.n_planes = vmeta->n_planes;
    for (std::size_t i = 0; i < out.n_planes; ++i) {
      // GstVideoMeta stores per-plane offset/stride as raw C arrays of
      // length GST_VIDEO_MAX_PLANES; index range is bounded above by
      // the n_planes check just made.
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      out.offsets.at(i) = vmeta->offset[i];
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      out.strides.at(i) = static_cast<std::uint32_t>(vmeta->stride[i]);
    }
    return out;
  }
  const auto n = static_cast<std::size_t>(GST_VIDEO_INFO_N_PLANES(&info));
  out.n_planes = std::min(n, k_max_planes);
  for (std::size_t i = 0; i < out.n_planes; ++i) {
    out.offsets.at(i) = GST_VIDEO_INFO_PLANE_OFFSET(&info, i);
    out.strides.at(i) = static_cast<std::uint32_t>(GST_VIDEO_INFO_PLANE_STRIDE(&info, i));
  }
  return out;
}

/// True when the buffer's primary memory is dmabuf-backed and so
/// suitable for the zero-copy import path.
[[nodiscard]] bool buffer_is_dmabuf(GstBuffer* buf) noexcept {
  if (gst_buffer_n_memory(buf) == 0U) {
    return false;
  }
  GstMemory* mem0 = gst_buffer_peek_memory(buf, 0);
  return mem0 != nullptr && gst_is_dmabuf_memory(mem0) != 0;
}

}  // namespace

struct GstAppsinkSource::Impl {
  GstElement* appsink{nullptr};
  GstAppsinkConfig cfg{};

  int drm_fd{-1};  // borrowed from drm::Device — never close()d here

  // Latest decoded sample (latest-frame-wins). Replaced by acquire();
  // unref'd on replacement / destruction / pause.
  GstSample* current_sample{nullptr};
  std::uint32_t current_fb_id{0};

  // Negotiated format, populated lazily from the first sample's caps.
  bool format_resolved{false};
  GstVideoInfo video_info{};
  SourceFormat format_cache{};

  // FB-ID ring keyed on the primary plane's dma-buf fd. LRU: front is
  // oldest, back is newest. Capacity == cfg.fb_cache_size. The element
  // type is from this TU's anon namespace; std::list's allocator
  // doesn't propagate the type's linkage outward.
  std::list<CachedFb> fb_cache;

  // Sysmem-fallback path: one dumb buffer the source memcpys into when
  // the producer hands back non-DMABUF GstMemory. Allocated lazily on
  // the first sysmem sample.
  std::optional<drm::dumb::Buffer> sysmem_buffer;
  std::uint32_t sysmem_fb_id{0};

  // Pending error surfaced by drive() / acquire() — caps changed
  // mid-stream, EOS, or pipeline error. Once set, every acquire()
  // returns it until the source is destroyed.
  std::error_code pending_error;
};

namespace {

void teardown_cached_fb(int drm_fd, CachedFb& entry) noexcept {
  if (entry.fb_id != 0) {
    drmModeRmFB(drm_fd, entry.fb_id);
    entry.fb_id = 0;
  }
  // Each unique GEM handle is closed once. handles[] may repeat the
  // same handle across planes when a single dma-buf fd was bound to
  // multiple planes; drmCloseHandle on the same handle twice is
  // tolerated but wasteful, so dedupe.
  std::array<std::uint32_t, k_max_planes> seen{};
  std::size_t seen_n = 0;
  for (std::size_t i = 0; i < entry.n_handles; ++i) {
    const auto h = entry.handles.at(i);
    if (h == 0) {
      continue;
    }
    bool already = false;
    for (std::size_t j = 0; j < seen_n; ++j) {
      if (seen.at(j) == h) {
        already = true;
        break;
      }
    }
    if (!already) {
      drmCloseBufferHandle(drm_fd, h);
      seen.at(seen_n++) = h;
    }
  }
  entry.n_handles = 0;
}

void teardown_sysmem(GstAppsinkSource::Impl& impl) noexcept {
  if (impl.sysmem_fb_id != 0) {
    drmModeRmFB(impl.drm_fd, impl.sysmem_fb_id);
    impl.sysmem_fb_id = 0;
  }
  impl.sysmem_buffer.reset();
}

void teardown_drm_state(GstAppsinkSource::Impl& impl) noexcept {
  for (auto& entry : impl.fb_cache) {
    teardown_cached_fb(impl.drm_fd, entry);
  }
  impl.fb_cache.clear();
  teardown_sysmem(impl);
  impl.current_fb_id = 0;
}

[[nodiscard]] std::error_code resolve_format_from_caps(GstAppsinkSource::Impl& impl,
                                                       GstSample* sample) noexcept {
  const GstCaps* caps = gst_sample_get_caps(sample);
  if (caps == nullptr) {
    return std::make_error_code(std::errc::protocol_error);
  }
  GstVideoInfo info{};
  if (gst_video_info_from_caps(&info, caps) == 0) {
    return std::make_error_code(std::errc::protocol_error);
  }
  const std::uint32_t fourcc = gst_video_dma_drm_fourcc_from_format(GST_VIDEO_INFO_FORMAT(&info));
  if (fourcc == DRM_FORMAT_INVALID) {
    return std::make_error_code(std::errc::operation_not_supported);
  }
  impl.video_info = info;
  impl.format_cache.drm_fourcc = fourcc;
  impl.format_cache.modifier = DRM_FORMAT_MOD_LINEAR;
  impl.format_cache.width = static_cast<std::uint32_t>(GST_VIDEO_INFO_WIDTH(&info));
  impl.format_cache.height = static_cast<std::uint32_t>(GST_VIDEO_INFO_HEIGHT(&info));
  impl.format_resolved = true;
  return {};
}

/// Detect a caps change by comparing the new sample's video info against
/// the cached one. Returns true when format/dimensions match.
[[nodiscard]] bool caps_match_cached(const GstAppsinkSource::Impl& impl,
                                     GstSample* sample) noexcept {
  const GstCaps* caps = gst_sample_get_caps(sample);
  if (caps == nullptr) {
    return false;
  }
  GstVideoInfo info{};
  if (gst_video_info_from_caps(&info, caps) == 0) {
    return false;
  }
  return GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_INFO_FORMAT(&impl.video_info) &&
         GST_VIDEO_INFO_WIDTH(&info) == GST_VIDEO_INFO_WIDTH(&impl.video_info) &&
         GST_VIDEO_INFO_HEIGHT(&info) == GST_VIDEO_INFO_HEIGHT(&impl.video_info);
}

/// Locate plane[i]'s dma-buf fd + offset within the buffer's memory
/// blocks. GStreamer can place planes in separate GstMemory entries OR
/// as offsets into a single contiguous one; gst_buffer_find_memory
/// resolves either layout.
[[nodiscard]] std::error_code locate_plane_fd(GstBuffer* buf, std::uint64_t plane_offset,
                                              std::uint32_t plane_stride,
                                              std::uint32_t plane_height, int& out_fd,
                                              std::uint64_t& out_offset_in_fd) noexcept {
  const std::size_t plane_size = static_cast<std::size_t>(plane_stride) * plane_height;
  unsigned int mem_idx = 0;
  std::size_t mem_skip = 0;
  if (gst_buffer_find_memory(buf, plane_offset, plane_size, &mem_idx, nullptr, &mem_skip) == 0) {
    return std::make_error_code(std::errc::protocol_error);
  }
  GstMemory* mem = gst_buffer_peek_memory(buf, mem_idx);
  if (mem == nullptr || gst_is_dmabuf_memory(mem) == 0) {
    return std::make_error_code(std::errc::operation_not_supported);
  }
  out_fd = gst_dmabuf_memory_get_fd(mem);
  out_offset_in_fd = static_cast<std::uint64_t>(mem->offset + mem_skip);
  return {};
}

/// Import a DMABUF-backed sample into a cached fb_id. On cache hit the
/// existing entry is moved to the back of the LRU list and reused. On
/// miss the kernel's GEM table dedupes identical fds across planes
/// automatically.
[[nodiscard]] std::error_code import_dmabuf_sample(GstAppsinkSource::Impl& impl,
                                                   GstBuffer* buf) noexcept {
  const PlaneLayout layout = resolve_plane_layout(buf, impl.video_info);
  if (layout.n_planes == 0) {
    return std::make_error_code(std::errc::protocol_error);
  }

  // Resolve plane[0]'s fd as the cache key. Buffer pools recycle the
  // same fds, so the cache key is stable across decoded frames using
  // the same backing memory.
  int key_fd = -1;
  std::uint64_t key_off = 0;
  if (auto ec = locate_plane_fd(buf, layout.offsets.at(0), layout.strides.at(0),
                                impl.format_cache.height, key_fd, key_off);
      ec) {
    return ec;
  }

  // Cache hit: splice the entry to the back of the LRU list and reuse.
  for (auto it = impl.fb_cache.begin(); it != impl.fb_cache.end(); ++it) {
    if (it->dmabuf_fd == key_fd) {
      impl.fb_cache.splice(impl.fb_cache.end(), impl.fb_cache, it);
      impl.current_fb_id = impl.fb_cache.back().fb_id;
      return {};
    }
  }

  // Cache miss. Per-plane fd resolution + drmPrimeFDToHandle.
  CachedFb entry;
  entry.dmabuf_fd = key_fd;
  entry.n_handles = layout.n_planes;
  std::array<std::uint32_t, k_max_planes> pitches{};
  std::array<std::uint32_t, k_max_planes> offsets{};
  std::array<std::uint64_t, k_max_planes> modifiers{};
  for (std::size_t i = 0; i < layout.n_planes; ++i) {
    int plane_fd = -1;
    std::uint64_t plane_offset_in_fd = 0;
    const auto plane_h_div =
        (i == 0) ? impl.format_cache.height
                 : static_cast<std::uint32_t>(GST_VIDEO_INFO_COMP_HEIGHT(&impl.video_info, i));
    if (auto ec = locate_plane_fd(buf, layout.offsets.at(i), layout.strides.at(i), plane_h_div,
                                  plane_fd, plane_offset_in_fd);
        ec) {
      teardown_cached_fb(impl.drm_fd, entry);
      return ec;
    }
    std::uint32_t handle = 0;
    if (drmPrimeFDToHandle(impl.drm_fd, plane_fd, &handle) != 0 || handle == 0) {
      teardown_cached_fb(impl.drm_fd, entry);
      return last_errno_or(std::errc::io_error);
    }
    entry.handles.at(i) = handle;
    pitches.at(i) = layout.strides.at(i);
    offsets.at(i) = static_cast<std::uint32_t>(plane_offset_in_fd);
    modifiers.at(i) = DRM_FORMAT_MOD_LINEAR;
  }

  std::uint32_t fb_id = 0;
  if (drmModeAddFB2WithModifiers(impl.drm_fd, impl.format_cache.width, impl.format_cache.height,
                                 impl.format_cache.drm_fourcc, entry.handles.data(), pitches.data(),
                                 offsets.data(), modifiers.data(), &fb_id,
                                 DRM_MODE_FB_MODIFIERS) != 0 ||
      fb_id == 0) {
    const auto ec = last_errno_or(std::errc::io_error);
    teardown_cached_fb(impl.drm_fd, entry);
    return ec;
  }
  entry.fb_id = fb_id;

  // Evict LRU when full, then push the fresh entry.
  while (impl.fb_cache.size() >= impl.cfg.fb_cache_size) {
    auto& victim = impl.fb_cache.front();
    teardown_cached_fb(impl.drm_fd, victim);
    impl.fb_cache.pop_front();
  }
  impl.fb_cache.push_back(entry);
  impl.current_fb_id = impl.fb_cache.back().fb_id;
  return {};
}

/// Import a system-memory sample by memcpying it into a lazily-allocated
/// dumb buffer that's bound to a single fb_id for the source's lifetime.
/// Single-plane formats only — multi-plane sysmem (NV12 in
/// vmalloc-backed memory, etc.) is rejected with operation_not_supported
/// because dumb::Buffer is one contiguous storage with one stride, not
/// the per-plane layout multi-plane formats need.
[[nodiscard]] std::error_code import_sysmem_sample(GstAppsinkSource::Impl& impl,
                                                   GstBuffer* buf) noexcept {
  if (!impl.sysmem_buffer.has_value()) {
    drm::dumb::Config dcfg;
    dcfg.width = impl.format_cache.width;
    dcfg.height = impl.format_cache.height;
    dcfg.drm_format = impl.format_cache.drm_fourcc;
    dcfg.bpp = 32;
    dcfg.add_fb = true;
    auto dev = drm::Device::from_fd(impl.drm_fd);
    auto buffer_r = drm::dumb::Buffer::create(dev, dcfg);
    if (!buffer_r) {
      return buffer_r.error();
    }
    impl.sysmem_buffer = std::move(*buffer_r);
    impl.sysmem_fb_id = impl.sysmem_buffer->fb_id();
    if (impl.sysmem_fb_id == 0) {
      impl.sysmem_buffer.reset();
      return std::make_error_code(std::errc::io_error);
    }
  }

  const PlaneLayout layout = resolve_plane_layout(buf, impl.video_info);
  if (layout.n_planes != 1) {
    return std::make_error_code(std::errc::operation_not_supported);
  }

  // GstMapInfo is a C struct whose `flags` field is a GstMapFlags enum
  // with no zero-valued enumerator; zero-init is what GStreamer's own
  // GST_MAP_INFO_INIT macro emits, but the static analyzer flags it
  // anyway. Suppress narrowly here, not project-wide.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  GstMapInfo map{};
  if (gst_buffer_map(buf, &map, GST_MAP_READ) == 0) {
    return std::make_error_code(std::errc::io_error);
  }

  auto bm = impl.sysmem_buffer->map(drm::MapAccess::Write);
  if (bm.empty()) {
    gst_buffer_unmap(buf, &map);
    return std::make_error_code(std::errc::io_error);
  }
  const auto dst_span = bm.pixels();
  const std::uint32_t dst_stride = bm.stride();
  const std::uint32_t src_stride = layout.strides.at(0);
  const auto* src_pixels = map.data + layout.offsets.at(0);
  const std::uint32_t copy_stride = std::min(src_stride, dst_stride);
  for (std::uint32_t y = 0; y < impl.format_cache.height; ++y) {
    std::memcpy(dst_span.data() + (static_cast<std::size_t>(y) * dst_stride),
                src_pixels + (static_cast<std::size_t>(y) * src_stride), copy_stride);
  }

  gst_buffer_unmap(buf, &map);
  impl.current_fb_id = impl.sysmem_fb_id;
  return {};
}

}  // namespace

GstAppsinkSource::GstAppsinkSource() : impl_(std::make_unique<Impl>()) {}

GstAppsinkSource::~GstAppsinkSource() {
  if (!impl_) {
    return;
  }
  if (impl_->current_sample != nullptr) {
    gst_sample_unref(impl_->current_sample);
    impl_->current_sample = nullptr;
  }
  teardown_drm_state(*impl_);
  if (impl_->appsink != nullptr) {
    gst_object_unref(impl_->appsink);
    impl_->appsink = nullptr;
  }
}

drm::expected<std::unique_ptr<GstAppsinkSource>, std::error_code> GstAppsinkSource::create(
    const drm::Device& dev, GstElement* appsink, const GstAppsinkConfig& cfg) {
  if (appsink == nullptr || cfg.fb_cache_size == 0U) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  if (GST_IS_APP_SINK(appsink) == 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  const int fd = dev.fd();
  if (fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }

  // gst_init is idempotent and safe to call after another caller has
  // already initialised GStreamer; passing nullptr argv is the
  // documented "I'm not a main()" shape.
  if (gst_is_initialized() == 0) {
    gst_init(nullptr, nullptr);
  }

  auto src = std::unique_ptr<GstAppsinkSource>(new GstAppsinkSource());
  src->impl_->cfg = cfg;
  src->impl_->drm_fd = fd;
  src->impl_->appsink = GST_ELEMENT(gst_object_ref(appsink));

  // Latest-frame-wins drop semantics: drop=true + max-buffers=1 +
  // sync=false means the upstream decoder never stalls on a slow
  // consumer; older samples are dropped before they reach acquire().
  // Pass 1 / 0 directly rather than glib's TRUE / FALSE — the latter
  // pulls in <glib.h>, which the misc-include-cleaner lint can't
  // trace through (and which forbids direct include of its sub-headers).
  if (cfg.configure_drop_oldest) {
    auto* sink = GST_APP_SINK(appsink);
    gst_app_sink_set_drop(sink, 1);
    gst_app_sink_set_max_buffers(sink, 1U);
    gst_base_sink_set_sync(GST_BASE_SINK(sink), 0);
  }

  return src;
}

drm::expected<void, std::error_code> GstAppsinkSource::drive() noexcept {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  if (impl_->pending_error) {
    return drm::unexpected<std::error_code>(impl_->pending_error);
  }
  // gst_element_get_bus() on a child element returns the bin's internal
  // child-bus, which is NOT where EOS / ERROR lands for the application —
  // the pipeline forwards those to its own application-facing bus.
  // Walk up to the topmost element (typically the GstPipeline) and use
  // its bus instead. For a standalone appsink (no parent), the loop is
  // a no-op and we use the appsink's own bus, which is correct.
  GstElement* top = GST_ELEMENT(gst_object_ref(impl_->appsink));
  for (;;) {
    GstObject* parent = gst_element_get_parent(top);
    if (parent == nullptr) {
      break;
    }
    gst_object_unref(top);
    top = GST_ELEMENT(parent);
  }
  GstBus* bus = gst_element_get_bus(top);
  gst_object_unref(top);
  if (bus == nullptr) {
    return {};
  }
  // Pop one message non-blocking and dispatch on type. Callers run
  // drive() once per frame so a backlog of messages is never expected.
  // GstMessageType is a bitmask flags enum; the OR is intentional and
  // does not name a single declared enumerator.
  // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
  GstMessage* msg =
      gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
  gst_object_unref(bus);
  if (msg == nullptr) {
    return {};
  }
  std::error_code ec;
  if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
    ec = std::make_error_code(std::errc::no_message_available);
  } else {
    ec = std::make_error_code(std::errc::io_error);
  }
  gst_message_unref(msg);
  impl_->pending_error = ec;
  return drm::unexpected<std::error_code>(ec);
}

drm::expected<AcquiredBuffer, std::error_code> GstAppsinkSource::acquire() {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  if (impl_->pending_error) {
    return drm::unexpected<std::error_code>(impl_->pending_error);
  }

  // Pull the most recent sample non-blocking. NULL means "no new
  // sample since last call" — re-use the cached fb_id if we have one.
  GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(impl_->appsink), 0);
  if (sample == nullptr) {
    if (impl_->current_fb_id == 0) {
      return drm::unexpected<std::error_code>(
          std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    return AcquiredBuffer{impl_->current_fb_id, -1, nullptr};
  }

  // First sample: latch caps. Caps change mid-stream tears down the
  // FB cache + sysmem fallback (their dimensions/format are stale)
  // and re-resolves; the new sample's buffer is then imported against
  // the fresh format. Consumers that cached `format()` need to re-query.
  if (!impl_->format_resolved) {
    if (auto ec = resolve_format_from_caps(*impl_, sample); ec) {
      gst_sample_unref(sample);
      impl_->pending_error = ec;
      return drm::unexpected<std::error_code>(ec);
    }
  } else if (!caps_match_cached(*impl_, sample)) {
    teardown_drm_state(*impl_);
    impl_->format_resolved = false;
    if (auto ec = resolve_format_from_caps(*impl_, sample); ec) {
      gst_sample_unref(sample);
      impl_->pending_error = ec;
      return drm::unexpected<std::error_code>(ec);
    }
  }

  GstBuffer* buf = gst_sample_get_buffer(sample);
  if (buf == nullptr) {
    gst_sample_unref(sample);
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::protocol_error));
  }
  const std::error_code import_ec =
      buffer_is_dmabuf(buf) ? import_dmabuf_sample(*impl_, buf) : import_sysmem_sample(*impl_, buf);
  if (import_ec) {
    gst_sample_unref(sample);
    return drm::unexpected<std::error_code>(import_ec);
  }

  // Replace current_sample only after a successful import — on failure
  // the previous sample's buffer is still bound to current_fb_id.
  if (impl_->current_sample != nullptr) {
    gst_sample_unref(impl_->current_sample);
  }
  impl_->current_sample = sample;

  // Producer-supplied fence: dup so the source-owned fd survives past
  // the GstSample's lifetime; the scene closes it post-commit.
  int fence_fd = -1;
  if (impl_->cfg.fence_extractor) {
    const int raw = impl_->cfg.fence_extractor(sample);
    if (raw >= 0) {
      fence_fd = ::fcntl(raw, F_DUPFD_CLOEXEC, 0);
    }
  }
  return AcquiredBuffer{impl_->current_fb_id, fence_fd, nullptr};
}

void GstAppsinkSource::release(AcquiredBuffer /*acquired*/) noexcept {
  // FB IDs are owned by the LRU cache (DMABUF path) or by the lazy
  // dumb-buffer fallback (sysmem path); they outlive any single
  // acquire/release pair. The underlying GstSample is unref'd when
  // the next sample replaces it, returning the buffer to the
  // upstream pool.
}

SourceFormat GstAppsinkSource::format() const noexcept {
  if (!impl_) {
    return {};
  }
  return impl_->format_cache;
}

void GstAppsinkSource::on_session_paused() noexcept {
  if (!impl_) {
    return;
  }
  // Drop every FB ID and GEM handle bound to the dying drm fd. The
  // cached GstSample stays — on resume the next acquire() re-imports
  // the same buffer against the new fd.
  teardown_drm_state(*impl_);
  impl_->drm_fd = -1;
}

drm::expected<void, std::error_code> GstAppsinkSource::on_session_resumed(
    const drm::Device& new_dev) {
  if (!impl_) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::invalid_argument));
  }
  const int fd = new_dev.fd();
  if (fd < 0) {
    return drm::unexpected<std::error_code>(std::make_error_code(std::errc::bad_file_descriptor));
  }
  impl_->drm_fd = fd;
  // Re-import the cached sample against the new fd so the next
  // acquire() returns a valid fb_id immediately, even before the
  // pipeline has produced a fresh sample. Without this the consumer
  // sees EAGAIN on the first post-resume commit; for a single-layer
  // scene that becomes a modeset with no plane armed, which the
  // kernel rejects (EAGAIN/EINVAL depending on driver). On failure
  // we leave current_fb_id at 0 and fall back to the next-sample
  // path — non-fatal, just slower to repaint.
  if (impl_->current_sample != nullptr && impl_->format_resolved) {
    GstBuffer* buf = gst_sample_get_buffer(impl_->current_sample);
    if (buf != nullptr) {
      const std::error_code import_ec = buffer_is_dmabuf(buf) ? import_dmabuf_sample(*impl_, buf)
                                                              : import_sysmem_sample(*impl_, buf);
      (void)import_ec;
    }
  }
  return {};
}

#else  // DRM_CXX_HAS_GSTREAMER

// Stub branch — every call returns function_not_supported. The lints
// `readability-convert-member-functions-to-static` and
// `performance-unnecessary-value-param` would normally fire because
// the bodies never reference `impl_` and the `release(AcquiredBuffer)`
// signature is by-value; but the real branch's signatures must match
// the header verbatim, so the suppressions live here, narrowly, and
// not on the public class itself.

struct GstAppsinkSource::Impl {};

GstAppsinkSource::GstAppsinkSource() = default;

GstAppsinkSource::~GstAppsinkSource() = default;

// NOLINTBEGIN(readability-convert-member-functions-to-static,
//             performance-unnecessary-value-param)

drm::expected<std::unique_ptr<GstAppsinkSource>, std::error_code> GstAppsinkSource::create(
    const drm::Device& /*dev*/, GstElement* /*appsink*/, const GstAppsinkConfig& /*cfg*/) {
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::function_not_supported));
}

drm::expected<void, std::error_code> GstAppsinkSource::drive() noexcept {
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::function_not_supported));
}

drm::expected<AcquiredBuffer, std::error_code> GstAppsinkSource::acquire() {
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::function_not_supported));
}

void GstAppsinkSource::release(AcquiredBuffer /*acquired*/) noexcept {}

SourceFormat GstAppsinkSource::format() const noexcept {
  return SourceFormat{};
}

void GstAppsinkSource::on_session_paused() noexcept {}

drm::expected<void, std::error_code> GstAppsinkSource::on_session_resumed(
    const drm::Device& /*new_dev*/) {
  return drm::unexpected<std::error_code>(std::make_error_code(std::errc::function_not_supported));
}

// NOLINTEND(readability-convert-member-functions-to-static,
//           performance-unnecessary-value-param)

#endif  // DRM_CXX_HAS_GSTREAMER

}  // namespace drm::scene
