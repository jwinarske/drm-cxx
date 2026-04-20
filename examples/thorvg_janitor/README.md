# thorvg_janitor

A drm-cxx port of ThorVG's Janitor game, intended as a substantive example
that exercises ThorVG's software raster onto a KMS plane.

## Status

**Scaffold only.** The directory exists, the upstream game sources are
vendored in, the build system finds ThorVG when present and skips
gracefully when not, and a placeholder `thorvg_janitor` target builds and
runs — but the DRM backend (`drm_template.hpp`) is a stub that prints a
message and exits. `tvggame.cpp` is not yet wired into the build; it sits
here so the full port can be landed against a known-good upstream tree.

What works today:

- `meson setup` / `cmake` configure cleanly with or without ThorVG.
- `-Dthorvg_janitor=auto` (Meson) or `DRM_CXX_BUILD_THORVG_JANITOR=AUTO`
  (CMake) log whether ThorVG was found.
- The `thorvg_janitor` binary exits with a "scaffold stub" message so
  anyone running it gets a clear signal rather than a crash.

What does not work yet:

- No DRM output. No ThorVG canvas bound to anything. No input handling.
- `tvggame.cpp` and `assets.h` are vendored but not compiled.

## Upstream

Source of `tvggame.cpp`, `assets.h`, `title.png`, and `LICENSE`:
<https://github.com/thorvg/thorvg.janitor>, MIT-licensed. `LICENSE` in this
directory is the upstream's `LICENSE` preserved verbatim for attribution.

## Build requirements (future)

When the backend lands, build requirements will be:

- ThorVG **1.0.4** or newer, built with at least:
  `-Dloaders=svg,ttf,jpg -Dengines=sw`
- The existing drm-cxx requirements (libdrm, libinput, libudev, xkbcommon)

On Fedora:

```
dnf install thorvg-devel
```

On Debian/Ubuntu ThorVG is currently self-built; expect to install from
source or a PPA.

## Build options

Meson:

- `-Dthorvg_janitor=auto` (default) — build if ThorVG is found, otherwise
  skip with a "thorvg not found, skipping" note at configure time.
- `-Dthorvg_janitor=enabled` — hard error if ThorVG is missing.
- `-Dthorvg_janitor=disabled` — never build, never probe.

CMake:

- `DRM_CXX_BUILD_THORVG_JANITOR=AUTO` (default) — same auto-probe behavior.
- `DRM_CXX_BUILD_THORVG_JANITOR=ON` — hard error if ThorVG is missing.
- `DRM_CXX_BUILD_THORVG_JANITOR=OFF` — never build.

## Layout (current)

```
examples/thorvg_janitor/
├── LICENSE              (upstream, MIT)
├── README.md            (this file)
├── assets.h             (upstream, vendored, not compiled yet)
├── drm_template.hpp     (drm-cxx replacement for upstream template.h — stub)
├── main.cpp             (minimal entry point for the scaffold binary)
├── title.png            (upstream asset, referenced by assets.h)
└── tvggame.cpp          (upstream, vendored, not compiled yet)
```

## Layout (target, once the backend lands)

```
examples/thorvg_janitor/
├── LICENSE
├── README.md            (rewritten: runtime controls, hardware notes)
├── assets.h             (compiled)
├── drm_template.hpp     (real backend: DRM device, atomic commits,
│                         page-flip gating, libinput integration)
├── drm_template.cpp     (if any out-of-line impl needed)
├── title.png
└── tvggame.cpp          (compiled, two hunks patched: #include + key polling)
```
