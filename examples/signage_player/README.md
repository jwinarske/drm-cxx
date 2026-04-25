# signage_player

A minimal three-layer LayerScene demo driven by a TOML playlist:

- **Background** — `GbmBufferSource`, repainted on slide rotation.
- **Overlay** — `DumbBufferSource`, painted once with static text.
- **Ticker** — optional `DumbBufferSource`, repainted every frame with
  a horizontally scrolling marquee.

The three layers' update cadences (per-slide, once-ever, every-frame)
are deliberately spread so the example exercises the dirty surface that
`drm::scene::LayerScene`'s eventual property-minimization pass needs to
reason about. ESC quits.

This is the second consumer of `drm::scene::LayerScene` (the first
being `thorvg_janitor`). Together they prove the scene's API survives
two distinct workloads — long-lived static-ish surfaces here vs.
animation-driven pixel mutation there.

## Status

Scaffold. The playlist parser handles the full schema below, but only
`kind = "color"` slides actually render slide content today. PNG /
Blend2D / ThorVG slide renderers land in follow-up commits.

The `[overlay]` and `[ticker]` text are drawn with Blend2D when the
build is configured with `-DDRM_CXX_BLEND2D=ON` (or `AUTO` and Blend2D
is installed). Without Blend2D the bands still fill with `bg_color` but
no glyphs are drawn — the build degrades cleanly via an `__has_include`
probe in `overlay_renderer.cpp`. A short list of common Linux font
paths is tried in order; if none is present the bands remain text-free.

## Run

```
sudo signage_player --playlist /path/to/playlist.toml
```

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
```

At least one slide is required.

See `example.toml` for a working sample.
