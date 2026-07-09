# camera

A libcamera → KMS scanout viewfinder built on `LayerScene`. Two modes:

- **`--probe`** — diagnostic. Walks both sides of the pipeline (KMS
  planes / `IN_FORMATS`, libcamera-enumerated cameras and Viewfinder
  StreamConfigurations), prints the (camera, plane, fourcc, size,
  modifier) tuple a streaming run would pick, and exits without
  acquiring streaming resources.
- **`--show`** — runtime. Streams from the first usable camera onto a
  scene layer at the negotiated configuration. Conversion tiers, in
  order of preference:
  1. **NV12 zero-copy** — camera already emits NV12 at a
     scanout-aligned stride; `ExternalDmaBufSource` wraps the libcamera
     dma-buf and the kernel scans it out directly.
  2. **MJPEG → NV12 via VA-API** (when `libva` + `libva-drm` are
     present at build time) — `VaapiJpegDecoder` decodes each JPEG
     into a reusable VA-API NV12 surface, exported once as a dma-buf
     and held by an `ExternalDmaBufSource` for the slot's lifetime.
     No CPU touches pixels; scanout reads the GPU-decoded surface.
  3. **YUY2 → XRGB**, **NV12 → XRGB**, **MJPEG → XRGB** via libyuv
     into a CPU-mapped `DumbBufferSource`.

  The path actually taken is logged once at startup.

Both libcamera (≥0.3.0) and libyuv are example-only build dependencies
and are auto-detected; if either is missing the target is skipped
rather than failing the whole drm-cxx build. Ubuntu 24.04 ships
libcamera 0.2.0, so the CI pipeline builds 0.5.2 from source — see
the comment in `examples/CMakeLists.txt` for details.

`libva` + `libva-drm` are also auto-detected; absent, the VA-API tier
is compiled out and MJPEG falls back to the CPU libyuv path. Linking
needs a runtime VA-API driver as well — `radeonsi_drv_video.so` on
amdgpu, `iHD_drv_video.so` on Intel.

## Run

```
sudo camera --probe [--no-vaapi] [/dev/dri/cardN]
sudo camera --show  [--no-vaapi] [/dev/dri/cardN]
```

`sudo` is the path of least resistance for a bare-TTY run; on a
seatd-managed system membership in the `seat` and `video` groups is
enough. `--probe` and `--show` are mutually exclusive.

`--no-vaapi` forces every MJPEG slot onto the libyuv CPU tier even
when the example was compiled with VA-API support. Useful when the
host VA driver is buggy on the GPU at hand — Ubuntu 20.04's
`intel-media-va-driver 20.1.1` (`iHD_drv_video.so`), for example,
segfaults inside `vaEndPicture` on UVC-emitted 4:2:2 MJPEGs; until the
package is upgraded, `--no-vaapi` keeps the example streaming.

## Application-allocated capture buffers (`--dma-heap=`, `--gbm-import`)

By default the NV12 zero-copy tier uses libcamera's own
`FrameBufferAllocator` (V4L2 MMAP). That memory isn't always
scanout-capable — some ISPs (e.g. Rockchip `rkisp1`) hand back MMAP
buffers the display can't `AddFB2`. Two flags make the *application*
allocate the capture buffers from memory the display can scan out, then
hand them to libcamera to fill (imported via `V4L2_MEMORY_DMABUF`, so the
ISP still writes straight into scanout memory — no copy):

- **`--dma-heap=system`** / **`--dma-heap=cma`** (Mode B) — allocate each
  buffer from `/dev/dma_heap/{system,cma}`. The **CMA** heap gives
  physically-contiguous memory, which an IOMMU-less display controller
  (RPi `vc4`) needs. Requires `CONFIG_DMABUF_HEAPS=y`.
- **`--gbm-import`** (Mode C) — allocate via GBM
  (`GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR`) so the buffer is
  GPU-driver-owned; use where only the GPU path reaches display memory
  (some i.MX setups).

Both only affect the NV12 zero-copy tier; the libyuv / VA-API copy tiers
ignore them. If the allocation or import fails the example logs it and
falls back to the libcamera allocator, so the slot still streams.
Validated end-to-end on a Raspberry Pi 5 (IMX219 → `pisp` NV12 →
`--dma-heap=cma` → `vc4` scanout).

## VA-API driver discovery

The VA-API tier opens the loader via `vaGetDisplayDRM(card_fd)`, then
relies on libva to dlopen the right `*_drv_video.so`. libva searches
`LIBVA_DRIVERS_PATH` first, then a compile-time default (the configure
prefix's `lib/dri`). A libva built from source with `--prefix=/usr/local`
defaults to `/usr/local/lib/x86_64-linux-gnu/dri`, which is empty on a
typical Debian/Ubuntu system — the distro ships drivers at
`/usr/lib/x86_64-linux-gnu/dri`. The mismatch surfaces as

```
libva info: Trying to open /usr/local/lib/x86_64-linux-gnu/dri/iHD_drv_video.so
libva info: va_openDriver() returns -1
[vaapi_jpeg_decoder] vaInitialize: unknown libva error (0xffffffff)
configure_slot[…]: VAAPI MJPEG unavailable, falling back to libyuv
```

Either rebuild libva with `--with-drivers-dir=/usr/lib/x86_64-linux-gnu/dri`
or set the env var at run time:

```
LIBVA_DRIVERS_PATH=/usr/lib/x86_64-linux-gnu/dri camera --show
```

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
- MJPEG software decode goes through libjpeg-turbo (`MJPGToARGB`);
  the VA-API tier replaces it transparently when both `libva` and a
  matching GPU driver are present. Baseline JPEGs only (UVC universally
  emits baseline). Truncated or non-baseline streams skip the frame
  rather than dropping the stream.
