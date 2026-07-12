// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>

namespace drm::planes {

enum class ContentType : uint8_t {
  Generic,
  Video,
  UI,
  Cursor,
};

struct Rect {
  int32_t x{}, y{};
  uint32_t w{}, h{};

  // Value equality over all four fields. Used by the scene's
  // `_if_changed` setters to suppress no-op dirty flips; hand-written
  // because the project targets C++17 (no defaulted operator==).
  [[nodiscard]] bool operator==(const Rect& o) const noexcept {
    return x == o.x && y == o.y && w == o.w && h == o.h;
  }
  [[nodiscard]] bool operator!=(const Rect& o) const noexcept { return !(*this == o); }
};

/// Property tags Layer recognizes. The set is closed: every KMS plane
/// property the scene + allocator emit lives here, and unknown names
/// passed to `set_property(std::string_view, …)` are silently dropped
/// (a kernel commit would EINVAL on them anyway). Adding a new
/// property is a single-file change: append before `Count`, extend
/// the prop_name / parse_prop_tag tables in layer.cpp.
enum class PropTag : uint8_t {
  FbId = 0,
  FbModifier,
  CrtcId,
  CrtcX,
  CrtcY,
  CrtcW,
  CrtcH,
  SrcX,
  SrcY,
  SrcW,
  SrcH,
  Rotation,
  Alpha,
  Zpos,
  PixelFormat,
  InFenceFd,
  Count,  ///< sentinel; not a valid property
};

constexpr std::size_t k_num_props = static_cast<std::size_t>(PropTag::Count);

/// Canonical KMS property-name string for a tag. The returned span is
/// a pointer into a `constexpr` string literal — no allocation, stable
/// for the lifetime of the program.
[[nodiscard]] std::string_view prop_name(PropTag tag) noexcept;

/// Look up a property tag by its canonical KMS name. Returns nullopt
/// for names that aren't part of the PropTag set.
[[nodiscard]] std::optional<PropTag> parse_prop_tag(std::string_view name) noexcept;

class Allocator;
class Output;

class Layer {
 public:
  /// Snapshot of a layer's properties. Used by the allocator to
  /// remember what was last committed to a plane so the next commit
  /// can diff against it and skip unchanged properties. Trivially
  /// copyable — copying is a memcpy.
  struct PropertySnapshot {
    std::array<uint64_t, k_num_props> values{};
    std::bitset<k_num_props> set_mask;
  };

  /// Range over the layer's currently-set properties. Iteration order
  /// is `PropTag` enum order; only set slots are yielded. The
  /// dereferenced value is a `std::pair<std::string_view, uint64_t>`
  /// returned by value — the string_view points at a `constexpr`
  /// literal, so callers can copy or store it without lifetime
  /// concerns.
  class PropertyView {
   public:
    // Lowercase to match the STL iterator-traits convention so this
    // type plays nicely with `std::iterator_traits<...>::iterator` in
    // generic code; the project's CamelCase lint doesn't know about
    // that idiom, hence the narrow NOLINT.
    class iterator {  // NOLINT(readability-identifier-naming)
     public:
      using value_type = std::pair<std::string_view, uint64_t>;
      using difference_type = std::ptrdiff_t;
      using iterator_category = std::input_iterator_tag;

      iterator(const Layer* owner, std::size_t idx) noexcept;
      [[nodiscard]] value_type operator*() const noexcept;
      iterator& operator++() noexcept;
      [[nodiscard]] bool operator==(const iterator& other) const noexcept;
      [[nodiscard]] bool operator!=(const iterator& other) const noexcept;

     private:
      void advance_to_set() noexcept;
      const Layer* owner_;
      std::size_t idx_;
    };

    explicit PropertyView(const Layer* owner) noexcept : owner_(owner) {}
    [[nodiscard]] iterator begin() const noexcept;
    [[nodiscard]] iterator end() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

   private:
    const Layer* owner_;
  };

  /// Set a property by canonical KMS name. Unknown names are silently
  /// dropped — the kernel would EINVAL on them at atomic-commit time
  /// anyway, so accepting them here would just hide the typo. Use
  /// the `PropTag` overload internally for the hot path; the string
  /// version exists for ergonomics at caller sites that already have
  /// the canonical literal.
  Layer& set_property(std::string_view name, uint64_t value);

  /// Fast-path overload. No string lookup; direct array set.
  Layer& set_property(PropTag tag, uint64_t value) noexcept;

  Layer& disable() noexcept;
  Layer& set_composited() noexcept;

  /// Transient composition flag. Allocator treats this exactly like
  /// `force_composited_` (skip from placement, route to composition
  /// fallback), but the bit is set and cleared by the scene per
  /// commit rather than by add_layer at layer-creation time. Lets
  /// scene-level constraints (e.g. EGL Streams Exclusive mixing
  /// mode forcing FB-ID layers through composition when a stream
  /// layer is on the same CRTC) live alongside user-supplied
  /// `force_composited` without conflating the two.
  Layer& set_transient_composited(bool composited) noexcept;
  [[nodiscard]] bool is_transient_composited() const noexcept;

  Layer& set_content_type(ContentType type) noexcept;
  Layer& set_update_hint(uint32_t hz) noexcept;
  Layer& set_app_priority(uint8_t priority) noexcept;

  [[nodiscard]] bool needs_composition() const noexcept;
  [[nodiscard]] std::optional<uint32_t> assigned_plane_id() const noexcept;

  [[nodiscard]] std::optional<uint64_t> property(std::string_view name) const;
  [[nodiscard]] std::optional<uint64_t> property(PropTag tag) const noexcept;

  /// Range of set (name, value) pairs. See `PropertyView`.
  [[nodiscard]] PropertyView properties() const noexcept;

  /// Snapshot the property state for diff comparison. Cheap: trivial
  /// copy of the underlying array + bitset.
  [[nodiscard]] PropertySnapshot snapshot() const noexcept;

  [[nodiscard]] std::optional<uint32_t> format() const;
  [[nodiscard]] uint64_t modifier() const;
  [[nodiscard]] uint64_t rotation() const;
  [[nodiscard]] bool requires_scaling() const;
  [[nodiscard]] uint32_t width() const;
  [[nodiscard]] uint32_t height() const;
  [[nodiscard]] Rect crtc_rect() const;

  [[nodiscard]] bool is_composition_layer() const noexcept;

  /// True iff the scene has pinned this layer to a specific plane and
  /// owns the property writes itself — the allocator must skip it
  /// during plane assignment, scoring, and the test-commit pipeline,
  /// the same way it skips `is_composition_layer()`. Used by EGL
  /// stream sources, whose backing plane state is established
  /// out-of-band (eglStreamConsumerOutputEXT) before the scene's
  /// atomic commit; the allocator's TEST commits would reject a
  /// stream-bound plane that lacks the kernel-internal FB_ID those
  /// extensions set up.
  [[nodiscard]] bool is_externally_bound() const noexcept;

  /// Mark this layer as externally bound. Default false. Set by the
  /// scene during layer lowering for sources whose `binding_model()`
  /// is `DriverOwnsBinding`.
  Layer& set_externally_bound(bool externally_bound) noexcept;

  [[nodiscard]] bool is_dirty() const noexcept;
  [[nodiscard]] ContentType content_type() const noexcept;
  [[nodiscard]] uint32_t update_hz() const noexcept;
  /// Application-level within-content-class placement priority (0 =
  /// default). Lowered from `LayerDesc::app_priority`; a hint for breaking
  /// ties among like-content layers when plane pressure forces a drop.
  [[nodiscard]] uint8_t app_priority() const noexcept;

  /// Order-independent hash of the layer's properties, skipping
  /// `FB_ID` (which changes every frame and would dirty the failure
  /// cache uselessly). Cached and invalidated on `set_property` /
  /// `disable`; reads are O(1) on the steady-state path.
  [[nodiscard]] std::size_t property_hash() const;

  void mark_clean() noexcept;

 private:
  friend class Allocator;
  friend class Output;
  friend class PropertyView::iterator;

  std::array<uint64_t, k_num_props> values_{};
  std::bitset<k_num_props> set_mask_;
  mutable std::optional<std::size_t> cached_hash_;
  bool force_composited_{false};
  bool transient_composited_{false};
  bool needs_composition_{false};
  bool dirty_{true};
  bool is_composition_layer_{false};
  bool externally_bound_{false};
  std::optional<uint32_t> assigned_plane_;
  ContentType content_type_{ContentType::Generic};
  uint32_t update_hz_{0};
  uint8_t app_priority_{0};
};

}  // namespace drm::planes
