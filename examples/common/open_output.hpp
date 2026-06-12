// SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
// SPDX-License-Identifier: MIT
//
// open_output.hpp — shared DRM device + output-pickup boilerplate for
// the small examples.
//
// Almost every example in this tree opens a card, prefers libseat's
// revocable fd when available, enables atomic + universal planes, and
// then either uses the device directly (raw-allocator demos) or walks
// the resources to find the first connected connector + its CRTC +
// preferred mode (scene demos). The two helpers here factor that out:
//
//   * `open_device(argc, argv)` — for examples that want only the
//     device + an optional Seat. Used by overlay_planes-style raw demos.
//
//   * `open_and_pick_output(argc, argv)` — adds the connector / CRTC /
//     mode pickup. Used by every scene-based example.
//
// Failures log to stderr via drm::println and surface as nullopt.
//
// Header-only by intent, mirroring select_device.hpp; the helpers are
// inline so consumers don't need to thread an extra TU through both
// build systems.

#pragma once

#include "select_connector.hpp"
#include "select_device.hpp"

#include <drm-cxx/core/device.hpp>
#include <drm-cxx/core/resources.hpp>
#include <drm-cxx/detail/format.hpp>
#include <drm-cxx/detail/span.hpp>
#include <drm-cxx/modeset/mode.hpp>
#include <drm-cxx/session/seat.hpp>

#include <xf86drmMode.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

namespace drm::examples {

/// True if a bare flag (e.g. "--no-seat") appears anywhere in argv.
[[nodiscard]] inline bool argv_has_flag(int argc, char* argv[], const char* flag) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], flag) == 0) {
      return true;
    }
  }
  return false;
}

/// Device + libseat session pair. The Seat (when present) holds the
/// revocable fd backing the Device; the Seat must outlive the Device,
/// which the field ordering guarantees (destruction is reverse-
/// declaration: device first, then seat).
struct DeviceCtx {
  std::optional<drm::session::Seat> seat;
  drm::Device device;
};

/// DeviceCtx + a single picked output (CRTC + connector + mode).
struct Output {
  std::optional<drm::session::Seat> seat;
  drm::Device device;
  std::uint32_t crtc_id{0};
  std::uint32_t connector_id{0};
  drmModeModeInfo mode{};
};

/// Pick a card from argv (or prompt), open it through libseat when a
/// backend is available, and enable atomic + universal planes. Pass
/// `--no-seat` in argv to bypass libseat and open the node directly (the
/// first opener takes DRM master) — for headless boards with no usable VT.
/// Returns nullopt on any failure; the helper has already logged the reason.
[[nodiscard]] inline std::optional<DeviceCtx> open_device(int argc, char* argv[]) {
  const auto path = drm::examples::select_device(argc, argv);
  if (!path) {
    return std::nullopt;
  }

  // --no-seat: skip libseat and open the DRM node directly (the first opener
  // becomes DRM master). For headless boards with no usable VT — e.g. a
  // serial-console image over SSH, where libseat's seat/VT setup fails.
  const bool no_seat = argv_has_flag(argc, argv, "--no-seat");
  std::optional<drm::session::Seat> seat;
  if (!no_seat) {
    seat = drm::session::Seat::open();
  }
  const auto seat_dev = seat ? seat->take_device(*path) : std::nullopt;
  auto dev_holder = [&]() -> std::optional<drm::Device> {
    if (seat_dev) {
      return drm::Device::from_fd(seat_dev->fd);
    }
    auto r = drm::Device::open(*path);
    if (!r) {
      return std::nullopt;
    }
    return std::move(*r);
  }();
  if (!dev_holder) {
    drm::println(stderr, "Failed to open {}", *path);
    return std::nullopt;
  }

  if (auto r = dev_holder->enable_universal_planes(); !r) {
    drm::println(stderr, "enable_universal_planes: {}", r.error().message());
    return std::nullopt;
  }
  if (auto r = dev_holder->enable_atomic(); !r) {
    drm::println(stderr, "enable_atomic: {}", r.error().message());
    return std::nullopt;
  }

  return DeviceCtx{std::move(seat), std::move(*dev_holder)};
}

/// `open_device` + walk DRM resources for a connected connector, resolve
/// a CRTC for it (the bound one on warm KMS, else one of its possible
/// encoders' CRTCs), and select the preferred mode. Returns nullopt on any
/// failure (no device, no connector, no CRTC, no mode) — the helper has
/// already logged the reason.
[[nodiscard]] inline std::optional<Output> open_and_pick_output(int argc, char* argv[]) {
  auto ctx = open_device(argc, argv);
  if (!ctx) {
    return std::nullopt;
  }
  auto& dev = ctx->device;

  const auto res = drm::get_resources(dev.fd());
  if (!res) {
    drm::println(stderr, "get_resources failed");
    return std::nullopt;
  }

  // Connector pickup follows kMainRank: prefer the internal panel
  // (eDP > LVDS > DSI > DPI), fall back to cable-out (HDMI > DP >
  // DVI > VGA). On a single-output system this is identical to the
  // historical "first connected" behavior; on multi-output systems
  // (docked laptop, dual-HDMI workstation) it picks the display the
  // user usually means rather than whichever the kernel enumerated
  // first. Examples wanting a different policy can call
  // drm::examples::pick_connector() directly with kInternalRank or
  // kExternalRank.
  const auto connector_ids = drm::span<const std::uint32_t>(res->connectors, res->count_connectors);
  drm::Connector conn = drm::examples::pick_connector(dev.fd(), connector_ids);
  if (!conn) {
    drm::println(stderr, "No connected connector with a mode");
    return std::nullopt;
  }

  // Resolve a CRTC. Prefer the currently-bound encoder/CRTC (warm KMS — a
  // compositor already drove this connector). On cold KMS — nothing has
  // bound an encoder yet, or the compositor exited — fall back to the
  // first CRTC any of the connector's possible encoders can drive, exactly
  // as a modeset would. Without this the present examples report "no
  // usable output" on a headless / freshly-booted board with a monitor
  // attached (e.g. StarFive once gdm is stopped), even though the
  // connector is connected and has modes.
  std::uint32_t crtc_id = 0;
  if (conn->encoder_id != 0) {
    if (const auto enc = drm::get_encoder(dev.fd(), conn->encoder_id); enc && enc->crtc_id != 0) {
      crtc_id = enc->crtc_id;
    }
  }
  for (int e = 0; e < conn->count_encoders && crtc_id == 0; ++e) {
    const auto enc = drm::get_encoder(dev.fd(), conn->encoders[e]);
    if (!enc) {
      continue;
    }
    for (int i = 0; i < res->count_crtcs; ++i) {
      if ((enc->possible_crtcs & (1U << static_cast<unsigned>(i))) != 0U) {
        crtc_id = res->crtcs[i];
        break;
      }
    }
  }
  if (crtc_id == 0) {
    drm::println(stderr, "No usable CRTC for connector {}", conn->connector_id);
    return std::nullopt;
  }

  const auto mode_res =
      drm::select_preferred_mode(drm::span<const drmModeModeInfo>(conn->modes, conn->count_modes));
  if (!mode_res) {
    drm::println(stderr, "No usable mode on connector {}", conn->connector_id);
    return std::nullopt;
  }

  return Output{
      std::move(ctx->seat), std::move(ctx->device), crtc_id, conn->connector_id, mode_res->drm_mode,
  };
}

}  // namespace drm::examples
