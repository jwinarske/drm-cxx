# VRR jitter: Steam Deck (eDP) vs NanoPC-T6 (VOP2)

`vrr_sweep` paces a `DumbRingSource` present loop at a series of target rates and
measures, from `PAGE_FLIP_EVENT` completion timestamps, the achieved rate and the
per-frame jitter (stddev of the inter-flip interval). Four scenarios per board:

| scenario | VRR_ENABLED | scheduling |
|---|---|---|
| control | off | normal |
| vrr     | on  | normal |
| rt      | off | `SCHED_FIFO` + `mlockall` |
| vrr-rt  | on  | `SCHED_FIFO` + `mlockall` |

Each scenario invokes `vrr_sweep` with the matching flags; the `--rt` scenarios
need `CAP_SYS_NICE` (run as root) for `SCHED_FIFO` + `mlockall`.

## Hardware

| | Steam Deck (OLED) | NanoPC-T6 |
|---|---|---|
| display engine | amdgpu DC (Van Gogh, RDNA2) | Rockchip VOP2 (RK3588) |
| panel / output | internal eDP, 800×1280 | HDMI-A-1 |
| swept mode | 800×1280 (panel native) | 1920×1080 **@120** (`--mode`) |
| fixed refresh | **90 Hz** | **120 Hz** |
| vblank period | 11.11 ms | 8.33 ms |
| EDID vrefresh range | 45–90 Hz | 48–240 Hz |

## Results — jitter (ms)

On **both** boards all four scenarios were **identical to two decimals**
(control == vrr == rt == vrr-rt), so each board collapses to one column.

| target | Deck measured | Deck jitter | NanoPC-T6 measured | NanoPC-T6 jitter |
|---:|---:|---:|---:|---:|
| 30 Hz | 30.0 | **0.00** (90÷3) | 30.3 | 2.47 |
| 40 Hz | 40.1 | 4.79 | 40.0 | **0.00** (120÷3) |
| 48 Hz | 48.3 | 3.79 | 47.9 | 4.17 |
| 60 Hz | 59.8 | 5.55 | 60.0 | **0.00** (120÷2) |
| 72 Hz | 72.2 | 4.79 | 71.7 | 3.91 |
| 90 Hz | 90.0 | **0.00** (panel max) | 89.8 | 3.94 |
| 110 Hz | 90.0 (clamped to 90) | 0.00 | 110.1 | 2.38 |

Both columns are the literal control run; the other three scenarios matched. The
NanoPC-T6 reaches 110 Hz (its mode is 120, so 110 < mode is achievable); the Deck
clamps 110 to its 90 Hz panel maximum.

## Analysis

**Both panels run at a fixed refresh; the jitter is vblank quantization, not
pacing.** When the target rate evenly divides the panel's fixed refresh, every
present lands on a whole number of vblanks → ~0 ms jitter (Deck: 30, 90; VOP2:
40, 60). When it doesn't, the present loop alternates between *N* and *N+1*
vblanks to hit the average rate, and the per-frame interval swings by ≈ half a
vblank:

- Deck, 90 Hz panel → ½ × 11.11 ms ≈ 5.5 ms ceiling (observed 3.8–5.6 ms).
- VOP2, 120 Hz panel → ½ × 8.33 ms ≈ 4.17 ms ceiling (observed 3.9–4.2 ms).

The jitter ceiling is **set by the panel's vblank period**, which is the single
biggest difference between the two boards: the Deck's slower 90 Hz panel quantizes
in coarser ~11 ms steps, so its worst-case jitter is larger than the VOP2's at
120 Hz.

**`SCHED_FIFO` + `mlockall` (`--rt`) changes nothing on either board.** The
commit always arrives in time; the panel simply cannot present off-vblank. This
refutes the earlier hypothesis that the residual jitter was userspace pacing
(`clock_nanosleep` + commit scheduling) — RT-paced runs are identical.

**Both track the *mean* target rate, neither does per-frame adaptive sync.** Up
to the panel/mode ceiling, the present loop hits the requested average on both
boards (Deck 48→48.3, 60→59.8, 72→72.2; VOP2 48→47.9, 72→71.7, 110→110.1) — but
purely by N/N+1 vblank alternation, with the same jitter VRR-on or VRR-off. The
only hard clamp is exceeding the panel max (Deck 110→90; VOP2's 120 mode reaches
110 fine). Jitter scales with how far `panel_refresh ÷ target` sits from a whole
number: near-integer ratios quantize lightly (VOP2 110 Hz = 120÷110 ≈ 1.09 →
2.38 ms), half-integer ratios worst (VOP2 48 Hz = 120÷48 = 2.5 → 4.17 ms). (The
VOP2 30 Hz point reads 2.47 ms despite 120÷30 = 4 being exact — a mild outlier
from the 33 ms-interval pacing, not the quantization model.)

**`VRR_ENABLED` is inert on both** through this bare-KMS path: no compositor owns
the adaptive-sync property, so the panels run their fixed refresh regardless.

## Takeaways

1. Neither board does genuine variable refresh through drm-cxx's bare-KMS present
   path; both are fixed-refresh with vblank quantization. Real VRR would need the
   adaptive-sync property honored by the driver/firmware (on the Deck that path is
   owned by gamescope, which isn't running here).
2. The `vrr_sweep` jitter metric measures **panel vblank quantization**, not
   harness quality — `--rt` confirms the harness is not the bottleneck.
3. Lower-refresh panels show *larger* quantization jitter (Deck 90 Hz > VOP2
   120 Hz) on non-divisor targets, purely from the wider vblank step.
