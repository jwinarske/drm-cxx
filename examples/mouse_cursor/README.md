# mouse_cursor

Shows a cursor that tracks the mouse via libinput on a DRM/KMS display.
Loads cursors from an installed XCursor theme (no vendored sprite) and
supports both hardware cursors and a software fallback via an overlay
plane.

## Build dependencies

- `libxcursor` (runtime + dev headers)
  - Debian/Ubuntu: `apt install libxcursor-dev`
  - Fedora:       `dnf install libXcursor-devel`
  - Arch:         `pacman -S libxcursor`

Both the CMake and Meson builds probe for it via pkg-config and will
fail configuration if it is missing.

## Theme data

libxcursor reads cursors from `$XCURSOR_PATH`, defaulting to
`~/.icons`, `~/.local/share/icons`, and `/usr/share/icons`. At least
one theme needs to be installed. `adwaita-icon-theme` is a reasonable
baseline (~11 MB; present on most desktop installs). For a visually
distinct alternative try `bibata-cursor-theme`.

On a server install with no theme data, install one:

    # Debian/Ubuntu
    apt install adwaita-icon-theme

    # Fedora
    dnf install adwaita-cursor-theme

If the fallback chain cannot find anything, the program aborts at
startup with a list of the themes it tried.

## Running

    mouse_cursor [--sw] [--theme NAME] [--cursor NAME] [--size N]
                 [/dev/dri/cardN]

Flags:

| Flag             | Effect                                                                      |
|------------------|-----------------------------------------------------------------------------|
| `--sw`           | Skip hardware cursor; render via an overlay plane + atomic commits.         |
| `--theme NAME`   | XCursor theme to try first. Falls back to Bibata, Adwaita, default, then libxcursor's built-ins. |
| `--cursor NAME`  | Initial shape (`default` if unspecified). Any name in the installed theme works. |
| `--size N`       | Requested cursor size in pixels. Overrides `DRM_CAP_CURSOR_WIDTH` on the HW path. |

The device path is optional — if omitted, the first suitable
`/dev/dri/card*` is picked.

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

**Hardware cursor renders once but animated shapes freeze.** Some
drivers don't re-read the cursor buffer when only the pixels change,
even with `drmModeSetCursor` called again on the same handle. Workaround:
run with `--sw` to render via the overlay plane. Mention the driver +
kernel version if you file an issue.

**`Failed to load cursor '...' at size ... from any theme`.** The
cursor name you asked for isn't present in any installed theme. Try
`default`, `pointer`, or install `adwaita-icon-theme`.

**Theme installed under `~/.icons` not picked up.** libxcursor honors
`XCURSOR_PATH` only if the variable is set. Export it before running:

    export XCURSOR_PATH=~/.icons:/usr/share/icons
    mouse_cursor --theme MyTheme

**Cursor looks small on a high-DPI display.** The HW cursor plane is
sized by the driver (`DRM_CAP_CURSOR_WIDTH`). If libxcursor returns a
smaller pre-rasterized frame than the plane, the example centers it
(intentionally) and the rest of the plane is transparent. Pass
`--size N` to ask libxcursor for a specific pre-rendered size if the
theme ships one.

## Related

`xcursor_smoke` is a companion binary that loads a named cursor from a
theme and prints metadata (frame count, per-frame `w×h` / hotspot /
delay, `frame_at()` sampling across one cycle). Useful for checking
that a theme has the cursors you expect before running the full
example:

    xcursor_smoke wait Adwaita 64
