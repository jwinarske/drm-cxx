# signage_player

A minimal five-layer LayerScene demo driven by a TOML playlist:

- **Background** — `GbmBufferSource`, repainted on slide rotation.
- **Overlay** — `DumbBufferSource`, painted once with static text.
- **Ticker** — optional `DumbBufferSource`, repainted every frame with
  a horizontally scrolling marquee.
- **Clock** — optional `DumbBufferSource` in the top-right corner,
  repainted only when the formatted time string changes (once per
  minute with the default `%H:%M`).
- **Logo** — optional `DumbBufferSource` in the top-left corner,
  painted once from a PNG (and once again on session resume).

The five layers' update cadences (per-slide, once-ever, every-frame,
once-per-minute, never-after-load) are deliberately spread so the
example exercises the dirty surface that `drm::scene::LayerScene`'s
eventual property-minimization pass needs to reason about. ESC quits.

When the hardware can't fit every optional layer (`amdgpu` exposes
just 2 OVERLAY planes for the whole device), `LayerScene`'s Phase 2.3
composition fallback CPU-blends the overflow layers onto a shared
`CompositeCanvas` and presents that canvas on a single OVERLAY plane.
The example doesn't have to shed optional layers up front — every
layer the playlist requests reaches the screen.

If the GPU is so plane-starved that even the canvas can't find a free
plane (no OVERLAY at all, PRIMARY taken by the background), the
overflow layers are silently dropped for that frame and a `log_warn`
entry is emitted. The example continues running rather than failing
hard — watch stderr for `composition fallback found no free plane`
if a layer unexpectedly fails to appear.

This is the second consumer of `drm::scene::LayerScene` (the first
being `thorvg_janitor`). Together they prove the scene's API survives
two distinct workloads — long-lived static-ish surfaces here vs.
animation-driven pixel mutation there.

## Status

Scaffold. The playlist parser handles the full schema below, but only
`kind = "color"` slides actually render slide content today. PNG /
Blend2D / ThorVG slide renderers land in follow-up commits.

The `[overlay]`, `[ticker]`, and `[clock]` text are drawn with Blend2D
when the build is configured with `-DDRM_CXX_BLEND2D=ON` (or `AUTO`
and Blend2D is installed). Without Blend2D the bands still fill with
`bg_color` but no glyphs are drawn — the build degrades cleanly via an
`__has_include` probe in `overlay_renderer.cpp`. A short list of
common Linux font paths is tried in order; if none is present the
bands remain text-free.

## Run

```
sudo signage_player --playlist /path/to/playlist.toml [--hotplug-follow]
```

`--hotplug-follow` opens a `drm::display::HotplugMonitor` and rebinds
the running scene to the new active connector / mode whenever a
hotplug event changes either. The playlist + Blend2D overlay survive
the rebind. Layer buffers keep the dimensions chosen at startup, so a
new mode with different dimensions will letterbox or crop — fine for
demonstrating the rebind primitive; a production app would also
re-allocate the layer sources at the new size.

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the membership in the `seat` group is enough.
The example takes the same `[device]` argument as the other drm-cxx
demos — passes through to `drm::examples::select_device`.

## Playlist schema

```toml
[[slide]]
kind = "color"          # one of: "color", "png", "blend2d", "thorvg"
color = "#ff8800"       # required when kind="color"; "#RRGGBB" or "#RRGGBBAA"
source = "image.png"    # required when kind="png" | "blend2d" | "thorvg"
duration_ms = 2000      # optional, default 2000

[overlay]               # optional
kind = "text"
text = "Hello, signage"
font_size = 32
fg_color = "#ffffff"
bg_color = "#80000000"

[ticker]                # optional, scrolling marquee at the bottom
text = "headlines  ·  more headlines  ·  "
font_size = 24
fg_color = "#ffffff"
bg_color = "#000000c0"
pixels_per_second = 120

[clock]                 # optional, top-right clock badge
format = "%H:%M"        # any strftime; "%H:%M" → repaint once per minute
font_size = 48
fg_color = "#ffffff"
bg_color = "#80000000"
```

At least one slide is required.

See `example.toml` for a working sample.
