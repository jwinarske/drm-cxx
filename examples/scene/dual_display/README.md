# dual_display

A minimal multi-CRTC `SceneSet` demo. Builds one `LayerScene` per
connected output on the supplied card and lays a single shared layer
across all of them via `SceneSet::add_layer`; every frame is one
`drmModeAtomicCommit` covering every CRTC, so changes on the shared
layer land on the same vblank on every output.

## What it shows

- **Per-output background.** Each child `LayerScene` carries its own
  full-screen `DumbBufferSource`, painted once with a different solid
  tint. The differing colors on the two screens are the visual proof
  that per-output specialization is wired through `SceneSet`.

- **Mirrored content.** A single `DumbBufferSource` is added via
  `SceneSet::add_layer` with one `SceneSetLayerSpec::Target` per
  child scene, each placing the shared buffer at the corresponding
  output's center. A horizontal scan bar advances one frame at a
  time; the fact that the bar is at the same column on both outputs
  every vblank is the visual proof that the combined atomic commit is
  tear-free across CRTCs.

- **Cross-CRTC atomic commits.** Every frame issues exactly one
  `drmModeAtomicCommit` carrying property writes for both CRTCs.
  The page-flip handler fires once per CRTC; the demo waits for all
  of them to land before issuing the next frame.

## Running

```
# Real hardware with two displays attached:
./dual_display

# Single-output workstation — provision vkms with two virtual
# connectors first, then drive the resulting card:
sudo scripts/vkms_dual.sh up
./dual_display /dev/dri/cardN     # N = the vkms node
```

Keys:

- `Esc` / `q` / `Ctrl+C` — quit
- `Ctrl+Alt+F<n>` — VT switch (libseat-managed)

## Hardware requirements

- A card with at least two connected outputs. The example errors out
  on cards with fewer than two; `scripts/vkms_dual.sh` is the
  zero-hardware workaround.
- libseat (logind/seatd) for the revocable fd path; the example
  works without it but won't survive a VT switch cleanly.

## Commit shape

Each frame the demo calls `SceneSet::commit` with the default
`NarrowPolicy::AutoOnModeset`. On uniform frames (every scene either
needs `ALLOW_MODESET` or none of them do) that's one
`drmModeAtomicCommit` covering both CRTCs, exactly the tear-free
contract the demo's mirrored scan bar relies on. When the modeset
state across scenes is mixed — most often after a hotplug-style
`add_scene` of a fresh output while existing scenes are already
steady — `SceneSet` splits the frame into two sequential commits,
modeset-needing scenes first. The steady-state scenes' tear-free
scanout isn't disturbed by the modeset on their neighbor.

Pass `NarrowPolicy::Combined` to force one ioctl per frame regardless,
or `NarrowPolicy::PerCrtc` for one ioctl per engaged scene (no
cross-CRTC sync).

## Known limitations

- **No multi-card.** The kernel doesn't support atomic commits
  spanning two file descriptors, so a dual-card workstation isn't
  driveable from one `SceneSet`. Use one process per card.
- **`would_request_modeset` over-includes user-set HDR.** It flips
  true on every `LayerScene::set_output_metadata` call, even when
  the new content matches the cached HDR blob (a kernel-side no-op
  the build pass would dedup). Worst case: one needlessly-split
  commit. Under-including was the alternative and would miss real
  HDR transitions; the trade favors splitting.
- **Auto-derived signaling changes still aren't observable
  pre-build.** When colorspace / HDR signaling shifts because the
  layer set changed (not because the application called
  `set_output_metadata`), only the build pass can see it. The
  affected scene's commit still carries `ALLOW_MODESET` for its
  group's ioctl — the grouping just won't have been optimal.