# hotplug_monitor

A small LayerScene that follows the active connector across hotplug
events. On startup it picks the first connected connector + its
preferred mode, brings up a four-quadrant test pattern with a small
"clock badge" overlay (a colored square whose hue advances each
frame), and starts polling `drm::display::HotplugMonitor` for udev
events.

When an event arrives, the example re-runs its connector / mode pick
and — if either changed — calls `LayerScene::rebind(crtc, connector,
mode)`. Layer handles survive the rebind verbatim, so the test pattern
and clock badge follow the new mode without restart. Buffer dimensions
are not re-allocated, so a smaller new mode crops and a larger one
letterboxes — fine for demonstrating the rebind primitive.

What this validates end-to-end:

- `drm::display::HotplugMonitor`'s udev netlink fast path with
  connector-id filtering.
- `LayerScene::rebind` on a live scene: the scene does the property
  recache, registry re-enumeration, and `ALLOW_MODESET` first-commit
  internally; the application loop only signals "the connector
  changed."
- libseat-driven session pause/resume. Switching VTs (Ctrl+Alt+F<n>)
  pauses the scene; returning to the example's TTY resumes it. Layer
  handles survive resume.

## Run

```
sudo hotplug_monitor [/dev/dri/cardN]
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system the `seat` group membership is enough.

Trigger an event by physically unplugging / replugging an HDMI or
DisplayPort cable. On systems with virtual connectors (vkms,
virtio-gpu) the connector status doesn't actually change unless the
host explicitly toggles it; bare-metal HDMI is the easiest demo.

Press `q` or `Esc` to exit.
