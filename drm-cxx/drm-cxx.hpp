// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Logging
#include "log.hpp"

// Core
#include "core/device.hpp"
#include "core/format.hpp"
#include "core/property_store.hpp"
#include "core/resources.hpp"

// Modeset
#include "modeset/atomic.hpp"
#include "modeset/mode.hpp"
#include "modeset/page_flip.hpp"

// Planes
#include "planes/allocator.hpp"
#include "planes/composition_layer.hpp"
#include "planes/layer.hpp"
#include "planes/matching.hpp"
#include "planes/output.hpp"
#include "planes/plane_registry.hpp"

// Input
#include "input/event_dispatcher.hpp"
#include "input/keyboard.hpp"
#include "input/pointer.hpp"
#include "input/seat.hpp"

// Display
#include "display/connector_info.hpp"
#include "display/edid.hpp"
#include "display/hdr_metadata.hpp"

// GBM
#include "gbm/buffer.hpp"
#include "gbm/device.hpp"
#include "gbm/surface.hpp"

// Sync
#include "sync/fence.hpp"
