# `drm::scene::SceneSet` — multi-CRTC orchestration

This document covers the *why* behind `drm::scene::SceneSet`: the
multi-CRTC coordinator that turns N `LayerScene` instances into one
atomically-committed multi-display surface. Read [`docs/scene.md`](scene.md)
first for `LayerScene`'s contract — `SceneSet` builds on it without
changing its semantics, and many of the same out-of-charter rules
apply here as well.

## Why a multi-CRTC abstraction at all

KMS already lets one `drmModeAtomicCommit` carry property writes for
multiple CRTCs in a single ioctl. The cost of that flexibility is the
plumbing: the application has to assemble the per-CRTC planes,
property ids, modeset blobs, and (under composition fallback) per-CRTC
canvas allocations into one `drm::AtomicRequest`, then thread the
post-commit reconciliation back to every per-CRTC state machine.
Doing that by hand against `LayerScene::commit()` either:

- **Commits each scene independently.** Two ioctls, two vblank
  acknowledgments, and zero cross-CRTC synchronization — the layer
  that should appear "on both displays simultaneously" appears one
  vblank apart, and steady-state PAGE_FLIP_EVENT routing fragments.
- **Reimplements the two-phase commit primitive.** Working around
  the fact that `LayerScene::commit()` builds its own `AtomicRequest`
  internally, by reaching into private state to combine them. That's
  what `SceneSet` does — once, in the library, behind a documented
  contract.

`SceneSet` owns a fixed (mutable) list of `LayerScene` instances,
exposes `commit()` and `test()` that fan out across every child, and
batches the resulting property writes into one combined atomic
commit. The library's tear-free synchronization guarantee survives:
if the kernel accepts the commit, every CRTC's slice took effect on
the same vblank.

## Layer placement model

`SceneSet` does not introduce a new layer abstraction. Layers still
live on individual `LayerScene` instances:

- **Per-scene layers** are added by reaching the child directly:
  `set.scene(i)->add_layer(LayerDesc{...})`. This is the canonical
  pattern for content that exists on exactly one output (per-output
  HUDs, screen-specific dashboards, the bottom-half of an instrument
  cluster). The shared `SceneSet::commit()` still batches its writes
  into the cross-CRTC commit.
- **Cross-scene layers** are added via `SceneSet::add_layer(spec)`.
  The supplied `SceneSetLayerSpec` carries one `shared_ptr<LayerBufferSource>`
  plus a vector of per-scene `{scene_index, DisplayParams, force_composited}`
  targets. The set wraps the shared source in an internal per-target
  forwarder so each child's `LayerScene::add_layer` receives its own
  `LayerBufferSource` pointer; the underlying source stays alive for
  the lifetime of every per-scene `LayerHandle`. This is the
  mirroring path (one logical layer presented on every output) and
  the per-output-specialization path (the same source displayed at
  different rects per output — a sidebar HUD on the right of display
  A and on the bottom of display B).

The two paths coexist on the same `SceneSet`. The combined commit
sees both equally.

### Shared-source constraints

A shared `LayerBufferSource` rides N scenes through N forwarders. The
underlying source's vtable is reached once per participating scene
per frame:

- `acquire()` / `release()` pass through verbatim. **Static-buffer
  sources** (`DumbBufferSource`, `ExternalDmaBufSource`) handle the
  repeated acquire / release pairs naturally — they hand back the
  same buffer every time. **Per-frame ring sources** (`V4l2DecoderSource`,
  `GstAppsinkSource`) do not — calling `acquire()` twice per frame
  silently drops one decoded frame.
- `on_session_resumed()` fires once per participating scene during a
  VT-switch resume cycle. Resumable static-buffer sources tolerate N
  back-to-back re-allocations; GBM-backed sources have not been
  audited for this pattern.

If you need a per-frame source mirrored across CRTCs, the right
pattern today is to add the source to one scene as a regular per-scene
layer, hand the most recent acquired buffer to a `DumbBufferSource`
or `ExternalDmaBufSource` per other CRTC, and add those as the
mirrored targets. The library may grow first-class per-frame mirroring
later if a real workload demands it.

## Commit semantics

`SceneSet::commit()` is the headline contract. Its lifecycle:

1. **Allocate one `drm::AtomicRequest`** bound to the device the
   `SceneSet` was created with.
2. **Build pass.** For each child scene in construction order, call
   `LayerScene::build_frame_into(req, caller_flags, test_only)`. Each
   call appends that scene's property writes to `req` and returns a
   `FrameBuildPtr` (or null when the scene is suspended). The set
   OR-combines `effective_flags_of(*state)` across every engaged
   scene's state so any scene that needs `ALLOW_MODESET` promotes the
   combined flags.
3. **Kernel commit.** One `drmModeAtomicCommit` (test or real)
   against the shared `req`.
4. **Finalize pass.** For each scene that produced an engaged state,
   call `LayerScene::finalize_frame(state, kernel_result)` so the
   scene's mark-clean, recorded-placement, FB-release, and
   suspended-on-EACCES paths run. The returned `CommitReport` lands
   at the scene's index in the output vector. Suspended scenes and
   slots left holes by `remove_scene` contribute a default-constructed
   (zero) `CommitReport`.

### Per-CRTC fallback (narrow)

The combined kernel commit is **skipped entirely** when no scene
contributed engaged state for the frame — every child is suspended,
every slot is a hole left by `remove_scene`, or some combination of
the two. This matters during a VT-switch: the seat revokes commit
privileges before libseat delivers `pause_cb`, and a multi-scene set
would otherwise issue an empty atomic commit on every frame for the
duration of the suspension. The narrow fallback returns per-scene
zero reports without touching the kernel.

The library deliberately does **not** split the combined commit when
some-but-not-all scenes are engaged. A single-engaged combined commit
costs the same one ioctl as the routed per-CRTC equivalent. Splitting
to address modeset cost asymmetry (one scene needs `ALLOW_MODESET`,
the others don't) would break the cross-CRTC sync guarantee that is
the whole point of `SceneSet`; for that case the recommended pattern
is the modeset warmup below.

### Modeset warmup pattern

`LayerScene::effective_flags_of(state)` returns `DRM_MODE_ATOMIC_ALLOW_MODESET`
on a scene's first commit after `create()` / `rebind()` / a
colorspace change / an HDR metadata transition. In the combined
commit those flags OR with the caller's flags, promoting the entire
combined commit to modeset mode. Most drivers reject
`DRM_MODE_PAGE_FLIP_EVENT` / `DRM_MODE_ATOMIC_NONBLOCK` paired with
`ALLOW_MODESET`, so the steady-state scenes lose their async-flip
path for that one frame and the modeset cost lands across every
participating CRTC.

The recommended workaround is **out-of-band warmup**: run each
scene's first commit on its own before attaching it to the
cross-CRTC cadence.

```cpp
auto fresh = drm::scene::LayerScene::create(dev, cfg).value();
// One-shot single-CRTC commit. The implicit ALLOW_MODESET binds the
// connector to the CRTC; the scene's first_commit_ flag clears.
(void)fresh->commit();

// From here, effective_flags_of(state) is 0 unless the scene later
// rebinds or transitions HDR / colorspace state.
auto idx = set->add_scene(std::move(fresh));
// set->commit() now sees a clean combined commit — no modeset
// promotion, no async-flip downgrade on the steady scenes.
```

The same pattern applies to `rebind()`: drive the rebind through a
single-CRTC commit before resuming the combined cadence. The
application's hotplug handler is the natural place to gate this.

## Hotplug coordination

`SceneSet` exposes `add_scene` and `remove_scene` mutation hooks; it
does not subscribe to `drm::display::HotplugMonitor` internally. The
event-loop-stays-in-the-application convention from the rest of the
library applies — the caller drives a poll/epoll loop over libinput,
libseat, the hotplug monitor, and any application-specific fds, and
calls `SceneSet::add_scene` / `remove_scene` synchronously in
response to connector plug / unplug events.

The mutation surface is deliberately small:

| Operation | What it does |
|---|---|
| `add_scene(unique_ptr<LayerScene>)` | Reuses the lowest-index hole if any; otherwise appends. Returns the slot index. |
| `remove_scene(size_t)` | Destroys the child scene; drops any cross-scene `SceneSetLayerSpec` pins targeting it; leaves a `nullptr` hole at the slot. |

The hole-preservation rule is what keeps `SetLayerHandle` ids and
indices that other callers hold stable across a remove. If
`remove_scene` compacted the vector, every cross-scene layer's pin
list would shift and the `id`-based generation guard wouldn't suffice.

### Hotplug pattern

```cpp
// The application keeps its own scene_index → connector_id map
// alongside the SceneSet; LayerScene doesn't expose its binding.
std::unordered_map<std::size_t, std::uint32_t> scene_connector;

// On HOTPLUG=1 uevent that reports an unplugged connector:
for (auto it = scene_connector.begin(); it != scene_connector.end();) {
  if (it->second == disconnected_connector) {
    set->remove_scene(it->first);
    it = scene_connector.erase(it);
  } else {
    ++it;
  }
}

// On HOTPLUG=1 uevent that reports a newly-plugged connector:
auto fresh = drm::scene::LayerScene::create(dev, fresh_cfg).value();
(void)fresh->commit();                       // warmup (see above)
const auto idx = set->add_scene(std::move(fresh)).value();
scene_connector[idx] = newly_plugged_connector;
// idx is stable until set->remove_scene(idx) runs.
```

The hotplug uevent carries the `CONNECTOR=<id>` hint on kernels
≥ 4.16 (see `drm::display::HotplugEvent::connector_id`). On older
kernels or blanket hotplug events, the application re-enumerates
connectors via `drm::get_resources` and decides for itself which
scene(s) to remove.

### Pre-existing handles after `remove_scene`

A `SetLayerHandle` that targets a removed scene **remains valid**:
`remove_scene` walks the set's slot list and drops the matching
per-scene pin, but the slot's `id` and `generation` are untouched.
The handle becomes a no-op for that scene's contribution; if the
slot had targets on other scenes (mirrored across multiple outputs),
those continue working. The corollary: a layer mirrored across N
scenes one of which gets unplugged retains N-1 live participations
without the application needing to remove and re-add it.

## Out of charter

For continuity with `LayerScene`'s discipline, the corresponding
exclusions for `SceneSet`:

- **Cross-CRTC layer transitions.** A layer that's mirrored across
  three outputs can't be animated to "slide off output A onto output
  B" through `SceneSet`. The application either reissues the
  `SceneSetLayerSpec` with updated rects, or builds the transition
  out of per-scene layers.
- **Scene-graph composition.** No "scene at index 0 composites onto
  scene at index 1." Each child scene is independent; their planes
  are siblings in the combined atomic commit.
- **Display configuration.** `SceneSet` doesn't enumerate outputs,
  pick modes, or resolve connector capabilities. The application
  builds each `LayerScene` against an output it chose and hands the
  result to `add_scene`.
- **Automatic hotplug.** The application owns the event loop and the
  `HotplugMonitor`. The library will not grow an opt-in helper here
  before a real workload asks for it.
- **Cross-fd coordination.** Every owned scene must be against the
  same `drm::Device`. The combined `AtomicRequest` is bound to one
  fd; a mismatched scene would fail with `-EBADF` at commit time.

The expectation is that applications stack `SceneSet` underneath
their own multi-output policy — the library handles the
synchronization mechanics, not the choreography.