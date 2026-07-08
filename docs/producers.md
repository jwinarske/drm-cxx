# Producers — getting content onto a scene layer

This is the practical guide to the buffer-source ecosystem: which sources
exist, when to reach for each, how to write a new one, and how sync fences and
format modifiers flow through a producer. For the *why* behind the polymorphic
`LayerBufferSource` design, see [`scene.md`](scene.md); for the module's API and
lifetime contract, see [`../src/scene/README.md`](../src/scene/README.md).

## Two families

drm-cxx has two producer surfaces, one layered on the other:

- **`drm::scene::LayerBufferSource`** (`src/scene/buffer_source.hpp`) — "where do
  this scene layer's pixels come from?" A source is bound to one layer of a
  `LayerScene`; the scene calls `acquire()` each frame to get a ready
  framebuffer and `release()` after the flip. This is the surface you implement
  to teach the scene about a new content source.
- **`drm::present::ScanoutProducer`** (`src/present/scanout_producer.hpp`) — a
  renderer-specific factory for the full-screen presenter. It negotiates
  modifiers and produces exactly one `LayerBufferSource` for a single-layer
  scene. This is the one renderer-specific seam of the present path.

`drm::present::ScanoutBackend` ties them together: given a `ScanoutProducer` it
discovers an output, negotiates the producer's `exportable_modifiers()` against
the plane's `IN_FORMATS`, calls `create_buffer()` to get a `LayerBufferSource`,
builds a single-layer `LayerScene` fed by it, and drives one atomic commit per
`present()`. Everything above the producer — target discovery, modifier
negotiation, plane allocation, composition fallback, deferred release, session
pause/resume, idle-Skip pacing — is shared. If you are presenting one
full-screen surface, use a `ScanoutProducer` + `ScanoutBackend`. If you are
building a multi-layer scene yourself, attach `LayerBufferSource`s to layers
directly.

## The source catalog

Eleven concrete content sources ship today, plus the EGL Streams source and two
internal forwarders. Pick by what already holds your pixels.

| Source | Backing | Reach for it when |
|---|---|---|
| `DumbBufferSource` | one CPU-writable linear dumb buffer | software-rendered cursors, CSDs, test patterns, static signage backgrounds |
| `GbmBufferSource` | one CPU-mapped LINEAR GBM scanout BO | software content on a GBM device, or a LINEAR-only KMS node; what `GbmScanoutProducer` hands out |
| `GbmSurfaceSource` | a `gbm_surface` front-buffer queue (2–3 BOs) rendered by EGL/Vulkan | GL/Vulkan-rendered layers; the GPU-rendered, multi-buffer sister to `GbmBufferSource` |
| `ExternalDmaBufSource` | one imported DMA-BUF (1–4 planes, any modifier) | a single foreign buffer — zero-copy capture, an NPU output, a Vulkan `VK_EXT_image_drm_format_modifier` export |
| `ExternalDmaBufRing` | an N-slot ring of imported DMA-BUFs | a rotating external producer feeding frames from its own thread (WebGPU/"water", CEF); the external analogue of `DumbRingSource` |
| `V4l2CameraSource` | a V4L2 CAPTURE-only endpoint | UVC webcams, embedded ISPs, `vivid` — DMABUF zero-copy or MMAP-copy fallback |
| `V4l2DecoderSource` | a stateful V4L2 video decoder CAPTURE queue | hardware H.264/HEVC decode (NV12 + P010/P012/P016) |
| `GstAppsinkSource` | a GStreamer `appsink` | pipelines whose demux/decode/filter already live in GStreamer |
| `CursorSource` | a `drm::cursor::Cursor` blitted into a dumb ARGB buffer | a cursor on hardware with no CURSOR plane (e.g. rockchip VOP2), presented as an ordinary layer |
| `NvBufSurfaceSource` | one NVIDIA L4T `NvBufSurface` (Jetson NVMM) | Jetson pipelines wanting a cache-mappable buffer the composition fallback can read at speed (gated `DRM_CXX_HAS_NVBUFSURFACE`) |
| `DumbRingSource` | a multi-buffered ring of linear dumb buffers | a CPU producer that wants the `EGL_EXT_buffer_age` repaint pattern (damage-aware, multi-slot); lives in `src/present/` but is a `scene::LayerBufferSource` |
| `EglStreamSource` | an EGL Streams consumer wired to a DRM plane | NVIDIA-proprietary / Tegra producers; reports `DriverOwnsBinding` (gated `DRM_CXX_HAS_EGL_STREAMS`, built via `EglStreamBuilder`, not a `ScanoutProducer`) |

### Capabilities at a glance

The behaviors that decide whether a source fits a given layer:

| Source | `map()` / compositable | Acquire fence | Release fence | Damage-aware | Idle-aware¹ |
|---|---|---|---|---|---|
| `DumbBufferSource` | yes | — | — | yes (`set_damage`) | — |
| `GbmBufferSource` | yes | — | — | — | — |
| `GbmSurfaceSource` | no | **yes** | — | — | — |
| `ExternalDmaBufSource` | no | **yes** | — | — | — |
| `ExternalDmaBufRing` | no | **yes** | **yes** | yes (per-slot) | **yes** |
| `V4l2CameraSource` | MMAP mode only | — | — | — | — |
| `V4l2DecoderSource` | no | — | — | — | **yes** |
| `GstAppsinkSource` | no | — | — | — | — |
| `CursorSource` | yes | — | — | yes (animated) | — |
| `NvBufSurfaceSource` | yes (cached mmap) | — | — | — | — |
| `DumbRingSource` | no² | — | — | yes (buffer-age) | — |
| `EglStreamSource` | no | — | — | — | (EAGAIN until bound) |

¹ *Idle-aware* = overrides `has_fresh_content()` to report "nothing new," which
lets `LayerScene`'s all-idle whole-commit Skip suppress a redundant flip. Only
`ExternalDmaBufRing` and `V4l2DecoderSource` do this today; every other source
defaults to `true` (always "changed"), which is the safe answer for a producer
the scene cannot introspect.
² `DumbRingSource` paints through a leased mapping in its `paint()` callback
rather than exposing `map()`, so it is not composition-fallback-readable.

### Internal forwarders

Two sources delegate to an inner source rather than owning pixels — you will not
construct these directly:

- **`ProxyBufferSource`** (`gl_scanout_producer.cpp`, anonymous) — the non-owning
  proxy `GlScanoutProducer` hands the scene over its producer-owned
  `GbmSurfaceSource`, so EGL tears down before the `gbm_surface` it wraps.
- **`SharedLayerBufferSource`** (`scene_set.cpp`, anonymous) — a per-scene
  forwarder holding a `shared_ptr` to one app-provided source, so multiple
  scenes in a `SceneSet` can share it. Only static-buffer sources tolerate the
  N-acquires-per-frame fan-out; a per-frame ring source does not.

## The `ScanoutProducer` trio

For a full-screen presenter, three producers cover the rendering paths:

| Producer | Renders with | Yields | Build gate |
|---|---|---|---|
| `GbmScanoutProducer` | nothing — one CPU-mappable LINEAR GBM buffer | `GbmBufferSource` | none (always built) |
| `GlScanoutProducer` | EGL/GLES into a `gbm_surface` (`make_current`/`swap_buffers`) | a proxy over an owned `GbmSurfaceSource` | `DRM_CXX_HAS_EGL` |
| `VkScanoutProducer` | Vulkan into a modifier-tiled `VkImage`, memory exported as dma-buf | `ExternalDmaBufSource` | `DRM_CXX_HAS_VULKAN` |

The GPU producers own their device, context, and buffers, and must **outlive the
scene** they feed (the scene holds a proxy; teardown is scene-first,
producer-last). Both never link their GPU library: libEGL is `dlopen`'d via
`drm::detail::egl_loader`, libvulkan by Vulkan-Hpp's `DynamicLoader`. They are
build-gated so the class vanishes when the headers are absent, and `create()`
fails with `not_supported` when the runtime library is missing — a GBM-only
deployment neither links nor loads either. `GbmScanoutProducer` carries no such
contract; it links GBM/DRM directly.

## Writing a new source

Implement `LayerBufferSource`. Only three methods are pure-virtual; the rest have
correct defaults for the common (`SceneSubmitsFbId`, CPU-producer) case.

**Required:**

- `acquire() -> expected<AcquiredBuffer, error_code>` — arrange that `fb_id`
  points at a ready-to-scan framebuffer and return it. Return
  `errc::resource_unavailable_try_again` (EAGAIN) — *not* an error — when you
  have no frame to contribute this vblank; the scene skips the layer for that
  commit and re-calls next frame. Sources that always have a valid buffer never
  need EAGAIN.
- `release(AcquiredBuffer) noexcept` — return the buffer to your free pool.
  Infallible by contract: if your release path can genuinely fail (remote /
  network producers), log and leak rather than propagate.
- `binding_model()` — return `SceneSubmitsFbId` unless the driver binds the
  producer to the plane directly (EGL Streams); `DriverOwnsBinding` is v2
  territory and the scene asserts on its appearance for a v1 layer.
- `format()` — the `SourceFormat` (fourcc, modifier, intrinsic w/h) the allocator
  reads. Must stay stable across a session pause/resume.

**Override when relevant:**

- `map(MapAccess)` — return a scoped `drm::BufferMapping` if your pixels live in
  CPU-readable memory. This is what makes a source **compositable**: when the
  allocator can't place the layer on a hardware plane, the composition fallback
  reads through `map()`. A source with no CPU mapping returns
  `function_not_supported` (the default) and stays dropped when unplaced —
  surfacing in `CommitReport::layers_unassigned`.
- `has_fresh_content()` — override to return `false` when you *know* you are idle
  (a rotating producer with nothing newly submitted). This enables the scene's
  idle-Skip. Leave the default `true` if you cannot tell — never skip a real
  update.
- `wants_release_fence()` / `release_with_fence(...)` — override to receive the
  displacing commit's OUT_FENCE, so a GPU producer can wait GPU-side before
  re-rendering a recycled buffer instead of CPU-blocking (see below).
- `on_session_paused()` / `on_session_resumed(new_dev)` — fd-backed sources must
  drop every GEM handle / FB ID bound to the old fd and re-allocate on `new_dev`
  after a VT switch. Pure-memory sources no-op.
- `bind_to_plane()` / `unbind_from_plane()` — v2 `DriverOwnsBinding` hooks; v1
  FB-ID sources leave them as the no-op default.

## Sync, fences, and modifiers

### Acquire fence → `IN_FENCE_FD`

An asynchronously-rendered source stamps a render-done sync_file into
`AcquiredBuffer::acquire_fence`; the pixels are only valid once it signals. The
scene arms it as the plane's `IN_FENCE_FD` so the display engine — not the CPU —
waits before scanout, and falls back to a CPU `wait()` only when the plane has no
`IN_FENCE_FD` property. The fence rides the buffer's lifecycle and closes its fd
on release/drop (the kernel does not take ownership of `IN_FENCE_FD`).

Sources that produce fences today: `GbmSurfaceSource` (GL, via an
`EGL_ANDROID_native_fence` per swap), `ExternalDmaBufSource` (Vulkan, via an
exported semaphore), and `ExternalDmaBufRing` (per-frame via `submit()`).
Synchronously-ready sources (dumb, GBM, V4L2, imported single DMA-BUFs) leave the
fence `nullopt`.

`CommitReport::in_fences_armed` / `in_fence_cpu_waits` count each outcome per
commit, so you can tell the KMS-side path from the CPU-wait fallback. Set
`DRM_CXX_FENCE_DEBUG=1` to trace them per commit at runtime.

### Release fence → `OUT_FENCE_PTR`

A source that recycles buffers can override `wants_release_fence()` to receive
the OUT_FENCE of the commit that displaces a buffer: once it signals, the buffer
is off-screen and safe to render into again. This lets a GPU producer wait
GPU-side rather than CPU-block on the (later) release edge. The scene only
requests an internal OUT_FENCE when some live source opts in — zero cost
otherwise. `ExternalDmaBufRing` is the source that uses it today.

### Modifier negotiation

`SourceFormat` carries a `modifier`, so a source advertises its tiling
(AFBC / DCC / UBWC / NVIDIA block-linear / LINEAR) without a separate per-layer
field. For a GPU producer choosing a modifier, the pattern is:

1. `LayerScene::candidate_modifiers(drm_format)` returns the union of modifiers
   any non-cursor plane on the scene's CRTC accepts.
2. Intersect with what the renderer can export
   (`eglQueryDmaBufModifiersEXT` / `VkDrmFormatModifierPropertiesListEXT`, exposed
   as `ScanoutProducer::exportable_modifiers()`).
3. Pick one and hand it back (`GbmSurfaceConfig::modifier`, or the Vulkan image's
   tiling modifier).

`IN_FORMATS` is only a *necessary* filter — a `DRM_MODE_ATOMIC_TEST_ONLY` commit
is the ground truth for what a plane actually scans out. `drm::fmt::FormatTable`
parses a plane's `IN_FORMATS` and `drm::fmt::classify()` labels a modifier's
bandwidth class for allocator scoring (never for correctness). The
`egl_scene` demo walks the full GL negotiation; `vulkan_scene` shows the Vulkan
counterpart (`gbm_surface` is EGL-only, so the honest Vulkan path is a
`VK_EXT_image_drm_format_modifier` export wrapped in `ExternalDmaBufSource`).

## Examples

| Demo | Shows |
|---|---|
| `examples/advanced/gl_present/` | `GlScanoutProducer` + `ScanoutBackend`, full-screen GL |
| `examples/advanced/egl_scene/` | GL producer with explicit modifier negotiation |
| `examples/advanced/vulkan_scene/` | Vulkan producer via `ExternalDmaBufSource` |
| `examples/scene/camera/` | `V4l2CameraSource` (zero-copy + MMAP fallback) |
| `examples/scene/v4l2_decode/` | `V4l2DecoderSource` decode → KMS |
