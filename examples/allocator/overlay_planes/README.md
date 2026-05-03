# overlay_planes

A small program demonstrating the **raw** native plane allocator —
`drm::planes::Allocator` directly, without `LayerScene`. Opens a
device, enumerates planes via `PlaneRegistry`, builds a small set of
virtual layers with explicit format / dst-rect / zpos, runs
`Allocator::apply()` against an `AtomicRequest`, and prints the
plane-to-layer assignment.

This is the low-level reference. Most application code should reach
for `drm::scene::LayerScene` instead — it owns plane lifetime, dirty
tracking, composition fallback, session pause/resume, and rebind, all
of which this example deliberately does not. Read this when you need
to:

- Understand what the allocator actually returns and how `apply()`
  writes it into an `AtomicRequest`.
- Integrate the allocator into an existing custom commit loop where
  `LayerScene`'s ownership model doesn't fit.
- Debug an allocator-shaped problem (an unexpected `unassigned`,
  a plane that should have matched but didn't) in isolation from the
  scene-level concerns.

For the pedagogical examples that exercise specific allocator features
on top of `LayerScene` (warm start, priority eviction, format
matching), see `scene_warm_start`, `scene_priority`, and
`scene_formats` in this same directory.

## Run

```
sudo overlay_planes [/dev/dri/cardN]
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the `seat` group membership is enough.

The example prints the discovered planes with their possible CRTCs
and the assignment the allocator made for the layers it constructs;
it does not commit a framebuffer, so nothing visible happens on
screen.
