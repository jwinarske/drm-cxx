<!--
SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
SPDX-License-Identifier: MIT
-->
# scripts/

Helper scripts for developing and testing drm-cxx.

| script | what it does |
|---|---|
| `build_deck.sh` | Cross-build the library + an example for the Steam Deck (see below). |
| `build_visionfive2.sh` | Cross-build (riscv64) + optionally flash an SD card for the StarFive VisionFive 2 (see below). |
| `build_beaglebone_black.sh` | Cross-build (armhf) + optionally flash an SD card for the BeagleBone Black. |
| `build-deps.sh` | Build the third-party deps the CI matrix needs from source. |
| `build-matrix.sh` | Run the local compiler/build-system matrix. |
| `format.sh` | clang-format the tree. |
| `vkms_dual.sh` | Load vkms with two connected outputs for the `*_vkms` tests. |
| `jetson_force_mode.sh` | Force a video mode on Jetson (see its header for caveats). |
| `drmdb_compat_scan.py` | Scan a drm.db dump for compatibility. |

## build_deck.sh — cross-build for the Steam Deck

SteamOS runs an older glibc / libstdc++ than a typical dev host, so a binary
built on the host links symbol versions the Deck lacks and won't run there.
`build_deck.sh` builds inside an `ubuntu:24.04` container (glibc 2.39, gcc-13) for
ABI compatibility. The Deck already ships `libdrm`/`gbm`/`libinput`/`udev`/
`xkbcommon`/`libdisplay-info` at runtime, so only the drm-cxx shared library and
the example binary are produced for deployment.

Requires `podman` on the host.

### Usage

```bash
# Build the default example (vrr_sweep) + the library:
scripts/build_deck.sh

# Build a specific example by name:
scripts/build_deck.sh idle_present
scripts/build_deck.sh driver_caps
```

Outputs land in the repo at:

- `deck-build/src/libdrm-cxx.so`
- `deck-build/<example>`

### Deploy + run on the Deck

```bash
scp deck-build/src/libdrm-cxx.so deck-build/vrr_sweep deck@<deck-host>:~/cs/
ssh deck@<deck-host> 'LD_LIBRARY_PATH=~/cs ~/cs/vrr_sweep /dev/dri/card0'
```

(`--rt` scenarios need `sudo` on the Deck for `CAP_SYS_NICE`.)

### Notes

- The container's `libdisplay-info` build is cached between runs in
  `$DECK_BUILD_CACHE` (default `~/.cache/drm-cxx-deck`); override to relocate it.
- Scope: the library + "plain" examples that only need the lib + fmt + libdrm/gbm
  (e.g. `vrr_sweep`, `idle_present`, `ring_present`, `damage_present`,
  `driver_caps`). Examples needing **blend2d** (`cluster_sim_vulkan`, `csd_*`) or
  generated **SPIR-V shaders** need extra in-container setup not covered here.

## build_visionfive2.sh — cross-build (+ flash) for the StarFive VisionFive 2

The VisionFive 2 (StarFive JH7110, quad SiFive U74, **riscv64**) runs a Debian
riscv64 userland. `build_visionfive2.sh` cross-builds inside a `debian:trixie`
container (riscv64 is an official release architecture there) using the
`riscv64-linux-gnu` toolchain, so the binaries link the exact `libdrm` / `fmt` /
`libdisplay-info` / `libinput` / `udev` versions the board ships and run there
unmodified.

**Scope.** The JH7110 GPU is an Imagination PowerVR (BXE-4-32); mainline open
Vulkan/GL (Mesa `powervr`) is experimental and too slow to use, so Vulkan is off.
The display side is the Verisilicon **DC8200** controller (`drm/verisilicon`),
which does KMS dumb-buffer scanout — so the examples that run usefully on-target
are the software / dumb-buffer ones (`software_present`, `ring_present`,
`atomic_modeset`, `signage_player`, `idle_present`, `driver_caps`, …). The GL
examples still build but need llvmpipe (slow) and are not the point.

Requires `podman` on the host (and `sudo` for the optional flash step).

### Usage — the call sequence is build, then image

**1. Build** (no args) — cross-compiles the library, every example, and every
test into `vf2-build/`:

```bash
scripts/build_visionfive2.sh
```

Outputs land in the repo at `vf2-build/src/libdrm-cxx.so`, `vf2-build/examples/*`,
and `vf2-build/tests/*`. Confirm the arch with
`file vf2-build/src/libdrm-cxx.so` → *UCB RISC-V, … double-float ABI*.

**2. Image** (`--image-sd`) — flashes the OS image to an SD card **and** stages
the just-built binaries onto it. This step requires a completed build (it copies
`vf2-build/` onto the card's rootfs) and **erases the target card**:

```bash
# Confirm the device is your removable card first (RM must be 1):
lsblk -dno NAME,RM,TYPE,SIZE,MODEL /dev/sdX

scripts/build_visionfive2.sh --image-sd /dev/sdX
```

That single call: verifies the pinned image against its SHA256 → `dd`s it to the
card → readback-verifies → mounts the rootfs → copies `libdrm-cxx.so` + every
example/test into `/home/ubuntu/drm-cxx` (`ubuntu` is the image's default login).

A pinned StarFive VisionFive 2 Ubuntu 24.04.3 preinstalled-desktop image
(`starfive-tech/VisionFive2` 6.0.0 release) is fetched and cached on first use;
pass `--image-url <url>` (and `--image-sha256 <hex>`) to track a different release.

| flag | meaning |
|---|---|
| `--image-sd /dev/sdX` | Whole-disk removable target to flash + stage onto (required to image). |
| `--stage-only` | Skip the download + flash; just (re-)stage the binaries onto an already-flashed card. |
| `--dut-user <name>` | Login whose `$HOME` receives the binaries (default `ubuntu`). |
| `--image-url <url>` | Override the pinned image (`.img` / `.xz` / `.bz2` / `.gz`). |
| `--image-sha256 <hex>` | Override the pinned checksum (skipped if empty). |
| `--no-verify-flash` | Skip the post-`dd` readback compare. |

> The binaries are staged into `/home/<dut-user>/drm-cxx` if that home exists in
> the image, otherwise into `/opt/drm-cxx` (world-readable). Preinstalled Ubuntu
> images create the login + its `$HOME` on *first boot*, so a fresh flash stages to
> `/opt/drm-cxx`; re-run with `--stage-only` after first boot to land in `~`.

### Run on the board

From a free VT (so the process is DRM master), on the VisionFive 2. Use the
**DC8200 KMS node** — on the StarFive Ubuntu image that is `card1` (`card0` is the
PowerVR render node):

```bash
cd /opt/drm-cxx && LD_LIBRARY_PATH=. ./software_present /dev/dri/card1 120 --vsync
# (or cd ~/drm-cxx if you re-staged with --stage-only after first boot)
```

### Notes

- **Booting requires firmware on the board.** The JH7110 needs a matching SPL +
  U-Boot in SPI NOR and the board's boot-mode switches set for SD; this script
  only writes the SD image. See the VisionFive 2 quick-start.
- The fetched image is cached in `$VF2_BUILD_CACHE` (default
  `~/.cache/drm-cxx-vf2`). The download is SHA-verified before it is promoted
  into the cache, so an interrupted/corrupt fetch never becomes the trusted file
  — re-run to re-fetch.
- Override the build container with `VF2_BUILD_IMAGE` (default `debian:trixie`).

### Troubleshooting — flashed card won't boot

On the VisionFive 2 the SD card holds **only the OS**; the first-stage bootloader
(SPL + U-Boot) lives in the board's **SPI NOR flash**. The board boots U-Boot
*from flash*, which then loads the OS from SD — booting SPL directly off the SD is
deprecated on this SoC. So a card that flashed + verified fine but won't boot is
almost always a **boot-mode** or a **stale SPI bootloader** problem, not a bad
image. Work it cheapest-first:

1. **Boot-mode switches.** Set the two switches (RGPIO_1, RGPIO_0) to **`0 0`
   (1-bit QSPI Nor Flash)** — *not* the "SD card" position, which is the
   deprecated direct-boot path.

   | mode | RGPIO_1 | RGPIO_0 |
   |---|---|---|
   | **QSPI Nor Flash** (use this) | 0 | 0 |
   | SD card (deprecated) | 0 | 1 |
   | eMMC | 1 | 0 |
   | UART (recovery) | 1 | 1 |

2. **Serial console** turns guesswork into a diagnosis. Attach a 3.3 V USB-UART to
   the 40-pin header (GND / board-TX / board-RX) at **115200 8N1** and power on:
   - nothing at all → power, switch position, or empty SPI flash (use UART recovery);
   - an SPL/U-Boot banner with an old (2021/2023-era) version → the bootloader
     predates the 202510 image layout → update SPI flash;
   - U-Boot loads but can't find/boot the SD → same fix.

3. **Update the SPI-flash bootloader.** Firmware files are
   `u-boot-spl.bin.normal.out` (SPL) and `visionfive2_fw_payload.img` (U-Boot)
   from the matching [starfive-tech/VisionFive2 release][vf2-fw]; the `flashcp`
   method needs the board already at ≥ `VF2_v2.5.0`. From a *booting* Linux (boot a
   known-good older card if needed):

   ```bash
   sudo apt install mtd-utils
   cat /proc/mtd      # confirm indices: mtd0=spl, mtd1=uboot, mtd2=data
   sudo flashcp -v u-boot-spl.bin.normal.out  /dev/mtd0
   sudo flashcp -v visionfive2_fw_payload.img /dev/mtd1
   ```

   Match the `/dev/mtdX` index to the partition *named* `uboot`/`data` in
   `/proc/mtd` rather than assuming the number — on newer firmware the U-Boot
   payload outgrew `mtd1`. If nothing boots at all, use the UART-recovery flow
   (switches to `1 1`) from the [official SPL/U-Boot update guide][vf2-spl]. The
   202510 image also ships a `spl-uboot` apt package, so once it boots the
   bootloader can be kept current via `apt`.

[vf2-fw]: https://github.com/starfive-tech/VisionFive2/releases
[vf2-spl]: https://doc-en.rvspace.org/VisionFive2/Quick_Start_Guide/VisionFive2_SDK_QSG/updating_spl_and_u_boot%20-%20vf2.html

`build_beaglebone_black.sh` is the armhf sibling of this script (TI AM335x,
tilcdc display, no GPU) and follows the same build-then-`--image-sd` flow.
