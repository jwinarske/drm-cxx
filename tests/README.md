# Tests

```sh
meson test -C builddir          # or: ctest --test-dir build
```

- **`unit/`** — host-only. No DRM device required; run anywhere.
- **`integration/`** — exercise real KMS through a DRM device. They run against
  either real hardware or the **vkms** virtual driver, and **skip cleanly when no
  suitable device is present** (so a headless CI runner stays green).

## vkms requirement for the integration tests

The integration tests that place more than one layer on distinct hardware planes
— `test_layer_scene_composition_vkms`, `test_layer_scene_minimization_vkms`,
`test_gbm_surface_source_vkms`, and others — need vkms loaded **with overlay
planes**:

```sh
sudo modprobe -r vkms
sudo modprobe vkms enable_overlay=1 enable_plane_pipeline=1
```

- `enable_overlay=1` is the important one: recent kernels default it to **off**,
  in which case vkms exposes only PRIMARY + CURSOR. A test that expects a second
  layer to reach a dedicated hardware plane (`layers_assigned == 2`,
  `layers_composited == 0`) then **fails an assertion** — it is *not* skipped,
  because a vkms node does exist; it just lacks the overlay. With overlays on,
  vkms exposes PRIMARY + several OVERLAY planes and these tests pass.
- `enable_plane_pipeline=1` enables vkms's plane composition path, used by the
  composition test that reads back rendered pixels.

> Note: on kernel 6.19+ vkms registers as a *faux* device, so the card's
> `uevent` reports `DRIVER=faux_driver`; the DRM driver name is still `vkms`, so
> the test device-discovery resolves it correctly.

CI does not load vkms, so these tests skip there; load it locally as above to run
them against virtual KMS.
