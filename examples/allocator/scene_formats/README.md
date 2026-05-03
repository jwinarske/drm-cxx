# scene_formats

A pedagogical demo for the allocator's bipartite plane-matching across
heterogeneous layer requirements. Up to four layers — three different
ARGB-class channel orderings plus one scaler-required dst-rect:

| Layer | Format    | Scaling     | What it stresses                  |
|-------|-----------|-------------|-----------------------------------|
| 0     | ARGB8888  | 1:1         | universally supported (baseline)  |
| 1     | ARGB8888  | 1:2 width   | scaler-capable plane only         |
| 2     | ABGR8888  | 1:1         | channel-order swap                |
| 3     | XRGB8888  | 1:1         | universally supported             |

KMS planes are not interchangeable. Each plane advertises a subset of
pixel formats (via the `IN_FORMATS` blob the registry parses) and a
subset of capabilities (scaling, rotation, modifier support). When a
scene mixes layer requirements like the four above, the allocator has
to solve a bipartite assignment: each layer must land on a plane that
supports its format **and** its capability needs. A greedy "take the
first plane that works for this layer" pass fails on contrived but
realistic shapes (the N+1 problem); drm-cxx's allocator runs an
actual bipartite solver, so such cases place cleanly when a placement
exists at all.

The four-layer set is deliberately mild — all four are 32 bpp (the
bound `DumbBufferSource` imposes), three require no scaling, and only
one has a non-trivial channel order. That keeps the example
self-contained and runnable on every driver while still exercising:

- **Format diversity.** ARGB / XRGB / ABGR — same byte width, distinct
  channel orderings; not all planes accept all three on every driver,
  particularly older or constrained-IP embedded SoCs.
- **Scaler matching.** Layer 1's dst-rect is twice the src-rect width,
  so it can only land on a plane whose `IN_FORMATS` / capabilities
  advertise scaling.

## Plane-budget awareness

The full four-layer set fits drivers with ≥4 PRIMARY+OVERLAY planes
per CRTC. On constrained pipes (amdgpu DCN typically gives 1 primary
+ 2 overlays = 3 usable planes per CRTC; many embedded SoCs are
tighter), four non-overlapping layers can't all land on hardware and
the allocator falls back to scene-wide composition — `assigned=0
composited=4` — which masks the demo's pedagogy. The example probes
the active CRTC's plane budget at startup and trims the layer set to
`min(4, usable_planes)`, with the table ordered most-distinctive-first
(baseline → scaler → channel-swap → redundant baseline) so each
budget keeps the most pedagogically interesting subset.

## Run

```
sudo scene_formats [/dev/dri/cardN]
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the `seat` group membership is enough.

Per-driver results vary; the printed `CommitReport` counts make the
actual outcome visible.
