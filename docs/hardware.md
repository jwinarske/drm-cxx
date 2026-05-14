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
| `amdgpu`     | RDNA2 desktop (1× DP-1, 1× HDMI-A-1)| 6.19.14-200.fc43    | signage_player, cluster_sim, hdr_demo, csd_smoke, capture_demo, cursor_rotate, dual_display, video_wall_multi |
| `amdgpu`     | RDNA1, integrated APU               | 6.11+               | Multi-CRTC `SceneSet::test`, dual_display |
| `i915`       | Skylake / Kaby Lake iGPU            | 6.x                 | Single-CRTC scene paths; cursor 64×64 floor (see quirk) |
| `vkms`       | Configfs-spawned (kernel ≥6.11)     | 6.11+               | Most integration tests; multi-CRTC via `scripts/vkms_dual.sh` |
| `vicodec`    | M2M FWHT codec endpoints            | 6.x                 | `V4l2DecoderSource` NV12 + P010 paths |
| NVIDIA       | Quadro RTX A2000 (NVRM 535.288.01) | 6.x                 | `EglStreamSource` end-to-end (acquire, atomic test, scene wiring). Desktop driver gap noted in quirks. |
| Tegra        | Jetson-class (NVRM kernel side)    | -                   | Producer-surface path exercised; not full visible-scanout validated. |

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
- **Desktop NVIDIA 535+ has no `EGL_NV_output_drm_atomic`.** The
  Streams pipeline appears to wire end-to-end but no producer frames
  reach KMS; not closable from drm-cxx. Point desktop NVIDIA users
  at GBM.

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
| `DRM_EXT_DMABUF_DEBUG=1`                     | Env var: prints which step inside `ExternalDmaBufSource::create` returned EINVAL. Zero-cost when unset. |

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
