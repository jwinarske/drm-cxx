# `drm::scene` EGL Streams support

How to scan out an EGL stream consumer plane through `LayerScene` on
NVIDIA proprietary / Tegra hosts. The library never link-binds
libEGL — every entry point is `dlopen`ed at runtime — so drm-cxx
itself stays loadable on Mesa-only systems; the streams API only
returns useful results when the host has a working proprietary EGL
stack.

This document covers the runtime story: capability probing, the
builder, the lifecycle the scene drives on stream sources, and the
empirical mixing probe that decides whether stream-consumer and
FB-ID planes can cohabit on one CRTC. End-to-end producer code lives
in `examples/advanced/stream_demo`; read that for a working
implementation alongside this prose.

## When to use streams

Pick streams when:

- The host runs NVIDIA's proprietary driver (or NVRM-based Tegra)
  and exposes `libEGL_nvidia.so.0` through libglvnd.
- You want GPU-rendered scanout without going through GBM (NVIDIA's
  GBM support is limited / version-fragile).
- Your producer is already GLES/Vulkan-shaped and writing to an
  EGLSurface is the path of least resistance.

Don't pick streams when:

- You're on Mesa (amdgpu, i915, lima, anv, radv, etc.) — GBM is the
  better target and drm-cxx's `GbmBufferSource` is the right
  primitive. The streams probe will correctly report `Unsupported`
  and you don't need to handle that path.
- You need a CPU-mapped scanout buffer — streams hide pixels behind
  the consumer extension; the scene cannot route stream layers
  through composition fallback. Use `DumbBufferSource` or
  `GbmBufferSource` with `WRITE | LINEAR`.

## Build & runtime requirements

drm-cxx must be built with `-DDRM_CXX_STREAMS=ON` (CMake) or
`-Dstreams=enabled` (meson). The streams gate adopts `egl.pc`'s
include paths but **never** adds `libEGL` to the link line; consumers
of the installed `drm-cxx/scene/egl_stream_builder.hpp` see the
`DRM_CXX_HAS_EGL_STREAMS` macro through the PUBLIC compile
definitions and can use it for their own gating.

At runtime the host needs:

- `libEGL.so.1` (libglvnd dispatcher) on the loader path.
- The proprietary EGL implementation (`libEGL_nvidia.so.0`) registered
  with libglvnd via a JSON shim under `/usr/share/glvnd/egl_vendor.d/`.
- An EGL device whose backing DRM node matches your `drm::Device`'s
  st_rdev — i.e. the kernel module and userspace EGL stack agree on
  which GPU they're driving.

`stream_probe` (`examples/advanced/stream_probe`) is the diagnostic
CLI; run it against the target node to verify these before doing
anything else.

## API surface

Two public types, both in `drm-cxx/scene/`:

| Type | Where | Purpose |
|---|---|---|
| `StreamCapability` + `probe_stream_capability(dev)` | `stream_capability.hpp` | Cheap up-front capability check. EGL-free POD result; safe to inspect on every host regardless of build gate. |
| `EglStreamBuilder` | `egl_stream_builder.hpp` | Constructs the EGLDisplay + EGLConfig + (optional) GLES context + stream source. Header is gated on `DRM_CXX_HAS_EGL_STREAMS`. |

The concrete `EglStreamSource` lives under `src/scene/` as an
internal header — end-user code never names it directly. The builder
returns an upcast `std::unique_ptr<LayerBufferSource>` you hand to
`LayerScene::add_layer`, along with the producer-side handles
(EGLSurface, EGLContext, EGLStreamKHR, EGLDisplay, EGLConfig).

## Quick start

```cpp
#include <drm-cxx/core/device.hpp>
#include <drm-cxx/scene/egl_stream_builder.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
#include <drm-cxx/scene/stream_capability.hpp>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <drm_fourcc.h>

int main(int argc, char* argv[]) {
  // 1. Open the device and pick a CRTC/connector/mode. (Use
  //    examples/common/open_output.hpp for boilerplate.)
  auto out = drm::examples::open_and_pick_output(argc, argv);
  if (!out) return EXIT_FAILURE;

  // 2. Probe. Bail cleanly if streams aren't usable.
  const auto cap = drm::scene::probe_stream_capability(out->device);
  if (!cap.usable()) {
    return EXIT_FAILURE;  // Mesa-only host, or no NVIDIA userspace.
  }

  // 3. Build the scene with the probed capability.
  drm::scene::LayerScene::Config cfg;
  cfg.crtc_id = out->crtc_id;
  cfg.connector_id = out->connector_id;
  cfg.mode = out->mode;
  cfg.stream_capability = cap;
  auto scene = *drm::scene::LayerScene::create(out->device, cfg);

  // 4. Build the stream source. The builder handles device matching,
  //    eglGetPlatformDisplayEXT, eglInitialize, eglChooseConfig, and
  //    eglCreateContext for you. Pass `existing_context` if you
  //    already have a GLES context bound to the device-bound display.
  drm::scene::EglStreamBuilder::Request req;
  req.capability = cap;
  req.device = &out->device;
  req.format = {DRM_FORMAT_ARGB8888, 0, 640, 360};
  auto bld = *drm::scene::EglStreamBuilder::build(req);

  // 5. Hand the source to the scene. `bld.source` is the upcast
  //    LayerBufferSource. Keep the producer handles around for
  //    rendering.
  drm::scene::LayerDesc desc;
  desc.source = std::move(bld.source);
  desc.display.src_rect = {0, 0, 640, 360};
  desc.display.dst_rect = {0, 0, 640, 360};
  scene->add_layer(std::move(desc));

  // 6. Make the producer surface current. From here `glClear` /
  //    `glDraw*` / `eglSwapBuffers` push frames into the stream.
  eglMakeCurrent(bld.display, bld.producer_surface,
                 bld.producer_surface, bld.context);

  // 7. First commit binds the consumer to its picked plane.
  scene->commit();

  // 8. Render loop. The consumer extension drives the plane state
  //    on its own — no per-frame scene.commit() is required for
  //    scanout to follow producer output.
  for (int i = 0; i < 300; ++i) {
    glClearColor(/* ... */);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(bld.display, bld.producer_surface);
  }

  // 9. Teardown. Drop the context-current binding before destroying
  //    the scene (it owns the source, whose destructor unwinds the
  //    stream consumer). The builder-allocated context is yours to
  //    destroy.
  eglMakeCurrent(bld.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (bld.context_created_by_builder) {
    eglDestroyContext(bld.display, bld.context);
  }
  // scene destructor runs here
  eglTerminate(bld.display);
}
```

## Lifecycle: how the scene drives a stream source

The scene-side wiring around a `DriverOwnsBinding` source differs
from the FB-ID buffer-source path in three places.

### 1. `add_layer` gating

`LayerScene::add_layer` rejects a `DriverOwnsBinding` source with
`errc::not_supported` when the configured `StreamCapability::mixing`
is `Unsupported`. The reverse isn't symmetric: usable capability
does NOT mean the scene has somewhere to put the source — that
decision happens at commit time when a plane is picked.

### 2. Pre-acquire plane pinning

Before each commit, the scene runs `ensure_stream_layer_pins`:

1. For every alive slot whose source is `DriverOwnsBinding` and that
   has no current pin, pick a CRTC-compatible non-cursor plane that
   supports the source's format. Preference order is OVERLAY first,
   PRIMARY second; canvas-reserved planes and planes already pinned
   to other stream layers are excluded.
2. Call `LayerBufferSource::bind_to_plane(plane_id)`. For
   `EglStreamSource` this enumerates the device's output layers via
   `eglGetOutputLayersEXT` filtered by `EGL_DRM_PLANE_EXT`, then wires
   the stream consumer to the matching layer via
   `eglStreamConsumerOutputEXT`. After this call the kernel has the
   plane's FB_ID via vendor-private state.
3. Remember the pin on the slot. Subsequent commits skip the picker
   and the bind — the pin is sticky until session pause, rebind, or
   layer removal.

If no plane is available, or `bind_to_plane` fails, the slot stays
unpinned. The source's `acquire()` returns EAGAIN and the scene
drops the layer for that frame; the next commit retries.

### 3. Allocator exclusion + manual property writes

Stream-pinned layers are filtered out of the allocator's bipartite
match (their `planes::Layer::is_externally_bound()` returns true).
The scene passes the pinned planes as `external_reserved` to
`Allocator::apply()` so:

- `disable_unused_planes` leaves the pinned plane alone — the
  consumer-extension FB_ID state survives between commits.
- `place_group` / `bipartite_preseed_group` / `try_test_commit`
  never consider the stream layer for plane assignment.

After `apply()` returns, `arm_stream_layer_planes` walks the
acquisitions, finds the pinned-but-unassigned ones, and writes their
`planes::Layer` property bag directly to the atomic request — every
property except FB_ID (the consumer extension owns it).
`arm_layer_plane_blend_defaults` and `arm_layer_plane_color_props`
fall through to the same pin so blend mode / COLOR_ENCODING /
COLOR_RANGE still get armed.

### 4. Teardown

`remove_layer` calls `source->unbind_from_plane()` before destroying
the source. `EglStreamSource::unbind_from_plane` destroys the stream
+ producer surface (most drivers can't retarget an active consumer)
and recreates them lazily on next bind. This invalidates the
caller's cached `producer_surface()` handle — callers caching the
handle across rebinds must re-query through the source.

## Empirical mixing probe

`StreamCapability::mixing` defaults to `Exclusive` after the static
probe — the conservative answer that says "this driver may not
permit FB-ID planes and stream-consumer planes on the same CRTC."
Whether that's actually true depends on the driver version, the
specific plane combination, and lunar phase. The empirical probe
formalizes a yes/no:

```cpp
const auto verdict = scene->probe_stream_mixing();
// verdict.value() is StreamMixingMode::Mixed on success,
// StreamMixingMode::Exclusive when the kernel rejected the test.
```

`probe_stream_mixing` runs a single `DRM_MODE_ATOMIC_TEST_ONLY`
commit that pairs the currently-pinned stream plane with an FB-ID
plane on the same CRTC (an internally-allocated 16×16 ARGB8888 dumb
buffer). Kernel acceptance upgrades the scene's cached
`StreamCapability::mixing` to `Mixed`; rejection leaves it at
`Exclusive`. The verdict is sticky for the scene's lifetime, cleared
on `rebind()` or `on_session_resumed()` because the prior fd's
kernel state no longer applies.

The probe requires at least one stream layer to be pinned and
committed. Without one it returns `errc::function_not_supported`.

`Exclusive` doesn't currently change scene behavior — the allocator
still places FB-ID layers on whatever planes are available, just
without integrating with the stream layer. If your driver truly
rejects mixing, the commit fails at runtime; the probe gives you
advance notice so you can route FB-ID layers through CPU composition
instead.

## Session pause / resume

`LayerScene::on_session_paused()` does three things on the streams
path:

1. Calls `source->on_session_paused()` on every alive layer, which
   tears down the EGL stream + producer surface (the DRM fd is going
   away, the consumer extension's kernel state is about to be
   reclaimed).
2. Clears every slot's `stream_pinned_plane_id` — the kernel-side
   binding is gone, so the pin is stale.
3. Sticky mixing-probe verdict is cleared on resume; the next call
   to `probe_stream_mixing` re-runs the kernel TEST.

`on_session_resumed(new_dev)` does the inverse:

1. `source->on_session_resumed(new_dev)` rebuilds the stream +
   producer surface against the new device. The EGLDisplay typically
   survives a VT switch (it's bound to the EGLDeviceEXT, not the DRM
   fd), so the producer surface re-attaches to the same display.
2. The next commit's `ensure_stream_layer_pins` re-picks a plane and
   re-calls `bind_to_plane`.

**Producer-side responsibility**: the user's GLES context outlives
the source; on resume, the producer surface from before the pause
may no longer be valid (the source destroyed its old stream + surface
on pause and made new ones on resume). Re-fetch via
`EglStreamSource::producer_surface()` after a resume event.

## Page-flip events on stream planes

When the driver exports `EGL_NV_output_drm_flip_event` (typical on
NVIDIA proprietary + Tegra), the kernel delivers a
`drm_event_vblank` when the stream consumer plane completes a
scanout. The event's `user_data` field carries an identifier the
caller queries from `EglStreamSource::flip_event_data()`:

```cpp
auto bld = drm::scene::EglStreamBuilder::build(req);
scene->add_layer({.source = std::move(bld.source), ...});
scene->commit();  // bind_to_plane runs, populates flip_event_data

const auto flip_id = bld.source_ptr->flip_event_data();
if (flip_id.has_value()) {
  // Wire `*flip_id` into your drmHandleEvent dispatch: every
  // vblank event whose `user_data` equals `*flip_id` is a
  // scanout completion on this source's consumer plane.
}
```

The identifier:

  * Lives on `EglStreamSource`, populated at `bind_to_plane` time
    via `eglQueryOutputLayerAttribEXT(EGL_DRM_FLIP_EVENT_DATA_NV)`.
  * Is `std::nullopt` before bind, when the driver doesn't export
    the extension, or when the query fails (logged at WARN).
  * Reset on `unbind_from_plane`, `on_session_paused`, or any
    cross-plane rebind that rebuilds the stream. The next bind
    queries a fresh value.

The library doesn't currently wire flip events into
`OutputSignaling` or expose a higher-level callback registry —
callers run their own `drmHandleEvent` loop. A scene-level
dispatcher could be added when a real consumer needs it; the
single-accessor surface ships now because the identifier is
otherwise unreachable.

## `rebind()`

A `LayerScene::rebind(new_crtc, new_connector, new_mode)` call moves
the scene to a different CRTC. Stream pins are CRTC-local:

1. For every pinned slot, the scene calls
   `source->unbind_from_plane()` — the old plane state is invalid
   under the new CRTC.
2. Pins are cleared.
3. Sticky mixing-probe verdict is cleared (the new CRTC may behave
   differently from the old one).

The next commit's pre-pass re-picks a plane on the new CRTC and
re-binds.

## Known driver quirks and the desktop-NVIDIA scanout gap

The streams path has been validated against:

| Driver | Version | Verdict |
|---|---|---|
| nvidia (proprietary) + libEGL_nvidia, desktop | 535.288.01 | Static probe: `Exclusive`, full extension chain present (`EGL_EXT_output_drm`, `EGL_KHR_stream`, `EGL_EXT_stream_consumer_egloutput`, `EGL_NV_stream_consumer_eglimage`, `EGL_KHR_stream_producer_eglsurface`). Empirical mixing probe: kernel accepts → upgraded to `Mixed`. Visible scanout: **not reachable on this driver** — see below. |

### Two NVIDIA-specific quirks that the library handles

Both confirmed via minimal C reproductions against `libEGL.so.1` /
`libEGL_nvidia.so.0`:

1. **`EGL_DRM_MASTER_FD_EXT` is required at display creation.**
   NVIDIA's EGL stack opens its own internal DRM fd; without being
   told the caller's master fd via this attribute,
   `eglStreamConsumerOutputEXT` returns `EGL_BAD_ACCESS` even when
   the application holds DRM master on its own fd. The builder
   passes `device->fd()` through `EGL_DRM_MASTER_FD_EXT` on the
   EGL 1.5 core `eglGetPlatformDisplay` call (the EXT variant takes
   `EGLint*` and can't carry a 64-bit fd value).

2. **Producer surface creation must follow consumer attach.**
   `eglCreateStreamProducerSurfaceKHR` returns `EGL_BAD_STATE_KHR`
   when called against a stream that has no consumer yet — even
   though the `EGL_KHR_stream_producer_eglsurface` spec is
   permissive about ordering. `EglStreamSource::create` defers
   producer-surface creation; `bind_to_plane` calls
   `eglStreamConsumerOutputEXT` first, then creates the producer
   surface. The producer surface is therefore `EGL_NO_SURFACE`
   until after the first commit that runs the bind pre-pass —
   query `source->producer_surface()` after `scene.commit()`
   (or `scene.prepare_stream_layers()` if you need the surface
   before the first commit).

### Desktop-NVIDIA visible-scanout gap (535.x)

Even with both quirks correctly handled, the consumer-to-KMS
hand-off does not complete on desktop NVIDIA 535:

- Including the stream plane's `CRTC_ID` / `CRTC_*` / `SRC_*`
  properties in the atomic commit (without `FB_ID`, which the
  stream consumer is supposed to provide) returns `EINVAL` from
  the kernel — desktop NVIDIA's atomic_check rejects an "active"
  plane with no framebuffer.
- Excluding the stream plane from the atomic commit lets the
  modeset succeed but leaves the consumer plane unbound to any
  CRTC; the producer's `eglSwapBuffers` then blocks indefinitely
  on the first or second swap because nothing pulls frames out
  of the stream.
- The Tegra-style first-frame handoff via
  `eglStreamConsumerAcquireAttribKHR` + `EGL_DRM_ATOMIC_REQUEST_NV`
  requires the `EGL_NV_output_drm_atomic` extension, which is **not
  exported on desktop NVIDIA 535.288.01**. KDE and Sway both
  dropped their EGL Streams backends because the same wall blocked
  them; NVIDIA's recommended path on the desktop is now GBM via
  `EGL_KHR_platform_gbm` (supported on 525+).

The library exposes the Tegra path (`prepare_stream_layers` +
internal `prime_first_commit` routing) so the code becomes useful
when `EGL_NV_output_drm_atomic` is available; on desktop the
plumbing wires up to the commit-shape level but visible scanout
needs a different driver. `examples/advanced/stream_demo` detects
this at runtime: it announces the gap and holds the modeset state
without attempting a producer render loop that would just wedge.

Mesa-only systems (amdgpu, i915, etc.) return `Unsupported` from
the static probe — the per-device EGL extension chain lacks
`EGL_KHR_stream`, so `EglStreamBuilder::build` returns
`errc::function_not_supported` before any allocation happens. The
gate is the right one; nothing on the streams path can succeed on
a Mesa-only stack.

## See also

- `examples/advanced/stream_probe/` — diagnostic CLI that dumps the
  full `StreamCapability` shape for a chosen DRM node.
- `examples/advanced/stream_demo/` — end-to-end working demo.
  Animates a clear color into a 640×360 stream layer centered over a
  full-screen dumb-buffer background. Hardware-only.
- `src/scene/stream_capability.hpp` — `StreamCapability` /
  `StreamMixingMode` type definitions and the static probe's
  contract.
- `src/scene/egl_stream_builder.hpp` — `EglStreamBuilder::Request` /
  `Result` and the build() factory.
