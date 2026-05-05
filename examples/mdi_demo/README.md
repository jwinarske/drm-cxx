# mdi_demo

A multi-document desktop on top of `drm::csd`. Each "document" is a
glass-themed decoration painted by `drm::csd::Renderer` into a
`drm::csd::Surface` and arrived on its own reserved DRM overlay plane
via `drm::csd::PlanePresenter` (Tier 0).

The example is the headline showcase for the CSD module's Tier 0
presenter — multiple movable, focusable, glass-styled windows on a
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
         [--dump PATH.png] [--presenter {plane|composite|fb}]
         [/dev/dri/cardN]
```

| Flag | Meaning |
|---|---|
| `--docs N` | Initial document count. Default `2`. Capped at the CRTC's available overlay budget — fewer planes mean fewer initial docs, not a hard error. |
| `--theme NAME` | One of `default` (Tier 0 / desktop), `lite` (Tier 1), `minimal` (Tier 2), or a path to a TOML theme file. Path themes layer onto `glass_default` so missing keys inherit. |
| `--dump PATH.png` | Output path for `Ctrl+S` snapshots. When omitted, `Ctrl+S` is a no-op. |
| `--presenter MODE` | Wired for the future Tier 1/2 presenters. v1 only implements `plane`; the other modes log an error and exit. |
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

## What's not in v1

- **Tier 1 (`CompositePresenter`) / Tier 2 (`FramebufferPresenter`).**
  The `--presenter` flag is wired so the override scaffolding is in
  place, but only `plane` actually runs.
- **Per-doc content surfaces.** v1 documents are decoration-only — the
  glass panel + title text is the entire window. The plan calls for a
  separate content buffer; that's a follow-up once Tier 1's damage
  tracker lands and tells us what shape content damage should take.
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
2. **Per-frame overlay commit.** `drm::csd::PlanePresenter::apply`
   writes the decoration overlays into a fresh `AtomicRequest`, the
   demo commits with `PAGE_FLIP_EVENT` and waits for the flip.

The two commits intentionally don't share a request — bg goes through
LayerScene's allocator (which expects a CRTC mode + connector), while
the per-frame path is overlay-only and has no need to revisit modeset
state. The same shape is used by `csd_smoke --presenter=plane`.

## Hardware validation status

Tested on:

- Granite Ridge AMD desktop, amdgpu, 1080p HDMI, kernel 6.11.

Should also work on virtio-gpu (verified by `csd_smoke --presenter=plane`
which uses the same library path) and any modern Intel desktop with
≥ 2 overlays. ARM SoCs with strict plane budgets fall back to a
smaller initial doc count automatically.