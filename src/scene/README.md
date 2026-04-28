# `drm::scene` — layer-based KMS scene

Handle-based layer model on top of `drm::planes::Allocator`. Layers describe what to display and where; the scene picks planes, falls back to a software canvas when the hardware can't fit a layer, and survives session pause/resume and CRTC/mode swaps without tearing down its handles.

This module is part of the drm-cxx library proper. It is built unconditionally; there is no `DRM_CXX_SCENE` opt-out. Public headers install under `<drm-cxx/scene/...>`.

```cpp
#include <drm-cxx/scene/dumb_buffer_source.hpp>
#include <drm-cxx/scene/layer_desc.hpp>
#include <drm-cxx/scene/layer_scene.hpp>
```

## Quick example

```cpp
auto dev = drm::Device::open("/dev/dri/card0").value();
dev.enable_universal_planes();
dev.enable_atomic();

drm::scene::LayerScene::Config cfg{crtc_id, connector_id, mode};
auto scene = drm::scene::LayerScene::create(dev, cfg).value();

auto bg = drm::scene::DumbBufferSource::create(
              dev, mode.hdisplay, mode.vdisplay, DRM_FORMAT_ARGB8888)
              .value();
// Rasterize into bg->pixels() ...

drm::scene::LayerDesc desc;
desc.source = std::move(bg);
desc.display.dst_rect = {0, 0, mode.hdisplay, mode.vdisplay};
auto handle = scene->add_layer(std::move(desc)).value();

drm::PageFlip page_flip(dev);
page_flip.set_handler([&](std::uint32_t, std::uint64_t, std::uint64_t) { /* flip done */ });

auto report = scene->commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip).value();
// report.layers_assigned / layers_composited / layers_unassigned
```

The first commit after `create()` (or `rebind()`, or `on_session_resumed()`) implicitly carries `DRM_MODE_ATOMIC_ALLOW_MODESET`; subsequent commits don't. Page-flip events are not auto-injected — pass `DRM_MODE_PAGE_FLIP_EVENT` yourself and let `drm::PageFlip::dispatch()` route the kernel's `user_data` back to your handler.

## API surface

| Header | Type | Role |
|---|---|---|
| `layer_scene.hpp` | `LayerScene` | Façade; create / add_layer / test / commit / rebind / session hooks. |
| `layer_desc.hpp` | `LayerDesc` | Pass-by-value descriptor for `add_layer`. Owns the source. |
| `layer.hpp` | `Layer` | Live layer state; setters mark dirty for the next commit. |
| `layer_handle.hpp` | `LayerHandle` | Stable, generation-tagged identifier. |
| `display_params.hpp` | `DisplayParams` | src/dst rect, rotation, alpha, zpos. |
| `buffer_source.hpp` | `LayerBufferSource` | Polymorphic backing-buffer interface. |
| `dumb_buffer_source.hpp` | `DumbBufferSource` | Single CPU-writable dumb buffer. |
| `gbm_buffer_source.hpp` | `GbmBufferSource` | Single CPU-mapped GBM scanout buffer (`SCANOUT \| LINEAR \| WRITE`). |
| `composite_canvas.hpp` | `CompositeCanvas` | Double-buffered ARGB8888 canvas the scene paints unassigned layers into. |
| `commit_report.hpp` | `CommitReport` | Diagnostic counters returned by `commit()` / `test()`. |
| `compatibility_report.hpp` | `CompatibilityReport` | Per-layer flags returned by `rebind()`. |

A live `LayerHandle` survives across `rebind()` and `on_session_resumed()` — same id, same generation. A stale handle (its layer was removed and the slot has been reused) returns `nullptr` from `get_layer` and silently no-ops `remove_layer`; the per-slot generation counter prevents it from ever resolving to a different layer.

## Buffer sources

`LayerBufferSource` is the single most load-bearing abstraction in the module. It separates **what the buffer is** (`SourceFormat`: format, modifier, intrinsic size) from **how it should be displayed** (`DisplayParams`: src/dst rect, rotation, alpha, zpos), mirroring the KMS concept boundary. The same source can be displayed multiple ways; the same display configuration can scan different buffers.

Two v1 implementations:

- **`DumbBufferSource`** — single CPU-writable dumb buffer. Use for software-rendered cursors, CSDs, test patterns, signage backgrounds — content where the producer is not racing scanout.
- **`GbmBufferSource`** — single CPU-mapped linear GBM scanout buffer. Same single-buffer semantics, allocated through GBM so future variants can negotiate modifiers, export DMA-BUFs, or front a `gbm_surface` for GL/Vulkan producers.

Both report `BindingModel::SceneSubmitsFbId`: the scene writes `FB_ID` to the atomic commit. Both expose `map(MapAccess)` — a scoped RAII guard returning a `drm::BufferMapping` — so consumers paint pixels and the composition fallback reads them through one unified surface. The guard's destructor pairs with `gbm_bo_unmap` for GBM-backed sources, so it must be held only across the actual CPU access.

The interface deliberately has v2 hooks (`bind_to_plane` / `unbind_from_plane`, `BindingModel::DriverOwnsBinding`) sitting unused. v1 sources do not override them. See [Future extensions](#future-extensions) below.

## Composition fallback

When the allocator cannot place every layer on a hardware plane (plane budget exceeded, no compatible plane for the layer's format/scaling, or `LayerDesc::force_composited` is set), the scene paints the unassigned layers into a `CompositeCanvas` in zpos order using SRC_OVER blending and arms the canvas onto a free overlay (or PRIMARY fallback) plane via direct property writes — no second allocator pass.

The canvas is double-buffered: the CPU paints into the back while the kernel scans the front. When `layer_count` exceeds the canvas-eligible plane count, the scene pre-reserves a plane via the allocator's `external_reserved` parameter so placement leaves it alone.

Sources whose `map(MapAccess::Read)` returns `errc::function_not_supported` (tiled/compressed/GPU-only buffers, future stream-consumed planes) cannot be rescued by composition; if the allocator drops them they stay dropped for the frame and surface in `CommitReport::layers_unassigned`.

`LayerDesc::force_composited` routes a layer through composition unconditionally — useful for diagnostic overlays, integration tests of the compositor path, and layers known to require CPU compositing.

## Session lifecycle

The scene mirrors `drm::cursor::Renderer`'s session contract for libseat-managed processes. `drm::session::Seat` callbacks fire from `seat->dispatch()`; the main loop owns the actual scene calls so it can sequence them with the rest of its state:

```cpp
seat->set_pause_callback([&]() {
  session_paused = true;
  flip_pending = false;
});
seat->set_resume_callback([&](std::string_view /*path*/, int new_fd) {
  pending_resume_fd = new_fd;
  session_paused = false;
});

// In the main loop, after seat->dispatch() returns:
if (pending_resume_fd != -1) {
  scene->on_session_paused();
  dev = drm::Device::from_fd(std::exchange(pending_resume_fd, -1));
  dev.enable_universal_planes().value();
  dev.enable_atomic().value();
  scene->on_session_resumed(dev).value();
  // Re-paint each layer's source — buffers were re-allocated on the new fd.
}
```

`on_session_paused()` is a pure forget — no ioctls fire on the libseat-revoked fd. `on_session_resumed(new_dev)` re-enumerates the plane registry, re-caches CRTC/connector property ids, rebuilds the allocator, and walks every live layer's source so it can drop GEM/FB handles bound to the dead fd and re-allocate against `new_dev`. Layer handles (including generations) are preserved verbatim. The next commit implicitly carries `ALLOW_MODESET` to bring the CRTC back up.

## Rebind

`LayerScene::rebind(crtc_id, connector_id, mode)` swaps the scene's KMS binding without destroying it — used for hotplug-driven mode changes and CRTC migration on connector reassignment. Same fd, same set of live layer handles, same buffer sources (those are tied to the fd, not the CRTC).

Returns a `CompatibilityReport` listing layers whose existing display configuration looks unsuitable for the new mode/registry (`DstRectOffScreen`, `RequiresScalingNotAvailable`, `FormatNotSupported`). Entries are advisory — the rebind completes regardless and the caller decides whether to reposition, replace, or remove flagged layers before the next commit.

A failure during `rebind()` (property recache against the new CRTC fails, registry re-enumeration fails) leaves the scene partially rebound; tear it down rather than retry.

## Diagnostics

Every `commit()` and `test()` returns a `CommitReport`:

```cpp
struct CommitReport {
  std::size_t layers_total;          // == assigned + composited + unassigned
  std::size_t layers_assigned;       // placed directly on a plane
  std::size_t layers_composited;     // rescued via the canvas
  std::size_t layers_unassigned;     // dropped this frame
  std::size_t composition_buckets;   // canvas planes used (0 or 1 in v1)
  std::size_t properties_written;    // total atomic-request properties
  std::size_t fbs_attached;          // FB_ID writes (subset of above)
  std::size_t test_commits_issued;   // allocator probe commits
};
```

The counters drive unit-test assertions, benchmark instrumentation (property-minimization elision, warm-start hit rates), and runtime telemetry. They are also the cheapest way to confirm a frame actually reached the kernel: a commit that succeeds but elides every property and every FB_ID write produces no `PAGE_FLIP_EVENT` — the scene defends against this by always re-emitting `FB_ID` even when the property snapshot is clean.

## Stability

- `LayerScene` and the headers it pulls in are public C++17 API. They follow drm-cxx's general posture: pre-1.0, breaking changes possible between minor releases, called out in `CHANGELOG.md`.
- The `LayerBufferSource` interface is the most stability-critical surface in the module. It was designed up-front to accommodate v2 binding models (`DriverOwnsBinding`) and foreign producers without breakage. Adding a new binding model or a new pure-virtual hook is a breaking change and will be sequenced with care.
- Buffer-source classes (`DumbBufferSource`, `GbmBufferSource`) are intentionally simple and may grow ring-buffer / multi-slot variants without renaming the existing types.
- `CommitReport` may grow new fields. Existing fields' meanings are stable.

## Known non-goals and future extensions

The scene's v1 charter is the **common case**: CPU- or GBM-rendered scanout, atomic KMS, single CRTC, planes the kernel and `drm::planes::Allocator` understand. Several adjacent technologies are deliberately out of scope today; the abstractions above were shaped so they can land later without API breakage.

### EGL Streams

EGL Streams remains the path NVIDIA's proprietary driver and Tegra-based automotive/industrial stacks use for non-GBM workflows. Stream-consumed planes mix awkwardly with FB-ID-attached planes in the same atomic commit (driver-version-dependent restrictions); the kernel surface and capability probes that would let the allocator reason about that mix are non-trivial and `vkms` cannot emulate them.

The `LayerBufferSource` interface accommodates streams architecturally: a future `EglStreamSource` would report `BindingModel::DriverOwnsBinding`, override `bind_to_plane` / `unbind_from_plane` to drive the EGL stream-consumer lifecycle, and the scene's commit path will skip `FB_ID` writes for those layers. The hooks are present and called today; v1 sources just don't override them. We see this audience and intend to cover it in v2.

### Foreign DMA-BUF producers (V4L2 / NPU / accel)

Linux `drivers/accel/` has absorbed NPU drivers (Intel Habana, AMD XDNA, Rockchip RKNN, Qualcomm QAIC) — these devices output DMA-BUFs for inference results, but they have no planes / CRTCs / connectors, so they cannot display anything themselves. V4L2 capture devices and hardware video decoders are the same shape. A future `AccelOutputSource` (or `Dmabuf­ImportSource`) implementing `LayerBufferSource` lets these producers drive scene layers the same way `GbmBufferSource` does — no scene-side change required. v1 ships without one because each producer ecosystem (V4L2, accel, vendor cameras) wants a slightly different acquire/release surface and we'd rather land the right shapes once we have a target consumer.

### Multi-CRTC orchestration

Spanning a logical scene across two physical CRTCs (dashboards, automotive multi-display, video walls), mirroring the same content across CRTCs with per-output post-processing, and cross-screen effects are all v2 territory. The current `LayerScene` is a single-CRTC abstraction by construction. A multi-CRTC composition layer would sit above `LayerScene`, not inside it.

### Other deferred work

- **Async page-flip completion / buffer release on scanout** — today the scene takes a `user_data` pointer and lets the caller pair `drm::PageFlip::dispatch()` with the matching commit. A higher-level `Frame` that batches acquire / commit / wait-for-flip / release is plausible once a real consumer wants it.
- **Multi-canvas composition pooling** — v1 emits one full-screen canvas at most. A multi-canvas variant (distinct planes for non-contiguous z-runs) is bounded by `CompositeCanvasConfig::max_canvases`, currently capped at 1.
- **Driver-quirk property minimization opt-outs** — `set_force_full_property_writes(true)` is a documented escape hatch for stacks that refuse to inherit unwritten plane state. Empirically rare; we have no confirmed instance, but the lever is in place.

## Examples

- **`examples/signage_player/`** — five-layer signage workload: GBM background, Blend2D overlay, scrolling ticker, clock, logo. Exercises composition fallback (gates optional layers on plane budget), `--hotplug-follow` mode swaps via `rebind()`, and libseat VT pause/resume. Hardware-validated on amdgpu.
- **`examples/thorvg_janitor/`** — ThorVG `tvggame` ported onto the scene; a single full-screen GBM-backed layer driven by ThorVG's SW renderer.
- **`examples/hotplug_monitor/`** — connector-id fast-path on top of `drm::display::HotplugMonitor` with an optional scene to verify rebind correctness across real hotplug events.

## Where to look in the code

- Allocator integration and property minimization: `src/planes/allocator.{hpp,cpp}`.
- Per-frame commit build, composition pre-reservation, session/rebind plumbing: `src/scene/layer_scene.cpp`.
- Canvas blending kernels (with unit tests for clipping and SRC_OVER correctness): `src/scene/composite_canvas.cpp`, `tests/unit/test_composite_canvas.cpp`.
- VKMS hardware-tier integration tests: `tests/integration/test_layer_scene_{composition,minimization,rebind}_vkms.cpp`.

## See also

- Top-level [README](../../README.md) for the library's broader feature set, build options, and migration notes.
- `docs/implementation_plan.md` for the milestone-level rationale, the M2 roadmap, and the v2 forward-compatibility analysis the design choices above were drawn from.
