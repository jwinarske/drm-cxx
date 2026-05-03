# `drm::scene` — design rationale

This document captures the *why* behind `drm::scene::LayerScene`. The
*what* — types, headers, lifecycle contract, examples, stability —
lives in [`src/scene/README.md`](../src/scene/README.md). Read that
first if you want to use the module; read this if you want to extend
it, audit its boundaries, or understand which capabilities are
deferred to v2.

## Why a layer scene at all

KMS gives the application a flat list of planes per CRTC, each with
its own format / scaling / zpos / rotation capabilities, and an
atomic-commit interface that accepts or rejects the whole frame as a
unit. A naive consumer either:

- **Hard-codes the assignment** ("primary for the background, overlay
  zero for the cursor"), which works on one driver and breaks on the
  next when plane counts or capabilities differ.
- **Re-derives the assignment from scratch on every frame** by
  iterating planes and calling `DRM_MODE_ATOMIC_TEST_ONLY` until one
  validates, which is slow and produces visible churn when the
  optimal assignment changes mid-stream.

`drm::planes::Allocator` solves the assignment problem. `LayerScene`
is the level above: it owns the plane→layer assignment across frames,
falls back to CPU composition when no assignment exists, and survives
session pause/resume and CRTC/mode swaps without tearing down the
caller's layer handles.

The scene's v1 charter is the **common case**: CPU- or GBM-rendered
scanout, atomic KMS, single CRTC, planes the kernel and
`drm::planes::Allocator` understand. Everything outside that —
EGL Streams, foreign DMA-BUF producers, multi-CRTC orchestration —
is deliberate v2 territory; the abstractions below were shaped so
they can land later without API breakage.

## Buffer source model

`LayerBufferSource` is the most stability-critical surface in the
module. It separates **what the buffer is** (`SourceFormat`: format,
modifier, intrinsic size) from **how it should be displayed**
(`DisplayParams`: src/dst rect, rotation, alpha, zpos), mirroring the
KMS concept boundary. The same source can be displayed multiple ways;
the same display configuration can scan different buffers.

Two v1 implementations:

| Source | Backing | Use case |
|---|---|---|
| `DumbBufferSource` | single CPU-writable dumb buffer | software-rendered cursors, CSDs, test patterns, signage backgrounds |
| `GbmBufferSource` | single CPU-mapped linear GBM scanout | software content where future variants may negotiate modifiers, export DMA-BUFs, or front a `gbm_surface` for GL/Vulkan producers |

Both report `BindingModel::SceneSubmitsFbId`: the scene writes
`FB_ID` to the atomic commit. Both expose `map(MapAccess)` — a scoped
RAII guard returning a `drm::BufferMapping` — so consumers paint
pixels and the composition fallback reads them through one unified
surface.

### Why polymorphic, why now

The interface is polymorphic in v1 because v2 needs it. Concretely:

- **`BindingModel::DriverOwnsBinding`** and the unused
  `bind_to_plane` / `unbind_from_plane` hooks are stubs for EGL
  Streams. A future `EglStreamSource` reports `DriverOwnsBinding`,
  drives the stream-consumer lifecycle in those hooks, and the
  scene's commit path skips `FB_ID` writes for those layers.
- **`SourceFormat` carries a `modifier`** so foreign producers
  (V4L2, NPU, video decoders) can advertise their tiling at the
  source level without a separate per-layer modifier field.

Adding a binding model or a pure-virtual hook in v2 would be a
breaking change. Threading both at v1 is a one-day investment that
buys the v2 audiences without painful API churn later.

### CPU mapping is not optional

Sources that cannot expose a CPU mapping (tiled / compressed /
GPU-only buffers, future stream-consumed planes) cannot be rescued
by the composition fallback. If the allocator drops one, it stays
dropped for the frame and surfaces in
`CommitReport::layers_unassigned`. This is by design — the canvas
does straight CPU SRC_OVER and has no GPU read path; trying to
rescue an opaque buffer would either silently corrupt the frame or
require a dependency the scene refuses to take (a GPU compositor
inside a "use planes when possible" abstraction is the wrong shape).

## Composition fallback

When the allocator cannot place every layer on a hardware plane
(plane budget exceeded, no compatible plane for the layer's format /
scaling, or `LayerDesc::force_composited` is set), the scene paints
unassigned layers into a `CompositeCanvas` in zpos order using
SRC_OVER blending and arms the canvas onto a free overlay (or
PRIMARY fallback) plane via direct property writes — no second
allocator pass.

The canvas is double-buffered: the CPU paints into the back while
the kernel scans the front. When `layer_count` exceeds the
canvas-eligible plane count, the scene pre-reserves a plane via the
allocator's `external_reserved` parameter so placement leaves it
alone.

What the canvas deliberately is **not**:

- **Not a multi-canvas pool.** v1 emits one full-screen canvas at
  most; non-contiguous z-runs collapse to `max(unassigned zpos)`.
  Multi-canvas pooling is bounded by `CompositeCanvasConfig::max_canvases`,
  currently capped at 1. A multi-canvas variant lands when a real
  consumer wants it.
- **Not a software compositor.** The blender is a single
  straight-CPU SRC_OVER pass. No GPU acceleration, no shader path,
  no scene-graph traversal. `drm::scene` is a hardware-plane
  scheduler with a CPU-blend safety net, not a Wayland compositor in
  miniature.
- **Not a way to dodge plane constraints.** Force-compositing a
  layer trades off the plane's zero-copy scanout against a CPU
  read+blend per frame. Useful for diagnostic overlays, integration
  tests, and the rare layer that genuinely needs CPU touch; bad as
  a default.

## Property minimization

The scene tracks per-layer last-committed state and emits only the
properties that changed. The two non-obvious invariants:

1. **`FB_ID` is always re-emitted** even when the property snapshot
   is clean. A commit that elides every property and every FB_ID
   write produces no `PAGE_FLIP_EVENT` — a silent "nothing changed"
   that breaks the caller's flip pump. The scene defends against
   this unconditionally; consumers don't need to.
2. **`alpha` and `pixel_blend_mode` are reset on every native plane
   on first commit after a session resume.** Both properties survive
   across compositor sessions on amdgpu (and likely elsewhere); a
   layer that previously ran at 80% alpha would inherit that value
   on the new session unless explicitly reset.

Both invariants are tested in `tests/integration/test_layer_scene_minimization_vkms.cpp`.

## Out of charter

`drm::scene` will deliberately not become any of the following.
Drawing these boundaries up front prevents scope creep from
well-meaning contributors:

- **A scene graph.** No parent/child layers, no transform
  inheritance, no group nodes. Layers are flat; positions are
  absolute. Consumers that want hierarchy build it on top.
- **A compositor.** No protocol, no clients, no surface negotiation,
  no input arbitration. The scene drives a single application's
  layers onto its own CRTC.
- **A widget toolkit.** No buttons, no labels, no event dispatch
  beyond the input crate the application already owns. The scene
  composes content; it does not produce it.
- **A renderer.** No Cairo, no Skia, no Blend2D path inside scene
  itself. `drm::capture` and the signage-player example use Blend2D
  because their *content* is text and shapes; the scene neither
  knows nor cares.

If a feature request can be reframed as one of the four above, the
answer is that it lives outside `drm::scene` — possibly in a
companion module (`drm::csd` for client-side decorations is one
example), possibly in the application.

## Forward compatibility

The library's v2 audience is specialized-stack users we deliberately
deferred in v1. Each is accommodated architecturally already.

### EGL Streams

EGL Streams remains the path NVIDIA's proprietary driver and
Tegra-based automotive / industrial stacks use for non-GBM
workflows. Stream-consumed planes mix awkwardly with FB-ID-attached
planes in the same atomic commit (driver-version-dependent
restrictions); the kernel surface and capability probes that would
let the allocator reason about that mix are non-trivial and
`vkms` cannot emulate them.

The `LayerBufferSource` interface accommodates streams as outlined
in the buffer-source section above. v2 is a new source class plus
the scene-side commit-path branch on `BindingModel`.

### Foreign DMA-BUF producers (V4L2 / NPU / accel)

Linux `drivers/accel/` has absorbed NPU drivers (Intel Habana,
AMD XDNA, Rockchip RKNN, Qualcomm QAIC) — these devices output
DMA-BUFs for inference results, but they have no planes / CRTCs /
connectors, so they cannot display anything themselves. V4L2
capture devices and hardware video decoders are the same shape.

A future `AccelOutputSource` (or `DmabufImportSource`) implementing
`LayerBufferSource` lets these producers drive scene layers the same
way `GbmBufferSource` does — no scene-side change required. v1
ships without one because each producer ecosystem (V4L2, accel,
vendor cameras) wants a slightly different acquire/release surface
and we'd rather land the right shapes once we have a target
consumer. The `examples/scene/camera/` example is the closest
existing reference: it imports libcamera DMA-BUFs through
`ExternalDmaBufSource`, which is the prototype for the
`DmabufImportSource` direction.

### Multi-CRTC orchestration

Spanning a logical scene across two physical CRTCs (dashboards,
automotive multi-display, video walls), mirroring the same content
across CRTCs with per-output post-processing, and cross-screen
effects are all v2 territory. The current `LayerScene` is a
single-CRTC abstraction by construction. A multi-CRTC composition
layer would sit above `LayerScene`, not inside it.

### Animation engine

Property interpolation (alpha, position, scale) hardware-accelerated
via plane properties where supported, degrading gracefully to
per-frame re-rasterization where not. Scoped for v2; the layer
mutation API the `layered_demo` example exercises is the foundation.

## Format modifiers as first-class

Format modifiers are *in v1*, not deferred. The motivation:

1. **Embedded SoCs depend on them for performance.** Mali GPUs use
   AFBC; AMD uses DCC; Rockchip, Samsung, and Mediatek all have
   vendor-specific tilings. A scene that doesn't reason about
   modifiers can't deliver on the embedded-UI value proposition —
   it forces every layer to linear, defeating the whole point of
   using planes for performance.
2. **v2 foreign buffer sources require it.** V4L2 cameras, video
   decoders, and NPU outputs almost universally emit modifier-tagged
   buffers. Adding modifier support cleanly in v2 is harder than
   baking it in from v1 because it touches `LayerDesc`,
   `LayerBufferSource::requirements()`, the allocator's eligibility
   checks, the composition fallback's source handling, the
   diagnostics surface, and every example using mixed buffer
   sources.
3. **The allocator already handles it.** drm-cxx's `PlaneRegistry`
   and `Allocator` query `IN_FORMATS` per plane (the
   modifier-capability data); the work was plumbing it through the
   scene's surface, not novel allocator logic.

`SourceFormat::modifier` defaults to `DRM_FORMAT_MOD_LINEAR` and
propagates through `add_layer` to the allocator's eligibility
check. The composition fallback consumes the source's CPU mapping,
so a tiled source's modifier is irrelevant to the canvas; the
canvas's own output remains `LINEAR`.

## Appendix: when to drop down to raw `drm::planes::Allocator`

`LayerScene` is the right answer for almost every consumer. Reach
for the raw `Allocator` when:

- **You're integrating a custom commit loop** that doesn't fit the
  scene's ownership model — e.g. you're driving multiple atomic
  request batches per frame, sequencing them with non-DRM kernel
  IO, or sharing the request with a sibling subsystem.
- **You want the assignment without the side effects.** The scene
  owns dirty tracking, FB lifetime, session resume, and rebind. The
  raw allocator just answers the bipartite question and writes
  properties to a request you provide. If you want only the answer,
  there is no overhead and no abstraction in your way.
- **You're building a different abstraction on top.** A multi-CRTC
  orchestrator, a Wayland compositor backend, a stream consumer
  arrangement — all of these reasonably build on the allocator
  directly rather than through `LayerScene`. `LayerScene` is
  designed as a single-CRTC application-tier abstraction, not as
  glue between subsystems.
- **You're debugging an allocator-shaped problem in isolation.**
  `examples/allocator/overlay_planes/` is the bare-minimum surface
  for this — open the device, enumerate planes, build a layer set,
  call `apply()`, inspect what came back. Reproducing an allocator
  edge case there is faster than reproducing it through `LayerScene`.

For everything else — and we mean everything else — `LayerScene`'s
ownership model, dirty tracking, and composition fallback save real
effort. The pedagogical examples in `examples/allocator/` show what
hand-rolling each individual scene feature looks like; pretty quickly
the answer is "this is what the scene is for."

## See also

- [`src/scene/README.md`](../src/scene/README.md) — module reference
  (types, headers, lifecycle contract, examples, stability).
- [`docs/implementation_plan.md`](implementation_plan.md) — the
  milestone-level roadmap the v1 design choices were drawn from.
- `examples/allocator/overlay_planes/README.md` — the raw-allocator
  reference example.
