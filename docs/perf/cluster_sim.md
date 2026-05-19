# cluster_sim perf — 2026-05 optimization journey

Notes from the May 2026 perf push that took `cluster_sim` from 2.9 fps
to 120 fps on Jetson Orin. Reproducible benchmark numbers, the
techniques applied, and which ones turned out to matter (or not). All
numbers from one Jetson Orin board, DP-1 to an LG UltraGear+ over
DisplayPort, kernel 5.15.148-tegra, NVIDIA proprietary `nvidia_drm`
stack.

## Summary

| Stage | Mode | FPS | Per-frame paint | Notes |
|---|---|---:|---:|---|
| Initial state | 2560×1440@60 | **2.9** | 340 ms | 4-layer ARGB SRC_OVER into a WC-mapped canvas |
| + XRGB layers | 2560×1440@60 | 30 | 16 ms | Hit the pure-opaque fast path in `composite_canvas.cpp` |
| + NEON `blend_into` | 2560×1440@60 | 30 | 11.5 ms | Still WC writes — only 28% reduction |
| + Cached shadow + dirty-rect flush | 2560×1440@60 | 30 | 13 ms (1 ms flush) | Composition itself fast, but inst layer source reads from dumb buffer were WC-bound |
| **+ Combined-instruments layer** | 2560×1440@60 | **60** | 1.2 ms | 2 layers → both on HW planes → zero CPU composition |
| + Mode switch | 1920×1080@120 | **120** | ~1.2 ms | EDID-listed alt mode via `--mode 1920x1080@120` |
| + Dial template cache | 1920×1080@120 | 120 | 0.6 ms | Static rim/face/ticks blitted, only needle repainted |
| + Warn cell cache | 1920×1080@120 | 120 | 0.30 ms | 8 templates (4 cells × lit/dim) memcpy-blitted |
| + Skip per-section paint | 1920×1080@120 | 120 | ~0.2 ms avg | Per-section keys; warn skipped 92% of frames |
| + Info digit cache | 1920×1080@120 | 120 | **0.05 ms avg** | 241 pre-rendered "N km/h" templates (43 MB RSS) |
| + Skip kernel commit | 1920×1080@120 | 120* | unchanged | ~4% of commits suppressed when nothing visibly changed |

\* Display still locked at refresh rate via the usual frames; commit-skip
just saves the userspace work and the kernel ioctl when phases are
quantized-identical.

## Frame jitter capture (30 s @ 1920×1080@120, same binary)

```
                       Frames  Skipped  Mean       Max       "Missed"
DumbBuffer bg          3453    147      8441.7us   35860us   33  (0.96%)
NvBufSurface bg        3441    159      8460.9us   33789us   38  (1.10%)
Re-capture 2026-05-19  1672    —        8477.5us   34681.9us 21  (1.26%)
```

"Missed" includes deliberate commit-skips (interval > 1.5× expected),
not actual dropped frames. Both backends are indistinguishable —
within run-to-run noise.

The 2026-05-19 re-capture is a 14 s DumbBuffer run after commits
`9e0a205` (TEST_ONLY no longer marks `last_committed_`) and `c0e0695`
(sticky-property re-emit minimization in `arm_layer_plane_*`) landed —
confirms no regression in the per-flip pipeline: mean inter-flip and
missed-vblank rate stay within run-to-run noise of the original
captures. Skipped column is "—" because today's run only logged the
combined `missed_vblanks` counter, not the deliberate commit-skips
separately.

## Techniques that helped

1. **XRGB instead of ARGB on composited layers** (`b15dab3` predecessor)
   — `CompositeCanvas::blend_into` has a pure-opaque-copy fast path
   that skips the SRC_OVER read-modify-write. Without this, every
   compositor pixel costs a WC-mapped destination read (~240 ns/px on
   Tegra). Set the source to XRGB (or set every alpha=0xFF on ARGB AND
   add an "opaque" flag to the source if you must) to dodge it.
2. **Merging layers to fit on hardware planes** — the single biggest
   step (30 → 60 fps). Tegra exposes 2 usable planes per CRTC (PRIMARY
   + OVERLAY). Anything past that triggers CPU composition with
   WC-buffer reads, which dominates the budget. If you can collapse
   small UI elements into a single full-screen ARGB layer with mostly-
   transparent pixels, the kernel composes them with the bg in
   hardware.
3. **Higher-refresh EDID mode** — `--mode 1920x1080@120` cuts pixel
   throughput by 56% (≈ resolution drop) and doubles temporal
   smoothness for needle motion. Validated in the LG UltraGear+'s
   advertised mode list.
4. **Template caching for static UI elements** — rim/face/ticks of
   each dial, the rounded-rect cells of each warning indicator (lit
   and dim), and the entire 0..240 km/h digit set. Per-frame paint
   becomes row-by-row memcpy from cached userspace memory into the
   inst buffer's sub-rect. ~9× faster than the equivalent Blend2D
   shaping path.
5. **Per-section skip-paint cache** — track a "last visible state"
   key per UI element (needle endpoint pixel, integer speed_kmh, 4-bit
   lit/dim mask). When the new key matches the prior, the entire paint
   for that section is skipped. Warn benefits the most (~92% skip
   rate); dials very little (~7%).
6. **Commit-skip on all-unchanged frames** — when every section's
   skip-paint cache says nothing to do AND no live producer (camera,
   decoder) has new content, the atomic commit is suppressed entirely.
   A one-shot timerfd re-evaluates one vblank period later. Rare in
   continuous animation (~4% of frames) but architecturally correct
   for idle modes.

## Techniques that did NOT help on this Tegra

- **NEON vectorization of `CompositeCanvas::blend_into`** — helped
  modestly (~28% blend reduction) but reads from WC source buffers
  still dominated. Worth keeping because it's free on cached-source
  paths.
- **Cached shadow buffer + dirty-rect flush in `CompositeCanvas`** —
  also helped modestly. The right fix architecturally; the proximate
  bottleneck (Tegra WC source reads) made it look smaller.
- **Custom CVT-RB2 modelines past EDID** — synthesized at runtime, get
  EINVAL from the Tegra DRM driver. Escape is the `video=` kernel
  cmdline; see `scripts/jetson_force_mode.sh`. Untested past 120 Hz.
- **Panel Self Refresh** — proprietary `nvidia_drm` doesn't expose
  DPCD/AUX; PSR isn't reachable.
- **NvBufSurface-backed bg layer** — no measurable difference on
  cluster_sim because the scene avoids CPU composition entirely.
  Library piece is correct and useful for any future scene that
  legitimately exercises `compose_unassigned` or a CPU readback path
  (camera inspection, motion detection).

## What surprised us

- **Dumb-buffer source READS** were the steady-state bottleneck for
  most of the journey, not writes. Blend2D + NEON writes hit WC
  streaming-store bandwidth fast enough; consumer-side reads (in
  composition) stall hard. Verified by patching out the load in the
  NEON inner loop — blend time dropped from 12 ms to 1.2 ms with
  reads disabled.
- **Allocator scoring on planes without `zpos`** — bipartite matching
  tied two non-composition layers when neither plane exposed `zpos_min`,
  picked an arbitrary assignment, and one of cluster_sim's runs landed
  bg on OVERLAY (natural-top on Tegra) covering the instruments on
  PRIMARY. The fix in `src/planes/allocator.cpp` treats explicit
  `layer.zpos=0` as PRIMARY-affinity and `layer.zpos>0` as
  OVERLAY-affinity when planes have no kernel zpos.
- **Skip-paint hit rates** asymmetric across UI elements — warn 92%,
  info 27%, dials only 7% at 120 Hz. Caching the cheap-to-render but
  rarely-changing parts (warn cells) gave less perf win than caching
  the expensive-to-render but-occasionally-changing parts (info
  digits), even though the skip-paint rates would suggest the opposite.

## How to reproduce

```sh
# Default — preferred EDID mode (2560×1440@60 on this hardware)
./buildDir/examples/scene/cluster_sim/cluster_sim /dev/dri/card1

# Higher refresh
./buildDir/examples/scene/cluster_sim/cluster_sim --mode 1920x1080@120 /dev/dri/card1

# With NvBufSurface bg (requires DRM_CXX_HAS_NVBUFSURFACE build)
CLUSTER_SIM_NVBUF=1 ./buildDir/examples/scene/cluster_sim/cluster_sim --mode 1920x1080@120 /dev/dri/card1

# Frame-jitter capture (dumps summary at clean exit)
CLUSTER_SIM_FRAME_JITTER=1 ./buildDir/examples/scene/cluster_sim/cluster_sim /dev/dri/card1
```

## Sibling: `cluster_sim_vulkan`

`examples/scene/cluster_sim_vulkan/` exists as the GPU-rendered
sibling. Current state is MVP (`vkCmdClearColorImage` bg + one CPU
Blend2D-painted dial, ~59 fps at 2560×1440). Feature parity with
`cluster_sim`'s visuals — radial bg gradient, both dials, info
readout, warning strip — needs a real render-pass + graphics-pipeline
path that composites cluster_sim's pre-rendered CPU templates as
Vulkan sampled images, plus shader-rendered needles. Pending after
`glslang-tools` was installed. See in-tree commits
`23982c0 cluster_sim_vulkan: MVP with Vulkan bg + CPU instruments`
and the follow-up that adds the shader pipeline.

The win once feature parity lands isn't "more fps" — `cluster_sim` is
already 120 fps locked. It's:
- Demonstrates the full Vulkan-rendered scene-layer pipeline end-to-
  end on this Tegra (proves the path Vulkan apps would actually take).
- Frees CPU entirely from the instruments paint (template upload
  happens once, all per-frame composition is on the GPU).
- Opens up cheap shader effects the CPU path can't do — subpixel-AA
  rim, SDF-based glow on the needle, etc.

## Commits

Squashed reference:
```
23982c0 cluster_sim_vulkan: MVP with Vulkan bg + CPU instruments
eb633e2 scene: NvBufSurfaceSource — LayerBufferSource backed by L4T NvBufSurface
e42c928 cluster_sim: optional NvBufSurface bg layer (CLUSTER_SIM_NVBUF=1)
c19ba86 cluster_sim: cache 241 center-info speed text templates
dce4b29 docs/hardware.md: Tegra L4T quirks from 2026-05-17/18 perf work
a24b413 cluster_sim: skip the kernel commit on all-unchanged frames
b15dab3 cluster_sim: skip per-layer paint when its visible state is unchanged
c593c6a cluster_sim: cache warning-indicator cells, blit instead of redraw
11dd5a0 scripts: jetson_force_mode.sh — inject video= kernel cmdline via extlinux
db8e08d cluster_sim: env-gated frame-jitter capture + signal handler
893cacb cluster_sim: cache static dial template, redraw only the needle
23e5bc1 cluster_sim: --mode falls back to a synthesized CVT-RB2 modeline
91b557c cluster_sim: --mode WxH[@Hz] CLI flag to pick a non-preferred mode
7f01bf6 scene: GstAppsinkSource version-gate <gst/video/video-info-dma.h>
1ba171f cluster_sim: merge speedo/tach/info/warnings into one full-screen layer
797e27e scene: cached shadow buffer + dirty-rect flush + NEON fast paths in CompositeCanvas
e3aaaad planes: zpos-affinity scoring for planes without kernel zpos prop
```
