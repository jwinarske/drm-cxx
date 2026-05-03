# capture_demo

A libinput-driven hardware-validation harness for `drm::capture`. Same
shape as `cursor_rotate`: take a DRM device, enable atomic, bind
libinput for keys, run an event loop, and on a keypress snapshot the
current CRTC composition to a PNG.

What this validates end-to-end on real hardware:

1. **Atomic cap.** `drmModeObjectGetProperties` exposes
   `CRTC_X/Y/W/H` on every plane. `snapshot()` fails fast if it can't
   read those for any plane on the target CRTC.
2. **`drmModeGetFB2` + `drmPrimeHandleToFD` + `mmap(PROT_READ)` round
   trip** for each bound plane. Planes with non-linear modifiers or
   non-ARGB / XRGB formats log a warning and get skipped — on most
   systems the primary plane is linear ARGB8888 and the output PNG
   matches the screen pixel for pixel.
3. **Blend2D composite** of every bound plane in zpos order to a
   `BL_FORMAT_PRGB32` output, then `BLImageCodec` PNG encode.

Built only when the library was configured with Blend2D support
(`-Dblend2d=enabled` / `-DDRM_CXX_BLEND2D=ON`, both auto by default
when Blend2D is installed).

## Run

```
sudo capture_demo [--out DIR] [--crtc ID] [/dev/dri/cardN]
```

| Flag           | Default      | Meaning                                              |
|----------------|--------------|------------------------------------------------------|
| `--out`        | current dir  | Directory for captured PNGs (`capture_<ts>.png`)     |
| `--crtc`       | auto-pick    | Capture a specific CRTC instead of the first one     |
| `[device]`     | first card   | DRM device path                                      |

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the `seat` group membership is enough.

## Key bindings

| Key             | Action                                                 |
|-----------------|--------------------------------------------------------|
| `C`, `Space`    | Snapshot the active CRTC → `capture_<ts>.png` in `--out` |
| `R`             | Re-report CRTC state and bound-plane count             |
| `Esc`, `Q`      | Quit                                                   |

## Preconditions

- Run from a TTY (Ctrl+Alt+F3) or another context where no compositor
  holds the DRM master. On a running Wayland / X session,
  `Device::enable_atomic()` will fail with `EACCES`.
- `input` group membership (or root) so libinput can open
  `/dev/input/event*`. The example also runs under a libseat session
  when available, which gives the same access without root.
