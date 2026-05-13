// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// multi_crtc_probe.hpp — enumerate connected outputs on a DRM device and
// answer "can the kernel commit this card's CRTCs simultaneously in one
// atomic request?".
//
// Reusable from any example that wants to drive more than one display
// off one card (cluster_sim's passenger display, dual_display, the
// future video_wall_multi). The probe issues a TEST_ONLY +
// ALLOW_MODESET commit pairing every supplied output with a small
// scratch dumb buffer on its primary plane; kernel acceptance is the
// signal that a SceneSet-style coordinator can land tear-free
// synchronized changes across the listed outputs.
//
// What this is NOT: a verdict about hotplug scenarios, plane budgets
// per CRTC under load, or any cross-card behavior. Multi-card setups
// remain out of scope (they would require coordinated kernel commits
// across two file descriptors, which the kernel does not support).

#pragma once

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/property_store.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/dumb/buffer.hpp>
#include <drm-cxx/modeset/atomic.hpp>
#include <drm-cxx/modeset/modeset.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <drm_fourcc.h>
#include <drm_mode.h>
#include <xf86drmMode.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace drm::examples::multi_crtc {

/// One connected display plus the CRTC + primary plane the probe
/// will drive for it.
struct ConnectedOutput {
  std::uint32_t connector_id{0};
  std::uint32_t crtc_id{0};
  std::uint32_t crtc_index{0};  // index in resources, for PlaneRegistry::for_crtc
  std::uint32_t primary_plane_id{0};
  drmModeModeInfo mode{};      // preferred mode (modes[0])
  std::string connector_name;  // "DP-1", "HDMI-A-1", etc.
};

enum class CombinedAtomicVerdict : std::uint8_t {
  NotApplicable,  // fewer than 2 outputs supplied
  Accepted,       // kernel accepted the TEST_ONLY commit
  Rejected,       // kernel rejected — see ProbeReport::error
};

inline const char* to_string(CombinedAtomicVerdict v) noexcept {
  switch (v) {
    case CombinedAtomicVerdict::NotApplicable:
      return "NotApplicable";
    case CombinedAtomicVerdict::Accepted:
      return "Accepted";
    case CombinedAtomicVerdict::Rejected:
      return "Rejected";
  }
  return "Unknown";
}

struct ProbeReport {
  CombinedAtomicVerdict verdict{CombinedAtomicVerdict::NotApplicable};
  std::error_code error;  // populated on Rejected; empty on Accepted/NotApplicable
  std::size_t outputs_in_probe{0};
};

namespace detail {

inline std::string connector_type_name(std::uint32_t type, std::uint32_t type_id) {
  // Match the kernel's drmModeGetConnectorTypeName mapping; we keep our
  // own table so callers don't need libdrm 2.4.112+.
  const char* base = nullptr;
  switch (type) {
    case DRM_MODE_CONNECTOR_VGA:
      base = "VGA";
      break;
    case DRM_MODE_CONNECTOR_DVII:
      base = "DVI-I";
      break;
    case DRM_MODE_CONNECTOR_DVID:
      base = "DVI-D";
      break;
    case DRM_MODE_CONNECTOR_DVIA:
      base = "DVI-A";
      break;
    case DRM_MODE_CONNECTOR_Composite:
      base = "Composite";
      break;
    case DRM_MODE_CONNECTOR_SVIDEO:
      base = "SVIDEO";
      break;
    case DRM_MODE_CONNECTOR_LVDS:
      base = "LVDS";
      break;
    case DRM_MODE_CONNECTOR_Component:
      base = "Component";
      break;
    case DRM_MODE_CONNECTOR_9PinDIN:
      base = "DIN";
      break;
    case DRM_MODE_CONNECTOR_DisplayPort:
      base = "DP";
      break;
    case DRM_MODE_CONNECTOR_HDMIA:
      base = "HDMI-A";
      break;
    case DRM_MODE_CONNECTOR_HDMIB:
      base = "HDMI-B";
      break;
    case DRM_MODE_CONNECTOR_TV:
      base = "TV";
      break;
    case DRM_MODE_CONNECTOR_eDP:
      base = "eDP";
      break;
    case DRM_MODE_CONNECTOR_VIRTUAL:
      base = "Virtual";
      break;
    case DRM_MODE_CONNECTOR_DSI:
      base = "DSI";
      break;
    case DRM_MODE_CONNECTOR_DPI:
      base = "DPI";
      break;
    case DRM_MODE_CONNECTOR_WRITEBACK:
      base = "Writeback";
      break;
    default:
      base = "Unknown";
      break;
  }
  return std::string(base) + "-" + std::to_string(type_id);
}

/// Resolve a CRTC for `connector` — prefer the encoder it's already
/// driving; fall back to scanning possible_crtcs from each compatible
/// encoder.
inline std::optional<std::pair<std::uint32_t, std::uint32_t>> pick_crtc_for_connector(
    int fd, drmModeConnectorPtr connector, drmModeResPtr res) {
  // Existing assignment via the connector's current encoder.
  if (connector->encoder_id != 0) {
    drmModeEncoderPtr enc = drmModeGetEncoder(fd, connector->encoder_id);
    if (enc != nullptr) {
      const auto crtc_id = enc->crtc_id;
      drmModeFreeEncoder(enc);
      if (crtc_id != 0) {
        for (int i = 0; i < res->count_crtcs; ++i) {
          if (res->crtcs[i] == crtc_id) {
            return std::make_pair(crtc_id, static_cast<std::uint32_t>(i));
          }
        }
      }
    }
  }
  // Scan every encoder this connector advertises and pick the first
  // CRTC compatible with at least one of them.
  for (int e = 0; e < connector->count_encoders; ++e) {
    drmModeEncoderPtr enc = drmModeGetEncoder(fd, connector->encoders[e]);
    if (enc == nullptr) {
      continue;
    }
    for (int i = 0; i < res->count_crtcs; ++i) {
      if ((enc->possible_crtcs & (1U << i)) != 0U) {
        const auto crtc_id = res->crtcs[i];
        drmModeFreeEncoder(enc);
        return std::make_pair(crtc_id, static_cast<std::uint32_t>(i));
      }
    }
    drmModeFreeEncoder(enc);
  }
  return std::nullopt;
}

}  // namespace detail

/// Walk every connector with `connection == DRM_MODE_CONNECTED` that
/// has at least one mode and a resolvable CRTC + primary plane.
/// Drops connectors that fail any precondition silently — callers
/// inspect the returned vector's size to discover the count.
inline std::vector<ConnectedOutput> enumerate_connected_outputs(const drm::Device& dev) {
  std::vector<ConnectedOutput> out;
  drmModeResPtr res = drmModeGetResources(dev.fd());
  if (res == nullptr) {
    return out;
  }
  auto registry_r = drm::planes::PlaneRegistry::enumerate(dev);
  if (!registry_r) {
    drmModeFreeResources(res);
    return out;
  }
  const auto& registry = *registry_r;

  for (int i = 0; i < res->count_connectors; ++i) {
    drmModeConnectorPtr c = drmModeGetConnector(dev.fd(), res->connectors[i]);
    if (c == nullptr) {
      continue;
    }
    if (c->connection != DRM_MODE_CONNECTED || c->count_modes == 0) {
      drmModeFreeConnector(c);
      continue;
    }
    auto crtc_pick = detail::pick_crtc_for_connector(dev.fd(), c, res);
    if (!crtc_pick.has_value()) {
      drmModeFreeConnector(c);
      continue;
    }
    const auto [crtc_id, crtc_index] = *crtc_pick;

    // First PRIMARY plane reachable from this CRTC.
    std::uint32_t primary_plane_id = 0;
    for (const auto* p : registry.for_crtc(crtc_index)) {
      if (p->type == drm::planes::DRMPlaneType::PRIMARY) {
        primary_plane_id = p->id;
        break;
      }
    }
    if (primary_plane_id == 0) {
      drmModeFreeConnector(c);
      continue;
    }

    out.push_back(ConnectedOutput{
        .connector_id = c->connector_id,
        .crtc_id = crtc_id,
        .crtc_index = crtc_index,
        .primary_plane_id = primary_plane_id,
        .mode = c->modes[0],  // preferred mode is index 0 by KMS contract
        .connector_name = detail::connector_type_name(c->connector_type, c->connector_type_id),
    });
    drmModeFreeConnector(c);
  }
  drmModeFreeResources(res);
  return out;
}

/// Issue a `DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET`
/// commit covering every supplied output simultaneously. Each output
/// gets a fresh 320x180 XRGB8888 dumb buffer attached to its primary
/// plane plus a full modeset on its CRTC. Kernel acceptance is the
/// signal that a SceneSet-style coordinator can land tear-free
/// synchronized changes across these outputs.
///
/// Side effects: none. Scratch buffers are torn down before return.
inline ProbeReport probe_combined_atomic(const drm::Device& dev,
                                         drm::span<const ConnectedOutput> outputs) {
  ProbeReport report;
  report.outputs_in_probe = outputs.size();
  if (outputs.size() < 2) {
    return report;  // NotApplicable (default verdict)
  }

  // Allocate a scratch dumb FB per output. Small (320x180) — the
  // kernel just needs a valid FB_ID; plane SRC_W/H below scales to
  // whatever the mode dimensions are.
  std::vector<drm::dumb::Buffer> scratch_fbs;
  scratch_fbs.reserve(outputs.size());
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    auto buf_r = drm::dumb::Buffer::create(dev, drm::dumb::Config{.width = 320,
                                                                  .height = 180,
                                                                  .drm_format = DRM_FORMAT_XRGB8888,
                                                                  .bpp = 32,
                                                                  .add_fb = true});
    if (!buf_r) {
      report.verdict = CombinedAtomicVerdict::Rejected;
      report.error = buf_r.error();
      return report;
    }
    scratch_fbs.push_back(std::move(*buf_r));
  }

  // Build N Modeset helpers (one per output) so we get MODE_ID +
  // ACTIVE + connector.CRTC_ID writes without re-implementing the
  // property dance.
  std::vector<drm::modeset::Modeset> modesets;
  modesets.reserve(outputs.size());
  for (const auto& o : outputs) {
    auto m_r = drm::modeset::Modeset::create(dev, o.crtc_id, o.connector_id, o.mode);
    if (!m_r) {
      report.verdict = CombinedAtomicVerdict::Rejected;
      report.error = m_r.error();
      return report;
    }
    modesets.push_back(std::move(*m_r));
  }

  // Cache plane property IDs for each output's primary plane.
  drm::PropertyStore props;
  for (const auto& o : outputs) {
    if (auto r = props.cache_properties(dev.fd(), o.primary_plane_id, DRM_MODE_OBJECT_PLANE); !r) {
      report.verdict = CombinedAtomicVerdict::Rejected;
      report.error = r.error();
      return report;
    }
  }

  // Build the combined atomic request.
  drm::AtomicRequest req(dev);
  if (!req.valid()) {
    report.verdict = CombinedAtomicVerdict::Rejected;
    report.error = std::make_error_code(std::errc::not_enough_memory);
    return report;
  }
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    if (auto r = modesets[i].attach(req); !r) {
      report.verdict = CombinedAtomicVerdict::Rejected;
      report.error = r.error();
      return report;
    }
    const auto& o = outputs[i];
    const auto plane = o.primary_plane_id;

    auto write = [&](const char* name, std::uint64_t value) -> std::error_code {
      auto pid = props.property_id(plane, name);
      if (!pid) {
        return pid.error();
      }
      auto r = req.add_property(plane, *pid, value);
      return r ? std::error_code{} : r.error();
    };

    const std::uint64_t crtc_w = o.mode.hdisplay;
    const std::uint64_t crtc_h = o.mode.vdisplay;
    const std::uint64_t src_w16_16 = static_cast<std::uint64_t>(320) << 16;
    const std::uint64_t src_h16_16 = static_cast<std::uint64_t>(180) << 16;

    for (auto [name, value] : std::initializer_list<std::pair<const char*, std::uint64_t>>{
             {"FB_ID", scratch_fbs[i].fb_id()},
             {"CRTC_ID", o.crtc_id},
             {"CRTC_X", 0},
             {"CRTC_Y", 0},
             {"CRTC_W", crtc_w},
             {"CRTC_H", crtc_h},
             {"SRC_X", 0},
             {"SRC_Y", 0},
             {"SRC_W", src_w16_16},
             {"SRC_H", src_h16_16},
         }) {
      if (auto ec = write(name, value); ec) {
        report.verdict = CombinedAtomicVerdict::Rejected;
        report.error = ec;
        return report;
      }
    }
  }

  if (auto r = req.test(DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET); !r) {
    report.verdict = CombinedAtomicVerdict::Rejected;
    report.error = r.error();
    return report;
  }
  report.verdict = CombinedAtomicVerdict::Accepted;
  return report;
}

}  // namespace drm::examples::multi_crtc
