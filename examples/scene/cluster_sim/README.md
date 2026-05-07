# cluster_sim — automotive instrument-cluster showcase

Status: **skeleton stage**. A single full-screen Blend2D-painted
backdrop with the standard libseat session + libinput keyboard + VT-
switch boilerplate. Subsequent commits add the actual cluster layers.

## Planned shape

The finished demo is the showcase referenced in `docs/roadmap.md`'s
Milestone 6 / Phase 6.2 follow-ups. It exercises the same primitives
as `layered_demo` and `signage_player` but composed into the layout
real automotive clusters use:

| Layer | Purpose | Format / source |
|-------|---------|-----------------|
| Bg | Gradient backdrop | XRGB8888 dumb buffer, Blend2D-painted once |
| Speedometer dial | Animated needle + ticks | ARGB8888 dumb buffer, Blend2D-repainted each frame |
| Tachometer dial | Animated needle + ticks | ARGB8888 dumb buffer, Blend2D-repainted each frame |
| Center info | Speed / gear / ODO readout | ARGB8888 dumb buffer, repainted on state change |
| Warning indicators | Turn signals, check-engine, etc. | Small ARGB8888 overlay, high-priority |
| Rear-view (optional) | NV12 video via `V4l2DecoderSource` | Toggled by key |

## What this exercises (target)

- **Multi-layer scene composition** with mixed formats (ARGB on top
  of XRGB on top of NV12).
- **Priority-driven plane allocation** — the warning-indicator layer
  is high-priority enough that the allocator pins it on a hardware
  plane regardless of how many dials had to fall back to
  composition. The bg can always be composited away.
- **Realtime per-frame Blend2D paint** — the dial sweeps repaint
  their ARGB layers each commit, demonstrating that per-frame paint
  is a workable pattern for ~250×250 dial-sized buffers.
- **`V4l2DecoderSource`** in a realistic embedded workload (rear-
  view camera as a scene layer).

## Status by step

- [x] Step 1 — skeleton + build wiring.
- [ ] Step 2 — animated speedometer + tach dials.
- [ ] Step 3 — center info + warning indicators.
- [ ] Step 4 (stretch) — `V4l2DecoderSource` rear-view layer.
- [ ] Step 5 — README screenshots from a hw run.

## Key bindings (skeleton)

- `Esc` / `q` / `Ctrl-C` — quit.
- `Ctrl+Alt+F<n>` — VT switch (forwarded to libseat).

Subsequent steps add bindings for toggling the rear-view layer,
forcing a warning-indicator state, etc.

## Building

`cluster_sim` is gated on Blend2D in both build systems:

- CMake: `-DDRM_CXX_BLEND2D=ON` (`AUTO` works if Blend2D is
  installed system-wide).
- Meson: Blend2D is auto-detected; pass `-Dblend2d=enabled` to
  hard-require.

## Running

```
./cluster_sim
```

Run it from a real VT (or under libseat through your compositor
launcher) so the libseat session integration has somewhere to
acquire master against.
