# vulkan_scene

End-to-end demo of a Vulkan-rendered scene layer. Shows the
canonical Vulkan-side workflow: allocate a `VkImage` with the right
DRM format modifier, export the underlying memory as a DMA-BUF, wrap
it as a scene `LayerBufferSource`, render each frame, commit.

## Why not `GbmSurfaceSource`?

`gbm_surface` is an EGL-only concept — Mesa fronts it via
`EGL_KHR_platform_gbm`, and there is no `VK_EXT_gbm_surface`
extension. The architecturally honest path for Vulkan + DRM/KMS
interop is:

* `VK_EXT_image_drm_format_modifier` — allocate a `VkImage` with a
  specific DRM format modifier the KMS side will accept.
* `VK_EXT_external_memory_dma_buf` + `VK_KHR_external_memory_fd` —
  export the bound `VkDeviceMemory` as a DMA-BUF fd via
  `vkGetMemoryFdKHR`.
* `drm::scene::ExternalDmaBufSource` — wrap the fd as a scene
  `LayerBufferSource`. The scene's interface is identical to what
  every other producer uses.

For applications that want a Vulkan-rendered scene layer *and*
GBM-surface semantics (multi-buffered swap chain managed by the
producer side), the Mesa Zink layer translates Vulkan → GL, which
makes the `egl_scene` workflow available to Vulkan apps at the cost
of an extra translation layer. Out of scope here.

For applications that want Vulkan rendering *direct-to-CRTC*
without scene composition, `VK_KHR_display` is the right path —
see the `vulkan_display` example for the enumeration side.

## What it shows

1. Pick the first connected output and bring up a `LayerScene`.
2. Background dumb-buffer layer keeps PRIMARY armed across modeset.
3. Create a `VkInstance` with the extensions needed to walk DRM
   properties and allocate exportable images.
4. Pick the `VkPhysicalDevice` whose DRM major / minor matches the
   open DRM fd — required so the Vulkan-allocated DMA-BUF can be
   imported by the KMS side. Cross-device import works on some
   platforms but isn't portable.
5. Allocate a `VkImage` at the output's resolution, ARGB8888, with
   `DRM_FORMAT_MOD_LINEAR` (universally accepted; non-LINEAR
   negotiation against `vkGetPhysicalDeviceImageFormatProperties2`
   adds quite a bit of code and isn't necessary for the demo).
6. Export the bound memory as a DMA-BUF fd, wrap it as an
   `ExternalDmaBufSource`, add as a scene layer.
7. First commit brings up modeset + presents the (UNDEFINED-layout)
   image. Run a `vkCmdClearColorImage` hue cycle for `--seconds`
   (default 5), `vkQueueWaitIdle` between each commit.

The demo is single-buffered, so a brief tear is visible during each
clear. Real applications would double-buffer or use a sync_file
through `IN_FENCE_FD` on the atomic commit.

## Run

```
sudo vulkan_scene [--seconds N] [/dev/dri/cardN]
```

`sudo` for the bare-TTY case; on a seatd-managed system membership in
the `seat` group is enough.

## Build gate

Built only when Vulkan support is on in the configure step
(`-Dvulkan=true` / `-DDRM_CXX_VULKAN=ON`, both default on). Requires
the chosen Vulkan ICD to expose:

* `VK_EXT_image_drm_format_modifier`
* `VK_KHR_external_memory` + `VK_KHR_external_memory_fd`
* `VK_EXT_external_memory_dma_buf`

Mesa (Intel ANV, AMD RADV, NVK) and the NVIDIA proprietary driver
all export these on current versions. If the ICD doesn't, the demo
exits with a clear diagnostic during `vkCreateDevice`.