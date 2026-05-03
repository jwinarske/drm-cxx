# cursor_rotate

Hardware-validation harness for `drm::cursor` V2. Brings up a libseat
session, opens the device, builds a `Renderer` per active CRTC, loads
a single `Cursor` from an XCursor theme, attaches it to every
renderer, and cycles its rotation through k0 → k90 → k180 → k270 at a
configurable period. Exit on `Esc`, `q`, or Ctrl-C — the libinput
path is the only reliable one on a real VT (libseat puts the TTY into
`KD_GRAPHICS`, which suppresses Ctrl-C signal generation).

What this validates end-to-end:

1. **Theme resolve cache.** Two back-to-back `Theme::resolve()` calls
   for the same `(name, theme)` pair are timed; the second should
   land in the memoization cache and come out in nanoseconds.
2. **`HOTSPOT_X` / `HOTSPOT_Y` plane properties.**
   `Renderer::has_hotspot_properties()` is printed per renderer.
   Expected `true` on virtualized guests (virtio-gpu, vmwgfx) and
   `false` on bare metal. Inside a VM, verify the host's native mouse
   cursor aligns with the guest sprite's tip — without these
   properties the host sees the sprite's top-left corner and
   misaligns by ~xhot pixels.
3. **Shared `Cursor` across multi-CRTC `Renderer`s.** One
   `Cursor::load` is performed; the result is wrapped in
   `shared_ptr<const Cursor>` and handed to every active CRTC's
   `Renderer`. Pixel storage is not duplicated.
4. **`set_rotation()` runtime setter.** Unless `--no-rotate` is
   passed, the example cycles rotations every `--period` ms. Each
   change is committed via an atomic plane commit; the sprite
   visibly rotates on screen.
5. **Software pre-rotation for planes without the rotation prop.**
   `Renderer::has_hardware_rotation()` is printed per renderer. On
   bare-metal planes that don't expose the rotation property (common
   on older GPUs and embedded stacks) it's `false`; the rotation
   cycle still works, with `blit_frame` doing the work on the CPU.

## Run

```
sudo cursor_rotate [--theme NAME] [--cursor NAME] [--size N] \
                   [--period MS] [--no-rotate] [/dev/dri/cardN]
```

| Flag           | Default            | Meaning                                     |
|----------------|--------------------|---------------------------------------------|
| `--theme`      | `Adwaita`          | XCursor theme name to resolve against       |
| `--cursor`     | `default`          | Cursor name within the theme                |
| `--size`       | 24                 | Cursor pixel size                           |
| `--period`     | 1000 ms            | Rotation step period                        |
| `--no-rotate`  | (off)              | Skip the rotation cycle (keep `k0` forever) |
| `[device]`     | first card         | DRM device path                             |

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the `seat` group membership is enough.

The example also forwards `Ctrl+Alt+F<n>` from the input source to
libseat, so VT switching works through the same input path that
delivers `Esc` / `q`.
