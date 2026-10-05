// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "layer.hpp"

namespace drm::planes {

// Composition layer: a regular Layer standing in for the composition canvas.
//
// When any layer needs composition, Allocator::apply parks it on the first
// PRIMARY the assignment left free. That is a preference, not a scanout
// contract: it keeps a PRIMARY armed when everything else is composited
// (some drivers, e.g. amdgpu, reject an active CRTC whose PRIMARY is unarmed).
// When every PRIMARY is taken it is simply not assigned.
//
// LayerScene does not rely on that placement. There the composition layer is
// a placeholder; the scene picks the canvas plane itself after allocation:
// the previous canvas plane, then free OVERLAYs, then PRIMARYs (the
// placeholder's plane counts as free), each confirmed by TEST_ONLY, with the
// canvas zpos above the armed stack. On a CRTC with two PRIMARYs and one taken,
// a free OVERLAY is chosen before the other PRIMARY.
//
// This header is a convenience alias and documentation point.
using CompositionLayer = Layer;

}  // namespace drm::planes
