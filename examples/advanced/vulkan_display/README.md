# vulkan_display

A minimal `VK_KHR_display` enumerator. Lists each Vulkan display, its
mode dimensions, and the planes that can scan out on it; cross-
referencing with the DRM plane registry shows how the same hardware
is exposed through both surfaces. Purely informational — there is no
rendering.

This example is about the libdrm ↔ Vulkan boundary and is the only
example in the tree that doesn't use `LayerScene`. It runs only when
the build was configured with Vulkan support (`-Dvulkan=true` /
`-DDRM_CXX_VULKAN=ON`, both default on).

## Run

```
sudo vulkan_display
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the `seat` group membership is enough. Like the
other examples it claims a libseat session before opening the device.

Sample output:

```
Vulkan displays: 1
  Display 'HDMI-A-1': 3840x2160 (handle=0x3)
    Plane 0: stack_index=0
    Plane 1: stack_index=1

Vulkan display planes: 2
  Plane 0: stack_index=0, supported_displays=1
  Plane 1: stack_index=1, supported_displays=1
```

The exact set of planes and the relationship between Vulkan plane
indices and DRM plane IDs is driver-specific.
