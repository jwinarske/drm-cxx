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
