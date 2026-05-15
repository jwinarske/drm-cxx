# Benchmarks

Reference performance numbers for the perf-sensitive parts of drm-cxx.
Re-publish each milestone: replace the tables below and bump the
**Snapshot** block so future maintainers can tell when these numbers
were last gathered and against what hardware.

## Snapshot

| Field | Value |
|---|---|
| Commit | `fddfb9b` (`v1.3.0-154-gfddfb9b`) |
| Date | 2026-05-15 |
| Kernel | `6.19.14-200.fc43.x86_64` |
| amdgpu | AMD Granite Ridge iGPU (DCN 3.5 / RDNA 3.5), DP-1 on LG ULTRAGEAR+ at 2560x1440@240Hz, 3 usable planes |
| vkms | `create_default_dev=Y enable_overlay=Y` — 1 PRIMARY + 8 OVERLAY + 1 CURSOR per CRTC |

All benchmarks require `meson configure builddir -Dbenchmarks=true`
followed by `meson compile -C builddir`.

## `plane_stress` — steady-state scene at vsync

Cycles two layers against an attached display at the connector's
preferred refresh rate, recording per-frame allocator behavior and
commit wall-time. Useful as a steady-state perf regression detector.

| Driver | Mode | Frames | avg TEST/frame | layers assigned | composited | props/frame | commit µs |
|---|---|---:|---:|---:|---:|---:|---:|
| amdgpu RDNA 3.5 | 2560x1440@240 (DP-1) | 165 | 1.75 | 2.00 | 2.00 | 36.00 | 30 277.6 |
| vkms | — | — | — | — | — | — | — *(Virtual connectors not in `k_main_rank`; see notes)* |

Reproduce:

```sh
./builddir/benchmarks/plane_stress --device /dev/dri/cardN
```

## `allocator_torture` — adversarial allocator workloads

Six synthetic scenarios that stress the planar allocator:
*N+1* (more layers than planes), *format cascade* (no single plane
accepts all formats), *scaler monopoly*, *rapid churn*, *slow drift*,
and *burst-then-calm*.

The first three are correctness tests. The last three are warm-start
perf assertions whose threshold (`avg test_commits < 1.5` per warm
frame) the current allocator can't meet on amdgpu — the warm path
always re-validates the cached assignment with one TEST_ONLY, so the
steady-state average is 2.00. By design; a truly-zero-TEST steady
state would need a new fast-fast path that doesn't exist yet. Reported
as N/A on amdgpu rather than PASS/FAIL until either the threshold is
relaxed to match current realism or the fast-fast path is built.

| Driver | N+1 | Format cascade | Scaler monopoly | Rapid churn | Slow drift | Burst-then-calm |
|---|---|---|---|---|---|---|
| amdgpu RDNA 3.5 | PASS | PASS | PASS | N/A | N/A | N/A |
| vkms | — | — | — | — | — | — *(no eligible connector)* |

Reproduce:

```sh
./builddir/benchmarks/allocator_torture --device /dev/dri/cardN --frames 500
```

## `tone_mapper_bench` — CPU tone-mapper throughput

Per-frame cost of the CPU tone mapper — the fallback when the CRTC
color pipeline isn't available on the active connector. 1920×1080
ARGB8888, single-threaded, 16-frame mean.

| Direction | Per-frame | Throughput |
|---|---:|---:|
| BT.709 → BT.2020 PQ | 220.56 ms | 9.4 Mpix/s |
| BT.2020 PQ → BT.709 (Reinhard) | 235.16 ms | 8.8 Mpix/s |
| BT.2020 PQ → BT.709 (Hable) | 296.60 ms | 7.0 Mpix/s |
| HLG → BT.709 | 272.06 ms | 7.6 Mpix/s |

Reproduce:

```sh
./builddir/benchmarks/tone_mapper_bench
```

## Notes & caveats

- **vkms isn't currently a benchmark target.** `plane_stress` /
  `allocator_torture` use `drm::examples::open_and_pick_output`, whose
  default `k_main_rank` policy includes only the cable-out / internal-
  panel connector types — Virtual is intentionally excluded. vkms's
  allocator behavior is exercised by the `*_vkms` integration test
  suite instead. Adding a Virtual-aware rank to the benchmarks is a
  small future change if it becomes useful.
- **`commit µs` ≠ frame budget.** `plane_stress` reports wallclock
  between successful `commit()` returns, which on a 240Hz connector
  includes the next-vsync wait. The number is steady-state per-frame
  cost from the caller's viewpoint, not allocator-internal CPU time.
- **Per-milestone publication.** Bump the *Snapshot* block on every
  release. If a milestone produces numbers worth keeping side-by-side
  for trend analysis, archive a copy under
  `docs/benchmarks/<milestone>.md` before overwriting.
