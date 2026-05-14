# video_wall_multi

An N×N grid of synthesized "video" cells laid out across **every
connected output on one card** as a single logical wall. Total wall
width is the sum of every output's `hdisplay`; vertical extent is
the first output's `vdisplay`. Each cell owns its own
`DumbBufferSource` and is repainted every frame with a diagonal
sweeping bar — same shape as [`video_grid`](../video_grid/README.md),
extended to multi-CRTC.

## What this demonstrates

- **Cross-CRTC atomic commits.** Every frame issues one (or two,
  when `NarrowPolicy::AutoOnModeset` splits across a fresh-output
  modeset transition) `drmModeAtomicCommit` covering every CRTC.
  The wall-spanning scan bar tracks the same column on every output
  on the same vblank — the visual proof.
- **Cells that straddle output boundaries.** When a logical cell's
  x-range crosses an output's `hdisplay` edge, the cell is registered
  via `SceneSet::add_layer` with **two targets** sharing one
  `DumbBufferSource`: each target carries the appropriate `src_rect`
  sub-rect of the cell content and a `dst_rect` translated into its
  output's local coordinate space. The shared source is acquired
  twice per frame (once per target) — `DumbBufferSource` handles
  this naturally.
- **Per-CRTC plane budget + composition fallback.** A 4×4 wall is
  16 cells. Each output's slice of those cells routinely overflows a
  3–6 plane budget; the cells the per-scene allocator can't place
  natively land on that scene's `CompositeCanvas`.

## Run

```
# Real hardware with two displays attached:
./video_wall_multi

# Single-output workstation — provision vkms with two virtual
# connectors first, then drive the resulting card:
sudo scripts/vkms_dual.sh up
./video_wall_multi /dev/dri/cardN     # N = the vkms node

# Physical layout override (DRM enumeration order doesn't carry
# physical placement). Name connectors in left-to-right physical order:
./video_wall_multi --order DP-1,HDMI-A-1
./video_wall_multi /dev/dri/card1 --order DP-1,HDMI-A-1
```

Keys:

| Key         | Action                            |
|-------------|-----------------------------------|
| `1`         | 2×2 logical grid (4 cells)        |
| `2`         | 3×3 grid (9 cells)                |
| `3`         | 4×4 grid (16 cells)               |
| `n` / Space | Next layout (wraps)               |
| `p`         | Previous layout (wraps)           |
| `Esc` / `q` | Quit                              |
| Ctrl+Alt+F<n> | VT switch (libseat-managed)     |

CLI flags:

| Flag                  | Effect                                  |
|-----------------------|-----------------------------------------|
| `--list-outputs`      | Print the connected connector names + modes on the picked card, then exit. Use to discover the names you'd pass to `--order`. |
| `--order <a,b,...>`   | Override left-to-right physical output order using connector names (e.g. `DP-1,HDMI-A-1`). Connectors not named appear after, in enumeration order. |

## Why `--order` matters

DRM exposes connectors in kernel-enumeration order, which doesn't
necessarily match how the monitors are physically arranged. With
the default order, the *logical* wall left edge sits on whichever
output the kernel enumerated first — typically `outputs[0]`. If
your physical monitors are arranged opposite to that, the cell that
straddles the logical output boundary ends up with its **right
portion on your physical left edge** and its **left portion on
your physical right edge** — same cell, same colour, both ends of
your physical wall. The `2×2` and `4×4` layouts make this very
visible; `3×3` happens to straddle by only a few pixels and looks
fine. Pass `--order` to align the logical wall with what your eyes
expect.

## Hardware requirements

- A card with at least two connected outputs. The example errors
  out on cards with fewer than two; `scripts/vkms_dual.sh` is the
  zero-hardware workaround.
- Outputs should share a common `vdisplay` for clean cell placement;
  the example uses the first output's `vdisplay` for the wall height
  and ignores anything taller below it. Mismatched widths are fine —
  cell widths are uniform across the logical wall regardless of
  per-output `hdisplay`, so a 1680 + 3440 pair gets a 2560-px-wide
  cell at 2×2 and the cell straddles the boundary.
- libseat (logind/seatd) for the revocable-fd path; the example
  works without it but won't survive a VT switch cleanly.

## Known limitations

- **Horizontal-only layout.** Outputs are laid left-to-right in
  enumeration order. No vertical or grid arrangement of outputs.
- **No multi-card.** The kernel doesn't support atomic commits
  spanning two file descriptors, so a dual-card workstation isn't
  driveable from one `SceneSet`. Use one process per card.
- **Uniform cell size.** When outputs have different widths, cells
  on the wider output cover proportionally more pixels per cell.
  The bar sweep speed (8 px/frame) is the same regardless, so the
  bar visually moves faster on smaller cells.
