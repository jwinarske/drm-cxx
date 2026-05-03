# layered_demo

An interactive pedagogical example exercising the `LayerScene`
mutation API. A static gradient background is always present; up to
eight colored tiles can be added, removed, moved, re-stacked, and
re-shaded at runtime. Each user input results in a scene mutation
followed by a single commit, so the example doubles as a tour of which
API call drives which kind of property write.

Best paired with the `CommitReport` printout (`F1`) for understanding
which mutations cost a re-allocation pass and which fold into the
warm-start fast path.

What this exercises:

- **Add / remove churn.** Number keys toggle a tile's presence; the
  scene's add/remove path runs every keystroke.
- **Position / zpos / alpha mutation.** Arrow keys, `z`/`x`, and
  `[`/`]` mutate a single layer's `DisplayParams` per commit. These
  typically stay in the warm-start fast path — the cached
  plane→layer assignment survives.
- **Format / size invariance.** Layer source dimensions and formats
  don't change once a tile is added, so the example never forces a
  cold full_search; this is deliberate, to keep the keypress→commit
  feedback loop snappy.

## Run

```
sudo layered_demo [/dev/dri/cardN]
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the `seat` group membership is enough.

## Key bindings

| Key             | Action                                                |
|-----------------|-------------------------------------------------------|
| `1` … `8`       | Toggle tile N (also selects it)                       |
| `Tab`           | Cycle selection through the active tiles              |
| Arrow keys      | Move the selected tile by 32 px                       |
| `z` / `x`       | Lower / raise the selected tile's zpos (clamped 3..10)|
| `[` / `]`       | Decrease / increase the selected tile's alpha         |
| `r`             | Reset every tile to its starting position / state     |
| `F1`            | Print scene state + last `CommitReport`               |
| `Esc` / `q`     | Quit                                                  |

Selection is tracked locally — number keys both toggle a tile's
presence and select it, Tab walks the currently-active tiles in
round-robin order. Mutations on an inactive selection are dropped
silently.
