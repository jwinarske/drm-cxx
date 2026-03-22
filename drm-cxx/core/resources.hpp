// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <memory>

namespace drm {

using Resources = std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)>;
using Connector = std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)>;
using Encoder = std::unique_ptr<drmModeEncoder, decltype(&drmModeFreeEncoder)>;
using CrtcPtr = std::unique_ptr<drmModeCrtc, decltype(&drmModeFreeCrtc)>;
using PlaneResPtr = std::unique_ptr<drmModePlaneRes, decltype(&drmModeFreePlaneResources)>;

Resources get_resources(int fd);
Connector get_connector(int fd, uint32_t id);
Encoder get_encoder(int fd, uint32_t id);
CrtcPtr get_crtc(int fd, uint32_t id);
PlaneResPtr get_plane_resources(int fd);

}  // namespace drm
