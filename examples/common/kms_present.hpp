// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
#pragma once
// examples/common/kms_present.hpp
//
// Minimal shared KMS plumbing for the render-offload examples. This is the
// "display node" side: find a connected output + its primary plane, and commit
// a framebuffer to it with a full modeset. Deliberately raw libdrm so the
// examples show exactly what reaches the kernel; in production this is what the
// drm-cxx allocator + AtomicRequest do for you.

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace kms {

struct Target {
  std::uint32_t connector_id = 0;
  std::uint32_t crtc_id = 0;
  int crtc_index = -1;
  std::uint32_t primary_plane = 0;
  drmModeModeInfo mode{};
};

inline std::uint32_t prop_id(int fd, std::uint32_t obj, std::uint32_t type, const char* name) {
  drmModeObjectProperties* props = drmModeObjectGetProperties(fd, obj, type);
  if (!props) return 0;
  std::uint32_t id = 0;
  for (std::uint32_t i = 0; i < props->count_props && !id; ++i) {
    drmModePropertyRes* p = drmModeGetProperty(fd, props->props[i]);
    if (p && std::strcmp(p->name, name) == 0) id = p->prop_id;
    if (p) drmModeFreeProperty(p);
  }
  drmModeFreeObjectProperties(props);
  return id;
}

// First connected connector, a CRTC its encoders allow, and that CRTC's primary
// plane. Returns nullopt if nothing is hooked up.
inline std::optional<Target> pick_target(int fd) {
  drmModeRes* res = drmModeGetResources(fd);
  if (!res) return std::nullopt;

  Target t;
  drmModeConnector* conn = nullptr;
  for (int i = 0; i < res->count_connectors; ++i) {
    drmModeConnector* c = drmModeGetConnector(fd, res->connectors[i]);
    if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
      conn = c;
      break;
    }
    if (c) drmModeFreeConnector(c);
  }
  if (!conn) {
    drmModeFreeResources(res);
    return std::nullopt;
  }
  t.connector_id = conn->connector_id;
  t.mode = conn->modes[0];
  // Optional lower-mode override for bandwidth-limited bring-up (e.g. a 4K
  // panel where you want to test at 720p): DRM_FORCE_MODE=WxH picks the first
  // matching mode from the connector's EDID list, else keeps the preferred.
  if (const char* want = std::getenv("DRM_FORCE_MODE"); want != nullptr) {
    unsigned fw = 0;
    unsigned fh = 0;
    if (std::sscanf(want, "%ux%u", &fw, &fh) == 2) {
      for (int m = 0; m < conn->count_modes; ++m) {
        if (conn->modes[m].hdisplay == fw && conn->modes[m].vdisplay == fh) {
          t.mode = conn->modes[m];
          break;
        }
      }
    }
  }

  for (int e = 0; e < conn->count_encoders && t.crtc_id == 0; ++e) {
    drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoders[e]);
    if (!enc) continue;
    for (int i = 0; i < res->count_crtcs; ++i)
      if (enc->possible_crtcs & (1u << i)) {
        t.crtc_id = res->crtcs[i];
        t.crtc_index = i;
        break;
      }
    drmModeFreeEncoder(enc);
  }
  drmModeFreeConnector(conn);
  if (t.crtc_id == 0) {
    drmModeFreeResources(res);
    return std::nullopt;
  }

  drmModePlaneRes* pr = drmModeGetPlaneResources(fd);
  for (std::uint32_t i = 0; pr && i < pr->count_planes && !t.primary_plane; ++i) {
    drmModePlane* pl = drmModeGetPlane(fd, pr->planes[i]);
    if (!pl) continue;
    if (pl->possible_crtcs & (1u << t.crtc_index)) {
      std::uint32_t type_prop = prop_id(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE, "type");
      drmModeObjectProperties* p =
          drmModeObjectGetProperties(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
      for (std::uint32_t k = 0; p && k < p->count_props; ++k)
        if (p->props[k] == type_prop && p->prop_values[k] == DRM_PLANE_TYPE_PRIMARY)
          t.primary_plane = pl->plane_id;
      if (p) drmModeFreeObjectProperties(p);
    }
    drmModeFreePlane(pl);
  }
  if (pr) drmModeFreePlaneResources(pr);
  drmModeFreeResources(res);
  return t.primary_plane ? std::optional<Target>(t) : std::nullopt;
}

// Commit `fb_id` to the target's primary plane with a full modeset. `flags`
// typically DRM_MODE_ATOMIC_ALLOW_MODESET, or that | DRM_MODE_ATOMIC_TEST_ONLY
// for the ground-truth probe. Returns 0 or -errno.
inline int commit_fb(int fd, const Target& t, std::uint32_t fb_id, std::uint32_t flags) {
  const std::uint32_t w = t.mode.hdisplay, h = t.mode.vdisplay;
  std::uint32_t mode_blob = 0;
  drmModeModeInfo mode = t.mode;
  drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &mode_blob);

  auto P = [&](std::uint32_t o, std::uint32_t ty, const char* n) { return prop_id(fd, o, ty, n); };
  const auto PL = DRM_MODE_OBJECT_PLANE, CR = DRM_MODE_OBJECT_CRTC, CO = DRM_MODE_OBJECT_CONNECTOR;

  drmModeAtomicReq* req = drmModeAtomicAlloc();
  drmModeAtomicAddProperty(req, t.crtc_id, P(t.crtc_id, CR, "MODE_ID"), mode_blob);
  drmModeAtomicAddProperty(req, t.crtc_id, P(t.crtc_id, CR, "ACTIVE"), 1);
  drmModeAtomicAddProperty(req, t.connector_id, P(t.connector_id, CO, "CRTC_ID"), t.crtc_id);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "FB_ID"), fb_id);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "CRTC_ID"), t.crtc_id);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "SRC_X"), 0);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "SRC_Y"), 0);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "SRC_W"),
                           std::uint64_t(w) << 16);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "SRC_H"),
                           std::uint64_t(h) << 16);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "CRTC_X"), 0);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "CRTC_Y"), 0);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "CRTC_W"), w);
  drmModeAtomicAddProperty(req, t.primary_plane, P(t.primary_plane, PL, "CRTC_H"), h);

  int r = drmModeAtomicCommit(fd, req, flags, nullptr);

  drmModeAtomicFree(req);
  if (mode_blob) drmModeDestroyPropertyBlob(fd, mode_blob);
  return r;
}

}  // namespace kms