# hdr_demo

Reference example exercising the HDR signaling, CRTC color
pipeline, and CPU tone-map fallback the library exposes:

- `probe_connector_capabilities` reports the connector's
  `max_bpc` / `Colorspace` / `HDR_OUTPUT_METADATA` presence.
- `LayerScene::set_output_metadata` populates the
  `HDR_OUTPUT_METADATA` blob with mastering luminance + MaxCLL /
  MaxFALL.
- Per-layer `DisplayParams::color_primaries` / `source_eotf`
  drive the connector `Colorspace` enum and the HDR static-
  metadata `eotf` field via `LayerScene`'s auto-derive.
- `probe_crtc_capabilities` + `CrtcColorPipeline` arm an
  identity DEGAMMA / CTM / GAMMA stack on hardware that
  exposes the CRTC pipeline.

## Layers

1. **Background** (`zpos=3`, full screen) — a horizontal gray ramp
   in ARGB8888. Tagged `color_primaries = Bt2020` and (per `--mode`)
   `source_eotf = SmpteSt2084Pq` / `Bt2100Hlg` / unset. The pixel
   content is intentionally simple so the demo runs on hardware that
   doesn't accept P010 on the primary scanout plane (RDNA earlier
   than RDNA3, older i915). Real PQ-encoded P010 content is a
   straightforward swap: change the `drm_fourcc` to `DRM_FORMAT_P010`
   and paint the appropriate Y values.
2. **SDR overlay** (`zpos=4`, top-right corner) — a 192×64 ARGB8888
   badge whose color reflects the current `--mode` (blue = PQ,
   green = HLG, red = SDR). Always tagged BT.709 SDR so the demo
   always shows what mixing SDR with HDR signaling looks like.
3. **HLG square** (`zpos=5`, centered) — a 256×256 ARGB8888 placeholder
   tagged `color_primaries = Bt2020` and `source_eotf = Bt2100Hlg`.
   The auto-derive resolves PQ over HLG when both are present in
   the same scene, so this layer is mostly documentation on
   hardware without per-plane CMS.

## CLI

```
hdr_demo [--mode {pq|hlg|sdr}] [--target-nits N]
         [--no-hw-pipeline] [--dry-run]
         [--hold-seconds N] [/dev/dri/cardN]
```

| Flag | Effect |
|---|---|
| `--mode` | `pq` (default), `hlg`, or `sdr`. Drives the background's `source_eotf` and the HDR_OUTPUT_METADATA blob's `eotf` field. |
| `--target-nits N` | Override the `max_display_mastering_luminance` / MaxCLL fields on the HDR_OUTPUT_METADATA blob. Default 1000 cd/m². |
| `--no-hw-pipeline` | Skip the `CrtcColorPipeline` setup. The demo still tags the layers, but the kernel-side LUT/CTM stays at default. |
| `--dry-run` | Print the configured shape and exit without committing. Useful when running on hardware that rejects the multi-layer HDR commit (the demo's status output is the bulk of its value). |
| `--hold-seconds N` | After committing, keep the scene live for `N` seconds before tearing down. Default 5. |

## Expected look

- **HDR-capable display** — the connector switches to BT.2020
  Colorspace, `HDR_OUTPUT_METADATA` advertises PQ at 1000 cd/m².
  The gray ramp shows the SDR encoding under HDR signaling
  (will look subtly different from baseline SDR — the kernel
  passes the bytes through; the sink interprets them per the
  declared colorimetry).
- **SDR-only display** — same configuration, but the sink
  ignores the HDR signaling and renders the gray ramp +
  overlay as plain BT.709 SDR.
- **`--mode sdr`** — Bypasses HDR entirely. Same content, no
  HDR_OUTPUT_METADATA. Looks like baseline SDR.

## Hardware notes

amdgpu DC validates the (Colorspace, HDR_OUTPUT_METADATA, plane
assignment) atomic state on every commit. Some pre-RDNA3
generations refuse multi-layer HDR commits — the demo's TEST
commit catches the EINVAL and falls back to dry-run output so
the configuration stays visible. RDNA3+ accepts the demo as
written.

i915 (Tigerlake+) accepts the same shape; older i915 stacks
silently lose the HDR_OUTPUT_METADATA write.

vkms exposes neither HDR_OUTPUT_METADATA nor the CRTC pipeline,
so on vkms the demo runs as plain SDR signaling regardless of
`--mode`.

## Status badge

An in-frame Blend2D-rendered status overlay (connector caps +
active tier + per-layer EOTF) would be a natural addition but
is not implemented to keep the example's dependency surface
minimal — the equivalent information goes to stderr at startup.
Adding it is a straight port of the existing `signage_player`
overlay renderer to this demo's layer set.
