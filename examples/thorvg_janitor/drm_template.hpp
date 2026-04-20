// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// drm_template.hpp — scaffold placeholder.
//
// Upstream ThorVG Janitor (thorvg/thorvg.janitor) ships template.h, an SDL2
// wrapper that provides the tvgdemo::{Demo, Window, SwWindow, ...} surface
// that tvggame.cpp depends on. This file is the drm-cxx replacement for
// that wrapper — a single primary plane, double-buffered dumb buffers,
// atomic commit with page-flip-gated commits, and libinput-driven input —
// but right now it is only a stub.
//
// The full implementation is tracked separately. When it lands, this file
// will provide tvgdemo::Demo (virtual content/update/click/motion) and a
// tvgdemo::main(Demo*, int, char**, ...) free function matching the
// upstream signature closely enough that tvggame.cpp compiles with only
// its `#include "template.h"` swapped to `#include "drm_template.hpp"`
// and a small replacement for SDL_GetKeyboardState.

#pragma once

#include <cstdio>
#include <cstdlib>

namespace tvgdemo {

// Forward-declared stub matching the upstream shape so tvggame.cpp's
// `struct ThorJanitor : tvgdemo::Demo` compiles against this header when
// the full scaffold lands. Left empty for now so the stub binary doesn't
// drag in ThorVG headers or libinput.
struct Demo;

// Stub entry point. Prints a status line and exits so anyone who wires
// this up today (deliberately or by mistake) gets a clear message rather
// than a SEGV from half-initialized state.
inline int main(Demo* /*demo*/, int /*argc*/, char** /*argv*/, bool /*clear_buffer*/ = false,
                unsigned /*width*/ = 800, unsigned /*height*/ = 800, unsigned /*threads*/ = 4,
                bool /*print*/ = false) {
  std::fprintf(
      stderr,
      "thorvg_janitor: drm_template.hpp is a scaffold stub; the DRM backend "
      "is not yet implemented. See examples/thorvg_janitor/README.md.\n");
  return EXIT_FAILURE;
}

}  // namespace tvgdemo
