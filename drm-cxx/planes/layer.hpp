// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

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
};

class Allocator;
class Output;

class Layer {
 public:
  Layer& set_property(std::string_view name, uint64_t value);
  Layer& disable() noexcept;
  Layer& set_composited() noexcept;

  Layer& set_content_type(ContentType type) noexcept;
  Layer& set_update_hint(uint32_t hz) noexcept;

  [[nodiscard]] bool needs_composition() const noexcept;
  [[nodiscard]] std::optional<uint32_t> assigned_plane_id() const noexcept;

  [[nodiscard]] std::optional<uint64_t> property(std::string_view name) const;
  [[nodiscard]] const std::unordered_map<std::string, uint64_t>& properties() const noexcept;

  [[nodiscard]] std::optional<uint32_t> format() const;
  [[nodiscard]] uint64_t modifier() const;
  [[nodiscard]] uint64_t rotation() const;
  [[nodiscard]] bool requires_scaling() const;
  [[nodiscard]] uint32_t width() const;
  [[nodiscard]] uint32_t height() const;
  [[nodiscard]] Rect crtc_rect() const;

  [[nodiscard]] bool is_composition_layer() const noexcept;
  [[nodiscard]] bool is_dirty() const noexcept;
  [[nodiscard]] ContentType content_type() const noexcept;
  [[nodiscard]] uint32_t update_hz() const noexcept;

  [[nodiscard]] std::size_t property_hash() const;

  void mark_clean() noexcept;

 private:
  friend class Allocator;
  friend class Output;

  std::unordered_map<std::string, uint64_t> properties_;
  bool force_composited_{false};
  bool needs_composition_{false};
  bool dirty_{true};
  bool is_composition_layer_{false};
  std::optional<uint32_t> assigned_plane_;
  ContentType content_type_{ContentType::Generic};
  uint32_t update_hz_{0};
};

}  // namespace drm::planes
