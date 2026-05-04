// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// csd/renderer.hpp — glass-theme decoration renderer.
//
// Renderer paints one decoration into a Surface buffer per draw call.
// It is "stateless" in the sense the plan uses: each call is fully
// described by (Theme, WindowState, target buffer); the Renderer
// itself only owns construction-time state — a font face for the
// title bar and a precomputed noise pattern for the frosted-glass
// look. Both are loaded once at construction and shared across draws.
//
// The public header is Blend2D-free. Tests and the future MDI shell
// reach the renderer through the same `draw()` entry that takes a
// drm::BufferMapping (what Surface::paint() returns) — so the
// Blend2D include is contained to renderer.cpp + the test TU that
// exercises it through draw().

#pragma once

#include "shadow_cache.hpp"
#include "theme.hpp"
#include "window_state.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/detail/expected.hpp>

#include <memory>
#include <string>
#include <system_error>

namespace drm::csd {

struct RendererConfig {
  // Explicit TTF/OTF path. If non-empty and the file loads, that font
  // is used unconditionally.
  std::string font_path;

  // When font_path is empty (or fails to load), try the same
  // well-known Linux font paths the signage_player overlay uses
  // (DejaVu / Liberation / Noto). When false and font_path is empty,
  // the renderer paints decorations without title text.
  bool try_system_font{true};
};

class Renderer {
 public:
  explicit Renderer(RendererConfig cfg = {});
  ~Renderer();

  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  Renderer(Renderer&&) noexcept;
  Renderer& operator=(Renderer&&) noexcept;

  // Paint `state` styled by `theme` into `target` (a Surface mapping
  // wrapped at the call site as a PRGB32 BLImage). The shadow patch
  // is fetched from / inserted into `shadows`. Returns an error only
  // when target is empty / malformed; partial-paint failures inside
  // Blend2D are logged via drm::log_warn but still return ok so the
  // caller's atomic commit isn't aborted by a glyph that didn't load.
  drm::expected<void, std::error_code> draw(const Theme& theme, const WindowState& state,
                                            drm::BufferMapping& target, ShadowCache& shadows);

  // True when a usable font face was loaded at construction. Tests +
  // the MDI shell can branch on this if they want to skip the title-
  // dependent UI path on hosts where no system font is available.
  [[nodiscard]] bool has_font() const noexcept;

 private:
  // BLFontFace + the precomputed noise BLPattern image live behind
  // this opaque struct so the public header stays Blend2D-free.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace drm::csd
