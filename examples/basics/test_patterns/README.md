# test_patterns

A single-layer LayerScene demo cycling through display-engineering
reference patterns — SMPTE bars, gradients, geometry grids — switched
at runtime from the keyboard. The pattern is rendered straight into a
CPU-mapped `DumbBufferSource` and the same buffer is recycled on every
switch (no allocator churn, no flicker beyond the in-place repaint).

This is the third consumer of `drm::scene::LayerScene` (after
`thorvg_janitor` and `signage_player`). It widens the API's coverage
to a paint-on-event workload — one full-screen primary-plane layer
that mutates only when the user asks for a different pattern.

## Run

```
sudo test_patterns
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system membership in the `seat` group is enough. The
example accepts the same optional `[device]` argument as the other
drm-cxx demos — passes through to `drm::examples::select_device`.

## Key bindings

| Key       | Pattern / action                          |
|-----------|-------------------------------------------|
| `1`       | SMPTE 75 % bars + PLUGE-style sub-bands   |
| `2`       | 1-pixel B/W vertical stripes (Nyquist / max-power) |
| `3`       | 11-step gray bars (gamma response)        |
| `4`       | 64 px black/white checkerboard            |
| `5`       | Continuous gray gradient (banding / dithering) |
| `6`       | Stacked R/G/B horizontal gradients        |
| `7`       | 64 px cross-hatch grid (geometry)         |
| `8`       | 5×5 procedural H-glyph grid (focus uniformity) |
| `n` / Space | Next pattern (wraps)                    |
| `p`       | Previous pattern (wraps)                  |
| `Esc` / `q` | Quit                                    |

## Pattern reference

- **SMPTE 75 % bars + PLUGE.** Three-band layout: 75 %-intensity colour
  bars across the top two-thirds, a reverse-blue band for chroma-decode
  verification, and a PLUGE-style bottom strip with three near-black
  pulses (≈ +2 %, 0 %, +4 %). The −2 % sub-black bar of the canonical
  RP 219 signal is **omitted on purpose** — an 8-bit framebuffer cannot
  represent sub-zero luminance and would clip to code 0,
  indistinguishable from the surround. Useful as a quick brightness /
  contrast reference; not a fully spec-compliant signal.

- **1-pixel B/W stripes.** Maximum spatial frequency the panel can
  resolve. On LCDs every column line drives at peak duty cycle, so
  this is also the worst-case for backlight current and column-driver
  power.

- **11-step gray bars.** Discrete gamma reference at 0 %, 10 %, …,
  100 %. Posterization between adjacent bars reveals an undersized
  internal LUT; missing steps reveal black-/white-clip miscalibration.

- **Checkerboard.** 64 px tiles. Geometry warp on a projection
  display shows up immediately as bent tile edges.

- **Gray gradient.** Continuous 0 → 255 horizontal sweep. Visible
  1-code steps mean the panel is straight 8-bit; smooth banding means
  it's doing FRC / temporal dithering.

- **R/G/B gradients.** Three stacked horizontal sweeps, one per
  channel. Per-channel non-linearity in the panel's gamma curves
  shows as colour cast at intermediate values.

- **Cross-hatch.** White grid on black, lines every 64 px, single
  pixel thick. Convergence / keystone / pincushion reference.

- **H pattern.** Procedural 5 × 5 grid of capital H glyphs. Used on
  projectors to check focus uniformity from centre to edge — the
  letter shape's two parallel vertical strokes make any defocus
  visible immediately. No font dependency; the glyph is just three
  filled rectangles per cell.

## Status

Scaffold-quality but feature-complete: every pattern paints, key
bindings work, libseat VT pause/resume is wired up so the pattern
re-paints against the fresh dumb-buffer mapping after a return from
another VT. Patterns are CPU-only — no Blend2D or other optional
dependency.

A `test_test_patterns` unit-test binary (under `tests/unit/`) covers
stride-aware pixel sanity for the deterministic patterns
(`pixel_stripes`, `checkerboard`, `gray_bars`, `smpte_bars`). It does
not pixel-perfect against a golden image — the bar-rounding choices
should remain easy to retune.
