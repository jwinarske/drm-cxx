// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Core
#include "core/device.hpp"
#include "core/resources.hpp"
#include "core/property_store.hpp"
#include "core/format.hpp"

// Modeset
#include "modeset/atomic.hpp"
#include "modeset/mode.hpp"
#include "modeset/page_flip.hpp"

// Planes
#include "planes/plane_registry.hpp"
#include "planes/layer.hpp"
#include "planes/output.hpp"
#include "planes/allocator.hpp"
#include "planes/composition_layer.hpp"

// Input
#include "input/seat.hpp"
#include "input/keyboard.hpp"
#include "input/pointer.hpp"
#include "input/event_dispatcher.hpp"

// Display
#include "display/connector_info.hpp"
#include "display/edid.hpp"
#include "display/hdr_metadata.hpp"

// GBM
#include "gbm/device.hpp"
#include "gbm/surface.hpp"
#include "gbm/buffer.hpp"

// Sync
#include "sync/fence.hpp"
