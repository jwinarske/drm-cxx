# scene_warm_start

A pedagogical demo for the allocator's warm-start optimization. Three
layers (background + indicator + HUD), 100 frames, two HUD
translations at frames 30 and 50. The interesting output is `props`
in the `CommitReport` printout:

```
frame  1 test_commits= 1 assigned=3 ... props= 32 fbs=3   ← cold
frame  2 test_commits= 1 assigned=3 ... props=  3 fbs=3   ← warm
frame  3 test_commits= 1 assigned=3 ... props=  3 fbs=3   ← steady
...
frame 30 test_commits= 1 assigned=3 ... props=  4 fbs=3
↑ HUD translated 16px — dirty layer, warm TEST still validates
frame 31 test_commits= 1 assigned=3 ... props=  3 fbs=3   ← steady
```

What's happening:

- **Frame 1** writes the full plane state for every assigned plane —
  `CRTC_ID`, `FB_ID`, src/dst rects, zpos, alpha, plus the mode blob.
  `props` is large.
- **Frame 2+** writes only the `FB_ID` deltas required to
  double-buffer scanout. `props` collapses to one per layer.
- **Move frames** (30, 50) write one extra dst-rect prop on the
  dirtied HUD layer. `props` ticks up by 1.

`test_commits` stays at 1 throughout because the allocator's bipartite
preseed solves this 3-layer scene on the first probe and the
`full_search` ladder never has to fall back. A more adversarial scene
(tight format / scaling / zpos constraints) would defeat the preseed
and `test_commits` would spike on frame 1 as the ladder steps through
each rung.

The warm-start invariant is "if the previous assignment is still
valid, re-validate it with one TEST_ONLY commit and write only the
delta properties." The dirty-but-still-warm path is the case worth
attention here: a moved layer is dirty, but the cached plane→layer
mapping is still satisfiable, so the warm TEST passes and the
allocator never re-enters `full_search`. A change the cached
assignment can't satisfy — formats, sizes that exceed the plane's
scaling, zpos values outside the plane's range — would fail the warm
TEST and kick `full_search` back in.

Buffer contents are static solid fills. Repainting a buffer would
also be a dirty event but would muddle the warm-start signal we're
trying to expose.

## Run

```
sudo scene_warm_start [/dev/dri/cardN]
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the `seat` group membership is enough. Output
goes to stderr.
