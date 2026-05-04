// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// keyboard — interactive keyboard-input demo. Shows that the
// drm::input pipeline (Seat -> Keyboard -> KeyRepeater) carries enough
// signal to implement a real text-input flow on top of LayerScene,
// including the IBus/GTK Ctrl+Shift+u Unicode-codepoint entry sequence
// (e.g. Ctrl+Shift, "u", "1", "f", "6", "0", "0", release Ctrl+Shift
// commits U+1F600). The display is a single full-screen XRGB8888
// LayerScene layer repainted via Blend2D on every event.
//
// Key bindings:
//   Any printable key  — typed into the history strip (auto-repeats)
//   Ctrl+Shift then u  — begin Unicode entry; subsequent hex digits
//                        accumulate. Release Ctrl+Shift to commit.
//   Esc inside entry   — cancel the in-flight Unicode entry
//   Ctrl+C             — quit

#include "../../common/open_output.hpp"
#include "../../common/vt_switch.hpp"

#include <drm-cxx/buffer_mapping.hpp>
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/input/key_repeater.hpp>
#include <drm-cxx/input/keyboard.hpp>
#include <drm-cxx/input/seat.hpp>
#include <drm-cxx/modeset/page_flip.hpp>
#include <drm-cxx/planes/layer.hpp>
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/session/seat.hpp>

// Blend2D ships either /usr/include/blend2d/blend2d.h (Fedora, Arch,
// upstream) or /usr/include/blend2d.h (some Debian packages). Mirror
// signage_player's probe so both layouts work without a build option.
#if __has_include(<blend2d/blend2d.h>)
#include <blend2d/blend2d.h>  // NOLINT(misc-include-cleaner)
#elif __has_include(<blend2d.h>)
#include <blend2d.h>  // NOLINT(misc-include-cleaner)
#else
#error "blend2d header not found"
#endif

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <linux/input-event-codes.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <system_error>
#include <utility>
#include <variant>

// The build system passes -DKEYBOARD_DEMO_FONT_PATH="..." pointing at
// the Noto Sans TTF fetched at configure time. The fallback below keeps
// this file IDE-parseable when clangd processes it without the build's
// compile_commands.json — the runtime open will then fail loudly.
#ifndef KEYBOARD_DEMO_FONT_PATH
#define KEYBOARD_DEMO_FONT_PATH "/dev/null"  // NOLINT(cppcoreguidelines-macro-usage)
#endif

namespace {

// ── Codepoint → UTF-8 ─────────────────────────────────────────

// Encode a single Unicode codepoint into UTF-8. Returns the byte count
// (1..4) or 0 for invalid (surrogate / >U+10FFFF).
std::size_t encode_utf8(std::uint32_t cp, std::array<char, 5>& out) noexcept {
  if (cp < 0x80U) {
    out[0] = static_cast<char>(cp);
    out[1] = '\0';
    return 1;
  }
  if (cp < 0x800U) {
    out[0] = static_cast<char>(0xC0U | (cp >> 6));
    out[1] = static_cast<char>(0x80U | (cp & 0x3FU));
    out[2] = '\0';
    return 2;
  }
  if (cp < 0x10000U) {
    if (cp >= 0xD800U && cp <= 0xDFFFU) {
      return 0;  // Lone surrogate.
    }
    out[0] = static_cast<char>(0xE0U | (cp >> 12));
    out[1] = static_cast<char>(0x80U | ((cp >> 6) & 0x3FU));
    out[2] = static_cast<char>(0x80U | (cp & 0x3FU));
    out[3] = '\0';
    return 3;
  }
  if (cp <= 0x10FFFFU) {
    out[0] = static_cast<char>(0xF0U | (cp >> 18));
    out[1] = static_cast<char>(0x80U | ((cp >> 12) & 0x3FU));
    out[2] = static_cast<char>(0x80U | ((cp >> 6) & 0x3FU));
    out[3] = static_cast<char>(0x80U | (cp & 0x3FU));
    out[4] = '\0';
    return 4;
  }
  return 0;
}

// Map KEY_0..KEY_9 + KEY_A..KEY_F (in their Linux keycode ordering) to
// a hex nibble. Returns -1 for non-hex keys. KEY_A..KEY_F are tested via
// xkb so AltGr-shifted layouts that re-map "a" to something else don't
// silently miss; for our purposes the keycode-based check is sufficient
// because hex entry is universal across Latin-script keyboards.
int hex_nibble_from_key(std::uint32_t key) noexcept {
  switch (key) {
    case KEY_0:
      return 0;
    case KEY_1:
      return 1;
    case KEY_2:
      return 2;
    case KEY_3:
      return 3;
    case KEY_4:
      return 4;
    case KEY_5:
      return 5;
    case KEY_6:
      return 6;
    case KEY_7:
      return 7;
    case KEY_8:
      return 8;
    case KEY_9:
      return 9;
    case KEY_A:
      return 10;
    case KEY_B:
      return 11;
    case KEY_C:
      return 12;
    case KEY_D:
      return 13;
    case KEY_E:
      return 14;
    case KEY_F:
      return 15;
    case KEY_KP0:
      return 0;
    case KEY_KP1:
      return 1;
    case KEY_KP2:
      return 2;
    case KEY_KP3:
      return 3;
    case KEY_KP4:
      return 4;
    case KEY_KP5:
      return 5;
    case KEY_KP6:
      return 6;
    case KEY_KP7:
      return 7;
    case KEY_KP8:
      return 8;
    case KEY_KP9:
      return 9;
    default:
      return -1;
  }
}

// ── Ctrl+Shift+u Unicode entry state machine ──────────────────

class UnicodeEntry {
 public:
  enum class State : std::uint8_t { Idle, Collecting };

  // Result of feeding a key into the state machine.
  struct Outcome {
    bool consumed{false};  // Don't pass to normal text handling.
    std::optional<std::uint32_t> committed_codepoint;
  };

  // Drive the state machine from a real (non-repeat) keyboard event.
  // The Keyboard reference is used to read modifier state.
  Outcome on_key(const drm::input::KeyboardEvent& ke, const drm::input::Keyboard& kb) {
    Outcome out;

    // Cancel mid-entry if Ctrl or Shift release before commit. The
    // commit-on-release is the GTK convention: collect digits while
    // both modifiers are held, commit when the user releases either.
    if (state_ == State::Collecting && !ke.pressed) {
      if (ke.key == KEY_LEFTSHIFT || ke.key == KEY_RIGHTSHIFT || ke.key == KEY_LEFTCTRL ||
          ke.key == KEY_RIGHTCTRL) {
        out.committed_codepoint = commit();
        out.consumed = true;
        return out;
      }
    }

    if (!ke.pressed) {
      return out;
    }

    // Enter Collecting on Ctrl+Shift+u. Only Ctrl+Shift held — no Alt /
    // Super — to avoid colliding with VT-switch chords (Ctrl+Alt+F<n>).
    const bool ctrl_shift_only =
        kb.ctrl_active() && kb.shift_active() && !kb.alt_active() && !kb.super_active();
    if (state_ == State::Idle && ctrl_shift_only && ke.key == KEY_U) {
      state_ = State::Collecting;
      hex_.clear();
      out.consumed = true;
      return out;
    }

    if (state_ == State::Collecting) {
      // Esc cancels.
      if (ke.key == KEY_ESC) {
        cancel();
        out.consumed = true;
        return out;
      }
      // Enter or Space commits early (also a GTK convention).
      if (ke.key == KEY_ENTER || ke.key == KEY_KPENTER || ke.key == KEY_SPACE) {
        out.committed_codepoint = commit();
        out.consumed = true;
        return out;
      }
      // Backspace removes one nibble.
      if (ke.key == KEY_BACKSPACE) {
        if (!hex_.empty()) {
          hex_.pop_back();
        }
        out.consumed = true;
        return out;
      }
      // Hex digit accumulates. Cap at 6 nibbles (U+10FFFF).
      const int nibble = hex_nibble_from_key(ke.key);
      if (nibble >= 0 && hex_.size() < 6) {
        constexpr std::array<char, 16> k_hex = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
        hex_.push_back(k_hex.at(static_cast<std::size_t>(nibble)));
        out.consumed = true;
        return out;
      }
      // Any other press inside Collecting is consumed-but-ignored: avoid
      // leaking unrelated keystrokes into the history strip mid-entry.
      out.consumed = true;
      return out;
    }

    return out;
  }

  [[nodiscard]] State state() const noexcept { return state_; }
  [[nodiscard]] std::string_view buffer() const noexcept { return hex_; }

 private:
  std::uint32_t commit() {
    std::uint32_t cp = 0;
    if (hex_.empty()) {
      cancel();
      return 0;
    }
    for (const char c : hex_) {
      cp <<= 4U;
      if (c >= '0' && c <= '9') {
        cp |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'A' && c <= 'F') {
        cp |= static_cast<std::uint32_t>(c - 'A' + 10);
      }
    }
    cancel();
    return cp;
  }

  void cancel() {
    state_ = State::Idle;
    hex_.clear();
  }

  State state_{State::Idle};
  std::string hex_;
};

// ── Demo state ────────────────────────────────────────────────

struct DemoState {
  std::string last_utf8;
  std::uint32_t last_codepoint{0};
  std::deque<std::string> history;  // Most-recent-first, capped.
  bool shift{false};
  bool ctrl{false};
  bool alt{false};
  bool super{false};
  bool caps_lock{false};
  bool num_lock{false};
  UnicodeEntry::State entry_state{UnicodeEntry::State::Idle};
  std::string entry_buffer;
};

constexpr std::size_t k_history_max = 24;

void push_history(DemoState& s, std::string utf8, std::uint32_t cp) {
  s.last_utf8 = std::move(utf8);
  s.last_codepoint = cp;
  s.history.push_front(s.last_utf8);
  while (s.history.size() > k_history_max) {
    s.history.pop_back();
  }
}

// ── Blend2D rendering ─────────────────────────────────────────

// NOLINTBEGIN(misc-include-cleaner) — Blend2D types come in via the
// __has_include block above; include-cleaner can't trace through it.

// Render into a private off-screen image. The dumb buffer driving scanout
// is single-buffered; binding a BLContext directly to its mapping makes
// every BL op visible, producing a flicker as the painter's progressive
// state — clear, then text, then banner — is scanned out one pass at a
// time. Painting into an off-screen BLImage keeps that progression off
// the display, and a single blit at the end of the frame minimizes the
// window during which scanout sees a half-updated frame.
void paint(BLImage& target, const DemoState& s, const BLFontFace& face) {
  BLImageData td{};
  if (target.get_data(&td) != BL_SUCCESS) {
    return;
  }
  const auto width = static_cast<std::uint32_t>(td.size.w);
  const auto height = static_cast<std::uint32_t>(td.size.h);
  if (width == 0U || height == 0U) {
    return;
  }

  BLContext ctx(target);
  ctx.set_comp_op(BL_COMP_OP_SRC_COPY);
  ctx.fill_all(BLRgba32(0xFF101820U));  // Dark slate background.
  ctx.set_comp_op(BL_COMP_OP_SRC_OVER);

  if (!face.is_valid()) {
    ctx.end();
    return;
  }

  const auto fw = static_cast<double>(width);
  const auto fh = static_cast<double>(height);

  // ── Modifier strip (top) ────────────────────────────────
  // Modifiers (currently held) on the left, locks (latched) on the
  // right. Locks use an amber tint so the eye can tell at a glance
  // which group lit up — held modifiers are blue, latched locks are
  // amber, both visually distinct from the inactive slate.
  {
    BLFont font;
    font.create_from_face(face, 28.0F);
    constexpr std::array<const char*, 6> labels = {"Shift", "Ctrl", "Alt", "Super", "Caps", "Num"};
    const std::array<bool, 6> active = {s.shift, s.ctrl, s.alt, s.super, s.caps_lock, s.num_lock};
    constexpr std::size_t k_first_lock = 4;
    const double box_w = 120.0;
    const double box_h = 56.0;
    const double gap = 12.0;
    const double group_gap = 28.0;  // Extra space between mods and locks.
    const double total_w = (box_w * 6.0) + (gap * 4.0) + group_gap;
    double x = (fw - total_w) / 2.0;
    const double y = 24.0;
    for (std::size_t i = 0; i < labels.size(); ++i) {
      const bool is_lock = i >= k_first_lock;
      const std::uint32_t active_fill = is_lock ? 0xFFE6A33EU : 0xFF3FA9F5U;
      const std::uint32_t fill_argb = active.at(i) ? active_fill : 0xFF252F3DU;
      const std::uint32_t fg_argb = active.at(i) ? 0xFFFFFFFFU : 0xFF8A98AAU;
      ctx.fill_round_rect(BLRoundRect(x, y, box_w, box_h, 8.0), BLRgba32(fill_argb));
      const std::string_view text = labels.at(i);
      BLGlyphBuffer gb;
      gb.set_utf8_text(text.data(), text.size());
      font.shape(gb);
      BLTextMetrics tm{};
      font.get_text_metrics(gb, tm);
      const double text_w = tm.bounding_box.x1 - tm.bounding_box.x0;
      const double tx = x + ((box_w - text_w) / 2.0) - tm.bounding_box.x0;
      const double ty = y + (box_h / 2.0) + (font.metrics().ascent / 2.5);
      ctx.fill_utf8_text(BLPoint(tx, ty), font, text.data(), text.size(), BLRgba32(fg_argb));
      x += box_w + gap;
      if (i + 1 == k_first_lock) {
        x += group_gap - gap;  // Inject the inter-group gap.
      }
    }
  }

  // ── Big centered glyph for the most recent character ────
  {
    BLFont font;
    font.create_from_face(face, static_cast<float>(std::min(fw, fh) * 0.32));
    const std::string_view glyph_text = s.last_utf8;
    if (!glyph_text.empty()) {
      BLGlyphBuffer gb;
      gb.set_utf8_text(glyph_text.data(), glyph_text.size());
      font.shape(gb);
      BLTextMetrics tm{};
      font.get_text_metrics(gb, tm);
      const double text_w = tm.bounding_box.x1 - tm.bounding_box.x0;
      const double tx = ((fw - text_w) / 2.0) - tm.bounding_box.x0;
      const double ty = (fh / 2.0) + (font.metrics().ascent / 4.0);
      ctx.fill_utf8_text(BLPoint(tx, ty), font, glyph_text.data(), glyph_text.size(),
                         BLRgba32(0xFFFFFFFFU));
    } else {
      BLFont small;
      small.create_from_face(face, 24.0F);
      constexpr std::string_view hint = "press a key";
      BLGlyphBuffer gb;
      gb.set_utf8_text(hint.data(), hint.size());
      small.shape(gb);
      BLTextMetrics tm{};
      small.get_text_metrics(gb, tm);
      const double text_w = tm.bounding_box.x1 - tm.bounding_box.x0;
      const double tx = ((fw - text_w) / 2.0) - tm.bounding_box.x0;
      const double ty = fh / 2.0;
      ctx.fill_utf8_text(BLPoint(tx, ty), small, hint.data(), hint.size(), BLRgba32(0xFF8A98AAU));
    }
  }

  // ── Codepoint label below the glyph ─────────────────────
  if (s.last_codepoint != 0U) {
    BLFont font;
    font.create_from_face(face, 32.0F);
    char buf[16] = {};
    std::snprintf(buf, sizeof(buf), "U+%04X", s.last_codepoint);
    const std::string_view text = buf;
    BLGlyphBuffer gb;
    gb.set_utf8_text(text.data(), text.size());
    font.shape(gb);
    BLTextMetrics tm{};
    font.get_text_metrics(gb, tm);
    const double text_w = tm.bounding_box.x1 - tm.bounding_box.x0;
    const double tx = ((fw - text_w) / 2.0) - tm.bounding_box.x0;
    const double ty = (fh / 2.0) + (std::min(fw, fh) * 0.20);
    ctx.fill_utf8_text(BLPoint(tx, ty), font, text.data(), text.size(), BLRgba32(0xFF8A98AAU));
  }

  // ── Unicode-entry banner (when active) ──────────────────
  if (s.entry_state == UnicodeEntry::State::Collecting) {
    const double banner_h = 80.0;
    const double banner_y = (fh - 200.0);
    ctx.fill_rect(BLRect(0.0, banner_y, fw, banner_h), BLRgba32(0xFF1B6FB0U));
    BLFont font;
    font.create_from_face(face, 36.0F);
    std::string text = "Ctrl+Shift+u  →  U+";
    text += s.entry_buffer.empty() ? "_" : s.entry_buffer;
    BLGlyphBuffer gb;
    gb.set_utf8_text(text.data(), text.size());
    font.shape(gb);
    BLTextMetrics tm{};
    font.get_text_metrics(gb, tm);
    const double text_w = tm.bounding_box.x1 - tm.bounding_box.x0;
    const double tx = ((fw - text_w) / 2.0) - tm.bounding_box.x0;
    const double ty = banner_y + (banner_h / 2.0) + (font.metrics().ascent / 2.5);
    ctx.fill_utf8_text(BLPoint(tx, ty), font, text.data(), text.size(), BLRgba32(0xFFFFFFFFU));
  }

  // ── History strip (bottom) ──────────────────────────────
  if (!s.history.empty()) {
    BLFont font;
    font.create_from_face(face, 28.0F);
    std::string strip;
    for (const auto& c : s.history) {
      strip += c;
      strip.push_back(' ');
    }
    BLGlyphBuffer gb;
    gb.set_utf8_text(strip.data(), strip.size());
    font.shape(gb);
    BLTextMetrics tm{};
    font.get_text_metrics(gb, tm);
    const double text_w = tm.bounding_box.x1 - tm.bounding_box.x0;
    const double tx = ((fw - text_w) / 2.0) - tm.bounding_box.x0;
    const double ty = fh - 40.0;
    ctx.fill_utf8_text(BLPoint(tx, ty), font, strip.data(), strip.size(), BLRgba32(0xFFD0D8E2U));
  }

  ctx.end();
}

}  // namespace

int main(int argc, char** argv) {
  // Parse our own flags before handing argv to open_and_pick_output —
  // strip them in place so the device/connector picker sees an argv
  // it understands. std::stoul throws on garbage; wrap with try.
  drm::input::RepeatConfig repeat_cfg{};
  auto consume_uint_flag = [&](std::string_view flag, std::uint32_t& out) -> bool {
    for (int i = 1; i < argc; ++i) {
      std::string_view const a = argv[i];
      std::string_view value;
      int consume = 0;
      if (a == flag && i + 1 < argc) {
        value = argv[i + 1];
        consume = 2;
      } else if (a.size() > flag.size() + 1 && a.substr(0, flag.size()) == flag &&
                 a[flag.size()] == '=') {
        value = a.substr(flag.size() + 1);
        consume = 1;
      } else {
        continue;
      }
      try {
        const auto v = std::stoul(std::string(value));
        if (v > std::numeric_limits<std::uint32_t>::max()) {
          drm::println(stderr, "keyboard: {} value out of range: {}", flag, value);
          return false;
        }
        out = static_cast<std::uint32_t>(v);
      } catch (const std::exception&) {
        drm::println(stderr, "keyboard: {} expects an integer, got '{}'", flag, value);
        return false;
      }
      // Shift remaining argv left to remove the consumed slot(s).
      for (int j = i; j + consume < argc; ++j) {
        argv[j] = argv[j + consume];
      }
      argc -= consume;
      return true;
    }
    return true;  // Flag not present; keep default.
  };
  if (!consume_uint_flag("--repeat-delay-ms", repeat_cfg.delay_ms) ||
      !consume_uint_flag("--repeat-interval-ms", repeat_cfg.interval_ms)) {
    return EXIT_FAILURE;
  }

  auto output = drm::examples::open_and_pick_output(argc, argv);
  if (!output) {
    return EXIT_FAILURE;
  }
  auto& dev = output->device;
  auto& seat = output->seat;
  const std::uint32_t crtc_id = output->crtc_id;
  const std::uint32_t connector_id = output->connector_id;
  const drmModeModeInfo mode = output->mode;
  const std::uint32_t fb_w = mode.hdisplay;
  const std::uint32_t fb_h = mode.vdisplay;
  drm::println("Mode: {}x{}@{}Hz on connector {} / CRTC {}", fb_w, fb_h, mode.vrefresh,
               connector_id, crtc_id);

  BLFontFace face;
  if (face.create_from_file(KEYBOARD_DEMO_FONT_PATH) != BL_SUCCESS) {
    drm::println(stderr, "keyboard: failed to load font from {}", KEYBOARD_DEMO_FONT_PATH);
    return EXIT_FAILURE;
  }

  bool session_paused = false;
  bool flip_pending = false;
  int pending_resume_fd = -1;

  auto bg_src = drm::scene::DumbBufferSource::create(dev, fb_w, fb_h, DRM_FORMAT_XRGB8888);
  if (!bg_src) {
    drm::println(stderr, "DumbBufferSource::create failed: {}", bg_src.error().message());
    return EXIT_FAILURE;
  }
  auto* bg = bg_src->get();

  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = crtc_id;
  cfg.connector_id = connector_id;
  cfg.mode = mode;
  auto scene_res = drm::scene::LayerScene::create(dev, cfg);
  if (!scene_res) {
    drm::println(stderr, "LayerScene::create failed: {}", scene_res.error().message());
    return EXIT_FAILURE;
  }
  auto scene = std::move(*scene_res);

  drm::scene::LayerDesc desc;
  desc.source = std::move(*bg_src);
  desc.display.src_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  desc.display.dst_rect = drm::scene::Rect{0, 0, fb_w, fb_h};
  desc.content_type = drm::planes::ContentType::Generic;
  if (auto h = scene->add_layer(std::move(desc)); !h) {
    drm::println(stderr, "add_layer failed: {}", h.error().message());
    return EXIT_FAILURE;
  }

  DemoState state;
  UnicodeEntry entry;

  // Off-screen render target. Painting goes here; a single row-by-row
  // memcpy then transfers the finished frame to the dumb buffer that's
  // actively scanning out, keeping intermediate paint state off the
  // display.
  BLImage offscreen(static_cast<int>(fb_w), static_cast<int>(fb_h), BL_FORMAT_PRGB32);

  auto repaint = [&]() noexcept {
    state.entry_state = entry.state();
    state.entry_buffer = std::string(entry.buffer());
    paint(offscreen, state, face);

    auto m = bg->map(drm::MapAccess::Write);
    if (!m) {
      return;
    }
    BLImageData od{};
    if (offscreen.get_data(&od) != BL_SUCCESS) {
      return;
    }
    const auto* src = static_cast<const std::uint8_t*>(od.pixel_data);
    const auto src_stride = static_cast<std::size_t>(od.stride);
    auto pixels = m->pixels();
    auto* dst = pixels.data();
    const auto dst_stride = static_cast<std::size_t>(m->stride());
    const auto row_bytes = std::min(dst_stride, static_cast<std::size_t>(fb_w) * 4U);
    for (std::uint32_t y = 0; y < fb_h; ++y) {
      std::memcpy(dst + (y * dst_stride), src + (y * src_stride), row_bytes);
    }
  };
  repaint();

  drm::PageFlip page_flip(dev);
  page_flip.set_handler(
      [&](std::uint32_t /*c*/, std::uint64_t /*s*/, std::uint64_t /*t*/) { flip_pending = false; });

  drm::input::InputDeviceOpener libinput_opener;
  if (seat) {
    libinput_opener = seat->input_opener();
  }
  auto input_seat_res = drm::input::Seat::open({}, std::move(libinput_opener));
  if (!input_seat_res) {
    drm::println(stderr, "Failed to open input seat (need root or 'input' group membership)");
    return EXIT_FAILURE;
  }
  auto& input_seat = *input_seat_res;

  auto keyboard_res = drm::input::Keyboard::create();
  if (!keyboard_res) {
    drm::println(stderr, "Keyboard::create failed: {}", keyboard_res.error().message());
    return EXIT_FAILURE;
  }
  auto& keyboard = *keyboard_res;

  auto repeater_res = drm::input::KeyRepeater::create(&keyboard, repeat_cfg);
  if (!repeater_res) {
    drm::println(stderr, "KeyRepeater::create failed: {}", repeater_res.error().message());
    return EXIT_FAILURE;
  }
  auto& repeater = *repeater_res;

  bool quit = false;
  bool dirty = false;
  drm::examples::VtChordTracker vt_chord;

  // Action handler: shared between real seat events and synthesized
  // repeat events. Modifier reflectors are refreshed by the seat-side
  // handler immediately after process_key, so this only handles glyph
  // history and the dirty flag.
  auto handle_text_input = [&](const drm::input::KeyboardEvent& ke) {
    if (ke.pressed && entry.state() == UnicodeEntry::State::Idle) {
      // Backspace pops the most-recent glyph; Delete clears the strip.
      // Both are routed before the printable-utf8 branch because xkb
      // hands them a control-character utf8 (0x08 / 0x7F) that the
      // ke.utf8 check below would reject.
      if (ke.key == KEY_BACKSPACE) {
        if (!state.history.empty()) {
          state.history.pop_front();
        }
        state.last_utf8 = state.history.empty() ? std::string{} : state.history.front();
        state.last_codepoint = 0;
      } else if (ke.key == KEY_DELETE) {
        state.history.clear();
        state.last_utf8.clear();
        state.last_codepoint = 0;
      } else if (ke.utf8[0] != '\0') {
        // A printable utf8 from the real keymap. Skip control characters.
        const auto first = static_cast<unsigned char>(ke.utf8[0]);
        if (first >= 0x20U && first != 0x7FU) {
          push_history(state, std::string(ke.utf8), ke.sym);
        }
      }
    }
    dirty = true;
  };

  drm::input::KeyboardLeds last_leds;

  input_seat.set_event_handler([&](const drm::input::InputEvent& event) {
    const auto* raw = std::get_if<drm::input::KeyboardEvent>(&event);
    if (raw == nullptr) {
      return;
    }
    drm::input::KeyboardEvent ke = *raw;
    keyboard.process_key(ke);

    // Refresh modifier and lock reflectors before any early-return
    // paths (vt_chord, quit, entry-consumed) — otherwise pure
    // modifier/lock press/release events get swallowed and the strip
    // goes stale. After locks change, push the LED state back to the
    // physical keyboard so its Caps/Num/Scroll lamps light up.
    state.shift = keyboard.shift_active();
    state.ctrl = keyboard.ctrl_active();
    state.alt = keyboard.alt_active();
    state.super = keyboard.super_active();
    state.caps_lock = keyboard.caps_lock_active();
    state.num_lock = keyboard.num_lock_active();
    auto leds = keyboard.leds_state();
    if (leds != last_leds) {
      input_seat.update_keyboard_leds(leds);
      last_leds = leds;
    }

    if (vt_chord.observe(ke, seat ? &*seat : nullptr)) {
      dirty = true;
      return;
    }
    // Only Ctrl+C exits — every other key (including Esc and 'q') is
    // typing input the demo wants to capture. seatd's KD_GRAPHICS
    // suppresses the kernel's normal ^C → SIGINT translation, so the
    // libinput keyboard is the only path that still sees the chord.
    if (ke.pressed && ke.key == KEY_C && keyboard.ctrl_active()) {
      quit = true;
      return;
    }

    auto outcome = entry.on_key(ke, keyboard);
    if (outcome.committed_codepoint.has_value()) {
      std::array<char, 5> buf{};
      const auto n = encode_utf8(*outcome.committed_codepoint, buf);
      if (n > 0) {
        push_history(state, std::string(buf.data(), n), *outcome.committed_codepoint);
      }
      dirty = true;
    }
    if (outcome.consumed) {
      dirty = true;
      return;
    }

    repeater.on_key(ke);
    handle_text_input(ke);
  });

  repeater.set_handler(handle_text_input);

  if (seat) {
    seat->set_pause_callback([&]() {
      session_paused = true;
      flip_pending = false;
      repeater.cancel();
      (void)input_seat.suspend();
    });
    // libseat fires resume once per tracked device — the DRM card and
    // every libinput evdev fd opened through Seat::input_opener. Only
    // the card fd belongs in pending_resume_fd; the input fds are
    // already plumbed back to libinput by the InputDeviceOpener path.
    // Without this filter the demo would swap output->device to the
    // last-iterated evdev fd and the post-resume atomic commit would
    // fail on a non-DRM fd.
    seat->set_resume_callback([&](std::string_view path, int new_fd) {
      session_paused = false;
      (void)input_seat.resume();
      constexpr std::string_view k_drm_prefix = "/dev/dri/";
      if (path.size() >= k_drm_prefix.size() &&
          path.substr(0, k_drm_prefix.size()) == k_drm_prefix) {
        pending_resume_fd = new_fd;
      }
    });
  }

  if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
    drm::println(stderr, "first commit failed: {}", r.error().message());
    return EXIT_FAILURE;
  }
  flip_pending = true;
  drm::println(
      "Type to print; Ctrl+Shift+u then hex digits (release Ctrl+Shift to commit) for "
      "Unicode entry. Ctrl+C to quit.");

  pollfd pfds[4]{};
  pfds[0].fd = input_seat.fd();
  pfds[0].events = POLLIN;
  pfds[1].fd = dev.fd();
  pfds[1].events = POLLIN;
  pfds[2].fd = seat ? seat->poll_fd() : -1;
  pfds[2].events = POLLIN;
  pfds[3].fd = repeater.fd();
  pfds[3].events = POLLIN;

  while (!quit) {
    int timeout = -1;
    if (flip_pending) {
      timeout = 16;
    }
    if (const int ret = poll(pfds, 4, timeout); ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      drm::println(stderr, "poll: {}", std::system_category().message(errno));
      break;
    }
    if ((pfds[0].revents & POLLIN) != 0) {
      (void)input_seat.dispatch();
    }
    if ((pfds[1].revents & POLLIN) != 0) {
      (void)page_flip.dispatch(0);
    }
    if ((pfds[2].revents & POLLIN) != 0 && seat) {
      seat->dispatch();
    }
    if ((pfds[3].revents & POLLIN) != 0) {
      repeater.dispatch();
    }

    if (pending_resume_fd != -1) {
      const int new_fd = pending_resume_fd;
      pending_resume_fd = -1;
      scene->on_session_paused();
      output->device = drm::Device::from_fd(new_fd);
      pfds[1].fd = dev.fd();
      if (auto r = dev.enable_universal_planes(); !r) {
        drm::println(stderr, "resume: enable_universal_planes failed");
        break;
      }
      if (auto r = dev.enable_atomic(); !r) {
        drm::println(stderr, "resume: enable_atomic failed");
        break;
      }
      if (auto r = scene->on_session_resumed(dev); !r) {
        drm::println(stderr, "resume: on_session_resumed failed: {}", r.error().message());
        break;
      }
      page_flip = drm::PageFlip(dev);
      page_flip.set_handler(
          [&](std::uint32_t, std::uint64_t, std::uint64_t) { flip_pending = false; });
      repaint();
      if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip); !r) {
        if (r.error() == std::errc::permission_denied) {
          flip_pending = false;
          continue;
        }
        drm::println(stderr, "post-resume commit failed: {}", r.error().message());
        break;
      }
      flip_pending = true;
      dirty = false;
    }

    if (flip_pending || session_paused) {
      continue;
    }

    if (dirty) {
      dirty = false;
      repaint();
      if (auto r = scene->commit(DRM_MODE_PAGE_FLIP_EVENT | DRM_MODE_ATOMIC_NONBLOCK, &page_flip);
          !r) {
        if (r.error() == std::errc::permission_denied) {
          session_paused = true;
          flip_pending = false;
          continue;
        }
        drm::println(stderr, "commit failed: {}", r.error().message());
        break;
      }
      flip_pending = true;
    }
  }

  return EXIT_SUCCESS;
}

// NOLINTEND(misc-include-cleaner)