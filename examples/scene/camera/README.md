# camera

A libcamera → KMS scanout viewfinder built on `LayerScene`. Two modes:

- **`--probe`** — diagnostic. Walks both sides of the pipeline (KMS
  planes / `IN_FORMATS`, libcamera-enumerated cameras and Viewfinder
  StreamConfigurations), prints the (camera, plane, fourcc, size,
  modifier) tuple a streaming run would pick, and exits without
  acquiring streaming resources.
- **`--show`** — runtime. Streams from the first usable camera onto a
  scene layer at the negotiated configuration. Where the camera's
  output already matches a scanout-capable plane (NV12 from a UVC cam
  whose stride is 256-byte-aligned), the frame goes through directly
  via `ExternalDmaBufSource`. Otherwise libyuv repacks each frame into
  an XRGB8888 `DumbBufferSource` — YUY2/NV12/MJPEG paths cover the
  common UVC fallbacks. The path actually taken is logged once at
  startup.

Both libcamera (≥0.3.0) and libyuv are example-only build dependencies
and are auto-detected; if either is missing the target is skipped
rather than failing the whole drm-cxx build. Ubuntu 24.04 ships
libcamera 0.2.0, so the CI pipeline builds 0.5.2 from source — see
the comment in `examples/CMakeLists.txt` for details.

## Run

```
sudo camera --probe [/dev/dri/cardN]
sudo camera --show  [/dev/dri/cardN]
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system membership in the `seat` and `video` groups is
enough. `--probe` and `--show` are mutually exclusive.

## `--probe` output

1. Connector / CRTC / mode summary for the selected device.
2. Per-plane table of the `IN_FORMATS` contents, or the bare format
   list when the driver doesn't expose `IN_FORMATS`.
3. libcamera-enumerated cameras with id, model, location, and the
   validated Viewfinder `StreamConfiguration` plus the full
   `StreamFormats` matrix.
4. The negotiated target — the format / size / plane / modifier tuple
   a streaming run would commit to.

## `--show` runtime

Steady-state path:

- libcamera viewfinder stream, externally-allocated `FrameBuffer`s.
- Per-frame `ExternalDmaBufSource::create_from_libcamera_buffer()` if
  the negotiated path is zero-copy, otherwise a libyuv repack into a
  cached `DumbBufferSource`.
- Single-layer `LayerScene` with the camera output displayed
  full-screen.

`q` / `Esc` / Ctrl-C exits. Camera unplug is handled — the example
re-probes, picks the next available camera if any, and rebinds the
scene; if none, the example exits with a clean shutdown.

## Known constraints

- amdgpu DC requires plane pitch to be a multiple of 256 bytes. UVC
  cameras whose NV12 stride is 768 (640px × 1) AddFB2 successfully;
  UVC cameras whose stride is 640 (the kernel's default minimum from
  `dumb_create`) do not, so those fall through to the libyuv repack
  path.
- MJPEG decode goes through libjpeg-turbo (`MJPGToARGB`); I-frame-only
  cameras and corrupt streams print a per-frame warning and skip the
  frame rather than dropping the stream.
