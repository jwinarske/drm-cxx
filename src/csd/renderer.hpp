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

#include <cstdint>
#include <memory>
#include <string>
#include <system_error>

namespace drm::csd {

// Single source of truth for decoration layout. Both the renderer and
// any shell that needs to hit-test the painted output read positions
// from here, so the two never desync. All coordinates are in
// decoration-local space (origin = decoration's top-left corner).
//
// The panel is the opaque glass rect inset by `theme.shadow_extent`
// inside the decoration's bounding box; the title bar lives at the top
// of the panel and the three traffic-light buttons are circles centered
// at (close_cx | minimize_cx | maximize_cx, button_cy) with radius
// `button_radius`. For hit testing, the conventional axis-aligned
// bounding box [cx-r, cx+r] × [button_cy-r, button_cy+r] is what the
// shell uses — at this size it's indistinguishable from a Euclidean
// distance check and avoids a sqrt per probe.
struct DecorationGeometry {
  // Panel rectangle (inside the shadow halo). The renderer fills the
  // glass background here; the shell's hit-test rejects clicks
  // outside it as "on the shadow" → not interactive.
  int panel_x{};
  int panel_y{};
  int panel_w{};
  int panel_h{};

  // Title bar height (zero if the theme disables the title bar). The
  // bar occupies [panel_y, panel_y + title_bar_height) in y, full
  // panel width in x.
  int title_bar_height{};

  // Button hit + visual radius and the y-coordinate shared by all
  // three button centers.
  int button_radius{};
  int button_cy{};

  // Button center x-coordinates, ordered Linux-conventionally:
  // Close rightmost, then Minimize, then Maximize toward the title
  // text. Successive centers step left by (2 * button_radius + gap).
  int close_cx{};
  int minimize_cx{};
  int maximize_cx{};
};

// Compute the layout for a decoration of size `deco_w` × `deco_h`
// styled by `theme`. Pure function — no allocation, no I/O — safe to
// call per-frame. Negative-extent / undersized themes clamp panel
// dimensions to zero rather than producing negative widths.
[[nodiscard]] DecorationGeometry decoration_geometry(const Theme& theme, std::uint32_t deco_w,
                                                     std::uint32_t deco_h) noexcept;

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
