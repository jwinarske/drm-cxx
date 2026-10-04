# Hardware compatibility & test setup

This document covers what drm-cxx has been validated against, how to
set up a real-hardware test rig, and the driver-specific quirks worth
knowing about before chasing a regression. Hardware coverage grew
incrementally — most quirks are codified in the relevant module's
unit tests or worked around in the library; this page exists so you
can audit "what runs where" without having to re-derive it from
commit history.

If you're adding a new piece of hardware to your fleet, the
recommended workflow is:

1. Run the diagnostic tools in [§ Diagnostic tools](#diagnostic-tools)
   to characterize the card.
2. Cross-reference [§ Validated GPUs](#validated-gpus) for known-good
   configurations and [§ Driver quirks](#driver-quirks) for known
   gotchas on this driver family.
3. Pick an example from [§ Reference examples](#reference-examples)
   that exercises the relevant code path end-to-end against your card.

---

## Validated GPUs

The table below lists hardware drm-cxx has been live-validated on, with
the kernels and the work items exercised. "Validated" means real
scanout — `drmModeAtomicCommit` returning success with visible pixels
on a physical display, not just `TEST_ONLY` acceptance.

| Driver       | Hardware                            | Kernel              | Work exercised |
|--------------|-------------------------------------|---------------------|----------------|
| `amdgpu`     | RDNA2 desktop (1× DP-1, 1× HDMI-A-1)| 6.19.14-200.fc43    | signage_player, cluster_sim, hdr_demo, csd_smoke, capture_demo, cursor_rotate, dual_display, video_wall_multi, egl_scene, vulkan_scene |
| `amdgpu`     | RDNA1, integrated APU               | 6.11+               | Multi-CRTC `SceneSet::test`, dual_display |
| `i915`       | Skylake / Kaby Lake iGPU            | 6.x                 | Single-CRTC scene paths; cursor 64×64 floor (see quirk) |
| `vkms`       | Configfs-spawned (kernel ≥6.11)     | 6.11+               | Most integration tests; multi-CRTC via `scripts/vkms_dual.sh` |
| `vicodec`    | M2M FWHT codec endpoints            | 6.x                 | `V4l2DecoderSource` NV12 + P010 paths |
| NVIDIA       | Quadro RTX A2000 (NVRM 535.288.01) | 6.x                 | `EglStreamSource` end-to-end (acquire, atomic test, scene wiring). Desktop driver gap noted in quirks. |
| Tegra        | Jetson Orin (nvgpu)                | 5.15.148-tegra (L4T)| Visible scanout on DP-1: `atomic_modeset`, `layered_demo`, `signage_player`, `hdr_demo`, `stream_demo` (CPU fallback tier), `v4l2_camera_demo --mode mmap`, `cursor_rotate`, `vulkan_display` (display enumeration), `vulkan_scene` (60fps vblank-locked), all `overlay_planes` / `scene_*` enumeration runs. Driver gaps noted in quirks. |
| `amdgpu`     | RDNA2 Van Gogh APU (Steam Deck)     | 6.11.11-valve29     | `drm::fmt`: **real DCC `tile27 dcc=1 retile` scanout** via both EGL and Vulkan (`egl_offload_scanout`, `vulkan_offload_scanout`). See [§ Hardware compression](#hardware-compression-format-modifiers). |
| `panfrost`   | Mali-G52 (Radxa Zero 3 / RK3566)    | 6.1.84-rk2410       | `drm::fmt`: **real ARM AFBC `16x16\|YTR\|SPARSE` scanout** via EGL on a VOP2 cluster plane (`egl_offload_scanout`). |
| `rockchip-drm` + libmali | Mali-G610 (RK3588 / NanoPC-T6) | 6.1.141      | `drm::fmt`: AFBC `IN_FORMATS` decode validated; compressed offload falls back to LINEAR (libmali limitation — see quirks). |
| `vc4`        | VideoCore (RPi4 / RPi5)             | 6.12.75-rpt         | `drm::fmt`: Broadcom SAND / VC4-T-tiled `IN_FORMATS` decode validated; LINEAR scanout. |
| `vc4`        | VideoCore IV (RPi Zero 2 W)         | 6.18.33-rpt (trixie)| Software/dumb scanout @ 1600x900, **hardware GLES present** (`egl_scene` ~60fps, `gl_present`, `gbm_surface_scanout` VC4-T-tiled), single-plane→56-plane scene/composition, present-path profiling. No hardware Vulkan (llvmpipe only). |
| `imx-drm`    | i.MX8M Mini LCDIF (Nitrogen8M Mini, NXP BSP) | 6.1.22 (Yocto) | `drm::fmt`: **no-`IN_FORMATS` fallback** — `FormatTable::from_plane` returns `ENOENT`, caller assumes LINEAR-only (legacy fourccs, no modifier surface). |
| `imx-drm`    | i.MX93 LCDIFv3 (FRDM-IMX93, NXP BSP) | 6.18.2 (Yocto) | Software/dumb-buffer scanout @ 720p60 (`software_present`, `damage_present`, `ring_present`, `idle_present`, `atomic_modeset`), single-plane CPU composition (`minimal_kms_probe`, `scene_*`, `layered_demo`), present-path profiling matrix. No GPU. |
| `imx-drm` + `galcore` | LCDIFv3 ×3 + Vivante GC7000UL (PANZER-PLUS Edge AIoT Computer, NXP BSP) | 6.6.23 (Yocto) | Software/dumb scanout @ 1080p60 + profiling matrix, **hardware GLES present** (`egl_scene`, `gl_present`, `shadertoy_egl`, `gbm_surface_scanout`, `egl_offload_scanout`), **explicit-sync IN_FENCE from a real GPU fence**, GPU composition (`GlCompositor`) on a single PRIMARY, Blend2D/ThorVG/CSD examples. Vulkan→KMS at 60 fps via `VkScanoutProducer`'s GPU-blit tier (no zero-copy path on this driver); VPU output via G2D GStreamer (see quirks). |
| `vc4` + `v3d` | VideoCore VII (Raspberry Pi 5, 8 GB) | 6.18.33-rpt (trixie) | Full example + test matrix on HDMI 1280×1440: every present/scene/allocator/cursor/Blend2D-CSD example, **GL and Vulkan scanout at 60 fps** (`egl_scene`, `vulkan_scene`, `vk_present`, `vk_out_fence`, offload demos), multi-plane native placement (`scene_priority` 8/8 assigned). 100/100 test binaries against `card0`. |
| `msm_drm` (downstream SDE) | SA8155P (Adreno 640) | 5.4 vendor | KMS on a shared-display node: present spine, **GL and Vulkan scanout at 60 fps**, Vulkan OUT_FENCE, native multi-plane placement with **multirect virtual-plane pairing**, GPU composition, Blend2D text. 83/84 test binaries. See quirks for the controller's plane rules. |

What's **not** validated:

- amdgpu RDNA3 — HDR `Colorspace + HDR_OUTPUT_METADATA + multi-plane`
  combinations may still EINVAL on certain commit shapes; the library
  carries a one-vblank settle on commit failure but RDNA3-specific
  acceptance hasn't been characterized.
- Intel Xe (newer Arc / Lunar Lake / Meteor Lake) — never live-tested.
  Code paths should work but no commits ship with hardware-validated
  evidence on this driver.
- Multi-card single-process — the kernel doesn't support atomic
  commits spanning two file descriptors. Use one process per card.

---

## Setting up a test rig

drm-cxx examples expect to hold DRM master on the card they drive,
which means **either** running from a bare TTY **or** running through
a session manager (logind / seatd) that revokes-on-VT-switch.

### Bare TTY

The simplest path for development. Switch to a tty (e.g. `Ctrl+Alt+F3`),
log in, and run the example directly. drm-cxx will hold master from
process start; libseat-backed examples self-detect the absence of a
session manager and fall back to direct device open.

Watch for: anything else holding master will refuse the open. If a
display manager (gdm/sddm) is running, switch to a free tty rather
than the one the DM is using.

### libseat / seatd

For development without leaving the desktop, install `seatd` and
ensure your user is in the `seat` group. Examples linking
`drm::session::Seat` (most of them) will route through libseat and
gain VT-switch survivability.

```
sudo dnf install seatd               # or apt install seatd
sudo usermod -aG seat $USER
sudo systemctl enable --now seatd
# log out and back in for the group change to take effect
```

VT switching (`Ctrl+Alt+F<n>`) is libseat-managed; examples that fan
session pause/resume across multiple scenes (`dual_display`,
`video_wall_multi`) handle the resume callback's once-per-managed-fd
fire correctly via the `/dev/dri/` path filter.

### vkms fallback

For hosts without spare physical displays or with a single output,
`scripts/vkms_dual.sh` provisions a virtual two-CRTC card via the
vkms configfs interface (kernel ≥6.11):

```
sudo scripts/vkms_dual.sh up        # creates the "dual" instance
ls /dev/dri/card*                   # a new card appears
sudo scripts/vkms_dual.sh down      # tears it down
```

The resulting card carries two connected virtual connectors (1024×768
each by default). Pass its `/dev/dri/cardN` path to any multi-CRTC
example or to `tests/integration/test_scene_set_vkms`.

---

## Multi-output testing

The quickest end-to-end check on a real dual-monitor rig is:

```
./builddir/examples/multi_crtc_probe --device /dev/dri/cardN \
    --scene-test --mirror
```

This enumerates connected outputs, issues a combined cross-CRTC
`DRM_MODE_ATOMIC_TEST_ONLY` commit, and exercises the
`SceneSet`-level `SharedLayerBufferSource` mirror path. Kernel
acceptance is the signal the rest of the multi-CRTC code paths will
work on this card.

For a visual smoke:

```
./builddir/examples/dual_display /dev/dri/cardN
./builddir/examples/video_wall_multi /dev/dri/cardN --list-outputs
./builddir/examples/video_wall_multi /dev/dri/cardN --order DP-1,HDMI-A-1
```

`video_wall_multi --list-outputs` reports the connectors the kernel
enumerates. Since DRM doesn't carry physical placement, you almost
always want to pass `--order <left-to-right>` matching how the
monitors are physically arranged on your desk; otherwise straddling
cells appear on the wrong physical edges.

Integration tests for multi-CRTC live in
`tests/integration/test_scene_set_vkms.cpp` (5 tests, GTEST_SKIP when
no vkms instance with ≥2 connected outputs is provisioned).

---

## Hardware compression (format modifiers)

`drm::fmt` (`src/fmt/format_mod.{hpp,cpp}`) handles DRM format
modifiers and hardware-compression-aware scanout. It was live-validated
across five GPU families on real silicon. Two distinct things were
measured per platform:

- **decode** — read a plane's `IN_FORMATS` blob and run every modifier
  through `classify()` / `describe()`.
- **scanout** — a GPU renders into a *compressed* buffer that the
  display actually presents (visible pixels, not just `TEST_ONLY`).

The governing principle, confirmed empirically on every board:

- **`IN_FORMATS` only prunes.** A plane advertising a modifier is
  necessary, not sufficient.
- **An atomic `TEST_ONLY` commit is the ground truth.**
- **Whether compression reaches the screen depends on the GPU
  *userspace* exposing a display-compatible compressed modifier as
  *renderable*.** Real compressed scanout needs the intersection of
  (modifiers the GPU can render + export) and (modifiers the plane can
  scan out) to contain a compressed entry. The display half is rarely
  the limiter; the render half usually is.

| GPU / userspace stack | decode | compressed scanout | why |
|-----------------------|--------|--------------------|-----|
| amdgpu RDNA2 desktop (Mesa 25.3.6)      | ✅ DCC | ❌ → `dcc=0` | Mesa exposes only `pipe_align` DCC as renderable; display wants `retile` → empty intersection |
| amdgpu Van Gogh — Steam Deck (Mesa, gamescope-tuned) | ✅ DCC | ✅ `tile27 dcc=1 retile`, **EGL + Vulkan** | Deck's Mesa exposes the displayable `retile` modifier → intersection non-empty |
| Mali-G52 / **panfrost** — Radxa Zero 3 (Mesa) | ✅ AFBC | ✅ `AFBC 16x16\|YTR\|SPARSE`, **EGL** | panfrost exposes AFBC as renderable; AFBC lives only on VOP2 **cluster** planes |
| Mali-G610 / **libmali** — RK3588        | ✅ AFBC | ❌ → LINEAR | libmali won't allocate an exportable AFBC buffer via `gbm_bo_create_with_modifiers` (fails on every cluster plane) |
| Broadcom **vc4** — RPi4 / RPi5          | ✅ SAND / T-tiled | n/a (LINEAR) | SAND / VC4-T-tiled are tiling, not compression; vc4 advertises no AFBC |
| **imx-drm** LCDIF — i.MX8M Mini         | n/a | n/a (LINEAR) | no `IN_FORMATS` at all; `from_plane` → `ENOENT` → caller falls back to LINEAR (the graceful-degradation path for legacy/simple display controllers) |

Decode coverage spans all three vendor decoders against
hardware-reported modifier values: AMD (DCC / tile / retile /
pipe-align), ARM AFBC (block size + `YTR` / `SPLIT` / `SPARSE` / `CBR`
/ `TILED` / `SC`), and Broadcom (VC4-T-tiled / SAND).

The two platforms where real compression reaches the screen — the
Steam Deck (AMD DCC) and the Radxa Zero 3 (ARM AFBC) — both rely on
`describe()` / `supports()` distinguishing modifier *sub-flags* exactly
(`pipe_align` vs `retile`; AFBC `SPARSE` vs `TILED|SC`). That fidelity
is what makes the render∩display intersection compute correctly; a
coarser "is it AFBC/DCC?" classifier would have mismatched on both
winning boards.

Same SoC family, different GPU userspace, opposite outcome: the RK3588
(libmali) and the Radxa Zero 3 (Mesa panfrost) are both Rockchip VOP2 +
Mali, yet only the Mesa-panfrost stack puts AFBC on screen. The split
is the renderer's willingness to export a compressed buffer, never
`drm::fmt`.

---

## Driver quirks

Workarounds for known kernel/driver behavior. Most are codified in
the library or its tests; this list exists so a future investigation
hits the cached answer first.

### amdgpu DC

- **Foreign-source PRIME imports rejected as FBs.** amdgpu's
  `AddFB2` requires the dmabuf's ops to be `amdgpu_dmabuf_ops`. Any
  dmabuf from a different exporter (vmalloc-backed, vgem, etc.)
  fails with EINVAL. Copy-to-amdgpu-BO is the only workaround.
- **LINEAR-modifier `AddFB2` pitch must be 256-aligned.** PRIME
  imports at sub-256 pitches EINVAL at `AddFB2`. `dumb_create`
  rounds up internally so internally-allocated buffers are safe.
- **PRIMARY must stay armed during atomic `TEST_ONLY`.** Disabling
  the primary plane while testing other-plane configurations EINVALs.
  `LayerScene`'s primary-anchor reservation handles this.
- **Cursor pixels are re-read live every vblank.** In-place cursor
  updates tear on amdgpu (not on i915). Per-frame cursor surfaces
  must double-buffer.
- **HDR `Colorspace` blob writes need `ALLOW_MODESET`.** Same-id
  re-writes are fine; transitions (clearing or changing) require
  modeset and may EINVAL on multi-plane commits on pre-RDNA3 silicon.
- **HW cursor plane size floors: 64 / 128 / 256.** `cursor_size_for_output`
  snaps up before clamping to `cap_w`.
- **RDNA scanout accepts P010, rejects P012 / P016.** `CREATE_DUMB`
  succeeds for all three; only the plane attach reveals the limit.
- **PRIMARY pinned at zpos=2.** Explicit `LayerDesc::zpos` values
  ≤2 collide with the background layer; examples use `>=3`.
- **OVERLAY plane briefly disabled on SRC_W/H change.** Visible as a
  ~1 vblank flicker; canvas underneath shows through.
- **Displayable DCC needs the `retile` modifier, and Mesa must expose
  it.** The GPU renders DCC `pipe_align` (`AMD_FMT_MOD` with
  `DCC_PIPE_ALIGN=1`), but DCN can only scan out non-pipe-aligned DCC —
  in practice the `retile` variant (`DCC_RETILE=1`, `PIPE_ALIGN=0`).
  Real DCC scanout therefore requires Mesa to advertise the `retile`
  modifier as *renderable*. Desktop Mesa 25.3.6 does **not** (only
  `pipe_align`), so an exported render target falls back to `dcc=0`;
  the Steam Deck's gamescope-tuned Mesa **does**, and `drm::fmt` drives
  real DCC to screen there. The display side advertises both variants
  on every box tested — the renderer is the gate.

### Intel i915

- **HW cursor only at 64×64.** No 128 / 256 fallback path; the cursor
  module's snap-up logic relies on this floor.

### vkms

- **Primary plane has no scaler.** Any commit where `SRC_W/H` differs
  from `CRTC_W/H` fails with ERANGE. Probes/tests that allocate
  undersized scratch FBs get false negatives — size them to the mode.
  (`examples/common/multi_crtc_probe.hpp` was fixed for this.)
- **CMS / HDR property surface is GAMMA_LUT only.** 256 entries; no
  `DEGAMMA_LUT` / `CTM` / `HDR_OUTPUT_METADATA` / `Colorspace` /
  `max_bpc`. Accepts P010 on primary plane.
- **Configfs interface requires kernel ≥6.11.** Older kernels expose
  vkms but without the runtime instance provisioning that
  `scripts/vkms_dual.sh` relies on.

### vicodec

- **Three /dev/video* nodes share the card name.** Probe by
  `VIDIOC_ENUM_FMT` looking for FWHT on the OUTPUT queue, not by
  string-matching the card name.

### Rockchip VOP2 + Mali (RK3566 / RK3588)

- **AFBC lives only on Cluster planes.** VOP2 advertises ARM AFBC
  modifiers on its Cluster planes (incl. the default primary on
  RK3588); the Esmart / Smart planes carry **zero** AFBC. A demo that
  targets a Smart plane sees a LINEAR-only `IN_FORMATS` and never gets
  compression even when the GPU can render it. (`DRM_FMT_PLANE`-style
  plane targeting was used during bring-up to reach a Cluster plane.)
- **Mesa panfrost exposes AFBC as renderable; libmali does not.** On
  the Mesa-panfrost stack (Radxa Zero 3), `gbm_bo_create_with_modifiers`
  with an AFBC modifier succeeds and the buffer scans out compressed.
  On the proprietary libmali stack (RK3588, GPU at `/dev/mali0` with no
  Mesa DRM render node), the same call **fails** for AFBC on every
  cluster plane — libmali only uses AFBC for its own internal EGL
  swapchain, not for an explicit exportable `gbm` allocation. The
  failure is at buffer allocation, upstream of any plane logic, so
  every cluster plane fails identically.
- **AFBC sub-flags must match.** panfrost renders some AFBC variants
  with `TILED|SC` set, which the VOP does not scan out; the plain
  `SPARSE` / `YTR|SPARSE` variants are in both sets and are what land.
  `drm::fmt`'s exact flag decode is what selects the intersecting one.

### Broadcom vc4 (Raspberry Pi)

- **SAND and VC4-T-tiled are tiling, not compression.** vc4 advertises
  `BROADCOM_SAND64/128/256` and `VC4_T_TILED` on its HVS planes;
  `classify()` correctly reports these as `Tiling`, not `Compression`.
  vc4 advertises no AFBC, so there is no compressed-scanout path to
  exercise on the Pi — only tiling/LINEAR.

#### Raspberry Pi Zero 2 W (VideoCore IV) — validated + profiled

Validation board: **Raspberry Pi Zero 2 W** (BCM2710A1, quad Cortex-A55-class
**A53, aarch64**, **415 MiB RAM**), Debian 13 trixie (`6.18.33-rpt`). `card0` and
`renderD128` are both `vc4` (VideoCore IV — GLES2/EGL via Mesa vc4, **no V3D /
no hardware Vulkan**). HDMI clamps to **1600x900@60** (an LG monitor's first of
14 modes). No compositor runs by default, so DRM master is free; run examples
with **`--no-seat`** (DRM opened directly, master taken as first opener).

`driver_caps` reports a far richer device than the LCDIF SoCs:
`addfb2_modifiers=true`, **`async_page_flip=true`**, `fb_damage_clips=false`,
`vrr_capable=true`, cursor 64×64, plane census **`PRIMARY=4 OVERLAY=48
CURSOR=4`** (56 planes). `plane_caps` shows real modifier support — Broadcom
`SAND64/128/256` on NV12/NV21 and `VC4_T_TILED` on the 8888 formats — so the
tiled-scanout path is exercisable here (it is tiling, not compression).

- **Build is cross, via `emb`** (the board's 415 MiB RAM rules out an on-target
  build). An `emb.yaml` (`cross.provider: arm-gnu`, **`generator: cmake`**,
  raspios-bookworm image sysroot + the `-dev` closure layered in, libdisplay-info
  0.2.0 as a source `augment`) cross-builds the examples; the glibc-2.36 binaries
  run forward-compatibly on the trixie board. **Use the CMake generator, not
  meson:** the arm-gnu provider emits a CMake toolchain file but no meson cross
  file, so a meson generator silently falls back to the host compiler (host
  libstdc++ vs sysroot glibc → `L_tmpnam`-style header errors). Under gcc-12
  (no `<print>`) the format adapter uses {fmt}, pulled by CMake FetchContent as
  a shared `libfmt.so.11` — ship it next to the binaries (the board has
  `libfmt.so.10`).
- **Software / dumb-buffer scanout works** — `software_present` presents 120
  frames XRGB8888 @ 1600x900. Single-plane→multi-plane composition,
  `minimal_kms_probe`, `scene_*` all run.
- **Hardware GLES present works and is the cheap path** — `egl_scene` runs at
  ~60 fps (301 frames / 5 s), `gl_present` (GlScanoutProducer) presents 120
  frames, and `gbm_surface_scanout` arms a **`VC4_T_TILED`** front buffer
  (TEST_ONLY accepted, on screen). Rendering on the vc4 GPU costs ~5× less CPU
  than a CPU full-frame present (see table).
- **Vulkan is not usable.** VideoCore IV has no V3D, so there is no hardware
  Vulkan; the only ICD is **llvmpipe (CPU)**, which `vulkan_display` finds 0
  displays on and which fails `vk_present` / `vulkan_scene` at
  `createDevice` (`ErrorExtensionNotPresent` — it lacks the dmabuf /
  external-memory device extensions the scanout path needs). Vulkan examples
  build but do not run here.
- **`VRR_ENABLED` is advertised but inert** — same as the LCDIF SoCs:
  `vrr_capable=true` and the property is accepted, but `vrr_sweep --vrr` gives
  jitter identical to the control at every off-vblank target (40 Hz 8.33 ms,
  48 Hz 7.19 ms, unchanged). Present at 60 Hz or a clean divisor.

##### Present-path profiling (decision guide)

All at **1600x900**, single A53 thread (the SoC has 4). CPU/frame is `user+sys`
(the per-frame `usleep`/flip-wait consumes no CPU, so it is pure
render+blit+commit work); **load@60** is the share of one A53 core to sustain
60 fps. The synchronous (non-`--vsync`) commit caps the CPU paths near 29 fps
wall even where the CPU has headroom for more.

| Workload                                   | render scope       | CPU/frame | load@60 (1 core) | takeaway |
|--------------------------------------------|--------------------|-----------|------------------|----------|
| `software_present` XRGB8888 (full *or* damage) | full 1600x900  | ~11.8 ms  | ~71 %            | damage hint is a **no-op** (`fb_damage_clips=false` + full redraw) |
| `software_present` RGB565                  | full 1600x900      | ~6.1 ms   | ~36 %            | 16 bpp **~halves** the cost — the A53 is memory-bandwidth bound at this size |
| `damage_present` (partial)                 | partial            | ~4.8 ms   | ~29 %            | incremental rendering ~2.5× cheaper than a full CPU redraw |
| `ring_present`                             | full ring repaint  | ~12.8 ms  | ~77 %            | heaviest CPU path |
| **`gl_present` (GLES → KMS)**              | **vc4 GPU**        | **~2.2 ms** | **~13 %**      | **hardware offload — ~5× less CPU; ~55 fps** |
| `idle_present` (static, 96 % skipped)      | skip unchanged     | ~0.8 ms avg | ~5 %           | idle-skip collapses CPU when the scene is static |

Guidance for a Pi Zero 2 W UI:

- **Render on the GPU.** `gl_present` / `egl_scene` (GLES via vc4) cost ~13 % of
  a core vs ~71 % for a CPU full-frame present — on a 415 MiB / 4×A53 board that
  headroom matters. Use a `GbmSurfaceSource` and let the HVS scan out a
  `VC4_T_TILED` buffer.
- **If CPU-only, prefer RGB565** (≈half the cost of XRGB8888 here — memory
  bound) and render incrementally; the `FB_DAMAGE_CLIPS` hint itself does
  nothing (`fb_damage_clips=false`).
- **Idle-skip** (`present::FrameEconomy`) for static screens.
- **Don't rely on VRR**, and **don't plan on Vulkan** (CPU-only, and the scanout
  path doesn't run).

### imx-drm (i.MX8M Mini)

Validation board: **Boundary Devices Nitrogen8M Mini**, NXP i.MX Yocto
BSP, kernel 6.1.22. `card0` is `imx-drm` (the LCDIF display controller);
the Vivante GPU is the proprietary `galcore` driver on `card1` (no Mesa
render node).

- **No `IN_FORMATS` on the display planes.** The LCDIF advertises a flat
  legacy fourcc list (XRGB8888, ARGB8888, RGB565, …) with **no**
  `IN_FORMATS` blob and no modifiers. `FormatTable::from_plane` returns
  `ENOENT`, and `ScanoutTarget::discover` leaves `primary_formats` empty
  so the caller assumes LINEAR — the graceful-degradation path for legacy
  display controllers. Validated: `discover()` finds the connector → CRTC
  → PRIMARY plane → preferred mode (1920×1080) with `in_formats=no`.
- **LINEAR-only scanout.** There is no tiling/compression modifier path
  to exercise here; the LCDIF scans out LINEAR. The Vivante GPU does AFBC
  internally but exposes nothing through KMS `IN_FORMATS`.
- **No native build tooling beyond a compiler.** The BSP image ships
  `gcc`/`g++` and `libdrm`/`libgbm` runtime but no meson/ninja and no
  `-dev` headers; bring drm-cxx's headers (or carry the API headers) and
  compile the needed sources directly when validating on this board.

### i.MX93 LCDIFv3 (FRDM-IMX93)

Validation board: **NXP FRDM-IMX93** (i.MX93, dual Cortex-A55, **aarch64**),
NXP i.MX Yocto BSP (`fsl-imx-xwayland`, "whinlatter"), **kernel 6.18.2**.
`card0` is `imx-drm` (the **LCDIFv3** display controller feeding an on-board
HDMI bridge). The i.MX93 has **no GPU at all** — no Vivante 3D, no Mesa render
node, only the 2D PXP and the LCDIF — so the software / dumb-buffer present
path is the *only* path; there are no GL/Vulkan/`egl` examples to run here.
Validated against an LG HDMI monitor that the LCDIF clamps to **1280×720@60**
(the single EDID mode it offers; higher modes exceed the LCDIFv3 pixel-clock
budget on this bridge).

`driver_caps` reports: `addfb2_modifiers=false`, **`async_page_flip=false`**,
prime import/export=true, cursor 64×64 (cap only), **`fb_damage_clips=false`**,
**`vrr_capable=true`**, psr=none. Plane census `PRIMARY=1 OVERLAY=0 CURSOR=0`
— the same single-plane / no-cursor shape as the i.MX8MM LCDIF and tilcdc, so
`driver_caps` emits the cap/registry-mismatch **`[WARN]`** (gate HW-cursor use
on the registry, never on `DRM_CAP_CURSOR_*`). The one PRIMARY plane (id 33)
advertises **`XR24 AR24 RG16 XB24 AB24 AR15 XR15`** with **no `IN_FORMATS`**
(LINEAR-only). Unlike tilcdc, **XRGB8888 (`XR24`) is present**, so the default
present path works with no `--rgb565` negotiation.

- **Build is native on-target, C++23, software-only.** The BSP ships `gcc`
  15.2.0 + `cmake` but no meson/ninja and is short several `-dev` packages, so
  the build runs *on the board*: `pip install meson ninja`, then `meson …
  -Dcpp_std=c++23`. Under C++23 + libstdc++ 15 the format adapter aliases
  `std::format`/`std::print` directly, so **`fmt` is never needed** (no `-dev`,
  no subproject build). Turn all GPU/heavy features off (`-Degl=disabled
  -Dvulkan=false -Dblend2d=disabled -Dgstreamer=disabled -Dcamera=disabled
  -Dstreams=disabled -Dsession=disabled -Dcursor=disabled -Dtests=false`). Three
  examples (`mouse_cursor`, `xcursor_smoke`, `egl_scene`) are still emitted by
  the examples list even with their feature disabled and fail to build, so build
  the working binaries by target name rather than `ninja all`.
- **`libdisplay-info` ≥0.2.0 is a hard dependency the BSP can't satisfy.** The
  image ships only `libdisplay-info.so.1` (0.1.1) and no headers; drm-cxx pins
  `>=0.2.0` for the HDR/colorimetry EDID APIs. The build script builds 0.2.0
  from source into `/usr/local` (it also drops a `pnp.ids` into
  `/usr/share/hwdata` first, which the BSP lacks), then points
  `PKG_CONFIG_PATH`/`LD_LIBRARY_PATH` at it. This is the same side-by-side
  install `scripts/build-deps.sh libdisplay-info` performs for short distros.
- **Headless over SSH → `--no-seat`.** The BSP runs `weston` on tty7 holding
  DRM master; `systemctl stop weston` frees the card, then DRM-master examples
  run as root with **`--no-seat`** (opens DRM directly + takes master as the
  first opener). `systemctl start weston` restores the desktop. Read-only tools
  (`driver_caps`, `plane_caps`) need neither stop nor seat.
- **Single-plane composition fallback works.** `minimal_kms_probe /dev/dri/card0
  3` rescues all 3 overlapping layers via CPU composition onto the one PRIMARY
  plane (`composited 3 / dropped 0`); `scene_formats`, `layered_demo`,
  `scene_priority` all composite onto plane 33. No overlay/cursor plane, so any
  scene with more layers than planes (i.e. >1) composites the overflow.
- **`VRR_ENABLED` is advertised but does nothing.** `vrr_capable=true`, the CRTC
  exposes `VRR_ENABLED`, and the EDID gives a vrefresh range (48–61 Hz), but
  `vrr_sweep --vrr` measures **byte-identical jitter to the control** at every
  off-vblank target (40 Hz: 8.34 ms vs 8.33 ms; 48 Hz: 7.19 ms vs 7.19 ms) — the
  LCDIFv3+bridge accepts the property on commit but never varies the porch
  timing. **Do not rely on VRR here.** The real jitter lever is `--rt`
  (SCHED_FIFO + mlockall): it took 30 Hz jitter from 1.76 ms to 0.02 ms.
- **Working example set** (verified on-device): `software_present`,
  `damage_present`, `ring_present`, `idle_present`,
  `atomic_modeset` (present + modeset), `driver_caps`, `plane_caps`,
  `minimal_kms_probe`, `multi_crtc_probe`, `stream_probe`, `compressed_scanout`,
  `scene_formats` (read-only diagnostics), `layered_demo`, `scene_priority`,
  `overlay_planes`, `scene_warm_start`, `vrr_sweep`. Excluded: anything needing
  a GPU, a cursor plane, multiple CRTCs, compression, or a camera.

#### Present-path profiling (decision guide)

All numbers at **1280×720**, single A55 thread, kernel 6.18.2. CPU/frame is
`user+sys` measured over the run (the per-frame `usleep`/flip-wait consumes no
CPU, so this is pure render+blit+commit work); **load@60** is the share of one
A55 core needed to sustain 60 fps (the board has 2 cores). 60 Hz flips are
rock-solid (0.02 ms jitter); the synchronous (non-`--vsync`) commit blocks ~½ a
vblank, so timer-mode wall-rate sits near 29 fps even though the CPU has headroom
to ~90+ fps.

| Workload (`software_present` unless noted) | render scope        | CPU/frame | load@60 (1 core) | takeaway |
|--------------------------------------------|---------------------|-----------|------------------|----------|
| XRGB8888, full redraw (default)            | full 720p           | 10.8 ms   | ~65 %            | baseline |
| XRGB8888, `--no-damage`                    | full 720p           | 10.7 ms   | ~64 %            | damage hint is a **no-op** (`fb_damage_clips=false` + full redraw) |
| RGB565 (`--rgb565`)                         | full 720p           | 10.2 ms   | ~61 %            | half the bytes → only ~5–6 % cheaper (per-pixel loop dominates) |
| `ring_present`                              | buffer-age repaint  | 8.1 ms    | ~49 %            | 3-slot ring repaints the aged region |
| `damage_present`                            | partial (box only)  | 2.45 ms   | ~15 %            | **incremental rendering is ~4× cheaper** than full redraw |
| `idle_present` (static, 96 % skipped)       | skip unchanged      | ~0.5 ms avg | ~3 %           | **idle-skip is ~100× cheaper** when the scene is static |

Guidance for an i.MX93 UI:

- **Format:** use XRGB8888 (it's supported and avoids RGB565 banding); RGB565's
  CPU saving (~5 %) rarely justifies the quality loss. There is no modifier /
  compression path — everything is LINEAR.
- **Damage:** the `FB_DAMAGE_CLIPS` *hint* does nothing (`fb_damage_clips=false`).
  The win is real only when the **application renders incrementally** so the
  dumb-BO blit shrinks — `damage_present` (true partial update) is 4× cheaper
  than a full redraw. A toolkit that repaints the whole frame each tick gets no
  benefit from passing damage rects.
- **Idle-skip** (`present::FrameEconomy`) is the single biggest lever: a static
  or rarely-changing screen (cluster, signage, status panel) drops to a few
  percent of one core. Prefer change-driven repaint over a fixed loop.
- **Refresh / pacing:** present at **60 Hz** (0.02 ms jitter) or a clean divisor
  (**30 Hz**); off-multiple rates (40/48 Hz) beat against the fixed 60 Hz vblank
  for ~7–8 ms jitter and **VRR will not fix it**. For tight pacing use
  `--rt`-style SCHED_FIFO + `mlockall`, not VRR.

### PANZER-PLUS Edge AIoT Computer (LCDIFv3 + Vivante GC7000UL)

Validation device: **PANZER-PLUS Edge AIoT Computer** (NXP i.MX8M Plus, quad Cortex-A53
@ 1.6 GHz, **aarch64**, 3.5 GiB), NXP i.MX Yocto BSP (`fsl-imx-xwayland`),
**kernel 6.6.23**, glibc 2.39, GCC 13.2. Two DRM nodes:

- **`card0` = `vivante`** (`galcore` 6.4.11) — the GC7000UL GPU. **Render-only:
  no CRTC, no connector.** Anything that picks "the first card" lands here and
  fails (see the test note below).
- **`card1` = `imx-drm`** — three **LCDIFv3** CRTCs (ids 33/36/39), each with
  exactly **one PRIMARY plane** (31/34/37, `zpos` immutable at 0) and **no
  overlay, no cursor**. Only the third CRTC (39) is wired to the DW-HDMI bridge
  (`HDMI-A-1`); the other two have no connector on this board. The
  encoder's `possible_crtcs` is therefore a single bit — legacy `SetCrtc` on CRTC
  33 returns `EINVAL`.
- `renderD128` (GPU), plus the Hantro VPU (`vsi_v4l2dec` / `vsi_v4l2enc`) and the
  ISI mem2mem scaler/CSC on `/dev/video*`.

`driver_caps /dev/dri/card1`: `addfb2_modifiers=false`, `async_page_flip=false`,
prime import/export=true, **`fb_damage_clips=false`**, **`vrr_capable=true`**,
psr=none, census `PRIMARY=3 OVERLAY=0 CURSOR=0` (with the cap/registry cursor
`[WARN]` — `DRM_CAP_CURSOR_*` claims 64×64 but there is no cursor plane). Plane
formats are `XR24 AR24 RG16 XB24 AB24 AR15 XR15` with **no `IN_FORMATS`**
(LINEAR-only) — **no YUV at all**. Validated on a **240 Hz-capable** HDMI
monitor (EDID range 48–240 Hz), but imx-drm exposes only five modes, topping out
at **1920×1080@60** (plus 1280×1024@75): the NXP DW-HDMI path filters the
high-refresh modes out. Every run here therefore scanned out at 60 Hz.

**Build — cross from an x86_64 host against the board's own sysroot:**
`scripts/build_imx8mp.sh <ssh-target> [--clang] [--deploy]` does all of the below
(toolchain fetch + checksum, sysroot mirror, dependency cross-builds, drm-cxx,
rpath scrub, deploy). Pitfalls it handles, each of which was hit:

- **Match the compiler to the BSP's libstdc++.** The image ships libstdc++ 13
  (`GLIBCXX_3.4.32`). A newer host cross-GCC (16) **rejects GCC 13's libstdc++
  headers** (`use of built-in trait '__is_trivially_copyable' in function
  signature`), and a `-static-libstdc++` from another version needs a
  `libstdc++.a` the BSP doesn't ship. Use the **Arm GNU Toolchain 13.2.rel1**
  (`aarch64-none-linux-gnu`) with `--sysroot=<board sysroot>`: its own 13.2
  headers, linked dynamically against the board's `libstdc++.so.6`.
- **Never `-static-libgcc` on `libdrm-cxx.so`.** It embeds a private copy of the
  DWARF unwinder; an exception thrown by the board's `libstdc++` then runs
  `libgcc_s`'s `_Unwind_RaiseException`, which calls the embedded personality's
  `_Unwind_SetGR` with a foreign context → `abort()`. Symptom: **any C++
  exception crossing the library is `std::terminate`** (`test_csd_theme` aborts
  on the toml++ `parse_error` that `load_theme_string` catches). Link
  `libgcc_s` dynamically — the BSP has it.
- **Filter the sysroot's own `-I/usr/include` / `-L/usr/lib` out of pkg-config.**
  pkgconf stops recognizing them as system dirs once they are sysroot-prefixed,
  so `-isystem <sysroot>/usr/include` lands ahead of libstdc++ and
  `#include_next <stdlib.h>` fails. A two-line `pkg-config` wrapper that drops
  exactly those two flags (and sets `PKG_CONFIG_SYSROOT_DIR`/`PKG_CONFIG_LIBDIR`)
  fixes it.
- **Use `gcc-ar`, not `ar`,** for the `ar` binary: the build is LTO and static
  helper archives (`test_patterns_painters`, `signage_playlist`) need the LTO
  plugin or their symbols vanish at link time.
- **C++23 still needs `{fmt}` here** — libstdc++ 13 has `std::format` but no
  `std::print`, so the adapter takes the vendored header-only fmt path
  (automatic; nothing to install).

**Dependencies the BSP lacks**, cross-built by the script and deployed to the
board's `/usr/local`: `libdisplay-info` 0.2.0 (BSP has none; the build reads the
host's hwdata `pnp.ids`), `libseat` (seatd 0.9.1, for `session`), googletest
1.15.2 (tests), ThorVG 1.0.4 (`thorvg_janitor`), Blend2D (capture/CSD/mdi),
libyuv (`cluster_sim` UVC rear-view), and a link-only GStreamer 1.24.0
(`video_player`; see Cameras below).
The BSP has no `/etc/ld.so.conf.d/` and does not search `/usr/local/lib*` —
create the directory with a `usr-local.conf` and run `ldconfig`. Present on the
image already: libdrm 2.4.116, NXP GBM 21.3.5 (`libgbm_viv`), EGL/GLESv2 (Vivante),
Vulkan loader 1.3.275, libinput, xkbcommon, libjpeg. Absent and not built:
libcamera (`camera`), libva.

**Clang + libc++ variant (`--clang`):** the BSP also ships the **LLVM 18 libc++
runtime** (`/usr/lib/libc++.so.1`, libc++abi merged in; NEEDED only libc and
`libgcc_s`) but no headers. `--clang` builds with host **clang 18** against the
same board sysroot: libc++ **18.1.8** headers are generated through libc++'s own
`runtimes` CMake (`__config_site` is generated; `install-cxx-headers` +
`install-cxxabi-headers`, `LIBCXXABI_USE_LLVM_UNWINDER=OFF`) into the sysroot
only, a link-time `libc++.so → libc++.so.1` symlink is added, and the binaries run
against the board's own libc++ with the unwinder staying `libgcc_s` (via
`--gcc-install-dir` into the BSP's GCC 13.2). The C++ dependencies are rebuilt
against libc++ into **`/usr/local/drm-cxx-libcxx`**, reached by rpath only (no
`ld.so.conf` entry) so they never shadow the GCC build's `/usr/local`.
Pitfalls:

- **`-stdlib=libc++` at link time only.** With `-nostdinc++` it is an unused
  compile argument, which Meson's compiler checks turn into an error ("A
  C++17-capable compiler is required").
- **libc++ 18 withholds `__cpp_lib_format`,** so C++23 still takes the `{fmt}`
  path — the build's `std::print` probe checks the same feature macros
  `detail/format.hpp` gates on, not whether `std::println` compiles.
- **Build libyuv with `CAN_COMPILE_SME=OFF`:** clang 18 emits SME kernels that
  need `__arm_tpidr2_save`, which GCC 13's libgcc lacks.

The test suite and examples (`vk_present`, `gl_present`, `egl_scene`,
`vulkan_scene`, `cluster_sim`, `cluster_sim_vulkan`, `signage_player`,
`plane_stress`) behave identically to the GCC build.

**Running:** the BSP starts a desktop compositor service that holds DRM master on
`card1`; stop it (service + its socket unit) before running KMS examples, start
it again afterwards. Always pass **`/dev/dri/card1`** explicitly — with two
cards the examples' device picker prompts on stdin. The BSP ships no XCursor
themes (set `XCURSOR_PATH` at one), and the `keyboard` demo's font path is the
absolute build-dir path baked in at configure time, so it only finds the font
when run from an identical path.

**Fixed while validating here:**

- **Layers shut out of a fixed-`zpos` PRIMARY.** The allocator required a
  layer's `zpos` to lie inside a plane's range even when the plane's `zpos` is a
  fixed slot it never writes, and LayerScene's primary-anchor reservation
  mirrored that — so on a single-PRIMARY controller any layer with `zpos ≥ 1`
  was always composited, even alone. Now: a fixed-slot plane admits any `zpos`,
  and the allocator instead checks that placed layers stack in the order they
  request (`planes/zpos_order.hpp`; the kernel accepts an inverted stack, so
  TEST cannot catch it). The scene treats the unique lowest layer as
  PRIMARY-eligible when every other layer sits above the slot, and the canvas
  may now use the plane the allocator parked the composition placeholder on.
  For every assignment admissible before, the stacking check is a no-op.

- **`AddFB2` of an explicit LINEAR on a modifier-less driver.** imx-drm lacks
  `DRM_CAP_ADDFB2_MODIFIERS` and rejects `DRM_MODE_FB_MODIFIERS` outright —
  even with `DRM_FORMAT_MOD_LINEAR`, which is exactly its implicit layout. Every
  dma-buf import (`ExternalDmaBufSource`/`Ring`, `GbmSurfaceSource`,
  `gbm::Buffer`, `fmt::import_dmabuf`/`ScanoutBuffer`, the V4L2 and GStreamer
  sources) now goes through one helper that takes the legacy path for INVALID,
  and for LINEAR when the cap is absent. This was the cause of every
  `ExternalDmaBufRing` failure on this board (and applies equally to the i.MX93,
  i.MX8MM, tilcdc and other LCDIF-class controllers).
- **Vulkan drivers that omit core-promoted extensions.** The VeriSilicon
  Vulkan 1.3 driver does not list `VK_KHR_external_semaphore` (core since 1.1),
  so requesting it by name failed `vkCreateDevice` with
  `ErrorExtensionNotPresent`. `VkScanoutProducer` now requests the 1.1-promoted
  names only when listed.
- **`GstAppsinkSource` dma-buf import never worked.** `locate_plane_fd` passed
  `NULL` for `gst_buffer_find_memory`'s `length` out-param, which GStreamer
  rejects (`assertion 'length != NULL' failed` → `Protocol error`), so every
  dma-buf-backed sample failed; only CPU-memory samples (e.g. `videotestsrc`)
  played. Found with the G2D pipelines here.
- **No-`IN_FORMATS` planes in the `drm::fmt` examples.** `compressed_scanout`,
  `gbm_surface_scanout`, `egl_offload_scanout`, `vulkan_offload_scanout` treated
  a missing `IN_FORMATS` as fatal; they now pair the legacy fourcc list with
  LINEAR. `cluster_sim_vulkan` maps `candidate_modifiers()`'s INVALID ("driver
  picks") to LINEAR before intersecting with the Vulkan modifier list.

**GPU (Vivante GC7000UL):**

- **GLES/EGL works end to end.** GBM on `card1` allocates from the display's
  contiguous pool and the GPU renders straight into it: `egl_scene`,
  `gl_present`, `shadertoy_egl`, `gbm_surface_scanout` and `egl_offload_scanout`
  (GPU-rendered LINEAR scanned out) all present.
- **Explicit sync validated:** `test_explicit_sync_fence_gl` arms a genuine GPU
  acquire fence as the plane's `IN_FENCE_FD`.
- **GPU composition is active** — `GlCompositor` reports EGL dma-buf import and
  the NV12 external-sampler path available.
- **Vulkan reaches this display through a GPU copy, at 60 fps.** Neither
  zero-copy direction works with this BSP's Vulkan driver (6.4.11):
  - *Export:* every Vulkan-exported dma-buf (either memory type, dedicated or
    not) is scatter-gather. imx-drm is a GEM-DMA driver with no IOMMU in front of
    the LCDIF, so `drmPrimeFDToHandle` has to bounce the buffer through swiotlb
    and fails (`swiotlb buffer is full (sz: 8355840 bytes)` → `ENOMEM`).
  - *Import:* the driver accepts a dma-buf import (from GBM on `card1` or a dumb
    buffer) but does **not alias** it — a `vkCmdFillBuffer`, an image clear and a
    buffer-to-image copy into the imported memory all leave the CPU-visible
    contents untouched.

  The GPU's own GL stack *can* read Vulkan's export: an EGLImage over the
  exported dma-buf samples what Vulkan wrote. `VkScanoutProducer` therefore
  tries, in order: export (zero-copy); a display-side buffer imported into
  Vulkan, kept only if a GPU write shows through it (self-checked; Vivante pads
  image height to 16 rows, so the buffer gets the extra rows); a **GPU blit** —
  each frame GL draws the exported image as one full-screen quad into a
  `GlScanoutProducer` surface on `card1`, kept only if a read-back of two test
  colors matches; and finally a CPU copy into two display-side dumb buffers
  (`ExternalDmaBufRing`). Here the blit tier is taken: `vk_present` runs at
  **60.7 fps at 1080p** (600 frames in 9.9 s, vblank-bound, ~9 % of one core;
  the GL draw is ~5 ms on the GPU) and `vk_out_fence` gets OUT_FENCE on 60/60.
  The CPU-copy tier it replaces ran ~50 ms/frame (~42 ms CPU), since the only
  host-visible memory type is uncached (~200 MB/s reads). The Vulkan examples
  draw through the same producer (`VkScanoutProducer::render`), so they take the
  same path: `vulkan_offload_scanout` lands on the plane, `vulkan_scene` runs
  ~30 fps (two full-screen layers, composited by the GPU) and
  `cluster_sim_vulkan` ~20 fps (bound by the GPU's 1080p fragment work).
  `vulkan_display` enumerates the display.
- **Vivante leaves Vulkan clears in tile status.** A clear
  (`vkCmdClearColorImage` or a render-pass `LOAD_OP_CLEAR`) on an exported image
  stays in fast-clear metadata and is never resolved into memory, even across a
  release to `VK_QUEUE_FAMILY_FOREIGN_EXT` or a layout change. Anything only
  cleared reads back as stale memory to the display, GL or the CPU; drawn and
  copied pixels land. Any transfer read of the image resolves the whole surface,
  so on this vendor (`VK_VENDOR_ID_VSI`) `VkScanoutProducer` appends a 1-pixel
  image-to-buffer copy after each render — the app's own `render()` frames
  included — before the release barrier. Only tiled images have tile status:
  this was found on a DRM-modifier "LINEAR" image, which Vivante tiles (next
  item). A `VK_IMAGE_TILING_LINEAR` image's clears land in memory directly
  (render-pass and `vkCmdClearColorImage` clears both checked without the
  resolve), so on the i.MX8M Plus path the resolve is now a safeguard for the
  tiled modifiers rather than a fix.
- **Vivante lays out "LINEAR" modifier images tiled.** An image created with
  `VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT` and `DRM_FORMAT_MOD_LINEAR` (the
  driver reports 0x0 back) is tiled in memory, so every consumer that trusts the
  modifier — GL, the display, the CPU — reads scrambled pixels. Uniform content
  (clears, solid fills) hides it; any real drawing shows it. A
  `VK_IMAGE_TILING_LINEAR` image is laid out linearly and renders correctly, so
  on this vendor `VkScanoutProducer` allocates LINEAR that way.
- **Vivante GBM modifier surfaces have two buffers.**
  `gbm_surface_create_with_modifiers{,2}` builds a two-buffer surface (the
  plain `gbm_surface_create` gets three), and `LayerScene` keeps two in flight,
  so the third `eglSwapBuffers` waited forever for a free buffer (`gl_present`
  hung on frame 3). `GbmSurfaceSource` now asks for LINEAR with the plain create
  plus `GBM_BO_USE_LINEAR`; `GBM_MULTI_BUFFER` does not raise the count on the
  modifier path.
- **Vivante's `libEGL` destroys GBM devices at exit.**
  `eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, dev)` records `dev` in a global
  list that `eglTerminate` never clears, and an exit handler calls
  `gbm_device_destroy` on every entry. An application that destroyed `dev`
  first (the correct order everywhere else) crashed at exit about half the time
  — a use-after-free that depends on whether the freed block was reused.
  `gbm::retain_for_egl()` records such devices once `EGL_VENDOR` identifies
  Vivante, and `GbmDevice` then leaves them for `libEGL` to free;
  `GlScanoutProducer`, `GlCompositor` and the EGL examples call it.
- **Untested: `galcore.mmu=0` for zero-copy export.** With the GPU MMU off,
  galcore must allocate every GPU buffer physically contiguous, so a Vulkan
  export should be importable by the LCDIF and the export tier would win
  (no copy at all). Not tried: it is a system-wide boot-argument change off
  NXP's supported configuration, every GPU allocation then competes for the
  contiguous pool/CMA (with the VPU, ISP and NPU) and is exposed to CMA
  fragmentation and compaction stalls, and GPU memory isolation between
  processes is lost. The blit already reaches 60 fps, so it is documented here
  as an option, not a recommendation.

**Single PRIMARY with a fixed `zpos` slot.** The PRIMARY's `zpos` range is
`[0, 0]`. A lone layer now lands on it natively whatever `zpos` it asks for
(see the fix above): the `layer_scene_census` static and widget-damage scenarios
run 1 TEST total, the FB-only fast path on 29 of 30 frames, and 39 property
writes instead of 300. What still composites is what this plane physically
cannot host: more than one overlapping layer (`scene_*`, `layered_demo`,
`minimal_kms_probe`, `plane_stress`), and any layer that is not full-screen —
the LCDIFv3 PRIMARY cannot position or scale, so a moved or smaller layer fails
TEST and goes to the canvas (census `scroll`: 4 TESTs to learn that, then
composited every frame).

**Composition samples layers in place.** `GlCompositor` imports each layer's
dma-buf as an EGLImage — dumb buffers and GBM surfaces export theirs — instead
of uploading its CPU pixels. On Vivante the upload was a CPU-side retile
(`gcoTEXTURE_Upload` ~89 % of all cycles): ~221 ms per 1080p layer, against
~3.5 ms imported, and CPU writes to a dumb buffer show through the import.
Measured: `plane_stress` (4 composited 256×256 layers, any churn mode) **60 fps**
(was 20), ~15 % of one core; `egl_scene` (two full-screen layers, GL + dumb)
**30 fps** (was 4.4, with the GL layer dropped), now GPU-bound on sampling two
LINEAR 1080p surfaces. If an import ever fails, `GlCompositor` falls back to the
CPU upload for good.

**Video:** the Hantro decoder decodes H.264/HEVC/VP8/VP9 to `NV12`/`P010`/`DTRC`,
but the LCDIFv3 planes have no YUV formats, so `V4l2DecoderSource::create` fails
at `AddFB2` (`EINVAL`) — `v4l2_decode` cannot scan the decoder output out
directly. (`GlCompositor`'s NV12 import could carry it; the source currently
requires a scanout FB.) For decoded video on screen use the GStreamer + G2D
pipeline below.

**Cameras (UVC):** validated with a Logitech C270 (USB 2, YUYV/MJPG) and an MX
Brio (USB 3, YUYV/MJPG/NV12), together on one hub.

- **Capture → Hantro H.264 encode works.** `camera_record --v4l2-encoder
  /dev/video0` (the `vsi_v4l2enc` node) records Constrained Baseline H.264 at
  640×480 (C270), 1280×720 and 1920×1080 (Brio), and from **both cameras
  concurrently** through the single encoder; every stream decodes cleanly with
  correct color.
- **Direct camera scanout is impossible here** — neither YUYV nor NV12 is a
  plane format, so `v4l2_camera_demo` refuses up front (its plane-format check),
  as does `V4l2CameraSource`.
- **CPU path:** `cluster_sim`'s UVC rear-view (YUYV→XRGB via libyuv into a dumb
  buffer, composited into the scene) shows the live camera, but the multi-layer
  scene is GPU-composited every frame and is **visibly choppy** (see the
  composition-cost note above). libyuv is not in the BSP;
  `scripts/build_imx8mp.sh` cross-builds it.
- **Hardware path (preferred): GStreamer + G2D.** `video_player --launch` with
  NXP's 2D engine doing the color conversion keeps the CPU out of the pixels:
  - camera: `v4l2src device=/dev/videoN ! video/x-raw,format=YUY2,width=1280,height=720
    ! imxvideoconvert_g2d ! video/x-raw,format=BGRx,width=1920,height=1080 ! appsink name=sink`
  - file: `filesrc location=clip.h264 ! h264parse ! vpudec ! imxvideoconvert_g2d
    ! video/x-raw,format=BGRx,width=1920,height=1080 ! appsink name=sink` (Hantro decode + G2D)

  **Have G2D scale to the mode size.** `video_player` sizes its one layer to the
  display mode, and the LCDIFv3 PRIMARY cannot scale; a 1280×720 sample against
  a 1920×1080 `SRC_*` rect fails the atomic TEST (`EINVAL`), the layer is dropped,
  no plane is armed, and the player stalls on a black screen waiting for a flip
  that never comes. With G2D scaling, the camera runs at 30 fps with a commit per
  vblank and zero drops, and is **visibly smooth** — against the choppy CPU
  libyuv path above. The file pipeline (60 s 720p H.264, Hantro `vpudec` + G2D
  upscale) is equally smooth: one commit per vblank, zero drops, and no
  `AddFB2`/PRIME import in steady state (the decoder's buffer pool cycles through
  cached FBs). Route camera and decoded video through G2D on this SoC.

  G2D's output is a contiguous dma-buf, so `GstAppsinkSource` imports it
  zero-copy onto the PRIMARY (unlike Vulkan exports). `--file` (decodebin3 →
  NXP V4L2 decoder → CPU `videoconvert`) and the default `videotestsrc` also
  play.
- The BSP ships the GStreamer 1.24.0 runtime (with the NXP `vpu`/`imxvideoconvert`
  plugins) but no headers; the build script cross-builds the matching 1.24.0
  core + plugins-base as a link-only stage that is never deployed, so
  `video_player` runs on the board's own GStreamer. The BSP kernel has no `vivid`/`vicodec`, so the V4L2 integration tests
  skip on this board.

**VRR is advertised but does nothing.** `vrr_capable=true` and the sink offers
48–240 Hz, but at 48 Hz — inside the sink's range, below the 60 Hz mode —
`vrr_sweep --vrr` measures the same jitter as the control (7.19 vs 7.19 ms): the
LCDIFv3+DW-HDMI path never stretches the timing. (40 Hz is below the sink's VRR
floor, so its identical 8.33 ms is expected either way.) Targets above 60 Hz
clamp to 60 because of the mode the controller offers, not the monitor. With
`--vrr`, 30 Hz measured 0.05 ms jitter vs 1.75 ms without (and `--rt` alone did
not help); 30 Hz is an exact divisor of 60, so this is pacing, not VRR.

**Tests:** with `DRM_CXX_TEST_CARD=/dev/dri/card1`, 96 of 98 test binaries pass
(729 tests). The remaining 3 cases in 2 `*_vkms` binaries
(`layer_scene_census` `FullFrameScroll` / `MultiLayerOverlap`,
`layer_scene_content_type`) assert native placement for scenes this single,
non-scaling PRIMARY physically cannot host (see above).

**Example status** (on-device, `card1`):

| Status | Examples |
|--------|----------|
| Works | `driver_caps`, `plane_caps`, `minimal_kms_probe`, `multi_crtc_probe` (`--scene-test`), `stream_probe`, `scene_formats`, `vulkan_display`, `xcursor_smoke`, `atomic_modeset`, `software_present`, `damage_present`, `ring_present`, `idle_present`, `vrr_sweep`, `compressed_scanout`, `layered_demo`, `video_grid`, `overlay_planes`, `scene_warm_start`, `scene_priority`, `test_patterns`, `signage_player`, `hdr_demo`, `cluster_sim`, `thorvg_janitor`, `hotplug_monitor`, `cursor_scene`, `keyboard`, `capture_demo`, `csd_smoke`, `mdi_demo`, `egl_scene`, `gl_present`, `egl_offload_scanout`, `gbm_surface_scanout`, `shadertoy_egl`, `plane_stress`, `allocator_torture`, `tone_mapper_bench` |
| Works with cameras / video | `camera_record` (C270 + MX Brio → Hantro H.264, incl. both at once), `cluster_sim` UVC rear-view (libyuv; choppy), `video_player` (videotestsrc, `--file`, and the hardware `vpudec`/`v4l2src` → `imxvideoconvert_g2d` pipelines) |
| Works (GPU copy) | `vk_present`, `vk_out_fence` (60 fps), `vulkan_offload_scanout`, `vulkan_scene` (~30 fps, composited), `cluster_sim_vulkan` (~20 fps) — all through `VkScanoutProducer` |
| Platform limit | `v4l2_decode`, `v4l2_camera_demo` (no YUV scanout); `mouse_cursor`, `cursor_rotate` (no cursor plane → `Renderer::create` `ENODEV`) |
| Not applicable | `dual_display`, `video_wall_multi` (one connected output); `stream_demo` (no EGL Streams) |
| Not built | `camera` (libcamera) |

#### Present-path profiling (decision guide)

All numbers at **1920×1080**, CPU governor `performance` (1.6 GHz), weston
stopped. CPU/frame is `user+sys` over 600 frames; for the `--seconds`-driven
examples it is the difference between a 13 s and a 3 s run, which leaves out
setup (shader builds, texture uploads). **load@60** is the share of one core
needed for 60 fps (the board has 4). Re-measured on `main` after the GPU-copy and
GPU-import changes.

| Workload (`software_present` unless noted) | render scope       | CPU/frame | load@60 (1 core) | fps | takeaway |
|--------------------------------------------|--------------------|-----------|------------------|-----|----------|
| XRGB8888, full redraw, `--vsync`            | full 1080p         | 6.55 ms   | ~39 %            | 60  | baseline; holds 60 fps vsync-locked |
| XRGB8888, `--no-damage`                     | full 1080p         | 6.53 ms   | ~39 %            | 60  | damage hint is a **no-op** (`fb_damage_clips=false`) |
| RGB565 (`--rgb565`)                         | full 1080p         | 4.75 ms   | ~29 %            | 60  | ~27 % cheaper (half the bytes) |
| `ring_present`                              | buffer-age repaint | 6.57 ms   | ~39 %            | 29¹ | no gain over full redraw at this size |
| `damage_present`                            | partial (box only) | 2.92 ms   | ~18 %            | 29¹ | **incremental rendering ~2.2× cheaper** |
| `idle_present` (change every 30th frame)    | skip unchanged     | 0.15 ms   | ~1 %             | 60  | **idle-skip is the biggest CPU lever** (96 % of flips avoided) |
| `gl_present` (GLES → KMS)                   | Vivante GPU        | 0.85 ms   | ~5 %             | 60  | **render on the GPU: ~8× less CPU** than a CPU full redraw |
| `vk_present` (Vulkan → GL copy → KMS)       | Vivante GPU        | 2.17 ms   | ~13 %            | 60  | Vulkan holds 60 fps through the GPU copy |
| `plane_stress` (4 composited 256² layers, `--churn reshade`) | GPU composition | 2.57 ms | ~15 % | 60 | layers sampled in place (was ~50 ms/frame uploading them) |
| `egl_scene` (GL + dumb, two full-screen layers) | GPU composition | 1.9 ms   | ~12 %            | 30  | GPU-bound: two LINEAR 1080p layers sampled per frame |
| `vulkan_scene` (Vulkan + dumb, two full-screen layers) | GPU copy + composition | 4.5 ms | ~27 % | 30 | same composition cost plus the copy |
| `cluster_sim_vulkan`                        | Vulkan render + GPU copy | 1.55 ms | ~9 %        | 20  | GPU-bound on its 1080p fragment work |
| `tone_mapper_bench` HLG→BT.709              | CPU tone map       | 679 ms    | —                | —   | CPU tone mapping is not real-time here (PQ→BT.709 Reinhard 498 ms, Hable 689 ms) |
| `allocator_torture --frames 600`            | allocator          | —         | —                | —   | 4 PASS (format cascade, rapid churn, slow drift, burst-then-calm); 2 SKIP (need ≥2 planes) |

¹ These two commit without `--vsync`; the synchronous commit blocks for about
half a vblank, so the wall rate sits near 29 fps with CPU to spare.

Guidance for UIs on this device:

- **Render with GLES straight into a GBM surface on `card1`** (`egl_scene` /
  `gl_present` / `GbmSurfaceSource`) and present it as the *only* layer on the
  PRIMARY (a lone full-screen layer lands natively at any `zpos`). That avoids
  composition entirely.
- **Multi-layer LayerScenes are fine for UI-sized layers.** With one plane
  every extra layer is composited, now by the GPU from the layer's dma-buf:
  several overlay-sized layers hold 60 fps; two full-screen layers run ~30 fps.
- **Software rendering is viable** at 1080p60 (~39 % of one core for full
  redraws); damage-driven and idle-skip rendering cut that to ~18 % and ~1 %.
- **Pacing:** 60 Hz is clean (≤0.1 ms jitter); 30 Hz works; 40/48 Hz beat
  against the fixed vblank (~7–8 ms jitter) and VRR does not help.
- **Video and Vulkan need a bridge:** decode to NV12 and convert to RGB (G2D via
  GStreamer) before scanout; Vulkan reaches the display only through
  `VkScanoutProducer`'s GPU copy (60 fps at 1080p, ~13 % of a core) — GLES
  straight into a GBM surface is still the cheapest path (~5 %).

### Raspberry Pi 5 (vc4 + v3d)

Validation device: **Raspberry Pi 5 Model B** (quad Cortex-A76,
8 GB), Raspberry Pi OS trixie, **kernel 6.18.33-rpt**, glibc 2.41. Three nodes:
`card0` = **vc4** (HDMI; the validated output is `HDMI-A-1` at 1280×1440),
`card1` = **drm-rp1-dsi** (`DSI-2`, 800×1280), `card2`/`renderD128` = **v3d**
(render-only; Mesa renderonly pairs it with vc4 for GBM). No compositor runs, so a
non-root user in `video`/`render`/`input` is DRM master as the first opener.

- **Build:** the repo's `rpi5-trixie` emb target (`-mcpu=cortex-a76`, tests on),
  with `-D DRM_CXX_VULKAN=ON` added — V3DV is a real Vulkan driver here. glibc 2.41
  matches the trixie toolchain, so nothing extra is shipped; run with
  `LD_LIBRARY_PATH=<deploy>/src`.
- **Everything that needs only one display works:** all present-spine,
  scene, allocator, cursor (`mouse_cursor`, `cursor_rotate` — real cursor planes),
  Blend2D/CSD (`capture_demo`, `csd_smoke`, `mdi_demo`, `cluster_sim`) and GPU
  examples. `egl_scene` and `vulkan_scene` hold **60 fps**; `vk_out_fence` produced
  and signaled an OUT_FENCE on 60/60 frames; Vulkan- and GL-rendered LINEAR buffers
  scan out (`vulkan_offload_scanout`, `egl_offload_scanout`). vc4 has enough planes
  that multi-layer scenes place natively (`scene_priority` 8/8, `scene_formats`
  4/4, `minimal_kms_probe` 3 assigned).
- **Tests:** 100/100 test binaries pass with `DRM_CXX_TEST_CARD=/dev/dri/card0`.
- **Not applicable here:** `dual_display`/`video_wall_multi` (one connected
  output on `card0`), `stream_demo` (no EGL Streams), `v4l2_decode` (the Pi 5's
  hardware decoder is HEVC-only — no H.264), `v4l2_camera_demo`/`camera_record`
  (no camera; no hardware encoder).

### SA8155P (Adreno 640 + downstream display driver)

Validation target: **SA8155P**, vendor kernel **5.4**, glibc 2.31. The display
driver reports `msm_drm` but is the **downstream SDE** stack, not upstream DPU, and
the target is a shared-display split: one controller publishes a connector per
display consumer, and only the consumer actually presenting is lit.

- **Nodes and connectors.** The KMS node used is `card1` (`card0` has no `/dev`
  node). `driver_caps`: `addfb2_modifiers=true`, `async_page_flip=false`,
  `fb_damage_clips=false`, `vrr_capable=true`, census `PRIMARY=5 OVERLAY=9
  CURSOR=0`. Several connectors report *connected* (`Virtual-1` 4096×2160,
  `DSI-3` 1920×720, `DP-4` 1280×720) but only **`DP-4`** is the visible panel — a
  rank- or first-connected pick renders flawlessly into an unlit slot. Pin it with
  **`DRM_CXX_CONNECTOR=DP-4`** (honored by the example output pickers and by
  `ScanoutBackend::Config::connector_id`). Leave the platform's own display
  services running; drm-cxx only needs `card1`, which is free when nothing else
  presents on it.
- **Planes.** No plane publishes `IN_FORMATS` (LINEAR via the legacy format
  list); every plane's `zpos` is **mutable `[0, 10]`**. Two controller rules matter:
  - **Multirect virtual planes.** 7 of the 14 planes are the second rectangle of
    a hardware pipe, published as their own OVERLAY and marked in the read-only
    `capabilities` blob with `primary_smart_plane_id=<parent>`. Staged without the
    parent the driver rejects them (`r1 only virt plane:N not supported`). The
    registry records the parent (`PlaneCapabilities::multirect_parent`), the
    allocator only places a virtual plane alongside its parent, and the canvas only
    lands on one whose parent is armed.
  - **Equal `zpos` = source split.** Two planes on the same stage are read as a
    left/right split pair and rejected unless adjacent (`invalid coordinates,
    stage:N`).
  - **zpos 9 and 10 are rejected**, even on a lone plane, although every plane
    advertises `[0, 10]`. The allocator writes zpos densely over the armed
    planes (same order, from each plane's minimum), which also keeps values
    distinct and the canvas low.
- **Build — clang against a glibc-2.31 (bullseye-class) sysroot.** The Arm GNU
  toolchains' own libstdc++ needs newer glibc, so C++ links with **clang +
  the sysroot's GCC 10 libstdc++** (`--gcc-toolchain=<sysroot>/usr
  -stdlib=libstdc++ -fuse-ld=lld`) at **C++17** (fmt and the span/expected
  polyfills are vendored). `libdisplay-info` is linked from the sysroot's static
  archive; the target has no `libseat`, so `session` is off. **Blend2D** requires
  C++20 `std::bit_cast`, which GCC 10's libstdc++ lacks — build it with the
  sysroot's **LLVM 18 libc++ linked statically and hidden**
  (`-nostdlib++ -Wl,-Bstatic -lc++ -lc++abi -Wl,--exclude-libs,ALL`): the
  resulting `libblend2d.so` needs only glibc ≥2.29 and exports no C++ runtime, so
  drm-cxx stays on libstdc++ across Blend2D's C ABI. Restrict the CMake toolchain's
  package lookup to the sysroot (`CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY`) — a host
  `tomlplusplus` package otherwise leaks into the cross configure.
- **Fonts** live outside `/usr/share/fonts` on this image; point the text
  renderers at one with **`DRM_CXX_FONT=<path.ttf>`** (checked first by the CSD
  renderer and the signage/cluster/camera examples).
- **GPU:** GL and Vulkan both scan out — `egl_scene`, `gl_present`,
  `shadertoy_egl`, `vulkan_scene`, `vk_present`, `vk_out_fence` (OUT_FENCE 60/60),
  `vulkan_offload_scanout`, `gbm_surface_scanout`. `egl_scene`/`vulkan_scene` hold
  **60 fps**. `vulkan_display` (VK_KHR_display) and `egl_offload_scanout`
  (`eglCreateImage` on the display node's bo) fail on this stack.
- **Native multi-plane works:** `scene_formats` 4/4 assigned, `scene_warm_start`
  3/3, `plane_stress` 4 layers native at one commit per vblank.
- **Tests:** 98/98 test binaries pass with `DRM_CXX_TEST_CARD=/dev/dri/card1`.
- **More layers than planes:** the allocator numbers zpos over the armed planes
  only, so composited layers don't consume the range and the canvas lands above
  the stack. `allocator_torture` 6/6 (N+1: 6 native + 9 composited on one
  canvas); `scene_priority` 7 native + canvas.
- `mouse_cursor`/`cursor_rotate` expect an already-active CRTC and exit when none
  is (no cursor plane exists either); `cursor_scene` works.

### TI AM335x (BeagleBone Black, tilcdc)

Validation board: **BeagleBone Black**, TI AM335x (single Cortex-A8, **ARMv7 /
armhf, 32-bit**), BeagleBoard.org **Debian 13.5 base/console image (2026-05-19),
kernel 6.18.32-bone35**. `card0` is `tilcdc` (the LCD controller); HDMI is driven
through the on-board NXP **TDA19988 (TDA998x)** encoder. There is **no GPU
acceleration** — the PowerVR SGX530 has no open 3D and no Vulkan — so the
software/dumb-buffer present path (`software_present`, `ring_present`) is the
target; GL examples only run on Mesa llvmpipe. Cross-build with
`scripts/build_beaglebone_black.sh` (podman + `debian:trixie` armhf multiarch +
meson, vulkan off).

`driver_caps` reports: addfb2_modifiers=true, **async_page_flip=false**,
prime import/export=true, cursor 64×64, **fb_damage_clips=false**, psr=none.
Its plane census reads `PRIMARY=1 OVERLAY=0 CURSOR=0`, so the cursor 64×64 is
the `DRM_CAP_CURSOR_*` default with **no backing CURSOR plane** — `driver_caps`
emits its cap/registry mismatch **`[WARN]`** here. This is the canonical case
the warning exists for: gate HW-cursor use on an actual CURSOR plane in the
registry, not on the cap (the cursor must otherwise be composited). The same
census + WARN are expected on the i.MX LCDIF / LCDIFv3 controllers.

- **Plane formats: RGB565/XBGR8888/BGR888 — no XRGB8888.** The single plane
  (id 33) advertises only `RG16` (RGB565), `XB24` (XBGR8888), `BG24` (BGR888),
  all LINEAR. drm-cxx's default XRGB8888 (`XR24`) is **not** in the list and is
  rejected at commit, so present with **RGB565** (`software_present --rgb565`, or
  `DumbScanoutSink` Config `drm_format=DRM_FORMAT_RGB565`).
- **Low pixel-clock ceiling; modes self-downgrade.** The LCDC ceiling sits below
  ~168 MHz, so `tilcdc`'s `mode_valid` filters the EDID automatically — e.g. an
  LG HDR DQHD offering native 1280×1440@60 (168.9 MHz) and 4K@30 (297 MHz) is
  downgraded to **1280×1024@60 (108 MHz)** with no manual `video=` forcing.
- **HDMI overlay path mismatch (no `/dev/dri` on first boot).** The factory eMMC
  U-Boot (2019.04) searches `/lib/firmware/` for `.dtbo` overlays, but the 2026
  image ships them under `/boot/dtbs/$(uname -r)/overlays/` — so the HDMI virtual
  cape (`BB-HDMI-TDA998x-00A0.dtbo`) fails to load, the `lcdc` node never
  appears, and there is no DRM device (U-Boot log: `uboot_overlays: unable to
  find [...]`). Fix without reflashing U-Boot:
  `sudo cp /boot/dtbs/$(uname -r)/overlays/*.dtbo /lib/firmware/ && sudo reboot`.
- **Seat/VT on a headless serial-console image.** There is no `seatd.service`;
  libseat falls back to its in-process builtin backend, which must open the VT
  (`/dev/tty0`) and therefore needs **root** — so DRM-master examples fail as the
  `debian` user over SSH (`Could not open target tty: Permission denied`). Run
  them as root (`sudo env LD_LIBRARY_PATH="$PWD" ./<example>`) or pass
  **`--no-seat`** (skips libseat, opens DRM directly + takes master as the first
  opener — handled by the common `open_output` helper). Read-only tools
  (`driver_caps`, `plane_caps`) need neither.
- **Minimal base image.** The console image ships none of the userspace libs
  drm-cxx links — install them once:
  `sudo apt install libdrm2 libfmt10 libgbm1 libinput10 libseat1 libxkbcommon0 libdisplay-info2`.
  Cursor demos additionally need a theme: `sudo apt install dmz-cursor-theme`.
- **Single-plane composition fallback.** tilcdc has exactly one plane (PRIMARY,
  no overlay), so any scene with more layers than planes must composite the
  overflow onto a canvas armed on that one plane. The `LayerScene` composition
  canvas was hard-coded ARGB8888, which tilcdc doesn't scan out, so the canvas
  could never be armed and the overflow layers were dropped. The canvas now
  adopts a plane-supported format (here **XBGR8888**, R↔B-swapped from the
  internal ARGB8888 blend); `minimal_kms_probe /dev/dri/card0 3` reports
  **composited 3 / dropped 0** onto the single plane (was assigned 1 / dropped 2).
- **No hardware cursor at all.** `DRM_CAP_CURSOR_*` reports 64×64, but that is a
  legacy default that lies: the plane registry has **no CURSOR plane**, and the
  legacy `drmModeSetCursor` ioctl returns **ENXIO**. The cursor renderer
  correctly gates on the registry and falls back to the legacy path (it does
  *not* try to arm a nonexistent plane — `mouse_cursor` logs `Cursor path:
  legacy drmModeSetCursor`), but legacy then fails on this controller, so a HW
  cursor is impossible — the cursor must be software-composited (a `CursorSource`
  layer through the now-working single-plane composition path). Gate HW-cursor
  use on the registry, never on `DRM_CAP_CURSOR_*`.
- **Working example set** (verified on-device; what `build_beaglebone_black.sh`
  installs): `software_present`, `ring_present`, `idle_present`, `damage_present`
  (all present via the RGB565 negotiation) + `driver_caps`, `plane_caps`,
  `stream_probe`, `minimal_kms_probe` (read-only diagnostics). The rest are
  excluded — they need XRGB8888 (rejected by tilcdc at `AddFB2`, not just absent
  from `IN_FORMATS`), a GPU, multiple CRTCs, a cursor plane, HDR, compression, or
  a camera. Any present example works here only if it calls
  `present::negotiate_scanout_format()` rather than hardcoding XRGB8888.

### StarFive JH7110 (VisionFive 2, riscv64)

Validation board: **StarFive VisionFive 2**, JH7110 (quad SiFive U74, **64-bit
RISC-V, rv64gc, riscv64**), **Ubuntu 24.04.3 LTS, kernel 6.12.5-starfive**.
`card1` is the `starfive` display subsystem (Verisilicon **dc8200** controller +
Innosilicon **inno-hdmi**); `card0`/`renderD128` is the Imagination PowerVR GPU
(`pvrsrvkm`, render-only). Cross-build with `scripts/build_visionfive2.sh`
(podman + `debian:trixie` riscv64 multiarch + meson, vulkan off). This is the
project's primary **riscv64** target, so it exercises the scalar (non-NEON)
fallbacks — `composite_canvas`'s `convert_row` shadow→scanout packing among them
(`test_composite_canvas`, `test_negotiate` and the full on-device test suite
pass: 63 gtest/assert binaries green, the rest skip for "no dumb-capable card"
when the desktop holds the device).

- **Cross-distro runtime soname gap.** The container builds against trixie
  (`libfmt.so.10`, `libdisplay-info.so.2`) but the board runs Ubuntu 24.04
  (`libfmt.so.9`, `libdisplay-info.so.1`). Ship the trixie `.so`s next to the
  binaries (the build leaves `libtomlplusplus.so.3` in the tree; extract
  `libfmt10`/`libdisplay-info2`/`libseat1` from a `debian:trixie` riscv64
  container) and point `LD_LIBRARY_PATH` at them, or rebuild against an Ubuntu
  base. `libinput.so.10`/`libdrm`/`libgbm` already match.
- **Rich multi-plane controller.** Two CRTCs; planes report PRIMARY×2,
  OVERLAY×4, CURSOR×2 (per-plane `zpos [0,5]`, rotation + scaling on most).
  `minimal_kms_probe /dev/dri/card1 3` assigns **3/3 to hardware planes, 0
  composited** — confirms the single-plane canvas-format change does not regress
  native assignment on a plane-rich controller (ARGB8888 stays the first canvas
  preference). The cursor renderer takes the **atomic CURSOR plane** path here
  (`mouse_cursor` logs `Cursor path: atomic CURSOR plane`), unlike tilcdc's
  legacy fallback.
- **inno-hdmi EDID read needs a live pipe.** With no forced mode the connector
  hotplug-detects (`status=connected`) but EDID reads back **`EDID has corrupt
  header`**, so the kernel populates **0 modes** and leaves the pipe disabled —
  no desktop, and KMS clients see "no usable output". Forcing any low-clock mode
  on the kernel cmdline powers the PHY/DDC enough that **EDID then reads
  correctly**; the desktop subsequently renegotiates the monitor's native mode
  (e.g. 3840×2160@30) from the now-valid EDID. **1080p60 is rejected** by the
  driver (`[drm] User-defined mode not supported "1920x1080"` for both the
  148.5 MHz CEA and the CVT-RB timing); **720p60 works** as the bootstrap.
  Persist it via `/etc/default/u-boot` `U_BOOT_PARAMETERS+=" video=HDMI-A-1:1280x720@60"`
  then `sudo u-boot-update` (this board uses U-Boot + extlinux, regenerated by
  `u-boot-update`).
- **Cold-KMS output resolution.** The desktop (gdm) owns DRM master on `card1`;
  stop it (`sudo systemctl stop gdm`) to run KMS examples (then `sudo … --no-seat`).
  Once gdm is stopped nothing has a bound encoder, so the example helper's old
  "first connected connector **with an attached encoder**" rule failed. The
  `open_and_pick_output` helper now resolves a CRTC from the connector's
  *possible* encoders when none is bound — so the present examples work on a
  freshly-booted / headless-but-connected board, not just under a running
  compositor.
- **Present path validated (riscv64, 4K@30).** With gdm stopped, `software_present`,
  `ring_present`, `idle_present` (frame-economy: 1 committed / 59 skipped) and
  `damage_present` all scan out via the dumb-ring + atomic-flip path. First-run
  `software_present` is occasionally a no-op (inno-hdmi pipe still settling after
  master changes hands); a retry presents — not a drm-cxx issue.
  `DRM_FORCE_MODE=WxH` (honored by `kms_present`) runs at a lower mode than the
  4K preferred — useful for bandwidth-limited bring-up.
- **PowerVR can fail to initialize at boot (firmware-load race) — recover
  without a reboot.** On some boots `pvrsrvkm` probes (~4 s) before the rootfs
  `/lib/firmware` is mounted, so `Direct firmware load for rgx.fw.… failed with
  error -2` and the GPU/render node **never appears** — only the dc8200 display
  node is present, and it enumerates as `card0` (numbering shifts vs the GPU-up
  case). A reboot just re-runs the same race. Recover live (the firmware is
  readable post-boot) by rebinding the driver:
  `echo 18000000.gpu | sudo tee /sys/bus/platform/drivers/pvrsrvkm/bind` — the
  RGX firmware + shader then load and `card1`/`renderD128` appear (reversed
  numbering: display `card0`, GPU `card1`). A permanent fix is firmware in the
  initramfs or a boot-time rebind unit (board provisioning).
- **GPU-accelerated scanout works via the GBM-on-display (kmsro) path — open
  EGL/GBM on the *display* node, not the GPU.** `card0`/`renderD128` (`pvrsrvkm`)
  is render-only; its Mesa stack (`pvr_dri.so`, `libGLESv2_PVR_MESA`, `libgbm`,
  RGX firmware loaded) does GLES/EGL/GBM. The correct pattern is to create the
  GBM/EGL renderer on the **display** node (`card1`), not the GPU: Mesa's
  **kmsro** layer transparently routes rendering to the PowerVR and allocates a
  display-native (LINEAR) scanout buffer, so the display never imports a foreign
  buffer. (This is the standard split-SoC compositor setup — the display node is
  the primary GBM device and the render GPU is secondary.) Build with the `egl` feature on (`-Degl=enabled`, decoupled from
  `-Dstreams`) and run on `card1`:
  - `egl_scene /dev/dri/card1` — GBM-surface EGL producer: **90 frames / 3 s (30 fps)**.
  - `gl_present /dev/dri/card1` — library `GlScanoutProducer`: **120 frames**.
  - `gbm_surface_scanout` — EGL swapchain: `display TEST_ONLY of LINEAR: ACCEPTED`,
    LINEAR scanned out.

  **Verify the renderer — the open PowerVR GL is software-class here.** Mesa's
  open PowerVR Gallium driver (`pvr_dri.so`) is incomplete, so GBM/EGL on the
  display node can resolve to `softpipe`/`llvmpipe` (software) rather than the
  GPU — check `glGetString(GL_RENDERER)`. Hardware GL is in principle reachable
  via **zink** (GL-on-Vulkan) over the PowerVR Vulkan driver (`libVK_IMG.so`,
  registered in `/etc/vulkan/icd.d/`; `vulkaninfo` shows "PowerVR B-Series
  BXE-4-32"), forced with `MESA_LOADER_DRIVER_OVERRIDE=zink` — **but zink over
  the B-series Vulkan currently segfaults during render** (the driver lacks
  `fillModeNonSolid`/`EXT_line_rasterization` and is too immature). So there is
  **no usable hardware GL on the JH7110** today; `drm::scene`'s GPU compositor
  (`GlCompositor`) rejects the software renderer and falls back to the CPU
  `CompositeCanvas` (the production-correct choice on this board). **Leave
  `DRM_CXX_COMPOSITOR_ZINK` unset here** — it would force the crashing zink path.
  Contrast the TI J721E below, where zink-on-PowerVR-Rogue does work. The CPU
  present/scanout paths above are unaffected.
- **`egl_offload_scanout` (manual cross-device import) does NOT work here — wrong
  direction for a kmsro display.** It allocates on the GPU node and asks the
  display to import, which **fails at `drmPrimeFDToHandle` with ENOSYS**: the
  `starfive` display advertises `DRM_PRIME_CAP_IMPORT` but cannot import a
  *foreign* (PowerVR) dma-buf. The GPU and display also share only `LINEAR`
  (PowerVR renders `LINEAR` + `0x92…` tiled; the dc8200 scans out `LINEAR` +
  `0x0b…` tiled). That manual-import path is for displays that import the GPU's
  buffer (e.g. Rockchip VOP2 + Mali AFBC); for a kmsro display use the
  GBM-on-display path above. (The two robustness fixes it prompted are still
  correct and kept: the example's LINEAR fallback when `gbm_bo_create_with_modifiers2`
  is rejected, and `import_dmabuf` using plain `AddFB2` for `INVALID`/LINEAR so
  import lands on minimal controllers that *do* import.)
- **Other examples validated on `card1` (multi-plane, atomic):** `scene_priority`
  / `scene_formats` (allocator + per-plane format probe — ARGB8888 scaler-required,
  ABGR8888 channel-swap, XRGB8888 no-scale), `cursor_scene` (atomic CURSOR plane),
  `atomic_modeset` (page flips on CRTC 32).
- **PRIME is directional here — don't read the offload ENOSYS as "no PRIME".**
  *GPU imports the display's buffer* (the kmsro direction) **works** — that's how
  `egl_scene` / `gl_present` render on the PowerVR into a card1-allocated
  buffer. Only *display imports the GPU's buffer* fails (`drmPrimeFDToHandle`
  ENOSYS), which is exactly the direction the GBM-on-display path avoids. The
  `DRM_PRIME_CAP_IMPORT` bit being set does not imply the driver can import a
  buffer allocated by a *different* device.
- **No HW compression is achievable for drm-cxx-produced content on this pairing.**
  `compressed_scanout /dev/dri/card1` negotiates correctly — it enumerates the
  dc8200's modifiers and ranks compression/tiling first (seven `0x0b…` candidates
  ahead of `LINEAR`) — but the chosen result is **`LINEAR`** ("no scanout saving").
  Two separate reasons, depending on who produces the pixels:
  - **GPU-produced content** (`GbmSurfaceSource` / the EGL producers): drm-cxx
    *does* negotiate modifiers here, and on amdgpu / Rockchip this path picks
    **DCC / AFBC** — so the negotiation itself is sound. On *this* board it still
    resolves to `LINEAR` because the GPU and display share **no** compressed/tiled
    modifier (PowerVR renders `LINEAR + 0x92…`; the dc8200 scans out
    `LINEAR + 0x0b…`; intersection = `LINEAR`). That's a hardware-pairing limit,
    not a drm-cxx one — no shared modifier exists to choose.
  - **CPU-produced content** (`dumb::Buffer` → `CompositeCanvas`, the software
    present path): always `LINEAR` *by construction*. `DRM_IOCTL_MODE_CREATE_DUMB`
    has no modifier concept (there is no "tiled dumb buffer"), and emitting a
    tiled/compressed layout would mean encoding an AFBC/DCC/tiling swizzle on the
    CPU — impractical (compression is a fixed-function HW block). The achievable
    bandwidth wins for the CPU path are a smaller-bpp format (RGB565 vs ARGB8888)
    and damage/dirty-rect (`FB_DAMAGE_CLIPS` + the canvas's per-frame dirty flush),
    not modifier compression.

### TI J721E (BeagleBone AI-64, aarch64)

Validation board: **BeagleBone AI-64**, TI J721E (dual Cortex-A72, **aarch64**),
**Debian 11 Bullseye, kernel 5.10-ti-arm64**. `card0` is `tidss` (the TI K3
Display SubSystem, has a CRTC); `card1`/`renderD128` is the Imagination
**PowerVR Rogue GE8430** (`pvrsrvkm`). Unlike the JH7110 above, the GPU
initializes **at boot** (RGX firmware `rgx.fw.22.104.…` loads cleanly).

- **Native Mesa GL on the display node is software (`llvmpipe`).** GBM/EGL on
  `card0` falls back to `llvmpipe` because Mesa wants `tidss_dri.so` — the kmsro
  display stub — and it is **not in this Mesa 22.3.5 build** (`MESA-LOADER:
  failed to open tidss`), so kmsro cannot pair the display with the PowerVR
  render node. Verify with `glGetString(GL_RENDERER)` before assuming hardware.
- **Hardware GL is reached via zink (GL-on-Vulkan).**
  `MESA_LOADER_DRIVER_OVERRIDE=zink` →`GL_RENDERER = zink (PowerVR Rogue
  GE8430)` — a real GPU. The PowerVR **Vulkan** driver is the enabler
  (`powervr_mesa_icd` + TI's `libVK_IMG.so`; `vulkaninfo` shows the Rogue).
  zink warns it lacks `fillModeNonSolid` / `EXT_line_rasterization` — irrelevant
  for the filled textured quads `GlCompositor` draws. (This is where the JH7110's
  zink path crashes but the Rogue's works.)
- **GPU→display dma-buf import is ENOSYS but non-fatal for readback.** zink logs
  `failed drmPrimeFDToHandle Function not implemented` (the PowerVR Vulkan device
  cannot import the tidss gbm buffer — the same directional-PRIME limit as the
  dc8200), but Mesa **CPU-copies** its render into the gbm front buffer, so
  `gbm_surface_lock_front_buffer` + `gbm_bo_map` reads correct pixels. Real KMS
  scanout of a GPU-rendered buffer would still hit the import wall →
  commit-failure → CPU demotion (handled by `LayerScene`).
- **`drm::scene` GPU composition — hardware pixel readback validated.** Running
  `GlCompositor`'s exact shader/geometry/blend on the PowerVR Rogue under zink
  and reading the result back via gbm map lands an opaque-red square at the
  canvas top-left, in the **red channel**, over a transparent-black clear —
  confirming Y-orientation, the BGRA swizzle and the premultiplied blend on real
  GPU hardware. The software-renderer guard correctly **accepts** `zink (PowerVR
  Rogue …)` and rejects software, so on these embedded split-GPU boards the GPU
  path is **opt-in** (Mesa's default is `llvmpipe` → guard → CPU canvas):
  - **`DRM_CXX_COMPOSITOR_ZINK=1` (preferred).** `GlCompositor` retries with zink
    **only when the default renderer is software**, so it never overrides a
    board's native hardware GL (amdgpu/i915 keep radeonsi/iris), and it scopes
    the override to the compositor's own context. Off by default.
  - `MESA_LOADER_DRIVER_OVERRIDE=zink` — the lower-level Mesa knob; forces zink
    for **all** GL in the process (including GBM producers), even where native GL
    is hardware, so prefer the drm-cxx knob. If this is already set, `GlCompositor`
    leaves it untouched.

  Do **not** set either on the JH7110 — zink-on-the-B-series segfaults; leave it
  on the CPU canvas there.
- **Build (bullseye/aarch64).** glibc 2.31 + gcc 10 — drm-cxx builds under
  `cpp_std=c++17` (the `detail/` polyfills cover `std::expected`/`span`/`format`).
  A full lib build additionally needs `libdisplay-info ≥0.2.0` (not packaged in
  bullseye — build from source) and `fmt` (meson auto-fetches it as a CMake
  subproject); `session`/`vulkan` can be disabled.

### NVIDIA EGL Streams

- **EGL needs `EGL_DRM_MASTER_FD_EXT`.** Without it,
  `eglStreamConsumerOutputEXT` EINVALs. Use the EGL 1.5 core
  `eglGetPlatformDisplay` (`EGLAttrib*`), not the EXT variant.
- **Producer-surface order matters.**
  `eglCreateStreamProducerSurfaceKHR` EINVALs on a consumer-less
  stream; defer producer surface creation until after
  `eglStreamConsumerOutputEXT`.
- **Mixing support is empirical-probe only.** Never trust version
  checks; one `DRM_MODE_ATOMIC_TEST_ONLY` commit is the honest signal.
- **Desktop NVIDIA 535+ and Tegra L4T 5.15 have no `EGL_NV_output_drm_atomic`.**
  The Streams pipeline appears to wire end-to-end but no producer
  frames reach KMS; not closable from drm-cxx. `stream_demo` self-
  detects this and falls back to a CPU-rendered background animation
  so the rest of the configuration still demonstrates. Point desktop
  NVIDIA users at GBM where GBM works (see Tegra L4T quirks for the
  Tegra GBM gap).

### Tegra L4T

Validated on kernel `5.15.148-tegra` (Jetson L4T R35-class). NVIDIA
runs HDR / color management / scanout-side compositing through their
proprietary NvDisplay stack, parallel to and outside the standard
DRM/KMS interface. As a result the KMS-facing surface is sparse:

- **No KMS-side HDR signaling.** The connector exposes no
  `HDR_OUTPUT_METADATA`, no `Colorspace`, and no `max_bpc` property.
  `hdr_demo` correctly reports `hdr_blob=false colorspace=false
  can_signal_hdr=false` and commits anyway with the auto-derive
  silently degrading to SDR — the "no silent banding" path in
  `LayerScene::set_output_metadata`. The bytes scan out; the sink
  sees plain SDR.
- **No CRTC color pipeline.** No `DEGAMMA_LUT`, no `CTM`, no
  `GAMMA_LUT` on the CRTC. `probe_crtc_capabilities` returns all
  false; the `hdr_demo --no-hw-pipeline` tier is forced regardless
  of the flag.
- **`gbm_surface_create_with_modifiers` returns ENOSYS.** `egl_scene`
  fails at `GbmSurfaceSource::create` after modifier negotiation has
  already succeeded (the format-modifier query path works; the
  surface-allocation step does not). Use EGL Streams instead — the
  pre-acquire / atomic-test path is fully functional even though the
  scanout extension above is missing.
- **V4L2 DMA-BUF zero-copy tears on motion.** Producer
  (`uvcvideo` over USB) doesn't attach `dma_resv` reservation fences
  to its dmabufs, and Tegra's KMS does not insert any synchronization
  on import either. The `LayerScene` release-timing contract is
  honored and the V4L2 buffer pool has headroom (verified at
  `buffer_count=8`); neither moves the needle. Suspected root cause
  is cache coherence between the USB controller's writes and the
  Tegra display engine's reads on the V4L2-allocated buffer —
  Tegra-specific kernel sync (NvBuf_SyncObj_Wait or equivalent)
  would have to ship in the kernel for this to land cleanly.
  **Workaround**: `v4l2_camera_demo --mode mmap` forces a per-frame
  CPU memcpy into a DRM dumb buffer; clean output verified on
  Logitech C920 and 046d:0825. The auto-mode default could
  reasonably be switched to MmapCopy on detected Tegra hosts, but
  that hasn't shipped.
- **V4L2 explicit fences absent.** The `V4L2_BUF_FLAG_OUT_FENCE` /
  `V4L2_BUF_FLAG_IN_FENCE` flags don't exist in this kernel's
  `linux/videodev2.h`, and `uvcvideo` would not expose them either.
  Any "real fix via V4L2 out-fence → KMS `IN_FENCE_FD`" approach to
  the tearing above is not implementable on this stack until those
  patches land upstream and L4T rebases.
- **`DRM_FORMAT_NV20` missing from libdrm 2.4.113.** L4T ships an
  older libdrm than the project's minimum. References to the
  fourcc need `#ifdef DRM_FORMAT_NV20` guards
  (`src/core/format.cpp`, `tests/unit/test_format.cpp`).
- **Vulkan IS fully available on Jetson Orin (nvgpu).** NVIDIA
  proprietary Vulkan 1.3.251 driver 540.4.0 with the ICD at
  `/etc/vulkan/icd.d/nvidia_icd.json`. All extensions drm-cxx wants
  are present: `VK_KHR_display`, `VK_EXT_acquire_drm_display`,
  `VK_KHR_swapchain`, `VK_EXT_image_drm_format_modifier`,
  `VK_EXT_external_memory_dma_buf`. `vulkan_scene` renders a
  Vulkan-produced ARGB8888 layer onto DP-1 at locked 60 fps.
  (Earlier validation runs misreported Vulkan as unavailable; the
  cause was a `vulkan_display` linker issue, not Tegra missing
  Vulkan — see the `drm::vulkan::Display::create` note in
  `src/vulkan/display.cpp` for the dlopen-on-demand fix.)
- **KMS planes have no `zpos` property.** All 6 planes (2 PRIMARY + 2
  CURSOR + 2 OVERLAY, two of each per CRTC) report `zpos_min`/`zpos_max`
  as absent. Stacking is fixed by plane type: OVERLAY above PRIMARY,
  CURSOR above OVERLAY. The allocator's standard +10 score for
  `layer.zpos == plane.zpos_min` can't fire here, so the bipartite
  matcher ties on otherwise-identical layers and picks a plane
  arbitrarily (full-screen bg can land on OVERLAY and hide an instrument
  layer on PRIMARY). The 2026-05-17 `allocator.cpp` fix adds a
  Tegra-style fallback: when `plane.zpos_min` is absent, `layer.zpos=0`
  scores +10 toward PRIMARY and `layer.zpos>0` scores +8 toward
  OVERLAY. Callers that want a specific stack order should set explicit
  `display.zpos` values (0 = bottom, >0 = above).
- **Mode validation is strict against EDID.** The driver rejects any
  atomic-commit mode blob not derived from an EDID-advertised mode with
  `EINVAL` at first commit — even when the panel's Display Range Limits
  descriptor declares the timing is in range. CVT-RB2 modelines
  synthesized at runtime (cluster_sim's `--mode WxH@Hz` for non-EDID
  refresh rates) all fail, including ones well within DP bandwidth.
  Verified with an LG UltraGear+ panel that advertises 240 Hz max V
  rate / 1060 MHz max pclk yet only ships 1920×1080@120 and lower
  Hz-DTDs in EDID; 1920×1080@144 / @165 / @240 synthesized modes all
  hit EINVAL. **Escape:** `video=DP-1:WxH@Hz` on the kernel cmdline
  injects the mode at boot before userspace ever commits. The helper
  `scripts/jetson_force_mode.sh --apply DP-1 WxH Hz` edits
  `/boot/extlinux/extlinux.conf` with a timestamped backup; reboot
  required. Untested for refresh > 120 Hz pending a tolerable
  reboot-recovery story.
- **DPCD / AUX channel not exposed to userspace.** The proprietary
  `nvidia_drm` / `nvidia_modeset` stack does not surface
  `/dev/drm_dp_aux*` or the standard DRM connector debugfs. PSR
  capability of the attached panel can't be probed, and even if the
  panel supports PSR (rare on external DP), the proprietary driver
  doesn't advertise or engage it. Commit-skip on all-unchanged frames
  is therefore CPU-side only — no panel-side power benefit available
  until the open-tegra driver path lands.
- **DRM dumb-buffer mmap is write-combined.** NEON reads from a source
  dumb buffer stall hard (~250 ns/px effective vs ~5 ns/px theoretical
  cached read), confirmed empirically by swapping the load with a
  constant in `CompositeCanvas::blend_into`'s pure_opaque_copy path
  (blend time drops from 12 ms to 1.2 ms for the same pixel count).
  Producer-side (Blend2D) writes hit the WC streaming-store ceiling
  fast enough; the problem is only on the consumer side when the same
  buffer is then read for CPU compositing. The cluster_sim refactor
  works around this by either (a) caching pre-rendered templates in
  userspace `std::vector<uint8_t>` and `memcpy`-blitting into the
  layer's dumb buffer per frame, or (b) merging multiple small layers
  into one larger layer that lands directly on a hardware plane (no
  CPU compose). Pattern: keep CPU-write paths to dumb buffers (fine),
  avoid CPU-read paths from them (slow).
- **Validated cluster_sim configurations.** `cluster_sim
  /dev/dri/card1` runs at 60 fps locked on the EDID-preferred
  2560×1440@60 mode; `cluster_sim --mode 1920x1080@120 /dev/dri/card1`
  runs at 120 fps locked. Zero missed vblanks over 30 s captures in
  both modes via `CLUSTER_SIM_FRAME_JITTER=1`. Per-frame paint cost is
  ~0.76 ms after the template-cache + skip-paint optimizations (see
  the `cluster_sim:` commits between 2026-05-17 and 2026-05-18).

### VAAPI

- **MJPEG decode surface chroma must match input sampling.** UVC
  emits 4:2:2 MJPG; mismatched decode surface yields green screens.
  Two-surface YUYV → VP → NV12 path covers 4:2:2. Logitech C270 is
  broken under radeonsi VCN regardless.

### General DRM

- **Atomic `TEST_ONLY + PAGE_FLIP_EVENT` = EINVAL.** Kernel rejects
  the combo. Mask `PAGE_FLIP_EVENT` before OR-ing in `TEST_ONLY`.
- **`drmIsMaster` is lagged.** The kernel can refuse commits with
  EACCES while `drmIsMaster` still reports 1. libseat's pause_cb
  lags even further — treat EACCES on commit as the timely pause
  signal.

---

## Diagnostic tools

Quick-reference for the in-tree utilities used to characterize
hardware:

| Tool                                         | What it answers |
|----------------------------------------------|-----------------|
| `examples/advanced/multi_crtc_probe`         | Can this card commit ≥2 CRTCs in one atomic ioctl? Does the `SceneSet` mirrored-layer path land? |
| `examples/scene/video_wall_multi --list-outputs` | What connectors are connected on this card, in DRM enumeration order? |
| `examples/advanced/stream_probe`             | Does this card / driver expose `EGL_EXT_output_drm` end-to-end? |
| `examples/basics/atomic_modeset`             | Does a single-CRTC TEST commit land? Smallest possible KMS smoke. |
| `examples/basics/hotplug_monitor`            | Does udev hotplug fire for this card's connector add/remove events? |
| `tests/integration/test_*`                   | Per-module hardware-gated tests; `GTEST_SKIP` when their prerequisites aren't met. |
| `examples/allocator/plane_caps`              | What `(fourcc, modifier)` pairs does each plane advertise, decoded to vendor + class? |
| `DRM_EXT_DMABUF_DEBUG=1`                     | Env var: prints which step inside `ExternalDmaBufSource::create` returned EINVAL. Zero-cost when unset. |
| `DRM_FMT_DUMP_EGL_MODS=1` / `DRM_FMT_DUMP_VK_MODS=1` | Env vars (`egl_offload_scanout` / `vulkan_offload_scanout`): print the renderable-vs-scannable modifier sets so you can see whether the render∩display intersection has a compressed entry. |
| `DRM_FMT_FORCE_COMPRESSION=1`                | Env var (`compressed_scanout`, `vulkan_offload_scanout`, `gbm_surface_scanout`): drop non-compression candidates, forcing the GPU to commit to a compressed buffer or fail — the definitive test of whether a stack can export compression. |

### Probe workflow on an unfamiliar card

```
# 1. Confirm the kernel exposes universal planes + atomic.
./builddir/examples/basics/atomic_modeset /dev/dri/cardN

# 2. If multi-output, characterize the combined-commit path.
./builddir/examples/advanced/multi_crtc_probe \
    --device /dev/dri/cardN --scene-test --mirror

# 3. Visual smoke on the multi-CRTC stack.
./builddir/examples/scene/dual_display /dev/dri/cardN

# 4. (NVIDIA only) check whether EGL Streams reaches KMS.
./builddir/examples/advanced/stream_probe /dev/dri/cardN
```

---

## Reference examples

Examples that double as live-validation harnesses for specific
hardware categories:

| Example                        | Validates |
|--------------------------------|-----------|
| `signage_player`               | 5-layer composition fallback + LayerScene allocator under load. amdgpu RDNA2 baseline. |
| `cluster_sim`                  | UVC + libyuv (rear-view tier), vicodec fallback. Automotive showcase. |
| `hdr_demo`                     | HDR pipeline phases 1-9; amdgpu (with HDR EDID), vkms (GAMMA_LUT only fallback). |
| `cursor_rotate`                | Cursor module HOTSPOT property + driver-side rotation, validated on F41 virtio-gpu. |
| `capture_demo`                 | UVC + libcamera zero-copy NV12, VAAPI MJPEG path. |
| `csd_smoke`                    | Client-side decoration overlay reservation. |
| `dual_display`                 | Minimal SceneSet multi-CRTC, mirrored layer, VT-switch survival. |
| `video_wall_multi`             | SceneSet spanning N CRTCs, cells straddling output boundaries, layout cycler. |
| `mdi_demo`                     | Blend2D Tier 0 plane presenter, window animation. |
| `stream_demo`                  | EGL Streams end-to-end (NVIDIA Quadro). |
| `vulkan_display`               | Vulkan WSI direct-to-display. |
| `egl_scene`                    | `GbmSurfaceSource` + EGL/GLES 3 producer + `LayerScene::candidate_modifiers` negotiation. |
| `vulkan_scene`                 | Vulkan-rendered scene layer via `VK_EXT_image_drm_format_modifier` + `ExternalDmaBufSource` (the deliberate non-`gbm_surface` Vulkan path). |
| `plane_caps`                   | Read-only `IN_FORMATS` dump decoded through `drm::fmt` (`classify` / `describe`). First tool to run on any new compression-capable card. |
| `compressed_scanout`           | `drm::fmt` end-to-end: GBM allocate → rank candidates → `TEST_ONLY` → present. amdgpu / vc4 / Rockchip. |
| `egl_offload_scanout`          | EGL/GLES render → dma-buf export → import on the display node. The AFBC/DCC offload harness (Steam Deck DCC, Radxa AFBC). |
| `vulkan_offload_scanout`       | Vulkan `VK_EXT_image_drm_format_modifier` render → dma-buf → import. Steam Deck DCC validated; panvk lacks the dma-buf extensions. |
| `gbm_surface_scanout`          | `gbm_surface` swapchain producer — the compositor-path counterpart to the offload demos. |

---

## Adding a new card

When you bring up drm-cxx on hardware that isn't in [§ Validated GPUs](#validated-gpus):

1. **Open an issue** with the output of `multi_crtc_probe` and the
   minimal example that fails (or doesn't, in which case add it to
   the validated list).
2. **Driver-specific quirks** belong in this file's [§ Driver quirks](#driver-quirks)
   section under the matching driver header. Cite a kernel version
   where the quirk was observed.
3. **Tests that gate on a specific driver** should `GTEST_SKIP` with
   a message naming both the driver and the required behavior, so
   `ctest` output reads cleanly across hardware.
