# cluster_sim — automotive instrument-cluster showcase

Status: **dials + center info + warnings added**. A Blend2D-painted
backdrop, animated speedometer + tachometer dials, a digital speed
readout between them, and a four-cell warning-indicator strip below.
The optional `V4l2DecoderSource` rear-view layer lands in a follow-up.

## What this exercises

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

## Key bindings

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
