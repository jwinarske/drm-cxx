# xcursor-mini

A minimal, X11-free subset of [libXcursor](https://gitlab.freedesktop.org/xorg/lib/libxcursor)
**1.2.3**, vendored and **modified** from its `src/file.c` (subset extraction +
X11 removal; see "Dropped" below).

- **Upstream:** libXcursor 1.2.3 — https://gitlab.freedesktop.org/xorg/lib/libxcursor
- **License:** `HPND-sell-variant` (SPDX) — *not* drm-cxx's MIT. The full upstream
  Keith Packard / Thomas E. Dickey notice is in [`LICENSE`](LICENSE) and reproduced
  verbatim at the top of `xcursor.h` and `xcursor_file.c`, which also carry SPDX tags.

## What this is

Only the `.xcursor` binary-format **file loader** — the call chain reachable
from `XcursorFilenameLoadImages()`. Nothing else from libXcursor is included.

## Why

Upstream `src/file.c` is itself X11-free, but it `#include`s `xcursorint.h`,
which transitively pulls in `<X11/Xlib.h>`, `<X11/cursorfont.h>`,
`<X11/extensions/Xrender.h>` and (optionally) `Xfixes`. Linking the real
`libXcursor.so` therefore drags in the whole Xlib/Xrender chain just to decode
a cursor file. drm-cxx runs on bare DRM/KMS with no X server, so it only needs
the decoder.

This vendored subset is compiled into drm-cxx and **statically linked to fully
replace system libxcursor** — no `libXcursor.so`, no X11 headers, no X11
runtime dependency.

## Files

- `xcursor.h` — public header: the `XcursorBool` / `XcursorUInt` / `XcursorDim`
  / `XcursorPixel` typedefs, the `XcursorImage` / `XcursorImages` / `XcursorFile`
  structs (field layout/types identical to upstream), and the two exported
  function declarations. Safe to include from C and C++.
- `xcursor_file.c` — the loader implementation extracted from upstream
  `src/file.c`. Includes are limited to `<stdint.h> <stdio.h> <stdlib.h>
  <string.h>` plus the local `xcursor.h`. The `.xcursor` format structs
  (`XcursorFileHeader`, `XcursorFileToc`, `XcursorChunkHeader`) and the
  format constants live here.

## Exported symbols

Only two symbols are exported; every other function from the upstream call
chain (image/images create+destroy helpers, the byte/uint/header/TOC/chunk
readers, `_XcursorReadImage`, `_XcursorResizeImage`, the stdio `XcursorFile`
adapter, `XcursorXcFileLoadImages`, `XcursorFileLoadImages`) is `static`.

- `XcursorImages *XcursorFilenameLoadImages(const char *file, int size);`
- `void XcursorImagesDestroy(XcursorImages *images);`

## Security note

The `.xcursor` files this code parses are attacker-controlled. Every bounds,
count, and overflow check from upstream `file.c` is preserved **verbatim**
(magic check, header-length floor, `ntoc <= 0x10000` cap, per-image
`XCURSOR_IMAGE_MAX_SIZE` / non-zero / hot-spot-in-bounds checks, chunk
type/subtype cross-checks). Do not weaken them.

## Dropped from upstream `file.c`

Everything outside the load-images path was removed:

- Comment chunks: `XcursorComment*`/`XcursorComments*` create/destroy,
  `_XcursorReadComment`, `_XcursorCommentLength`.
- The entire write/save path: `_XcursorWriteUInt`, `_XcursorWriteBytes`,
  `_XcursorWriteFileHeader`, `_XcursorFileWriteChunkHeader`,
  `_XcursorWriteImage`, `_XcursorWriteComment`, `XcursorXcFileSave`,
  `XcursorFileSave*`, `XcursorFilenameSave*`, `_XcursorImageLength`,
  `_XcursorFileHeaderLength`.
- Other load entry points not needed by drm-cxx: `XcursorXcFileLoadImage`,
  `XcursorXcFileLoadAllImages`, `XcursorXcFileLoad`, `XcursorFileLoadImage`,
  `XcursorFileLoadAllImages`, `XcursorFileLoad`, `XcursorFilenameLoadImage`,
  `XcursorFilenameLoadAllImages`, `XcursorFilenameLoad`, and the
  `_Xcursor*` resize/`FILE` variants used only for `XCURSOR_RESIZED`.
- `XcursorImagesSetName` (name is never set by the kept path).
- The `DEBUG_XCURSOR` tracing machinery (`enterFunc` / `returnAddr` / etc.,
  from `xcursorint.h`); kept functions return values directly.
- All X11/Xlib/Xrender/Xfixes/`Display` types and includes.
- `_XcursorReadBytes` — only reachable from the dropped comment-loading
  path, so removed here (the image-loading chain never calls it).
