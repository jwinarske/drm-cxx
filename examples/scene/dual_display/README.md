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

## Known limitations

- **No connector hotplug.** Adding or removing an output at runtime
  is Phase 8.2.3 (`SceneSet::add_scene` / `remove_scene`); the
  current demo wires its scene list at startup and treats hotplug as
  out of scope.
- **No per-CRTC fallback.** Every frame issues the combined commit
  even when only one child changed. Phase 8.2.2 is where that
  optimization lands.
- **No multi-card.** The kernel doesn't support atomic commits
  spanning two file descriptors, so a dual-card workstation isn't
  driveable from one `SceneSet`. Use one process per card.