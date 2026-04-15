// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "layer.hpp"

namespace drm::planes {

// Composition layer is a regular Layer that is always assigned to the primary
// plane when any other layer needs software composition. This header exists
// as a convenience alias and documentation point.
using CompositionLayer = Layer;

}  // namespace drm::planes
