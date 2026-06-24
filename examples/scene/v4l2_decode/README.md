# v4l2_decode — stateful V4L2 H.264 decode to a KMS plane

Feeds an Annex-B H.264 file to `drm::scene::V4l2DecoderSource` and scans the
decoded NV12 frames out on a KMS plane. A dongle-free, GStreamer-free exercise
of the hardware-decode → display path on a stateful V4L2 decoder such as the
Raspberry Pi's `bcm2835-codec` (`/dev/video10`).

## What this exercises

- **Stateful V4L2 decode**: raw Annex-B is fed to the OUTPUT queue and the
  decoder parses headers, raises `SOURCE_CHANGE`, and produces frames on the
  CAPTURE queue — all driven through `V4l2DecoderSource`.
- **Zero-copy scanout**: each decoded CAPTURE buffer is imported as a KMS
  framebuffer and placed on a hardware plane by `LayerScene`; the plane scales
  the decoded frame to the display.
- **Decode/present decoupling**: a pump thread drives the decoder continuously
  while the main thread runs the KMS commit loop, so the decoder never starves
  while a flip is in flight. One mutex serializes source access.

## Usage

Run on a free VT that owns DRM master (no compositor):

```
v4l2_decode /dev/dri/cardN --file clip.h264 [--no-seat] [--codec /dev/videoN] \
            [--modifier linear|sand] [--size WxH]
```

- The DRM card is the first argument.
- `--codec` selects the decoder node; the default scans `/dev/video*` for an
  H.264 M2M decoder (the Pi's nodes start at `/dev/video10`).
- `--size WxH` sizes the decoder only. The display stays at the connector's
  preferred mode and the hardware plane scales the decoded frame to fill it.

It prints the negotiated `(fourcc, modifier)` and a `displayed N frames`
heartbeat (one line per 60 frames scanned out). Quit with Ctrl-C.

## Generating a compatible test clip on the host

Most desktop encoders default to a profile a small embedded decoder cannot
handle (see below). Pin the profile and pixel format explicitly. With GStreamer:

```
gst-launch-1.0 -e \
  videotestsrc num-buffers=300 ! \
  video/x-raw,format=I420,width=1280,height=720,framerate=30/1 ! \
  x264enc bitrate=4000 speed-preset=ultrafast key-int-max=15 ! \
  video/x-h264,profile=constrained-baseline ! \
  h264parse config-interval=1 ! \
  video/x-h264,stream-format=byte-stream,alignment=au ! \
  filesink location=test720.h264
```

The same with ffmpeg:

```
ffmpeg -f lavfi -i testsrc=size=1280x720:rate=30 -t 10 \
  -c:v libx264 -profile:v baseline -pix_fmt yuv420p -g 15 \
  -bsf:v h264_mp4toannexb -f h264 test720.h264
```

Key points:

- **`I420` / `yuv420p`** — 4:2:0 chroma. A 4:4:4 source pushes the encoder to a
  4:4:4 profile that fixed-function decoders do not implement.
- **`constrained-baseline` / `baseline`** — within reach of every hardware
  decoder. `main` and `high` also work on most parts; `high-4:4:4` does not.
- **`stream-format=byte-stream`** — Annex-B (start codes), which the example
  feeds directly. `config-interval=1` repeats SPS/PPS so the decoder can lock on
  even when feeding starts mid-stream.

Confirm the profile before copying it to the target:

```
gst-launch-1.0 -v filesrc location=test720.h264 ! h264parse ! fakesink \
  2>&1 | grep -o 'profile=(string)[a-z0-9-]*'
```

## What happens if the decoder does not support the format

A fixed-function decoder advertises H.264 but only implements a subset of
profiles. The Raspberry Pi's `bcm2835-codec`, for example, decodes up to High
but **not** High 4:4:4. Fed an unsupported stream, it accepts and consumes the
bitstream (OUTPUT buffers cycle normally) but never raises `SOURCE_CHANGE` and
never emits a CAPTURE frame — so a naive player shows a silent black screen.

The example guards against this: if the clip is fed through several times and
nothing has reached the screen, it stops and reports the likely cause instead of
hanging on black:

```
no frames decoded after feeding the clip 5 times -- the V4L2 decoder does not
support this stream (e.g. an H.264 profile beyond the hardware's capability,
such as High 4:4:4)
```

If you hit this, re-encode the clip with `constrained-baseline` (or `main` /
`high`) and 4:2:0 chroma as shown above.
