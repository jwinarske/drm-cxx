# signage_player

A minimal two-layer LayerScene demo driven by a TOML playlist. The
background layer is a `GbmBufferSource`; the overlay is a
`DumbBufferSource`. Slides cycle on a timer; ESC quits.

This is the second consumer of `drm::scene::LayerScene` (the first
being `thorvg_janitor`). Together they prove the scene's API survives
two distinct workloads — long-lived static-ish surfaces here vs.
animation-driven pixel mutation there.

## Status

Scaffold. The playlist parser handles the full schema below, but only
`kind = "color"` slides actually render content today. PNG / Blend2D
/ ThorVG renderers and the overlay text painter land in follow-up
commits.

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
```

At least one slide is required.

See `example.toml` for a working sample.
