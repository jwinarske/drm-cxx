# cluster_sim — automotive instrument-cluster showcase

Status: **rear-view layer added**. A Blend2D-painted backdrop,
animated speedometer + tachometer dials, a digital speed readout
between them, a four-cell warning-indicator strip below, and an
optional rear-view camera layer driven by `V4l2DecoderSource`
against vicodec (toggled with `R`).

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
- [x] Step 2 — animated speedometer + tach dials.
- [x] Step 3 — center info + warning indicators.
- [x] Step 4 — `V4l2DecoderSource` rear-view layer (vicodec-driven).
- [ ] Step 5 — README screenshots from a hw run.

## Key bindings

- `Esc` / `q` / `Ctrl-C` — quit.
- `R` — toggle the rear-view camera layer. Requires the `vicodec`
  kernel module (`modprobe vicodec`); without it, cluster_sim emits
  a one-shot startup log line explaining the skip and the toggle is
  a no-op.
- `Ctrl+Alt+F<n>` — VT switch (forwarded to libseat).

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

## Hardware validation

The example needs a live VT to take master; running from inside an
X / Wayland session fails at `drm::session::Seat::open` with
`EBUSY`. Drop to a tty (`Ctrl+Alt+F3`, log in), then:

```
modprobe vicodec    # for the rear-view toggle
modprobe vkms       # only needed for integration tests, not cluster_sim
sudo ./cluster_sim  # or run as a user in the 'video' / 'input' groups
```

What to look for:

- **Bg + dials + center info + warnings** render immediately;
  the speedo + tach needles sweep on decorrelated periods (6 s /
  4 s); the speed readout follows the speedometer phase; the
  four warning cells blink on independent periods.
- **`R` toggle** brings up a 320×240 rear-view layer in the
  top-right corner. The startup log line tells you whether the
  rear-view is wired (`rear-view: ready (...) press R to toggle`)
  or skipped (`rear-view: vicodec not loaded ...`). With vicodec
  loaded, pressing `R` while running should add the layer; press
  `R` again to remove it.
- **`Ctrl+Alt+F<n>`** switches VT — the scene drops on pause and
  re-imports against the new fd on resume; needles continue
  sweeping with the elapsed-time clock so the post-resume frame
  doesn't snap back to t=0.

If the rear-view layer is corrupted or the allocator drops it, the
relevant references are the `reference_drm_plane_layout_nv12_only`
gap (NV12 special case is the only multi-DRM-plane format
currently handled by `derive_drm_plane_layout`) and the
`reference_amdgpu_primary_zpos_pin` invariant (dials at zpos=3,
warnings at zpos=4, rear-view at zpos=5 to clear the PRIMARY
collision).
