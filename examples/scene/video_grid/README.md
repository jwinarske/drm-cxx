# video_grid

An N×N grid of synthesised "video" cells laid out across a single
output. Each cell owns its own `DumbBufferSource` and is repainted
every frame with a diagonal sweeping bar over a flat base color,
simulating per-cell motion. Cells alternate between XRGB8888 and
ARGB8888 so the format-cascade path is exercised alongside the plane
budget.

The grid stresses three things at once:

- **Plane-budget overflow.** A 4×4 grid is 16 layers; typical KMS
  budgets are 3–6 planes per CRTC. The composition fallback kicks in
  for the layers the allocator can't place natively, blending them
  onto a shared `CompositeCanvas` and arming that canvas on a single
  reserved overlay plane.
- **Allocator churn.** The runtime layout switch (1 / 2 / 3) removes
  every existing layer and adds a fresh set at the new cell
  dimensions. Each switch walks `LayerScene::add_layer` and
  `remove_layer` without tearing down the scene.
- **Format heterogeneity.** Alternating ARGB / XRGB layers force the
  allocator to think about which planes accept which channel orderings
  on this driver — different drivers will land different cells on
  hardware vs. canvas.

`CommitReport::layers_assigned` / `layers_composited` / `composition_buckets`
make the actual hardware split visible per frame.

## Run

```
sudo video_grid [/dev/dri/cardN]
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the `seat` group membership is enough.

## Key bindings

| Key         | Action                            |
|-------------|-----------------------------------|
| `1`         | 2×2 grid (4 cells)                |
| `2`         | 3×3 grid (9 cells)                |
| `3`         | 4×4 grid (16 cells)               |
| `n` / Space | Next layout (wraps)               |
| `p`         | Previous layout (wraps)           |
| `Esc` / `q` | Quit                              |
