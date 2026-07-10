# mdi_demo

A multi-document desktop on top of `drm::csd`. Each "document" is a
glass-themed decoration painted by `drm::csd::Renderer` into a
`drm::csd::Surface` and presented either on its own reserved DRM overlay
plane (`drm::csd::PlanePresenter`, the default) or software-composited
onto the primary plane (`drm::csd::CompositePresenter`).

The example is the headline showcase for the CSD module's presenters —
multiple movable, focusable, glass-styled windows on a
single CRTC, all rendered through the production library types you'd
use to write a real shell.

## What this example demonstrates

| Feature | How |
|---|---|
| Plane-per-decoration presentation | `drm::csd::OverlayReservation` reserves N overlays at startup; `PlanePresenter` writes their `FB_ID` / geometry / zpos every frame. |
| Glass theme rendering | `drm::csd::Renderer::draw` paints into each doc's `csd::Surface` on focus / hover / theme changes. |
| Focus stacking | The shell maps doc → reserved plane in z-order each frame; bringing a doc forward rewrites the FB_IDs across slots without re-reserving anything. |
| Title-bar drag | Pointer wiring updates `Document::{x,y}`, picked up the next frame as `CRTC_X / CRTC_Y` writes — no decoration repaint. |
| Theme swap | `--theme {default,lite,minimal}` selects a built-in; `--theme PATH` loads a TOML override layered on the desktop default. |
| CRTC snapshot | Ctrl+S calls `drm::capture::snapshot` and writes the result to `--dump`. |

## Usage

```
mdi_demo [--docs N] [--theme {default|lite|minimal|PATH}]
         [--dump PATH.png] [--presenter {auto|plane|composite|fb}]
         [/dev/dri/cardN]
```

| Flag | Meaning |
|---|---|
| `--docs N` | Initial document count. Default `2`. Capped at the CRTC's available overlay budget — fewer planes mean fewer initial docs, not a hard error. |
| `--theme NAME` | One of `default` (desktop), `lite`, `minimal`, or a path to a TOML theme file. Path themes layer onto `glass_default` so missing keys inherit. |
| `--dump PATH.png` | Output path for `Ctrl+S` snapshots. When omitted, `Ctrl+S` is a no-op. |
| `--presenter MODE` | `auto` (default) lets `csd::probe_presenter` pick `plane` or `composite` from the plane budget (see below); `plane` gives each decoration its own overlay; `composite` software-blends every decoration onto the primary plane; `fb` blits into `/dev/fb0` (legacy framebuffer / no-KMS targets). |
| `/dev/dri/cardN` | DRM device. `select_device` prompts when omitted. |

## Controls

| Action | Input |
|---|---|
| Move a doc | Left-click + drag the title bar. |
| Focus a doc | Left-click anywhere inside it (focused doc rises to the top of the stack). |
| Close focused doc | Left-click the close (red) traffic-light button, or `Ctrl+W`. |
| Cycle focus | `Ctrl+Tab` (sends focused doc to the back). |
| Spawn a new doc | `Ctrl+N`. Capped at the plane budget; the demo logs and ignores presses past the cap. |
| Snapshot CRTC | `Ctrl+S` → writes to `--dump`. |
| Quit | `Esc`, `Q`, or `Ctrl+C`. |
| Switch VT | `Ctrl+Alt+F<n>` (libseat sessions only). |

## Preconditions

Same as every other CSD example:

- Run from a TTY (Ctrl+Alt+F3) or a libseat session — a Wayland / X
  session holding DRM master will reject the atomic commit with EACCES.
- Build with `DRM_CXX_HAS_BLEND2D=1` (the gate that pulls in `drm::csd`).

## Presenter selection (`--presenter=auto`, the default)

`csd::probe_presenter` picks between the two KMS presenters from what the
CRTC can give it: it tries to reserve one overlay per document, and if it
gets **all** of them the plane presenter wins (every window on its own
plane); otherwise it falls back to the composite presenter on the primary
(every window still shows, software-blended — no per-window plane limit).
The `fb` presenter is not a candidate here (it needs a non-master device);
select it explicitly with `--presenter=fb`. The probe borrows the overlay
`OverlayReservation` back to the caller to hold for the plane pick, since
the `PlanePresenter` writes to those leases for its whole lifetime.

## Composite mode (`--presenter=composite`)

`CompositePresenter` blends every decoration into one double-buffered
ARGB8888 canvas (`scene::CompositeCanvas`) and scans that canvas out from
the CRTC's **primary** plane — no per-decoration overlay. It's the path
for plane-starved hardware (mid-range ARM: i.MX8 DCSS, RK3399 VOP, Mali
Komeda) that can't give each window its own plane. The blend is
damage-tracked: each frame clears only the previous frame's footprint and
copies only the touched scanline bands to scanout, so an idle desktop
costs almost nothing. Decoration count is bounded by `--docs`, not by the
plane budget.

The desktop background is solid black in v1 (a gradient / wallpaper under
the composited decorations is a follow-up — it needs the canvas to carry
a persistent background source). Everything else — drag, focus stacking,
theme swap, snapshot — behaves identically to Plane mode.

## Framebuffer mode (`--presenter=fb`)

`FramebufferPresenter` has no KMS at all: it opens `/dev/fb0`, mmaps it,
and blits the composited decorations straight into that memory (reusing
`scene::CompositeCanvas`'s blend + row-conversion helpers). It's the
fallback for legacy framebuffers and no-plane targets — simplefb, a
bootloader/VESA fb, or the DRM-emulated fbcon when no KMS plane is free.

This mode takes a different bring-up (`run_fb` in `main.cpp`): it opens
the DRM card with a plain `open()` wrapped by `Device::from_fd` — a
**non-master** client — purely to allocate the decoration `Surface`s.
`CREATE_DUMB` and `AddFB2` don't need DRM master, and staying non-master
is what keeps the kernel fbcon live; `Device::open()` would call
`drmSetMaster` and suspend fbcon, so the mmap writes would land on an
inactive framebuffer. No modeset, no cursor plane, no atomic commit — the
blit into `/dev/fb0` *is* the present. Run it from a console VT with
nothing else holding DRM master. v1 is a single-buffered full-frame blit;
`FBIOPAN_DISPLAY` double-buffering and blit-side damage are follow-ups.

Validated on a Raspberry Pi 5: `/dev/fb0` is a 1280×1440 RGB565
DRM-emulated fbcon; `--presenter=fb` opens it, allocates three glass
decorations non-master from `card0`, and blits them in — the live
framebuffer's contents change (fbcon stays active) rather than freezing
under a DRM master.

## What's not in v1
- **Per-doc content surfaces.** v1 documents are decoration-only — the
  glass panel + title text is the entire window. A separate content
  buffer is a follow-up, once the composite damage tracker tells us what
  shape content damage should take.
- **Resize.** The traffic-light Maximize button is painted but inert;
  resize requires reallocating the decoration `Surface`, which lives
  alongside the still-undecided rebind story for csd.
- **Minimize.** Same disposition — painted, inert.
- **Focus animation cross-fade.** Plan §256 calls for an 8-frame
  cross-fade between two cached shadow patches on focus change. v1
  hard-flips: one frame later the rim is the new color. The shadow
  cache supports both elevations already; the animation hook lands
  with the animation engine in plan step 7.
- **Live theme reload.** `--watch` from the plan is unimplemented.
- **Mid-flight session pause/resume.** Pause works (input fds get
  released); resume bails out and asks the user to re-run. Resuming
  in place needs csd::Surface rebuild against the fresh fd plus a
  PlanePresenter rebuild — same shape as cursor::Renderer's resume.

These are listed in the order of how often they come up; the next two
are the most common asks.

## Implementation notes

The example holds two atomic-commit paths:

1. **Modeset commit** at startup. `drm::scene::LayerScene` arms the bg
   primary plane and the CRTC's mode. One `commit(PAGE_FLIP_EVENT)`,
   wait for the flip, done. The bg's primary-plane state survives
   subsequent overlay-only commits because we never write any of its
   properties — atomic state is per-property, last-write-wins.
2. **Per-frame commit.** `Presenter::apply` writes into a fresh
   `AtomicRequest`, the demo commits with `PAGE_FLIP_EVENT` and waits for
   the flip. In Plane mode that's decoration overlays; in Composite mode
   it's the primary plane's `FB_ID` (the freshly-blended canvas). Either
   way the bg modeset from step 1 isn't revisited.

The two commits intentionally don't share a request — bg goes through
LayerScene's allocator (which expects a CRTC mode + connector), while
the per-frame path has no need to revisit modeset state. In Composite
mode the canvas takes over the primary from frame 1, so the startup
gradient shows for a single frame before the black-desktop canvas
replaces it. The same shape is used by `csd_smoke --presenter=plane`.

## Hardware validation status

Tested on:

- Granite Ridge AMD desktop, amdgpu, 1080p HDMI, kernel 6.11.

Should also work on virtio-gpu (verified by `csd_smoke --presenter=plane`
which uses the same library path) and any modern Intel desktop with
≥ 2 overlays. ARM SoCs with strict plane budgets fall back to a
smaller initial doc count automatically.