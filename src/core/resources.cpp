// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT

#include "resources.hpp"

#include <xf86drmMode.h>

#include <cstdint>

namespace drm {

Resources get_resources(int fd) {
  return {drmModeGetResources(fd), &drmModeFreeResources};
}

Connector get_connector(int fd, uint32_t id) {
  return {drmModeGetConnector(fd, id), &drmModeFreeConnector};
}

Encoder get_encoder(int fd, uint32_t id) {
  return {drmModeGetEncoder(fd, id), &drmModeFreeEncoder};
}

CrtcPtr get_crtc(int fd, uint32_t id) {
  return {drmModeGetCrtc(fd, id), &drmModeFreeCrtc};
}

PlaneResPtr get_plane_resources(int fd) {
  return {drmModeGetPlaneResources(fd), &drmModeFreePlaneResources};
}

}  // namespace drm
