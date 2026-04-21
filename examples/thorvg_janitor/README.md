# thorvg_janitor

A drm-cxx port of ThorVG's Janitor game — a substantive example that
renders ThorVG's software canvas directly onto a KMS primary plane and
uses libinput for keyboard + pointer.

## Controls

- **Left / Right** — steer
- **Up** — thrust
- **A** — fire
- **Escape** — quit

## Rendering path

`drm_template.hpp` replaces upstreams SDL-based `template.h`. The
rendering loop:

1. Opens the DRM device, enables atomic + universal planes, picks the
   connected connector, resolves the preferred mode, and finds a CRTC's
   PRIMARY plane.
2. Allocates **two** ARGB8888 dumb buffers at mode resolution, mmaps
   both for CPU writes, wraps them in DRM framebuffers.
3. Initializes ThorVG's `SwCanvas` pointed at the first back buffer.
4. On the first frame: atomic commit with `MODE_ID` blob + `CRTC.ACTIVE
   = 1` + connector `CRTC_ID` + primary plane's `FB_ID`/`CRTC_X/Y/W/H`/
   `SRC_X/Y/W/H`, flagged with `DRM_MODE_ATOMIC_ALLOW_MODESET |
   DRM_MODE_PAGE_FLIP_EVENT`.
5. On subsequent frames: re-targets the canvas at the *other* buffer,
   draws, commits with `DRM_MODE_ATOMIC_NONBLOCK |
   DRM_MODE_PAGE_FLIP_EVENT`, and rotates.
6. Between commits the loop waits on a `poll()` set containing both the
   libinput seat fd and the DRM fd. Page-flip events are drained via
   `drm::PageFlip::dispatch(0)`; only after the driver reports the flip
   complete does the loop commit the next frame.

Keyboard input: libinput press/release events populate a
`KEY_MAX`-sized bool array keyed by Linux keycode, which `tvggame.cpp`
polls through `tvgdemo::key_pressed(KEY_A)` and friends. The original
SDL `SDL_SCANCODE_*` ↔ drm-cxx `KEY_*` mapping is a straight renamed
(e.g. `SDL_SCANCODE_A` → `KEY_A`).

Pointer input: motion and button events feed the Demo's `motion` /
`clickdown` / `clickup` callbacks.

## Upstream sources

`tvggame.cpp`, `assets.h`, `title.png`, and `LICENSE` are vendored from
<https://github.com/thorvg/thorvg.janitor>, MIT-licensed. `LICENSE`
here is the upstream's text verbatim for attribution.

Modifications to the upstream sources: exactly two hunks in
`tvggame.cpp`:

1. `#include "template.h"` → `#include "drm_template.hpp"`
2. The `SDL_GetKeyboardState` block → `tvgdemo::key_pressed(KEY_*)`
   calls, with a short comment explaining the rename.

Everything else in `tvggame.cpp` and all of `assets.h` / `title.png`
are byte-for-byte identical to upstream.

## Build requirements

- **ThorVG 1.0.4+** — the probe is `pkg_check_modules(... thorvg-1>=1.0.4)`
  (upstream 1.x installs `thorvg-1.pc`, not `thorvg.pc`). Build with at
  least `-Dloaders=svg,ttf -Dengines=cpu -Dbindings=capi`.
- The existing drm-cxx requirements (libdrm, libinput, libudev,
  xkbcommon).

Fedora's `thorvg-devel` is currently 0.15.x (module name `thorvg`, not
`thorvg-1`) and does **not** satisfy this probe. Build from source:

```sh
git clone --depth 1 --branch v1.0.4 https://github.com/thorvg/thorvg.git
cd thorvg
meson setup build -Dengines=cpu -Dloaders=svg,lottie,ttf -Dbindings=capi
sudo meson install -C build        # default prefix: /usr/local
```

The default prefix puts `thorvg-1.pc` under `/usr/local/lib64/pkgconfig/`,
which isn't on pkg-config's default search path on Fedora. Export it
before configuring drm-cxx:

```sh
export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig:$PKG_CONFIG_PATH
pkg-config --modversion thorvg-1   # should print 1.0.4
```

## Build options

Meson:

- `-Dthorvg_janitor=auto` (default) — build if ThorVG is found,
  otherwise skip with a `thorvg_janitor: thorvg not found, skipping`
  message at configure time.
- `-Dthorvg_janitor=enabled` — hard error if ThorVG is missing.
- `-Dthorvg_janitor=disabled` — never build, never probe.

CMake:

- `DRM_CXX_BUILD_THORVG_JANITOR=AUTO` (default) — same auto-probe
  behavior.
- `DRM_CXX_BUILD_THORVG_JANITOR=ON` — hard error if ThorVG is missing.
- `DRM_CXX_BUILD_THORVG_JANITOR=OFF` — never build.

## Permissions

Same as the other KMS examples in this repo: the binary takes DRM
master on the selected CRTC and needs read access to `/dev/input/event*`.

- A free VT is the simplest environment. Ctrl+Alt+F3, log in, run
  `thorvg_janitor`.
- The user must be in the `video` group (for `/dev/dri/card*`) and
  either in `input` or running as root (for libinput). Logging out and
  back in is required after first adding yourself to those groups.
- Running over SSH will not get a seat session because logind does not
  assign a seat to a non-interactive login. The fallback `Device::open`
  path still works if the user has direct device permissions.

## Seat session

When the build finds `libseat`, every example — this one included —
calls `drm::session::Seat::open()` at startup and holds the
seat for the process lifetime. libseat auto-selects between three
backends so the same code works across systemd / non-systemd targets:

- **logind** (DBus to `org.freedesktop.login1`) on systemd hosts.
- **seatd** (Unix-socket protocol) on Alpine, void, and embedded
  targets where seatd is the seat manager.
- **builtin** (no daemon — direct VT and device manipulation) for
  root-owned or tty-owner processes.

Three things this gets us:

- **SIGKILL recovery.** If the janitor is killed with `kill -9`, its
  seat connection drops; the seat provider notices, releases control
  on our behalf, and triggers a VT switch-back to the text console.
  Without this, the scanout stays frozen on the last rendered frame
  until the user `chvt`'s manually.
- **Revocable device fds.** The DRM fd handed out by libseat is
  revoked cleanly on VT-switch-out — ioctls fail rather than racing
  whoever else gets master.
- **VT-switch resume.** The example listens for the seat's
  `enable_seat` event and rebuilds per-fd state (mode blob, dumb
  buffers, framebuffers) on the fresh fd libseat hands back.

If `libseat` isn't found at build time — or no backend is available at
runtime (e.g. SSH login on a systemd box) — `Seat::open()`
returns `std::nullopt` and the example falls back to a plain
`drm::Device::open()`.

### Installing libseat

`libseat` is packaged as `libseat-dev` on Ubuntu 24.04+ and
`libseat-devel` on Fedora 39+. Install from your distro's package
manager — no source build required.

## Running

```
thorvg_janitor [/dev/dri/cardN]
```

If the device argument is omitted, `drm::examples::select_device`
enumerates `/dev/dri/card*` — auto-selecting when exactly one card is
present and prompting otherwise. The engine selector from upstream
(`gl` / `wg` / `sw` as a plain token) is accepted but ignored with a
note — this backend is SW-only. Game logical size
is fixed by `tvggame.cpp` constants (`SWIDTH`/`SHEIGHT`); the
framebuffer is allocated at the display's preferred mode and the game
draws into that coordinate space.
