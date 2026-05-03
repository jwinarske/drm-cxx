# mouse_cursor

Shows a cursor that tracks the mouse via libinput on a DRM/KMS display.
Loads cursors from an installed XCursor theme through the library's
`drm::cursor` module and drives whichever KMS path the chosen CRTC
makes available (dedicated cursor plane, atomic overlay, or legacy
`drmModeSetCursor`).

## Build dependencies

The example has no direct `libxcursor` dep — the library's `drm::cursor`
module does. As long as drm-cxx itself was built with `-Dcursor=enabled`
or `DRM_CXX_CURSOR=ON` (both default to auto-detect), no extra packages
are needed here.

If drm-cxx was configured without cursor support, this example won't
build either. To enable it:

- Debian/Ubuntu: `apt install libxcursor-dev`
- Fedora:       `dnf install libXcursor-devel`
- Arch:         `pacman -S libxcursor`

Then reconfigure the top-level build.

## Theme data

The library's XDG walk inspects `$XCURSOR_PATH` if set, otherwise
`~/.icons`, `$XDG_DATA_HOME/icons`, every `$XDG_DATA_DIRS/icons` entry,
and `/usr/share/pixmaps`. At least one theme with a `cursors/`
subdirectory needs to be installed. `adwaita-icon-theme` is a
reasonable baseline (~11 MB; present on most desktop installs). For a
visually distinct alternative try `bibata-cursor-theme`.

On a server install with no theme data, install one:

    # Debian/Ubuntu
    apt install adwaita-icon-theme

    # Fedora
    dnf install adwaita-cursor-theme

The library's resolver follows each theme's `Inherits=` chain and falls
back through every indexed theme in discovery order. If nothing
matches, the program aborts at startup.

## Running

    mouse_cursor [--theme NAME] [--cursor NAME] [--size N]
                 [--plane ID] [/dev/dri/cardN]

Flags:

| Flag             | Effect                                                                      |
|------------------|-----------------------------------------------------------------------------|
| `--theme NAME`   | Preferred theme. The resolver walks its `Inherits=` chain and falls back through every indexed theme if the requested cursor name isn't present. |
| `--cursor NAME`  | Initial shape (`default` if unspecified). Any name in any installed theme works — alias classes are applied (`pointer`↔`hand2`, `e-resize`↔`right_side`, etc.). |
| `--size N`       | Requested cursor size in pixels. Feeds both `Cursor::load` (which picks the nearest on-disk size libxcursor ships) and `RendererConfig::preferred_size`. |
| `--plane ID`     | Force a specific KMS plane id, bypassing the library's plane selection (CURSOR → OVERLAY → legacy). Useful for exercising a non-default path. |

The device path is optional — if omitted, the first suitable
`/dev/dri/card*` is picked.

The startup log line reports which path the renderer landed on:

    Cursor path: atomic CURSOR plane (plane 31)

or `atomic OVERLAY plane` / `legacy drmModeSetCursor`.

Runtime controls, once the cursor is on screen:

- **Middle mouse button** — advance to the next shape in the cycle.
- **Keys 1..9** — jump directly to that shape (9 clamps to the last entry).
- **Escape or Ctrl-C** — quit.

Cycle order: `default`, `pointer`, `text`, `crosshair`, `help`,
`wait`, `progress`, `not-allowed`, `grabbing`. `wait` and `progress`
are animated and will cycle frames at roughly 60 Hz while selected.

## Permissions

DRM master and input both require access that a normal user session
does not have:

- **Input**: the process needs to read `/dev/input/event*`. Either
  run as root, or add the user to the `input` group and re-login.
  Without this, seat open fails with an immediate error.
- **Video / DRM**: the process needs write access to `/dev/dri/card*`.
  Usually membership in `video` is sufficient on systemd-logind
  systems, but on a headless login (SSH into a VT-less session),
  logind may refuse to hand out a DRM master. Running from a
  physical VT (Ctrl+Alt+F3, log in, run the binary) is the most
  reliable setup.
- **VT switching**: if another process (a compositor, `getty`) holds
  the DRM master on the active VT, atomic commits will fail. Switch
  to a free VT before running.

## Troubleshooting

**Cursor loads but nothing appears on screen.** Another DRM client
(compositor, display manager) probably holds master. Switch VTs
(Ctrl+Alt+F2..F6) and try from there.

**`No XCursor themes found on system search path`.** No directory on
the XDG walk contains a `cursors/` subdir or an `index.theme`. Install
`adwaita-icon-theme` or set `XCURSOR_PATH` explicitly.

**`Failed to load cursor '...' (theme hint '...', size ...)`.** The
cursor name isn't present in any installed theme, even after walking
the `Inherits=` chain and applying the alias table. Try `default`,
`pointer`, or install `adwaita-icon-theme`.

**Theme installed under `~/.icons` not picked up.** The library's
default walk does include `~/.icons`. If a theme there isn't visible,
either its directory is missing a `cursors/` subdirectory (the library
skips cursor-less themes) or `$XCURSOR_PATH` is set to something that
omits it. Unset it or add your path explicitly:

    export XCURSOR_PATH=~/.icons:/usr/share/icons
    mouse_cursor --theme MyTheme

**Cursor looks small on a high-DPI display.** The HW cursor plane is
sized by the driver (`DRM_CAP_CURSOR_WIDTH`). If the theme ships a
smaller pre-rasterized frame than the plane, the library centers it
and the rest of the plane stays transparent. Pass `--size N` to ask
libxcursor for a specific pre-rendered size if the theme ships one,
or switch to a theme with larger variants.

**Different driver, different path chosen.** The library's selection
order is CURSOR plane → OVERLAY plane → legacy. If the startup log
reports the legacy path on a modern driver, it means universal planes
or atomic modesetting failed to enable — check the prior error lines.

## Related

`xcursor_smoke` is a companion binary that resolves a named cursor
through `drm::cursor::Theme` and `drm::cursor::Cursor` and prints
metadata (theme chain, frame count, per-frame `w×h` / hotspot /
delay, `frame_at()` sampling across one cycle). Useful for checking
that a theme has the cursors you expect before running the full
example:

    xcursor_smoke wait Adwaita 64
