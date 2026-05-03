# scene_priority

A pedagogical demo for the allocator's content-type / refresh-rate
driven priority eviction. Eight layers laid out in a 4×2 grid with
distinct solid-color fills, each tagged with a `ContentType` and an
`update_hint_hz`:

| Layer | Priority | Tag                  | Likely outcome           |
|-------|----------|----------------------|--------------------------|
| 0, 1  | 100      | `Video`, 60 Hz       | hardware plane           |
| 2, 3  |  80      | `UI`, 60 Hz          | hardware plane           |
| 4, 5  |  50      | `UI`, 30 Hz          | may evict to canvas      |
| 6, 7  |  10      | `Generic`, 0 Hz      | likely composited        |

Eight layers don't fit on a typical KMS plane budget (3–6 planes per
CRTC; amdgpu DCN gives 1 PRIMARY + 2 OVERLAY = 3 usable, minus one
reserved for the canvas → 2 hardware slots). The allocator's
`layer_priority()` function returns 100 for `ContentType::Video`, 80
for layers with an `update_hint_hz > 30`, 50 for layers with any
`update_hint_hz`, and 10 for everything else; eviction order follows
that ranking.

What you should see in the printed `CommitReport`:

```
Layer  0 priority=100 (Video, 60Hz)        → wants hardware
Layer  1 priority=100 (Video, 60Hz)        → wants hardware
Layer  2 priority= 80 (UI,    60Hz)        → wants hardware
Layer  3 priority= 80 (UI,    60Hz)        → wants hardware
Layer  4 priority= 50 (UI,    30Hz)        → may evict to canvas
Layer  5 priority= 50 (UI,    30Hz)        → may evict to canvas
Layer  6 priority= 10 (Generic, 0Hz)       → likely composited
Layer  7 priority= 10 (Generic, 0Hz)       → likely composited

frame   1 assigned=2 composited=6 unassigned=0  ← 3 planes, 1 reserved
```

Driver-specific plane budgets shift the exact split — the printed
report makes the actual numbers visible on your hardware.

Pure consumer of the existing scene API. Static solid-fill buffers,
painted once before the loop. Buffer dirtying would muddle the
priority signal we're trying to expose.

## Run

```
sudo scene_priority [/dev/dri/cardN]
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the `seat` group membership is enough.
