# egl_scene

End-to-end demo of `drm::scene::GbmSurfaceSource` fronted by an
EGL / GLES 3 renderer. Shows the full producer-side workflow: open
the output, build the scene, negotiate a DRM format modifier, hand
the scene's `gbm_surface` to EGL, render frames, commit.

## What it shows

1. Pick the first connected output and bring up a `LayerScene`.
2. Add a dumb-buffer background layer so PRIMARY has an FB across
   modeset.
3. Modifier negotiation:
   * `LayerScene::candidate_modifiers(DRM_FORMAT_ARGB8888)` returns
     the set the scene's plane allocator will accept.
   * A throw-away EGL display is spun up on a transient
     `gbm_device`, `eglQueryDmaBufModifiersEXT` is called, and the
     intersection of the two lists is computed.
   * The first scene-preferred modifier that EGL also supports is
     picked, falling back to `DRM_FORMAT_MOD_INVALID` if no overlap
     exists.
4. Construct a `GbmSurfaceSource` with that single modifier.
5. Build a real EGL display over the source's own `gbm_device`
   (`GbmSurfaceSource::native_device()`), a GLES 3 context, and a
   window surface over `GbmSurfaceSource::native_surface()`.
6. Render one frame upfront so the source has a front buffer to
   lock — Mesa won't populate the gbm_surface's dispatch table
   until the EGL/Vulkan producer has bound it and pushed a frame.
7. Add the GBM-surface layer to the scene and commit.
8. Run a hue-cycle clear-color render loop for `--seconds` (default
   5), pacing at ~60 Hz.

## Run

```
sudo egl_scene [--seconds N] [/dev/dri/cardN]
```

`sudo` for the bare-TTY case; on a seatd-managed system membership in
the `seat` group is enough. The example claims a libseat session
before opening the device, like every other scene demo.

## Build gate

Built only when libEGL + glesv2 are both present at configure time
(via pkg-config). The library itself never links these — only this
example does.

## Why use this over `vulkan_scene`

EGL is the canonical producer side of GBM. `gbm_surface` itself is
an EGL-only concept — Mesa fronts it via `EGL_KHR_platform_gbm`. If
you're writing GL or GLES code that scans out through KMS, this is
the demo that maps to your stack. For Vulkan producers, see
`vulkan_scene`.