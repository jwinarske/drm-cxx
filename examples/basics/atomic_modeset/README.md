# atomic_modeset

The smallest possible LayerScene program: open a device, pick the first
connected connector + its CRTC + preferred mode, paint a single
full-screen XRGB8888 layer with a horizontal black-to-white gradient,
commit it once, wait for the page-flip, and exit.

This is the "hello world" of the example tree. The whole program fits
in one screen of code and exercises the steady-state path:

- `drm::examples::open_and_pick_output()` — libseat-backed device open
  plus connector / CRTC / mode selection (the same helper every other
  example uses).
- `drm::scene::DumbBufferSource::create()` — single CPU-writable
  scanout buffer.
- `drm::scene::LayerScene::create()` + `add_layer()` — register the
  layer.
- `drm::scene::LayerScene::commit(DRM_MODE_PAGE_FLIP_EVENT, &page_flip)`
  — first commit implicitly carries `ALLOW_MODESET` since the scene
  has never bound the CRTC; subsequent commits would not.
- `drm::PageFlip::dispatch()` — drain the page-flip event before exit.

The horizontal gradient is the visual confirmation: pixels reach the
screen at the right offsets, in the right channel order. A wrong-byte
write (BGR vs. RGB) tints the gradient blue or red.

## Run

```
sudo atomic_modeset [/dev/dri/cardN]
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the `seat` group membership is enough. Pass a
specific device path or let `select_device` prompt.

The example exits after the first page-flip event arrives — no input
loop, no animation. If you want to see the gradient stay on screen,
re-run from a TTY rather than a compositor session and let the
display settle.
