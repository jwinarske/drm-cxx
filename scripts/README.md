<!--
SPDX-FileCopyrightText: (c) 2025 The drm-cxx Contributors
SPDX-License-Identifier: MIT
-->
# scripts/

Helper scripts for developing and testing drm-cxx.

| script | what it does |
|---|---|
| `build_deck.sh` | Cross-build the library + an example for the Steam Deck (see below). |
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
