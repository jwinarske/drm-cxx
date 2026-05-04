# csd_smoke

Throwaway hardware-validation harness for `drm::csd` ahead of Step 5 of the Blend2D integration plan (the three CSD presenters). Renders one glass-themed window decoration on top of a gradient background and arms the resulting composition for a few seconds.

This example is **not part of the supported example surface** — it exists to shake out the rendering chain on real hardware before the production Plane presenter (Tier 0) gets built atop it. Once the presenter lands, this example is expected to be retired or absorbed into a more substantial demo.

## Usage

```
csd_smoke [--seconds N] [--theme {default|lite|minimal}] [--png OUT.png] [/dev/dri/cardN]
```

| Option | Default | Description |
|---|---|---|
| `--seconds N` | 5 | Hold the composition on screen for N seconds before exiting. |
| `--theme NAME` | `default` | Built-in theme: `default` (Tier 0 glass-default), `lite` (Tier 1 glass-lite), `minimal` (Tier 2 glass-minimal). |
| `--png OUT.png` | — | After arming, snapshot the CRTC via `drm::capture` and write a PNG. Useful for headless regression checks; the on-screen composition still holds for `--seconds` afterward. |
| `/dev/dri/cardN` | prompt | DRM device. Omit to be prompted via `select_device`. |

## Preconditions

- Run from a TTY (Ctrl-Alt-F3) or a libseat session — a Wayland / X session holding DRM master will reject `enable_atomic` with `EACCES`.
- Build must have `DRM_CXX_HAS_BLEND2D=1` (the gate that pulls in `drm::csd`).

## What this validates

1. `csd::Surface::create` returns a CPU-mappable, KMS-scanout-ready ARGB8888 buffer. Both the GBM and dumb paths are exercised depending on the host; the chosen path is logged at startup.
2. `csd::Renderer::draw` paints the glass theme into the Surface's mapping (shadow halo, panel gradient, specular highlight, frosted noise, title text, traffic-light buttons, rim) without crashing on a real allocator.
3. The painted Surface's FB ID reaches an overlay plane via `drm::scene::LayerScene`'s allocator — proves the (format, modifier, zpos) story is consistent with what the production Plane presenter (Tier 0) will need.
4. Optional `--png` round-trip: `drm::capture::snapshot` reads back the on-screen composition and encodes it. The PNG should match what's on the monitor.

## Expected output

```
csd_smoke: 1920x1080@60Hz on connector 87 / CRTC 56
csd_smoke: Surface backing = GBM
csd_smoke: renderer.has_font() = true
csd_smoke: theme = glass-default
csd_smoke: page flip on CRTC 56: seq=...
csd_smoke: holding for 5s
```

A horizontal black-to-white gradient fills the screen with an 800×200 glass decoration centered ~80 px from the top, "drm-cxx csd_smoke" rendered as the title.
