// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// Scaffold entry point. Delegates to the stub tvgdemo::main() in
// drm_template.hpp so the thorvg_janitor target exists and links, but the
// DRM backend implementation is tracked separately.
//
// When the backend lands, this file either goes away (tvggame.cpp becomes
// the sole source) or gets folded into drm_template.cpp.

#include "drm_template.hpp"

int main(int argc, char** argv) {
  return tvgdemo::main(nullptr, argc, argv);
}
