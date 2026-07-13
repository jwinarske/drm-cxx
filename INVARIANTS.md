# Relied-upon invariants

Contracts external consumers (compositors, embedders, toolkit backends) are
known to depend on — each with where it lives and the test that pins it. A
change that would break one of these is an API break even if the signature is
unchanged: check the pinning test still holds, and if the behavior must change,
update the test and this file in the same commit.

Integration tests suffixed `_vkms` need the VKMS virtual driver (or any modeset
card via `DRM_CXX_TEST_CARD`) and self-skip without one; they run on dev
machines and HIL, not GitHub CI.

---

## 1. Deferred buffer release (release after the flip, not after the ioctl)

A buffer acquired from a `LayerBufferSource` is released back to the source only
**after the page-flip that stops scanning it out has completed** — two commits
deep, not when `commit()`'s ioctl returns. Releasing at ioctl-return would let a
producer (V4L2 capture, a GBM ring) overwrite a buffer the display engine is
still scanning out, tearing on hardware whose producer driver attaches no
reservation fence.

- **Defined:** `src/scene/buffer_source.hpp` (`release` / `release_with_fence`
  doc: "The scene calls this after page-flip completion").
- **Pinned by:** `test_layer_scene_release_vkms.cpp` ::
  `LayerSceneReleaseVkms.DefersReleaseTwoCommitsDeep`.

## 2. A failed commit releases that frame's acquisitions before returning

When a real `commit()` is rejected by the kernel, `finalize_frame` releases the
frame's held acquisitions **immediately** (no flip is in flight to gate their
release on) and returns the error. A non-`EACCES` failure does **not** put the
scene into suspended mode. Self-healing producer rings rely on this to reuse
their buffers after a rejected commit; the dropped frame heals on the next.

- **Defined:** `src/scene/layer_scene.cpp` (`Impl::finalize_frame`, the
  failure branch and its "external contract" note).
- **Pinned by:** `test_layer_scene_release_vkms.cpp` ::
  `LayerSceneReleaseVkms.CommitFailureReleasesAcquisitions`.

## 3. `PageFlip::dispatch` never returns `errc::interrupted`

`dispatch()` retries `EINTR` internally, so a signal landing mid-wait (SIGCHLD,
interval timers, profilers) never abandons a flip still in flight. For a bounded
`timeout_ms > 0`, the remaining budget is recomputed from a steady clock across
retries, so a signal storm cannot stretch the wait past `timeout_ms`. A foreign
source's own fd reads may still observe `EINTR` — that remains the caller's.

- **Defined:** `src/modeset/page_flip.hpp` (`dispatch` doc) /
  `src/modeset/page_flip.cpp`.
- **Pinned by:** `test_page_flip.cpp` ::
  `PageFlipEintr.DispatchInfiniteRetriesUntilReady`,
  `PageFlipEintr.DispatchBoundedTimeoutHonorsBudgetUnderStorm`.

## 4. FB-only fast path: the real commit's `atomic_check` is the arbiter

When only content changed on already-placed layers (FB_ID / damage / fence, with
geometry, format, and modifier byte-identical to the last accepted commit —
detected via `property_hash`, which excludes FB_ID / IN_FENCE_FD), the allocator
reuses the cached plane assignment and **skips the redundant `TEST_ONLY`**. The
real commit's own `atomic_check` stays the sole arbiter: a rejection invalidates
the cached allocation and re-searches on the next frame. The failure mode is a
dropped-then-healed frame, never corruption. `CommitReport.fb_delta_fast_path`
is set and `test_commits_issued` is 0 on such a frame.

Corollary consumers depend on: **`property_hash` isolates content from
placement** — every placement/format property (src/dst rects, rotation, alpha,
zpos, COLOR_*, pixel format, modifier) moves the hash; FB_ID and IN_FENCE_FD do
not. A property misclassified as content would let a real change skip the test.

- **Defined:** `src/planes/allocator.cpp` (`is_fb_only_frame`, the fast-path
  branch) and `src/scene/layer_scene.cpp` (the reject-invalidates-cache safety
  net in `finalize_frame`).
- **Pinned by:** `test_layer.cpp` ::
  `LayerTest.PropertyHashIsolatesFbOnlyContentFromPlacement`;
  `test_layer_scene_minimization_vkms.cpp` ::
  `LayerSceneMinimizationVkms.SteadyStateWritesOnlyFbIdPerAssignedLayer`,
  `FbOnlyFastPathReTestsOnPlacementChange`;
  `test_layer_scene_census_vkms.cpp` :: `LayerSceneCensusVkms.StaticSteadyState`.

## 5. Teardown does not wait on the kernel — drain the last flip first

Destroying a `LayerScene` releases its buffers and framebuffers **without
waiting on the kernel**. If the last real commit armed `DRM_MODE_PAGE_FLIP_EVENT`
and that event has not been dispatched, the flip still references a buffer the
destructor tears down (the RmFB-on-in-flight-FB hazard). Land it first: call
`LayerScene::drain(pf, timeout_ms)` — a no-op when nothing is armed — or dispatch
the event yourself, before the scene goes out of scope.

- **Defined:** `src/scene/layer_scene.hpp` (`drain` doc and the `~LayerScene`
  teardown note).
- **Pinned by:** `test_layer_scene_release_vkms.cpp` ::
  `LayerSceneReleaseVkms.DrainLandsPendingFlipBeforeTeardown`.

## 6. `dumb::Buffer::map` is a zero-cost view of a lifetime mmap

A dumb buffer is mmap'd once at `create()` and the mapping is held for the
buffer's full lifetime and is cache-coherent by kernel construction. `map()` is a
thin wrapper around `data()` / `stride()`; the returned `BufferMapping` guard's
destructor is a **no-op**. Consumers may therefore `map()` per frame with no
syscall cost and write pixels directly — the access mode is recorded on the guard
but ignored at the dumb-buffer level.

- **Defined:** `src/dumb/buffer.hpp` (`map` / `data` doc).
- **Pinned by:** `test_buffer_mapping.cpp` ::
  `BufferMapping.DefaultIsEmptyAndDestructorIsNoOp` (guard is a borrowed,
  no-op-destructor view); `test_dumb_buffer.cpp` :: `DumbBuffer.*` (create /
  ownership).
