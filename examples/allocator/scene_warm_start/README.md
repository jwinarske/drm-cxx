# scene_warm_start

A pedagogical demo for the allocator's warm-start optimization and its
FB-only fast path. Three layers (background + indicator + HUD), 60
frames, two HUD translations at frames 30 and 50. The interesting
output is `test_commits` in the `CommitReport` printout:

```
frame  1 test_commits= 1 assigned=3 ... props= 44 fbs=3   ← cold
frame  2 test_commits= 0 assigned=3 ... props=  3 fbs=3   ← fast path
frame  3 test_commits= 0 assigned=3 ... props=  3 fbs=3   ← steady
...
frame 30 test_commits= 1 assigned=3 ... props=  4 fbs=3
↑ HUD translated 16px — placement changed, one warm TEST re-validates
frame 31 test_commits= 0 assigned=3 ... props=  3 fbs=3   ← steady
```

What's happening:

- **Frame 1 (cold)** writes the full plane state for every assigned
  plane — `CRTC_ID`, `FB_ID`, src/dst rects, zpos, alpha, plus the mode
  blob (`props` is large) — and issues one `DRM_MODE_ATOMIC_TEST_ONLY`
  to validate the first assignment.
- **Frame 2+** change only `FB_ID` (double-buffering), so `props`
  collapses to one per layer **and** `test_commits` drops to **0**: every
  layer's property hash — geometry, format, modifier, everything but
  `FB_ID` / fence — is byte-identical to the assignment the kernel already
  accepted, so the allocator reuses the cached allocation and skips the
  redundant TEST. The real commit's `atomic_check` stays the sole arbiter.
- **Move frames** (30, 50) translate the HUD 16px. `CRTC_X` changes, so
  the property hash moves, the fast path is defeated, and the warm path
  re-validates the (still valid) plane→layer mapping with exactly one
  TEST. The next frame is back on the fast path at 0.

Over these 60 frames the driver's `atomic_check` runs only **3** times
(frame 1 + the two moves) instead of 60 — the steady-state win. A change
the cached assignment can't satisfy — formats, sizes that exceed the
plane's scaling, zpos values outside the plane's range — fails the warm
TEST and kicks `full_search` back in, spiking `test_commits` further.

Buffer contents are static solid fills. Repainting a buffer would also
be a content event but would muddle the signal we're trying to expose.

## Run

```
sudo scene_warm_start [/dev/dri/cardN]
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the `seat` group membership is enough. Output
goes to stderr.
