# Changelog

## v2.0.1 — 2026-07-21: allocator warm-start stability

### Fixes

- **`Allocator::score_pair` now adds a warm-start stability bonus.** When the
  layer *set* changes between frames — e.g. a compositor inserting or
  re-splitting a backing-store layer while overlay layers stay put —
  `full_search` re-solved the whole plane assignment from scratch and could
  reshuffle already-placed layers onto different planes, reprogramming every
  overlay in one commit (visible as flicker). Scoring now strongly prefers
  keeping a layer on the plane it held last frame. The bipartite matcher
  maximizes cardinality first and uses the score only to order tie-breaks, so
  this never places fewer layers and never overrides format/zpos/type validity;
  a genuinely new layer still displaces an old one only when a plane is
  contested.

## v2.0.0 — 2026-07-15: present spine, format registry, foreign producers, EGL Streams, multi-CRTC, HDR/color, CSD presenters

The major bump is earned by two source-breaking changes (below), not only by the
size of the additive surface. Every consumer must recompile: four more public
signatures changed compatibly at the source level but not at the ABI level.

### Breaking changes

- **`drm::planes::PlaneCapabilities::supports_format_modifier(uint32_t, uint64_t)` is removed.**
  Plane format/modifier eligibility is now decided by `drm::fmt::FormatTable`,
  built per-plane in `PlaneRegistry`. Migrate to
  `plane.format_table.supports(fourcc, drm::fmt::Modifier{modifier})` — note the
  modifier is a typed `drm::fmt::Modifier` there, not a raw `uint64_t` — or to
  `PlaneCapabilities::bandwidth_class(modifier)` (which does take a plain
  `uint64_t`) when the question is cost rather than support.
- **`drm::scene::CompositeCanvas::drm_fourcc()` is no longer `static`.** It is a
  virtual member overriding `CompositionTarget`, so the canvas can report the
  format it actually allocated rather than a fixed one. Call it on an instance.
  `CompositeCanvas::modifier()` is unchanged and remains `static`.
- **ABI: all consumers must rebuild.** Beyond the two above, four public
  signatures gained defaulted parameters or shifted layout —
  `Allocator::apply` (now takes `bool test_only = false`),
  `session::Seat::take_device` (now takes `TakeDeviceOpts opts = {}`),
  `LayerScene::commit`, and `planes::Layer::properties`. Source-compatible; not
  binary-compatible.

### `drm::present` — scanout spine (new)

- **`ScanoutBackend` + the `ScanoutProducer` seam** — a render-to-scanout path
  that owns buffer rotation and commit, with three producers: `GbmScanoutProducer`,
  `GlScanoutProducer` (EGL/GLES) and `VkScanoutProducer` (Vulkan). The core never
  link-binds `libEGL` or `libvulkan` — producers reach them through `dlopen` and
  the dynamic loader, so a build with the headers present still runs on a target
  without the runtimes.
- **`BufferRing`** — slot pool behind the producers, with buffer-age reporting for
  partial repaint.
- **`ScanoutTarget` / `display::ScanoutTarget`** — output discovery for the spine.
- **Modifier negotiation across all planes**, not just the primary.
- **`DumbScanoutSink`** — presents CPU-rendered frames through a dumb-buffer ring
  for GPU-less targets, with vsync pacing, RGB565 (16-bpp) scanout to halve
  bandwidth on legacy SoCs, damage-aware `present`, and a `DumbRingSource` that
  drives buffer-age partial repaint.
- **Opt-in `RestorePolicy`** — restores the CRTC on teardown instead of leaving
  the last frame latched.

### `drm::present` — frame economy

- **`FrameEconomy`** — idle suppression: an unchanged frame is skipped rather than
  committed, wired into `ScanoutBackend`. Paired with per-frame damage and
  buffer-age tracking so a partial repaint replays only what changed.
  `FB_DAMAGE_CLIPS` is opt-in and driver-dependent — some drivers advertise it on
  only a subset of their planes and others not at all, so the damage hint is a
  no-op on those and the full redraw stands (see `docs/hardware.md` for the
  per-board picture).
- **VRR driven from the `DriverProfile`** in `ScanoutBackend`, plus async flip.
- **Page-flip epoll integration** and `EINTR`-safe dispatch.

### `drm::present` — explicit sync

- **`IN_FENCE_FD` / `OUT_FENCE_PTR`** — acquire fences from Vulkan and GL
  producers (the GL side exports via EGL native-fence), and a scanout-completion
  fence so a producer knows when a buffer is reusable. Enables cross-device
  scanout. Fence counters surface in `CommitReport`.

### `drm::fmt` — format/modifier registry (new)

- **`drm::fmt`** — format-modifier and hardware-compression module: `classify()`,
  `describe()`, `BandwidthClass`, per-format scanout cost, and
  `rotation_compatible`. Vendor decoding covers NVIDIA compression (corrected) and
  VeriSilicon (vendor `0x0b`).
- **`FormatTable`** — built per-plane in `PlaneRegistry` and now the single gate on
  plane format/modifier eligibility, replacing the hand-rolled parser (this is what
  removed `supports_format_modifier`).
- **Fixes:** misaligned read while parsing the `IN_FORMATS` modifier blob;
  LINEAR/INVALID-modifier dma-bufs import via plain `AddFB2`.

### `drm::display` — driver profile, HDR, color pipeline

- **`DriverProfile`** — capability probe, including `FB_DAMAGE_CLIPS`, VRR and PSR.
- **CRTC color pipeline** — DEGAMMA / CTM / GAMMA.
- **HDR** — EDID-sourced HDR / colorimetry / wide-gamut, `HdrMetadataCache` wired
  into `LayerScene::set_output_metadata`, and a `ToneMapper` for CPU HDR↔SDR
  fallback with a LUT-accelerated inner loop. The tone mapper is a correctness
  fallback, not a real-time path.
- **EDID make/model/serial + physical size**, connector `mode_list`, vrefresh
  range, and one shared `crtc_for_connector()` across the output pickers.

### `drm::scene` — external producers

- **`ExternalDmaBufRing`** — for external rotating DMA-BUF producers (the
  CEF/water class of producer), carrying per-frame damage, keeping the
  `acquire()` damage drain allocation-free under lock, and carrying damage across
  a fence-deadline drop rather than dropping it.
- **`ExternalDmaBufPool`** — lazy, dynamically populated dma-buf source, bounded
  with LRU eviction and a `reset_generation` for decoder resolution changes.
- **`DmaBufSourceCache`** and a DMA-BUF export API for direct GPU composition.
- **`V4l2DecoderSource`** — over V4L2 stateful decoders (real H.264), with
  DMA-BUF export and `has_fresh_content` reporting for idle-skip.

### `drm::scene` — GPU composition

- **`GlCompositor`** — GPU-backed composition fallback for `LayerScene`, with a
  hardened software-renderer guard for zink.

### `drm::csd` — presenters

- **`CompositePresenter`** (software composite onto one plane),
  **`FramebufferPresenter`** (`/dev/fb0` blit), and **`probe_presenter`** for
  startup KMS presenter selection, plus a composite-presenter desktop backdrop.
- **Incremental damage** — damage-clipped compositing and blit-damage for the
  framebuffer presenter.

### `drm::capture` — JPEG

- **libjpeg-backed JPEG encoder** behind a build option, plus an NV12 encoder and
  camera snapshot.

### `drm::log` — every message redirectable

- **libinput and libseat diagnostics now route into `drm::log`**, tagged
  `[libinput]` / `[libseat]`, so they follow `set_log_sink` instead of escaping to
  the wrapped libraries' own stderr handlers. `input::SeatOptions::log_handler`
  overrides the default routing for a consumer that wants libinput's stream kept
  separate.
- **drm-cxx's own output too** — `src/` no longer writes to stderr/stdout outside
  `log.hpp`'s default sink. The `DRM_ALLOC_DEBUG` / `DRM_EXT_DMABUF_DEBUG`
  channels are redirectable while keeping their env-var gate as their threshold
  (the gate is not subject to the global level — asking for a channel by name is
  not vetoed by the default, mirroring GStreamer's per-category thresholds).


### `drm::scene` — EGL Streams

- **`drm::scene::StreamCapability` + `probe_stream_capability(dev)`** — runtime capability probe for EGL Streams support. dlopen-only against `libEGL.so.1` (libdrm-cxx is never link-bound to libEGL, even when the headers are present at build time); inspects client-side `EGL_EXT_platform_device` / `EGL_EXT_device_drm`, enumerates `EGLDeviceEXT`s, matches each against the caller's `drm::Device` by `st_rdev`, and returns the per-device-display extension set (`EGL_EXT_output_drm`, `EGL_KHR_stream`, `EGL_EXT_stream_consumer_egloutput`, `EGL_NV_stream_consumer_eglimage`, `EGL_KHR_stream_producer_eglsurface`) along with vendor / version / client-api strings. Mesa-only systems return `StreamMixingMode::Unsupported` deterministically; NVIDIA proprietary / Tegra return `Exclusive` (conservative default until the empirical probe upgrades it).
- **`drm::scene::EglStreamBuilder`** — public one-stop entry point for constructing an EGL-stream-backed `LayerBufferSource` end-to-end. Hides the `EGLDevice` match, `EGLDisplay` creation, `EGLConfig` selection, GLES context creation, and source wiring behind a single static `build(Request) → Result` call. `Request` takes the probed capability, the `drm::Device`, the producer-side dimensions / DRM FourCC, and optional `existing_display` / `existing_context` handles for callers who already have an EGL stack on the chosen device. `Result` carries an upcast `unique_ptr<LayerBufferSource>` ready for `LayerScene::add_layer`, plus the bound `display` / `egl_config` / `context` / `producer_surface` / `stream` handles. Defaults to `EGL_STREAM_BIT_KHR` + ES2 renderable + RGBA8888 + no depth/stencil + a GLES 3.0 context. `EglStreamSource` is internal — public construction is exclusively through the builder.
- **`drm::scene::EglStreamSource`** — internal `LayerBufferSource` whose producer is an EGL stream's producer surface and whose consumer is bound directly to a DRM plane via `EGL_EXT_output_drm` / `EGL_EXT_stream_consumer_egloutput`. `binding_model()` reports `DriverOwnsBinding` so the scene's commit path skips `FB_ID` writes; the EGL consumer extension arms the plane via vendor-private kernel state. `bind_to_plane(plane_id)` is idempotent on the same plane and tears down + recreates stream + producer surface on cross-plane rebinds. `acquire()` returns `AcquiredBuffer{fb_id=0}` once bound, EAGAIN before bind / while paused / after teardown. `on_session_paused` destroys stream + producer surface; `on_session_resumed` reuses the surviving device-bound `EGLDisplay` and rebuilds.
- **`LayerScene` DriverOwnsBinding plane pinning** — `ensure_stream_layer_pins` runs before `acquire_all` in the commit path. For every alive slot whose source reports `DriverOwnsBinding`, the scene picks the first format-compatible non-cursor plane on the bound CRTC (preferring OVERLAY, falling back to PRIMARY; avoids the canvas reservation plane and other stream pins), calls the source's `bind_to_plane`, and records the pin on the slot. `arm_stream_layer_planes` writes every non-`FB_ID` non-immutable property from the layer's bag to the pinned plane after `Allocator::apply` succeeds; `arm_layer_plane_blend_defaults` and `arm_layer_plane_color_props` fall back to the stream pin when the allocator didn't assign the layer, so a previous compositor's `pixel-blend-mode` / `COLOR_ENCODING` / `COLOR_RANGE` can't bleed through. `remove_layer` calls `unbind_from_plane` on the source before destroying it; `on_session_paused` and `rebind` clear every pin.
- **Allocator exclusion of externally-bound layers** — `planes::Layer::set_externally_bound` mirrors the existing `is_composition_layer` filtering everywhere (warm-start new-layer detection, both `needs_composition_` flagging passes, the best-partial fallback layer list), plus a new pre-`split_independent_groups` filter step in `full_search` that strips externally-bound layers before group splitting. The existing `external_reserved_` mechanism handles `disable_unused_planes`' leave-alone semantics. `apply_layer_to_plane_real` defensively skips `FB_ID` writes for externally-bound layers.
- **`LayerScene::probe_stream_mixing()`** — empirical mixing probe via a one-shot `DRM_MODE_ATOMIC_TEST_ONLY` commit pairing an already-bound stream consumer plane with a freshly-attached FB-ID plane on the same CRTC. Kernel acceptance upgrades the cached `StreamCapability::mixing` from `Exclusive` to `Mixed` and the verdict is sticky for the rest of the scene's lifetime. Cleared on `rebind` and on session resume. `function_not_supported` when capability is `Unsupported` or no stream layer is bound; `resource_unavailable_try_again` when every candidate plane is taken; otherwise returns the current mixing mode regardless of upgrade.
- **`Exclusive` mixing constraint enforcement** — when the empirical probe has confirmed the driver can't cohabit FB-ID planes with a stream consumer plane on one CRTC, `apply_exclusive_mixing_constraint` runs after `ensure_stream_layer_pins` and sets `planes::Layer::transient_composited_` on every alive non-stream layer so they route through the composition canvas. Distinct from `force_composited_` (user-set, persistent) so a probe re-run can flip behavior without stomping user intent. Gated on `mixing_probe_ran_` so the first commit isn't tightened before the kernel has confirmed the verdict.
- **NVIDIA quirk wiring** — `EglStreamBuilder` passes `EGL_DRM_MASTER_FD_EXT` via the EGL 1.5 core `eglGetPlatformDisplay` (`EGLAttrib*`) attribute list; without it NVIDIA's stack opens its own DRM fd internally and `eglStreamConsumerOutputEXT` fails `EGL_BAD_ACCESS` even when the application holds master on its own fd. Producer surface creation is deferred to `bind_to_plane` (`eglCreateStreamProducerSurfaceKHR` returns `EGL_BAD_STATE_KHR` on a consumer-less stream regardless of what the spec permits). On systems exporting `EGL_NV_output_drm_atomic` (Tegra) the scene's first commit routes through `EglStreamSource::prime_first_commit` and `eglStreamConsumerAcquireAttribKHR(EGL_DRM_ATOMIC_REQUEST_NV)` so the driver fills in `FB_ID` from the first producer frame and submits the commit itself; `drm::AtomicRequest::native_handle()` exposes the raw `drmModeAtomicReq*` for the handoff. Desktop NVIDIA 535 doesn't export the extension — the documented scanout gap.
- **`EglStreamSource::flip_event_data()`** — `EGL_NV_output_drm_flip_event` identifier queried via `eglQueryOutputLayerAttribEXT(EGL_DRM_FLIP_EVENT_DATA_NV)` at `bind_to_plane` time, returned as `std::optional<std::uint64_t>`. Lets callers running `drmHandleEvent` map a `drm_event_vblank` back to the source that fired it. Returns `nullopt` before bind, on Mesa / older proprietary stacks, or when the query fails.
- **`drm::scene::egl_runtime`** — internal shared dlopen registry hoisted out of `stream_capability.cpp`. Resolves the full streams entry-point set under one `std::call_once`: bootstrap symbols via direct `dlsym`, device-enumeration and streams / output-layer extension entry points via `eglGetProcAddress`. Process-singleton so the builder, the probe, the source, and any future caller share one runtime instead of duplicating the dlopen.
- **`LayerScene::Config::stream_capability`** — explicit pass-through field so applications opt into streams by calling `probe_stream_capability` themselves rather than paying the dlopen cost on every scene construction. Survives `rebind()` and pause/resume verbatim (the capability describes the driver, not the connector). Accessor `LayerScene::stream_capability()` lets layers and producer-side builders consult the result.
- **`add_layer` gating on `BindingModel::DriverOwnsBinding`** — registration rejects sources reporting `DriverOwnsBinding` when the scene was constructed with `StreamMixingMode::Unsupported`. Failure mode is `errc::not_supported` at registration time rather than at first commit. v1 sources are unaffected — none override `binding_model()`.
- **Build wiring** — `meson -Dstreams={auto,enabled,disabled}` / `cmake -DDRM_CXX_STREAMS={AUTO,ON,OFF}`. Headers-only build dependency on `egl.pc` (or a bare `<EGL/egl.h>` + `<EGL/eglext.h>` probe when pkg-config doesn't see it); libEGL is never linked into libdrm-cxx so the library stays loadable on Mesa systems with no EGL runtime. `DRM_CXX_HAS_EGL_STREAMS` is now PUBLIC on the CMake target / forwarded via meson's `streams_cpp_args` so dependents see the same gate value as the library was built with — required because the installed `egl_stream_builder.hpp` pulls in `EGL/egl.h`. `DRM_CXX_HAS_EGL_STREAMS=0` builds short-circuit `probe_stream_capability` to a constant `Unsupported` and compile the streams TUs as empty translation units.

### `drm::scene` — SceneSet cross-CRTC orchestration

- **`drm::scene::SceneSet`** — coordinator owning a fixed vector of `LayerScene` instances and driving them through one `drmModeAtomicCommit` per frame. `commit()` and `test()` OR-combine per-scene atomic flags across engaged scenes and submit a single ioctl; per-scene `CommitReport`s come back in construction order, suspended / hole slots stay zero. Built on a new `LayerScene::build_frame_into` / `finalize_frame` split so the per-scene commit path stays usable on its own. Tear-free synchronized changes across multiple displays off one fd hangs off this primitive.
- **`SceneSet::add_layer(SceneSetLayerSpec)`** — fan-out routing for a single `BufferSource` shared across N scenes, each with its own `DisplayParams` (rect, zpos, alpha, rotation, `force_composited`). An internal `SharedLayerBufferSource` forwarder rides every participating scene; the application-provided `shared_ptr<LayerBufferSource>` keeps the underlying source alive until the last participating scene drops its layer. `SetLayerHandle` is the opaque handle returned for later `remove_layer`. The underlying source sees one `acquire` / `release` pair per participating scene per frame — static-buffer sources tolerate the pattern naturally, per-frame ring sources (V4L2, GstAppsink) are documented as out of scope.
- **`SceneSet::add_scene` / `remove_scene`** — hotplug mutation hooks. `add_scene` reuses the lowest hole left by a prior remove before appending so previously-issued `SetLayerHandle` indices stay stable across both operations. `remove_scene` walks every active `SceneSetLayerSpec` slot and drops pins targeting the removed scene; shared-source layers mirrored across other still-live scenes keep their remaining participations. Removed slots are preserved as `nullptr` in the scene vector — `scene(index)` returns `nullptr`, `scene_count()` still reports the high-water mark until a later `add_scene` refills. SceneSet doesn't subscribe to `HotplugMonitor` internally — the caller owns the udev poll loop.
- **`NarrowPolicy` for cross-CRTC commit grouping** — per-call grouping policy on `SceneSet::commit` / `test`. `AutoOnModeset` (default) issues one ioctl when every engaged scene is on the same side of the modeset boundary, otherwise two (modeset-needing side first, steady side second) so a fresh `add_scene` doesn't pull unrelated CRTCs through `ALLOW_MODESET`. `Combined` keeps the single-ioctl behavior; `PerCrtc` unconditionally splits. The partition planner (`drm::scene::detail::partition_for_policy`) is a pure function over `(is_hole, wants_modeset)` slot-state vectors for direct unit testing.
- **`LayerScene::would_request_modeset()`** — const peek used by `AutoOnModeset` to plan grouping before the build pass. Returns `first_commit_ || hdr_dirty_pending_`; `set_output_metadata` flips the HDR-pending flag unconditionally, a successful commit clears it. Catches mid-session HDR transitions that would otherwise miss the narrowing — same-content `set_output_metadata` calls force one needless split as the cheaper failure mode. Auto-derived colorspace / HDR signaling changes still escalate the real build pass to `ALLOW_MODESET` but don't influence grouping pre-build.
- **Zero-engaged commit-skip** — `do_commit_all` counts engaged scenes during the build pass and short-circuits the kernel commit entirely when none of the children contributed state (every scene suspended, every slot a hole, or some combination). Avoids the empty atomic-commit ioctl that previously fired every frame across a VT-switched session. Partial engagement still rides one combined ioctl, preserving the cross-CRTC tear-free guarantee.
- **`LayerScene::build_frame_into` / `finalize_frame`** — build / finalize split replacing the monolithic `do_commit`. `build_frame_into(req, flags, test_only)` appends this scene's per-frame property writes onto a caller-supplied `AtomicRequest` and returns an opaque `FrameBuildState` (or a null one signaling the scene is suspended and should be skipped). `finalize_frame` takes that state plus the kernel result and reconciles per-layer scene state (`mark_clean`, recorded placements, FB release, the suspended flag on EACCES). `LayerScene::commit()` / `test()` are thin orchestrators over the split — single-scene callers see no behavior change.

### `drm::scene` — GbmSurfaceSource

- **`drm::scene::GbmSurfaceSource`** — `LayerBufferSource` wrapping a `gbm_surface` front-buffer queue that an EGL or Vulkan context renders into. `acquire()` locks the front buffer the producer most-recently swapped, registers it as a KMS FB via `drmModeAddFB2WithModifiers`, and returns the `fb_id`; `release()` returns the BO to the surface free pool. `fb_id`s are cached per `gbm_bo*` (a `gbm_surface` rotates among 2-3 BOs in practice) and torn down on destruction or session pause. `Config` takes a single committed-to DRM format modifier — pass `DRM_FORMAT_MOD_INVALID` to skip the modifier hint, or call `LayerScene::candidate_modifiers(drm_format)` to learn what the allocator will accept and pick one before construction. Scope is the single-DRM-plane packed RGB formats (8888 / 565 / 2101010); semi-planar YUV is renderer-output territory rather than scanout input. `native_device()` and `native_surface()` expose the underlying `gbm_device*` / `gbm_surface*` for producer-side EGL or Vulkan binding; both identities change across `on_session_resumed` and callers must re-query. Session pause/resume mirrors the `EglStreamSource` posture: the `gbm_surface` and cached `fb_id`s drop on pause, and on resume the source rebuilds against the new DRM fd while the producer must re-bind its EGL/Vulkan stack against the new surface.
- **`LayerScene::candidate_modifiers(drm_format)`** — returns the union of modifiers any non-cursor plane on this scene's CRTC will accept for the given format. Drives the producer-side negotiation: feed the returned list into `eglQueryDmaBufModifiersEXT` / `VkDrmFormatModifierPropertiesListEXT`, intersect with the renderer's supported set, then construct a `GbmSurfaceSource` with the picked modifier.

### `drm::scene` — V4l2CameraSource

- **`drm::scene::V4l2CameraSource`** — `LayerBufferSource` that owns a V4L2 capture device end-to-end (`QUERYCAP` + `S_FMT` + `REQBUFS` + `STREAMON`) and feeds frames into the scene with latest-frame-wins `drive()` semantics. Two memory paths sit behind a runtime probe: `VIDIOC_REQBUFS(DMABUF)` + `drmPrimeFDToHandle` + `drmModeAddFB2WithModifiers` for zero-copy scanout where the DRM device accepts the producer's dma-bufs, or `VIDIOC_REQBUFS(MMAP)` + per-frame memcpy into a double-buffered dumb-buffer pair as the universal fallback. NV12 (semi-planar 4:2:0, both single- and multi-plane V4L2 flavors) and YUYV (packed 4:2:2) in scope; MJPEG and planar YUV420 deliberately out — decoder territory and `drmModeAddFB2` layout mismatch respectively. Session pause/resume drops DRM-side state (FB IDs, GEM handles, dumb buffers) and rebuilds it against the new device on resume; the V4L2 fd is unaffected by VT switches.

### `drm::scene` — ExternalDmaBufSource modifier passthrough

- **`ExternalDmaBufSource::create` modifier passthrough** — the LINEAR-only pre-check is gone; the caller's modifier flows verbatim to `drmModeAddFB2WithModifiers` and the kernel's per-driver `IN_FORMATS` blob is the validator. Unlocks VAAPI surfaces (radeonsi / iHD NV12 export modifiers are vendor tiled bit-patterns, not LINEAR), V4L2 stateful decoders that prefer tiled output, and future GBM-with-modifier flows. Existing LINEAR callers are unaffected; no public API change.

### `drm::scene` — GStreamer source, EAGAIN flow control

- **`drm::scene::GstAppsinkSource`** — bridges a caller-owned GStreamer `appsink` element into the LayerScene buffer-source contract so any pipeline terminating in `appsink name=sink` can drive a KMS layer. Two import paths picked per sample: dmabuf zero-copy (`drmPrimeFDToHandle` + `drmModeAddFB2WithModifiers`, fb_id LRU keyed on the dma-buf fd) for hardware decoders; sysmem memcpy into a lazily-allocated dumb buffer for software decoders. Latest-frame-wins drop semantics (`drop=true`, `max-buffers=1`, `sync=false`) so a slow consumer never stalls the upstream pipeline. Format negotiation is lazy; mid-stream caps changes tear down the FB cache + sysmem fallback and re-resolve. Session integration: `on_session_paused` drops fb_ids and GEM handles bound to the dying fd; `on_session_resumed` re-imports the cached `current_sample` against the new fd so the next `acquire()` returns a valid fb_id immediately, even before the pipeline produces a fresh sample after PAUSED→PLAYING. Gated on `DRM_CXX_GSTREAMER` (CMake) / `gstreamer` (Meson); the disabled branch returns `errc::function_not_supported` from every method so consumers can feature-test against the API.
- **EAGAIN as flow control** — `LayerBufferSource::acquire()` may return `errc::resource_unavailable_try_again` when a live source has no frame to contribute this vblank (typically pre-preroll, or a producer that fell behind without a cached frame to re-issue). `LayerScene::commit()` skips the layer for that commit and counts it under the new `CommitReport::layers_skipped_no_frame`; the next commit re-calls `acquire()`. Sources that always have a valid buffer (DumbBuffer, GbmBuffer, ExternalDmaBuf) never need to return EAGAIN. The unassigned-residual math now subtracts skips so the "layer dropped" warning doesn't fire on a benign frame stall, and `Allocator::plane_statically_compatible` rejects layers with no format so a skipped layer can't phantom-place on a plane.

### `drm::csd` — Tier 0 presenter, plane coordinator, focus/hover animations

- **`drm::csd::OverlayReservation`** — startup-time plane picker for decoration surfaces. `reserve(crtc_index, format, count, min_zpos)` returns OVERLAY-typed plane IDs in zpos-ascending order that support the FourCC, sit at or above `min_zpos`, are CRTC-compatible, and aren't already taken on any other CRTC. `release(crtc_index)` is idempotent — relevant on shared-plane hardware (Mali Komeda) where releasing one CRTC's planes makes them available to another; no-op on partitioned hardware (amdgpu DCN, Intel Tigerlake+, i.MX8 DCSS, RK3399 VOP). Shortfall returns `errc::resource_unavailable_try_again` so a presenter can degrade to a software tier per-CRTC.
- **`drm::csd::Presenter`** — abstract seam between painted decoration surfaces and the path that scans them out. `Renderer` and `ShadowCache` stay tier-agnostic; only the presenter changes between tiers (Plane = desktop / well-provisioned ARM / virtio-gpu, Composite + Fb tiers TBD).
- **`drm::csd::PlanePresenter`** (Tier 0) — one DRM overlay plane per decoration. Resolves geometry, blend-mode, per-plane alpha, and zpos property ids once at construction; `base_zpos` parameter controls the stacking value the first reserved plane receives (pass `primary_zpos_max+1` on amdgpu where the primary is pinned at zpos 2). `apply(surfaces, req)` adds property writes to a caller-owned request — caller keeps full control of TEST/COMMIT, `ATOMIC_NONBLOCK`, `PAGE_FLIP_EVENT`, and `IN_FENCE_FD`. Pre-multiplied blend + `alpha=0xFFFF` forced every frame so a previous compositor's state can't bleed through. The `compute_writes` helper is a pure function (slots + surfaces → property writes) for unit testing without a live DRM fd.
- **`drm::csd::WindowAnim`** — value-type per-window animator holding focus + hover progress, eased ease-out-cubic. Shell calls `retarget_focus` / `retarget_hover` on state changes, then `tick(dt)` once per frame. Output mirrors into `WindowState::{focus_progress, hover_progress}`; sentinel `k_progress_unset = -1.0F` keeps callers without an animator visually identical.
- **`ShadowCache::blit_cross_fade(key_a, key_b, t)`** — tweens between cached shadow patches through real intermediate pixels rather than snapping; endpoints (`t == 0` or `1`) short-circuit to `blit_into` so the no-animation path costs the same as before. Per-row scratch buffer keeps allocation off the hot path.
- **Renderer cross-fade hooks** — `draw_glass` lerps the rim color and feeds the focus weight to the shadow cross-fade; `draw_button` takes a `hover_weight` in `[0, 1]` and lerps fill ⇄ hover so transitioning buttons read as the in-between tint.
- **`decoration_geometry(theme, w, h)`** — single source of truth for panel inset, title-bar height, and button center positions. Used by both the renderer's paint path and the mdi_demo shell's hit-tester so paint and click can't desync.
- **`PlaneRegistry::from_capabilities(std::vector<PlaneCapabilities>)`** — synthetic-source factory for unit tests and replay/snapshot tools that don't have a live device.

### `drm::cursor` — sprite sizing + atomic-path correctness

- **Per-rotation hardware-rotation probe** — i915's cursor advertises the `rotation` property but only enumerates `ROTATE_0` / `ROTATE_180`; the prior property-presence shortcut routed all four values to the kernel and EINVAL'd on 90/270. `probe_plane_properties()` now walks the property's enum table and builds `rotation_supported_mask`; unsupported angles transparently route through `blit_frame`'s software pre-rotation while supported ones still ride kernel scanout. `set_rotation()` re-blits across HW↔SW boundaries so a single Renderer can sit in HW for 0/180 and SW for 90/270 on the same i915 plane. `has_hardware_rotation()` reports the per-rotation answer ("is HW handling *this* angle?") instead of static property-presence.
- **Ping-pong dumb buffers on the atomic path** — amdgpu DC reads cursor pixels live each vblank, so an in-place `blit_frame` followed by a position commit visibly tears for ~1 vblank. Two ARGB8888 buffers; `blit_frame` writes the back buffer, `stage_position` emits its `FB_ID`, `commit_position` swaps the active index on success. `commit_buffer()` (the buffer the next commit will publish) drives `CRTC_W/H`, `SRC_W/H`, and the hotspot math without callers needing to know which slot is active. Legacy path stays single-buffered (rotation rejected; `drmModeSetCursor` handles uploads).
- **Oversize sprite handling + accepted-buffer probe** — `box_downscale_rotated` is an area-weighted integer-arithmetic downscaler composed through the rotation sampler; needed because libxcursor returns the theme's closest size, not necessarily ≤ requested (Adwaita ships 24/30/36/48/72/96, so a 64 request hands back 72). `probe_acceptable_size` walks `{256, 128, 64}` via `DRM_MODE_ATOMIC_TEST_ONLY` at create time and keeps the first that passes — `DRM_CAP_CURSOR_WIDTH` is an upper bound, not an enumerated set (i915 accepts only 64; amdgpu DC accepts 64/128/256). `last_blit_w/h` tracked across blits so `buffer_hotspot` scales the hotspot in lockstep with the pixel downscale.
- **System "default" theme bridge in `Theme::resolve`** — between the user's named theme chain and the discovery-order fallback, BFS the system "default" theme (typically `/usr/share/icons/default` symlinked to the active pack — DMZ-White, Yaru, etc.). Stops resolve from landing on whichever partial pack happened to sort first on disk.
- **0-size substitution** — `XcursorFilenameLoadImages(..., 0)` falls back to 24 px, well below the 64 px KMS cursor floor (i915 minimum; amdgpu DC accepts 64/128/256). Substitute 64 px when callers pass 0; explicit sizes pass through unchanged.

### Examples

- **`examples/mdi_demo/`** — multi-document desktop showcase exercising csd Tier 0 end-to-end: `PlanePresenter` scheduling, `OverlayReservation` plane coordination, glass theme on multiple decorated surfaces over a shared background. Each `Document` carries its own `WindowAnim`; the main loop ticks via `Shell::tick_animations(dt)` and pointer motion retargets hover even outside an active drag.
- **`examples/common/cursor_size.hpp`** — shared per-output sizing helper. Derives DPI from the connector's EDID `mmWidth` + active mode resolution, snaps to the conventional 96 / 192 / 288 ladder via `round(dpi / 96)`, and returns separate sprite + buffer sizes — sprite is free to land below 64 (Renderer centers it inside the buffer), buffer must be one of `{64, 128, 256}` for the kernel to accept it.
- **`examples/advanced/csd_smoke/`** gains `--presenter {scene|plane}`. `scene` (default) keeps the single-LayerScene flow as the smallest reproducer for renderer / surface bugs; `plane` drives bg through LayerScene and arms decoration through `OverlayReservation` + `PlanePresenter` via a second atomic commit. Also picks up GBM probe (production allocator when available, dumb fallback otherwise) and equals-form argv parsing (`--theme=lite`).
- **`examples/scene/camera/`** — fps/cameras status badge (320×56 ARGB8888 at zpos=64, bottom-right) repainted on each 1-second fps tick. New `status_overlay_renderer.{hpp,cpp}` TU paints translucent fill unconditionally and centered text via Blend2D when the umbrella gate is on; `CAMERA_STATUS_HAS_BLEND2D` is plumbed through CMake/Meson rather than `__has_include` because Fedora and Arch ship Blend2D headers in `/usr/include` even when not linked, and a header probe would silently pull `BL` paths into the TU and break the link. Camera layers move to `ContentType::Video` so the priority allocator keeps them on real overlays even when budget is one short — the badge (Generic, `update_hint_hz=1`) yields its plane to `CompositeCanvas` instead. VT-switch pause/resume hardened: scene CPU mappings + cached FB ids dropped via `on_session_paused()` before the kernel revokes the fd, and `resume_cb` filtered on `/dev/dri/` so libinput-fd resumes can't clobber the DRM card slot. Validated on amdgpu Granite Ridge by driving `loginctl activate` against a sibling tty.
- **`examples/scene/video_player/`** — minimal demo of `drm::scene::GstAppsinkSource`. Three pipeline shapes: default `videotestsrc` (works on any installed plugin set), `--file PATH` (filesrc → decodebin3, sw or hw decoders depending on what's installed), `--launch "<pipeline>"` (caller-supplied, must terminate in `appsink name=sink`). Pre-pulls the first decoded sample before the first commit — single-layer scene has no always-ready background, so a cold commit would arm no plane and the kernel would modeset the CRTC blank. EACCES from `scene->commit` is treated as a soft pause (drmIsMaster lags libseat's pause_cb); EAGAIN as a single-frame retry. Genuine errors set an `error_exit` flag so the process returns `EXIT_FAILURE` to systemd / shell scripts; EOS, signal, and user-quit stay `EXIT_SUCCESS`. Quit path is the libinput keyboard (Esc / Q / Ctrl+C) — when libseat puts the TTY in KD_GRAPHICS the kernel suppresses Ctrl-C signal delivery. Validated on amdgpu Granite Ridge (2026-05-06): videotestsrc pipeline, multiple Ctrl+Alt+F<n> cycles, no exit, no log output past the startup banner.
- **`examples/scene/camera/` — VAAPI / libyuv / NV12 zero-copy tiers** — three new converter tiers gated by build-time auto-detection of libva / libva-drm. `MjpegVaapiNv12`: VAAPI hardware MJPEG decode into a scanout-ready NV12 surface, exported as a dma-buf and wrapped in an `ExternalDmaBufSource`. Dual-mode 4:2:0 / 4:2:2 — VCN's hardware JPEG decoder requires the surface layout to match the source sampling, so a 4:2:2 MJPG (every UVC webcam in the test matrix) routes through a YUYV intermediate plus a VAAPI Video Processing pass into NV12; `configure_slot` speculates 4:2:2 first and rebuilds at the correct sampling on a 4:2:0 first frame. `ZeroCopy` (`LibcameraNv12Source`): one `drmModeAddFB2WithModifiers` FB per libcamera buffer fd, flipped between on `requestCompleted` — no per-frame churn, no CPU touch. The negotiator detects amdgpu via `drmGetVersion` and skips this tier upfront (amdgpu DC's foreign-dma-buf provenance check rejects every UVC import). `Nv12ToXrgb` / `MjpegToXrgb` (libyuv): camera-local `DoubleDumbSource` ping-pongs writes and scans across two dumb buffers so producers above 30 fps don't tear under amdgpu's live cursor pixel reads. Negotiator is plane-budget-aware — overflow cameras dynamically downgrade to the libyuv tier once canvas reservation engages. Per-camera VAAPI quirk list (Logitech C270 forced onto libyuv MJPG on radeonsi VCN). Process-wide VADisplay refcounted across slots since libva refuses a second `vaInitialize` on the same DRM fd. `--no-vaapi` escape hatch. Validated on amdgpu Granite Ridge with Logitech MX Brio + C920 + C270 across 1-, 2-, and 3-camera configurations.
- **`examples/advanced/multi_crtc_probe/`** + **`examples/common/multi_crtc_probe.hpp`** — `TEST_ONLY` + `ALLOW_MODESET` commit pairing every connected output with a small scratch dumb buffer on its primary plane; kernel acceptance is the signal that cross-CRTC synchronized changes can land tear-free off a single fd. `--hotplug` installs a `HotplugMonitor` and re-probes on each connector add/remove. `--scene-test` builds one `LayerScene` per connected output with a trivial layer and runs `SceneSet::test`; `--mirror` routes a 256×256 ARGB8888 buffer across every output via `SceneSet::add_layer`. Scratch FBs are mode-sized per output so the probe answers on vkms (whose primary plane has no scaler and rejects SRC != CRTC with ERANGE) without requiring plane-scaling support. The header-only helper is reusable from `dual_display`, `video_wall_multi`, and `cluster_sim`'s passenger-display path.
- **`examples/dual_display/`** — minimal multi-CRTC SceneSet demo. One full-screen tinted background per output (per-output specialization fans through `SceneSet`) plus one mirrored shared `DumbBufferSource` with a horizontal scan bar advancing one pixel per frame — visual proof that the combined commit is tear-free across CRTCs. Up-front combined-atomic feasibility probe so kernel rejection yields a useful diagnostic before any allocation lands. Per-output `flips_outstanding` counter; next frame defers until all CRTCs have landed. libseat session pause / resume fans out to each child `LayerScene`; resume_cb filtered on `/dev/dri/` to ignore the libinput multi-fire. EACCES on commit treated as the timely pause signal since drmIsMaster lags libseat. Validated on amdgpu Granite Ridge (HDMI-A-1 1680×1440 + DP-1 3440×1440); `scripts/vkms_dual.sh` provisions a zero-hardware path.
- **`examples/video_wall_multi/`** — N×N grid of synthesized "video" cells laid out across every connected output on one card as a single logical wall (wall width = sum of `hdisplay`, height = first output's `vdisplay`). Cells straddling an output boundary register two targets on a single shared `LayerBufferSource` via `SceneSet::add_layer` with sub-rect `src_rect` and per-output `dst_rect`, exercising the `SharedLayerBufferSource` forwarder against a real allocator + commit cycle (not just a `TEST_ONLY` probe). Stresses three primitives in one demo: cross-CRTC atomic commits, per-CRTC plane budget overflow + composition fallback, and multi-target `SceneSetLayerSpec`. Runtime layout switch (1 / 2 / 3 / n / p) walks add/remove churn across N scenes. `--order <left>,<right>,...` overrides the left-to-right physical arrangement since DRM enumeration order doesn't carry physical placement; `--list-outputs` enumerates connector names + modes for the follow-up `--order` invocation.
- **`examples/advanced/egl_scene/`** — end-to-end producer-side walkthrough for `GbmSurfaceSource`. Opens the output, builds the scene, queries `LayerScene::candidate_modifiers(ARGB8888)`, spins a probe EGL display over a transient `gbm_device` to call `eglQueryDmaBufModifiersEXT`, intersects, picks a modifier, then builds the real `GbmSurfaceSource` with that single modifier. `eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR)` targets the source's own `native_device()` so the EGL binding and the source share one `gbm_device` instance (different `gbm_device` instances against the same DRM fd don't accept each other's surfaces). GLES 3 clear-color hue-cycle loop, `eglSwapBuffers` then scene commit; a background dumb buffer keeps PRIMARY armed for the initial modeset.
- **`examples/advanced/vulkan_scene/`** — Vulkan counterpart, deliberately not using `GbmSurfaceSource`. `gbm_surface` is EGL-only; Vulkan has no equivalent GBM-surface extension, so the architecturally honest "Vulkan renders a scene layer" path is `VK_EXT_image_drm_format_modifier` + `VK_EXT_external_memory_dma_buf` to export a `VkImage` as DMA-BUF, wrapped in an `ExternalDmaBufSource`. Demo allocates an ARGB8888 LINEAR image at the output resolution, exports via `vkGetMemoryFdKHR`, wraps with `ExternalDmaBufSource`, adds as a scene layer, and runs a `vkCmdClearColorImage` hue-cycle loop with `vkQueueWaitIdle` between commits. The file header + README open with the rationale so users hitting the directory expecting gbm-surface integration find the explanation up front.
- **`examples/v4l2_camera_demo/`** — minimal one-source / one-layer / one-CRTC demo of `V4l2CameraSource`. Probes `/dev/video*` for NV12 (preferred) or YUYV. Checks the target DRM device's plane `IN_FORMATS` up front and bails with a clear message when no plane advertises the camera's native format — amdgpu DC's NV12-only-among-YUVs whitelist is the common case, and surfacing that as a refusal-to-start beats a bare EINVAL from inside `V4l2CameraSource::create`. Aspect ratio preserved against the framebuffer via a centered fit rather than stretched. Wired through the shared `open_output` / `session_pump` / `event_loop` helpers.
- **`examples/advanced/stream_demo/`** — EGL Streams end-to-end against real NVIDIA hardware. Opens `/dev/dri/card0` (argv-selectable), probes capability, builds a full-screen background `DumbBufferSource` plus a 640×360 stream layer centered on it, runs the full protocol (probe → `EglStreamBuilder` with master fd + deferred producer surface → `add_layer` → `ensure_stream_layer_pins` → consumer attach → first commit → empirical mixing probe). On Tegra (`EGL_NV_output_drm_atomic` present): make-current + GLES 3 render loop into the producer surface. On desktop NVIDIA 535 (extension absent): CPU-renders a hue cycle into the background and commits per frame; the stream consumer plane stays wired but inert (producer frames don't reach KMS without the Tegra acquire path) and a stderr message points at `docs/streams.md`. Gated on `DRM_CXX_HAS_EGL_STREAMS` and `glesv2.pc`; links real EGL + GLES (the library itself never does).
- **`examples/advanced/csd_smoke/`** gains `--presenter {scene|plane}`. `scene` (default) keeps the single-LayerScene flow as the smallest reproducer for renderer / surface bugs; `plane` drives bg through LayerScene and arms decoration through `OverlayReservation` + `PlanePresenter` via a second atomic commit. Also picks up GBM probe (production allocator when available, dumb fallback otherwise) and equals-form argv parsing (`--theme=lite`).

### Documentation

- **`docs/multi_output.md`** — design rationale behind `drm::scene::SceneSet`: why a multi-CRTC abstraction exists at all, how layers route to scenes, what `add_layer` / `remove_layer` / `add_scene` / `remove_scene` mean, and where the combined cross-CRTC commit fits relative to per-scene commits. Mirrors the shape of `docs/scene.md` so the two read as a pair.
- **`docs/hardware.md`** — canonical "what runs where". Validated GPUs table (amdgpu RDNA1 / RDNA2, i915, vkms, vicodec, NVIDIA Quadro RTX A2000, Tegra) with kernels and the examples exercised on each. Test-rig setup (bare TTY, libseat / seatd group, the `vkms_dual.sh` fallback for single-output workstations). Multi-output testing recipe (`multi_crtc_probe` scene-test, `dual_display`, `video_wall_multi --list-outputs` / `--order`). Driver-quirks index covering amdgpu DC (foreign-source PRIME, LINEAR pitch alignment, primary armed during `TEST_ONLY`, live cursor reads, HDR + Colorspace modeset requirement, cursor size floors, RDNA P010 / P012 / P016 scanout, primary zpos pin, OVERLAY reconfig flicker), i915 cursor floor, vkms primary-scaler absence + CMS / HDR property surface + configfs version floor, vicodec endpoint discrimination, NVIDIA EGL Streams desktop gap + producer-surface ordering + empirical mixing probe, VAAPI MJPEG chroma matching, and general DRM atomic gotchas. Diagnostic-tools table + probe workflow recipe; reference-examples table; "adding a new card" workflow.
- **`docs/streams.md`** — user-facing EGL Streams reference: when to choose streams vs GBM, build / runtime requirements (libEGL.so.1 + libglvnd + `EGLDeviceEXT` / DRM-rdev match), quick-start walking probe → builder → `add_layer` → commit → teardown with the deferred-producer-surface contract called out, scene-side lifecycle (`add_layer` gating, `ensure_stream_layer_pins`, allocator exclusion, the FB_ID-less commit shape, session pause / resume / rebind semantics), empirical mixing probe contract + sticky verdict caching, page-flip events on stream planes, verified-driver table plus the desktop NVIDIA 535 scanout gap.

### Fixes

- **Allocator surfaces EACCES from test commits** — `try_test_commit` returned `bool`, so `apply_previous_allocation` flattened every TEST_ONLY failure to EAGAIN. Callers that gate on EACCES to soft-pause never saw it; non-trivial pipelines exited on the first frame after master revocation. `try_test_commit` now returns `std::error_code`; `apply_previous_allocation` propagates EACCES verbatim and keeps EAGAIN flattening for every other failure. `Allocator::apply` short-circuits on EACCES from the fast path and the warm-start path so callers see the master-loss signal instead of a pile of doomed test commits and a composition pass that would also EACCES. The fast path also gains the warm-start's recovery semantic: non-EACCES test failure falls through to full_search rather than returning EAGAIN as if no other plane assignment could possibly fit.

### Hardware validation

- `examples/camera/` validated on i915 (UVC + i915 laptop, 2026-05-05). Exercises the per-frame `arm_layer_plane_color_props` path (`COLOR_ENCODING` / `COLOR_RANGE` on overlay planes) that amdgpu DC OVERLAYs don't expose. RPi5 (vc4) and RK3588 (rkisp1) remain untested.
- Camera example VAAPI / libyuv / NV12 zero-copy tiers validated on amdgpu Granite Ridge (kernel 6.19, mesa 25.3.6) with Logitech MX Brio + C920 + C270 across 1-, 2-, and 3-camera configurations. Each camera lands on its theoretically optimal path; 3-camera mode dynamically downgrades VAAPI slots to libyuv MJPG once canvas reservation engages.
- `drm::scene::SceneSet` validated on amdgpu Granite Ridge (HDMI-A-1 1680×1440 + DP-1 3440×1440, kernel 6.19.14, 2026-05-13). `multi_crtc_probe` combined-atomic Accepted; `dual_display` steady-state with the synchronized scan bar across both outputs; `video_wall_multi` 2×2 / 3×3 / 4×4 layout cycling clean; VT-switch round-trip pause / resume clean. `scripts/vkms_dual.sh`-provisioned card2 (2× Virtual 1024×768) exercises the spanning-grid path on a synthetic driver.
- EGL Streams pipeline validated on NVIDIA Quadro RTX A2000 (NVRM 535.288.01, 2026-05-13). Modeset OK, first commit OK, stream layer pinned to OVERLAY plane 43, empirical mixing probe upgrades cached `StreamMixingMode` to `Mixed`. The 640×360 stream region is not separately visible — desktop NVIDIA 535's `EGL_NV_output_drm_atomic` absence is documented in `docs/streams.md`, not a library bug. Tegra acquire path wired but not validated (no Tegra board in test rig).
- `V4l2CameraSource` MMAP path validated against `/dev/video*` + vkms; UVC zero-copy DMABUF path pending an UVC-providing test rig with a non-amdgpu target.

### Scripts

- **`scripts/build-deps.sh`** — from-source builder for the four CI-tracked dependencies (libcamera, blend2d + sibling asmjit clone, libseat/seatd, libyuv). Default install prefix `/usr/local`; `PREFIX`, `WORKDIR`, `JOBS`, `SUDO`, and per-project `*_REF` env vars cover the variations a developer or runner is likely to want. No-arg builds all four; otherwise builds the listed subset. Library-appropriate meson / cmake config per project; one `ldconfig` refresh at the end.
- **`scripts/vkms_dual.sh`** — provisions a dual-connector vkms instance via the >= 6.11 vkms configfs interface (two CRTCs, two encoders, two connectors, two PRIMARY planes), then writes `enabled=1`. `up` / `down` / `status`. Lets the multi-CRTC integration tests and `multi_crtc_probe` / `dual_display` / `video_wall_multi` run on hosts without two physical outputs.

### Tests

- `tests/unit/test_csd_animator.cpp` — target / start / tick / apply_to coverage.
- `decoration_geometry` tests in `test_csd_renderer.cpp` — panel inset, button ordering, undersized clamp, paint↔geometry agreement at the close-button center.
- `OverlayReservation` tests — partitioned (3-overlay-per-CRTC) and shared (6-overlay-pool) hardware shapes, shortfall + try-again, hotplug release/reattach, idempotent release, re-reserve replacement, zpos-floor filter, format-mismatch culling, zpos-less plane skip, span stability, `count == 0` trivial path.
- `PlanePresenter::compute_writes` tests — error paths (`no_buffer_space`, empty slots), disarm path (closed surface emits `FB_ID=0` + `CRTC_ID=0` and skips geometry), armed path (every required write present, SRC dimensions in 16.16, CRTC geometry matches `SurfaceRef`), optional-property gating (blend + alpha emitted only when slot prop ids are non-zero).
- `tests/integration/test_gst_appsink_source_vkms.cpp` — sysmem-memcpy import end-to-end against VKMS, EOS surfacing, mid-stream caps change. DMABUF zero-copy hardware-validated separately (CI has no hw decoder).
- `tests/unit/test_gst_appsink_source.cpp` — argument validation; `function_not_supported` branch when GStreamer is off.
- `tests/unit/test_scene_set.cpp` — empty-set construction + commit / test / `scene(index)` bounds, `add_layer` validation gates (null source, empty targets, out-of-range index, hole-index rejection, stale-handle no-op), `AddScene.RejectsNull`, `RemoveScene.OutOfRangeIsNoOp`, `Commit.AllNullScenesSkipsKernelCommit`, and `SceneSetPartition.*` covering Combined / AutoOnModeset / PerCrtc over empty / all-hole / uniform-steady / uniform-modeset / mixed inputs.
- `tests/integration/test_scene_set_vkms.cpp` — `CombinedTestAcceptsMultiCrtc`, `MirroredLayerAcceptsAcrossMultiCrtc`, `AddRemoveSceneRoundTrip`, `AutoOnModesetSplitsThenReconverges`, `PerCrtcAcceptsTwoEngagedScenes`, `AutoOnModesetSplitsOnUserSetHdr` against a `vkms_dual.sh`-provisioned dual-connector vkms instance; self-skips when no qualifying vkms instance is present.
- `tests/unit/test_v4l2_camera_source.cpp` — argument-validation contract (null / empty device path, zero fourcc, zero dimensions, out-of-range buffer counts, MJPEG / planar fourcc rejection) plus open + `QUERYCAP` failure paths against `/dev/null` and a guaranteed-ENOENT path.
- `tests/integration/test_v4l2_camera_source_vkms.cpp` — MMAP path end-to-end and Auto-mode resolution against any `/dev/video*` + vkms; self-skips on no CAPTURE device, no vkms, or when vkms rejects the FB at the source's format (the documented YUV scanout-FB gap on vkms).
- `tests/unit/test_egl_stream_builder.cpp` — capability / dimension / null-device early-return guards (`function_not_supported`, `invalid_argument`).
- `tests/unit/test_egl_stream_source.cpp` — capability / display / config / dimension guards plus the post-probe-but-no-streams-chain case; sentinel handles use function-pointer addresses (the invalid-argument guard fires before any real EGL call would touch them).
- `tests/integration/test_egl_streams_hw.cpp` — hardware-gated coverage skipping on Mesa-only hosts: probe contract on a real device, builder result fields, builder rejection paths against a real builder, and end-to-end `SceneCommitWiresUpStreamLayerEndToEnd` exercising `bind_to_plane` + producer surface + first commit + empirical mixing probe upgrade (Mixed on NVRM 535.288.01).

## v1.3.0 — 2026-05-04: CSD module + keyboard repeat / LED state / keymap reload

### `drm::csd` — client-side decorations (gated on Blend2D, `-Dblend2d=enabled` / `DRM_CXX_HAS_BLEND2D`)
- **`drm::csd::Theme`** — toml++-loaded theme schema. `Color` POD (r/g/b/a), `Theme` carries panel gradient stops, specular, noise amplitude, traffic-light fills + hover variants, shadow color, rim color, title font hints. Built-in themes: `default`, `lite`, `minimal`.
- **`drm::csd::Surface`** — one CPU-mappable, KMS-scanout-ready ARGB8888 buffer + framebuffer ID per managed decoration. `Surface::create` tries GBM first (LINEAR ARGB8888 with SCANOUT+WRITE, modifier `DRM_FORMAT_MOD_LINEAR` pinned) and falls back to dumb on GBM unavailability or allocation failure; chosen path recorded on `backing()` (`SurfaceBacking::Gbm` / `SurfaceBacking::Dumb`). A second overload takes just a `Device` for the dumb-only path used by headless tests.
- **`drm::csd::WindowState`** — POD the shell hands the renderer per paint pass: title, focused flag, `HoverButton`, dirty bitfield reserved for partial-redraw. Header-only, Blend2D-free.
- **`drm::csd::ShadowCache`** — LRU of pre-blurred decoration shadows keyed on `(width, height, Elevation, theme_id)`. Single-channel alpha mask of the rounded-rect panel, three-pass separable box blur, PRGB32 tinting by the theme's shadow color. Composites via SRC_OVER through the `ShadowDest` interface; intentionally Blend2D-free.
- **`drm::csd::Renderer`** — paints one decoration into a `Surface` per call. Glass theme: soft shadow halo from `ShadowCache` (Option C — pre-blurred patch from the alpha margin, SRC_OVER underneath the panel), vertical linear-gradient panel, specular highlight via `BL_COMP_OP_SCREEN` clipped to the top edge, frosted noise tile (deterministic 64×64 LCG, MULTIPLY at the theme's noise amplitude), title text (2-pass shadow, skipped when no font face loads), three traffic-light buttons with radial-gradient fills + per-button hover variant, 1-px inner-stroke rim (focused vs blurred color). `RendererConfig` carries the theme + font face + content rect.

### `drm::input` — keyboard repeat, LED state, keymap reload
- **`drm::input::KeyRepeater`** — timerfd-driven auto-repeat synthesis for held keys. Per-key eligibility from `xkb_keymap_key_repeats` (modifiers + lock keys do not repeat). `sym` / `utf8` re-resolved on every tick against current xkb state, so Shift / AltGr level switches during a hold take effect on the next repeat. Defaults: 600 ms initial delay, 25 Hz interval. `fd()` for poll/epoll integration, `dispatch()` to drain expirations and emit one event per tick. `cancel()` to drop in-flight repeat across session pause.
- **`KeyboardEvent::repeat`** — flag distinguishing synthesized events from real ones.
- **`Keyboard::should_repeat`, `caps_lock()` / `num_lock()` / `scroll_lock()` accessors, `KeyboardLeds` snapshot struct, `leds_state()`.**
- **`Keyboard::create_from_string(buffer)`** — wraps `xkb_keymap_new_from_buffer` for Wayland-style mmap'd keymap fds; buffer copied internally; explicit-length form handles non-NUL-terminated `string_view` safely.
- **`Keyboard::reload(KeymapOptions)`** — rebuilds `xkb_keymap` + `xkb_state` in place from new RMLVO names. Strong-exception: a malformed RMLVO leaves the existing keymap intact. Held-key state preserved (replays `XKB_KEY_DOWN` for each tracked evdev keycode so a still-pressed Shift / Ctrl / letter survives the swap and a subsequent release transitions cleanly). Lock latch preserved (snapshots `leds_state()` before the swap, restores via `set_leds()` after the held-key replay). Caller is expected to push the latch out via `seat.update_keyboard_leds(kb.leds_state())` after success.
- **`Keyboard::set_leds(KeyboardLeds)`** — synthesizes press+release for each lock key whose desired state differs from the current xkb-tracked state. Used internally by `reload` and externally by callers honoring an externally-provided lock-state hint (e.g. logind "Caps Lock was on at session start"). Documented limitation: Scroll Lock silently no-ops on layouts whose compat doesn't mod-map `<SCLK>` (xkb's default complete compat).
- **`input::Seat::update_keyboard_leds`** — pushes the xkb-tracked lock state back to the kernel so the physical LEDs follow Caps / Num / Scroll Lock. Tracks keyboard-capable libinput devices internally; the last applied state is re-pushed to any newly-added device so VT-resume and hotplug do not leave LEDs lagging.

### Examples
- **`examples/basics/keyboard/`** — Blend2D-rendered keyboard demo: text entry with auto-repeat + IBus/GTK Ctrl+Shift+u Unicode-codepoint sequence. Downloads Noto Sans at configure time; gated off by default behind `-Dkeyboard` / `-DDRM_CXX_BUILD_KEYBOARD`; hard-requires Blend2D when enabled.
- **`examples/advanced/csd_smoke/`** — throwaway hardware-validation harness for `csd::Renderer`. Paints one glass decoration into a `csd::Surface` and arms it on an overlay plane via `LayerScene`. Validates the `(format, modifier, zpos)` story end-to-end and exposes `--seconds N` / `--theme {default|lite|minimal}` / `--png OUT.png` (round-trip via `drm::capture::snapshot` for headless regression checks). Gated on `DRM_CXX_HAS_BLEND2D`.
- **`examples/scene/layered_demo`** — `KeyRepeater` wired into the poll loop so arrow-key nudging and the `[` / `]` alpha controls auto-repeat. Real and synthesized events share the action handler; `Keyboard::process_key` always runs on the raw event so xkb modifier state stays current for the repeater's re-resolution. `repeater.cancel()` on session pause.

### Fixes
- **`signage_player`** — `overlay_renderer.cpp` Blend2D paths now gate on `DRM_CXX_HAS_BLEND2D` instead of `__has_include(<blend2d/...>)`, so a `-Dblend2d=disabled` configure actually disables the Blend2D path on distros (Fedora, Arch) that always ship `blend2d-devel` under `/usr/include`.

### Tests
- `tests/unit/test_csd_theme.cpp`, `test_csd_surface.cpp`, `test_csd_shadow_cache.cpp`, `test_csd_renderer.cpp`.
- `tests/unit/test_key_repeater.cpp` — config validation, release-disarms-the-timer invariant, repeat-eligibility filtering, synthesized-event ignore path.
- `tests/unit/test_input.cpp` gains Caps Lock latch + LED snapshot coverage; round-trip `create_from_string` against a serialized "us" RMLVO keymap; bogus buffer fails; `set_leds` drives caps + num lock latches up and down idempotently; `reload` preserves a held Shift across the swap (level switch survives, release after reload still transitions); `reload` preserves the Caps Lock latch; `reload` with bogus RMLVO leaves the existing "us" keymap working.

## v1.2.0 — 2026-05-03: Scene API + example tree

### `drm::scene` — high-level layer scene
- **`drm::scene::LayerScene`** — declarative layer API above `planes::Allocator::apply`. `add_layer` / `remove_layer` / `set_dst_rect` / `set_src_rect` / `set_zpos` / `set_alpha` / `set_source` mutate state; `commit()` runs the allocator, builds the `AtomicRequest`, and returns a `CommitReport` with `layers_assigned` / `layers_composited` / `layers_unassigned` / `properties_written` / `fbs_attached` / `test_commits`.
- **Property minimization** — per-plane snapshot diffing skips redundant property writes; `FB_ID` always re-emits (page-flip protocol). `force_full_property_writes` opt-out for debugging.
- **Composition fallback** — `CompositeCanvas` (double-buffered ARGB8888 surface, ping-pong via `begin_frame()`); `compose_unassigned()` blends layers that did not reach a hardware plane and arms the canvas onto a free plane. `LayerDesc::force_composited` knob; canvas plane pre-reservation when `layer_count() > eligible_canvas_planes`.
- **`LayerScene::rebind(crtc, connector, mode)`** — explicit teardown + re-enumerate + rebuild; layer handles + sources survive. `CompatibilityReport` flags off-screen layers.
- **VT-switch lifecycle** — `on_session_paused()` / `on_session_resumed()` tear down + restore buffer mappings; pairs with `drm::session::Seat`.
- **Per-layer placement readout** — `Layer::assigned_plane_id()` exposes which hardware plane the allocator landed each layer on.
- **Polymorphic buffer sources** — `LayerBufferSource` abstract base + `AcquiredBuffer { fb_id, acquire_fence_fd, opaque }`. `cpu_mapping()` returns `nullopt` for tiled / non-LINEAR sources.
  - `DumbBufferSource` — scene-allocated 32bpp dumb buffer.
  - `ExternalDmaBufSource` — caller-owned DMA-BUF fds with `(format, modifier, plane[])` metadata; single-plane LINEAR + multi-plane (NV12, YUV420). `on_release()` callback fires after scanout completes.

### `drm::cursor` — hardware cursor with software fallback
- XCursor theme resolver + KMS cursor renderer with runtime rotation, `HOTSPOT_X` / `HOTSPOT_Y` virtualization, hardware-validated rotation harness.

### `drm::session::Seat` — session manager glue (gated by `DRM_CXX_SESSION`)
- libseat-backed logind / seatd / builtin mux. `enable_seat` / `disable_seat` / `switch_session`. `InputDeviceOpener` lets `input::Seat` route privileged opens through libseat.

### `drm::display::HotplugMonitor`
- Connector hotplug event stream over `udev`. `fd()` for poll/epoll integration, `dispatch()` to drain.

### `drm::capture` — Blend2D-backed CRTC snapshot
- Per-plane composition snapshot of an active CRTC, PNG encode via Blend2D. Companion `capture_demo` example, VKMS integration-test harness.

### Allocator improvements
- **Format-modifier-aware bipartite matching** — `IN_FORMATS` modifier list considered in plane eligibility; `LayerDesc::modifier` field.
- **Priority eviction** — `ContentType::Video` = 100, `update_hint_hz > 30` = 80, `update_hint_hz > 0` = 50, default = 10. Eviction is priority-driven.
- **Warm-start path** — `apply_previous_allocation` re-validates with one `TEST_ONLY`, producing `test_commits=0` (after the validating one) in steady state.
- **Two-tier placement** — per-group spatial placement, then a scene-wide partial fallback (drop most-constrained, retry) when total_assigned == 0.

### Plane registry
- `ColorEncoding` (`BT_601` / `BT_709` / `BT_2020`) + `ColorRange` (`Limited` / `Full`) enums.
- `PlaneCapabilities::has_color_encoding` / `has_color_range` plus cached enum integers.
- `DisplayParams::color_encoding` / `color_range` per-frame overrides; `LayerScene::arm_layer_plane_color_props` arms them on planes that expose the props.

### `drm::PageFlip`
- `add_source(fd, callback)` — register foreign fds (libcamera `eventfd`, `signalfd`, etc.) on the same epoll loop the page-flip dispatcher uses.

### `drm::Device`
- `Device::from_fd(int)` — wrap a caller-owned fd (e.g. one handed back by `libseat_open_device`).

### `drm::input::Seat`
- `InputDeviceOpener { open, close }` — caller-supplied open/close callbacks routed through libseat for `/dev/input/event*` opens. Per-fd cap re-enable on resume.

### Examples
- Bucketed tree: `examples/{basics,scene,allocator,advanced}/`.
- New: `signage_player`, `hotplug_monitor`, `cursor_rotate`, `capture_demo`, `video_grid`, `layered_demo`, `scene_warm_start`, `scene_priority`, `scene_formats`, `test_patterns`, `camera`, `thorvg_janitor`.
- Rewritten: `atomic_modeset` on `LayerScene`, `mouse_cursor` on `drm::cursor`.
- Shared helpers: `examples/common/open_output.hpp` (`open_device` + `open_and_pick_output` factor the libseat fd-open + first-connected-connector pickup), `select_connector.hpp` (`pick_connector` with `k_main_rank` / `k_internal_rank` / `k_external_rank`), `select_device.hpp`, `vt_switch.hpp` (Ctrl+Alt+F<n> chord), `format_probe.hpp`.

### Benchmarks (gated by `DRM_CXX_BUILD_BENCHMARKS=ON` / `-Dbenchmarks=true`)
- `plane_stress` — synthetic LayerScene workload; `--layers / --formats / --size / --churn / --churn-rate / --duration / --csv / --quiet` with per-frame CSV output.
- `allocator_torture` — six adversarial cases (N+1, format cascade, scaler monopoly, rapid churn, slow drift, burst-then-calm); PASS/FAIL/SKIP exit codes.

### Documentation
- `README.md` rewritten around `LayerScene` as the headline feature.
- `docs/scene.md` — design rationale, buffer-source model, extension points (EGL Streams, foreign DMA-BUF, multi-CRTC, animation), out-of-charter items.
- Per-example `README.md` files across the bucketed tree.
- Doxygen briefs filled in across the public scene headers.

### Build + CI
- thorvg 1.0.4, Blend2D, and libcamera v0.5.2 built from source in CI; cached.
- libseat-dev installed from apt.
- Weekly `drmdb` compat CI.
- VKMS integration-test pattern (`tests/integration/test_*_vkms.cpp` with `GTEST_SKIP` self-skip when VKMS isn't loaded).

## v1.1.0 — C++17 migration

- **Project language target lowered from C++23 to C++17.** The library
  still picks up `std::expected`, `std::span`, and
  `std::print` when the toolchain has them; otherwise the `drm::expected`,
  `drm::span`, `drm::print`, and `drm::format` adapter headers transparently
  fall back to `tl::expected`, `tcb::span`, and `{fmt}`.
- Source tree `drm-cxx/` renamed to `src/`. Public `<drm-cxx/...>` include
  layout is unchanged for consumers — served at build time via a `drm-cxx`
  symlink into `src/` in the build tree and at install time via the normal
  `${includedir}/drm-cxx` install layout. Polyfill headers are vendored
  under `${includedir}/drm-cxx/vendor` so downstream consumers don't need
  `tl-expected` / `tcb-span` installed separately.
- `std::move_only_function` replaced with `std::function` in `Seat`,
  `EventDispatcher`, and `PageFlip` handler types. Existing call sites and
  lambdas continue to work unchanged (all were copy-constructible).
- `std::erase_if`, `std::string::starts_with`, and `Container::contains`
  call sites rewritten with their C++17 equivalents; transparent
  heterogeneous `unordered_map::find(string_view)` replaced with a
  `std::string` materialization at the single call site that used it.

## v1.0.0

Initial release of drm-cxx, a C++23 native re-implementation of drmpp.

### Core
- `drm::Device` — DRM device fd RAII with capability enables
- `drm::Resources` / `drm::Connector` / `drm::Encoder` / `drm::CrtcPtr` — RAII wrappers for DRM mode objects
- `drm::PropertyStore` — KMS property ID cache with `drmModeObjectGetProperties`
- `drm::core::format_name()` / `format_bpp()` — DRM format helpers

### Modeset
- `drm::AtomicRequest` — atomic commit builder with `drmModeAtomicAlloc` RAII
- `drm::ModeInfo` — mode selection: preferred, resolution match, refresh targeting
- `drm::PageFlip` — vblank event loop with epoll + `drmHandleEvent` v3

### Plane Allocator (replaces libliftoff)
- `drm::planes::PlaneRegistry` — hardware plane enumeration with capability detection
- `drm::planes::Layer` — virtual layer with dirty tracking, content hints, geometry
- `drm::planes::Output` — per-CRTC output with layer management and zpos sorting
- `drm::planes::Allocator` — constraint-solving allocator with 7 improvements:
  1. Static compatibility matrix pruning
  2. Best-first search order
  3. Warm-start from previous frame
  4. Test-commit failure memoization
  5. Hopcroft-Karp bipartite pre-solve
  6. Content-type layer priority
  7. Spatial intersection splitting
- `drm::planes::BipartiteMatching` — standalone Hopcroft-Karp implementation

### Input
- `drm::input::Seat` — libinput + udev RAII with typed event dispatch
- `drm::input::Keyboard` — xkbcommon RAII with RMLVO and file-based keymap loading
- `drm::input::Pointer` — motion accumulator and button state tracker
- `drm::input::EventDispatcher` — multi-handler fan-out
- Rich event types: `KeyboardEvent`, `PointerEvent` (motion/button/axis), `TouchEvent`, `SwitchEvent`

### Display
- `drm::display::parse_edid()` — libdisplay-info EDID parsing with colorimetry, HDR, EOTF extraction
- `drm::display::ConnectorInfo` / `ColorimetryInfo` / `HdrStaticMetadata`

### GBM
- `drm::gbm::GbmDevice` — `gbm_create_device` RAII
- `drm::gbm::Surface` — `gbm_surface_create` with front buffer locking
- `drm::gbm::Buffer` — `gbm_bo` accessor with smart release (surface-aware)

### Sync
- `drm::sync::SyncFence` — native sync via `linux/sync_file.h` (replaces libsync)

### Vulkan (optional)
- `drm::vulkan::Display` — VK_KHR_display enumeration with dynamic Vulkan loading
- `drm::vulkan::DrmSurface` — surface handle placeholder

### Infrastructure
- `drm::print` / `std::print` logging with `drm::LogLevel` runtime gating
- pkg-config generation
- GTest unit test suite (13 suites, 100+ tests)
- CI: GCC-13/14, Clang-16/17 matrix
- Examples: `atomic_modeset`, `overlay_planes`, `vulkan_display`

### Breaking changes from drmpp
- Namespace: `drmpp::` -> `drm::`
- All returns use `drm::expected<T, std::error_code>` (aliases `std::expected` on C++23, `tl::expected` on C++17)
- No libliftoff, bsdrm, libsync, spdlog, or rapidjson dependencies
- Callbacks use `std::function<>` handlers instead of virtual dispatch
- Header paths: `#include <drm-cxx/...>` canonical layout
