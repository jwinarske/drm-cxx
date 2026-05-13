# Changelog

## unreleased — CSD Tier 0 presenter, mdi_demo, cursor hardening, GstAppsinkSource

### `drm::scene` — EGL Streams capability probe

- **`drm::scene::StreamCapability` + `probe_stream_capability(dev)`** — runtime capability probe for EGL Streams support. dlopen-only against `libEGL.so.1` (libdrm-cxx is never link-bound to libEGL, even when the headers are present at build time); inspects client-side `EGL_EXT_platform_device` / `EGL_EXT_device_drm`, enumerates `EGLDeviceEXT`s, matches each against the caller's `drm::Device` by `st_rdev`, and returns the per-device-display extension set (`EGL_EXT_output_drm`, `EGL_KHR_stream`, `EGL_EXT_stream_consumer_egloutput`, `EGL_NV_stream_consumer_eglimage`, `EGL_KHR_stream_producer_eglsurface`) along with vendor / version / client-api strings. Mesa-only systems return `StreamMixingMode::Unsupported` deterministically; NVIDIA proprietary / Tegra return `Exclusive` (conservative default — empirical mixing detection lands alongside `EglStreamSource`).
- **`LayerScene::Config::stream_capability`** — explicit pass-through field so applications opt into streams by calling `probe_stream_capability` themselves rather than paying the dlopen cost on every scene construction. Survives `rebind()` and pause/resume verbatim (the capability describes the driver, not the connector). Accessor `LayerScene::stream_capability()` lets layers and producer-side builders consult the result.
- **`add_layer` gating on `BindingModel::DriverOwnsBinding`** — registration now rejects sources reporting `DriverOwnsBinding` when the scene was constructed with `StreamMixingMode::Unsupported`. Failure mode is `errc::not_supported` at registration time rather than at first commit; callers who hit this either need to populate `Config::stream_capability` via the probe or remove the stream source. v1 sources are unaffected — none override `binding_model()`.
- **Build wiring** — `meson -Dstreams={auto,enabled,disabled}` / `cmake -DDRM_CXX_STREAMS={AUTO,ON,OFF}`. Headers-only build dependency on `egl.pc` (or a bare `<EGL/egl.h>` + `<EGL/eglext.h>` probe when pkg-config doesn't see it); libEGL is never linked into libdrm-cxx so the library stays loadable on Mesa systems with no EGL runtime. `DRM_CXX_HAS_EGL_STREAMS=0` builds short-circuit `probe_stream_capability` to a constant `Unsupported`.

### `drm::scene` — GStreamer source, EAGAIN flow control

- **`drm::scene::GstAppsinkSource`** — bridges a caller-owned GStreamer `appsink` element into the LayerScene buffer-source contract so any pipeline terminating in `appsink name=sink` can drive a KMS layer. Two import paths picked per sample: dmabuf zero-copy (`drmPrimeFDToHandle` + `drmModeAddFB2WithModifiers`, fb_id LRU keyed on the dma-buf fd) for hardware decoders; sysmem memcpy into a lazily-allocated dumb buffer for software decoders. Latest-frame-wins drop semantics (`drop=true`, `max-buffers=1`, `sync=false`) so a slow consumer never stalls the upstream pipeline. Format negotiation is lazy; mid-stream caps changes tear down the FB cache + sysmem fallback and re-resolve. Session integration: `on_session_paused` drops fb_ids and GEM handles bound to the dying fd; `on_session_resumed` re-imports the cached `current_sample` against the new fd so the next `acquire()` returns a valid fb_id immediately, even before the pipeline produces a fresh sample after PAUSED→PLAYING. Gated on `DRM_CXX_GSTREAMER` (CMake) / `gstreamer` (Meson); the disabled branch returns `errc::function_not_supported` from every method so consumers can feature-test against the API.
- **EAGAIN as flow control** — `LayerBufferSource::acquire()` may return `errc::resource_unavailable_try_again` when a live source has no frame to contribute this vblank (typically pre-preroll, or a producer that fell behind without a cached frame to re-issue). `LayerScene::commit()` skips the layer for that commit and counts it under the new `CommitReport::layers_skipped_no_frame`; the next commit re-calls `acquire()`. Sources that always have a valid buffer (DumbBuffer, GbmBuffer, ExternalDmaBuf) never need to return EAGAIN. The unassigned-residual math now subtracts skips so the "layer dropped" warning doesn't fire on a benign frame stall, and `Allocator::plane_statically_compatible` rejects layers with no format so a skipped layer can't phantom-place on a plane.

### `drm::csd` — Tier 0 presenter, plane coordinator, focus/hover animations

- **`drm::csd::OverlayReservation`** — startup-time plane picker for decoration surfaces. `reserve(crtc_index, format, count, min_zpos)` returns OVERLAY-typed plane IDs in zpos-ascending order that support the FourCC, sit at or above `min_zpos`, are CRTC-compatible, and aren't already taken on any other CRTC. `release(crtc_index)` is idempotent — relevant on shared-plane hardware (Mali Komeda) where releasing one CRTC's planes makes them available to another; no-op on partitioned hardware (amdgpu DCN, Intel Tigerlake+, i.MX8 DCSS, RK3399 VOP). Shortfall returns `errc::resource_unavailable_try_again` so a presenter can degrade to a software tier per-CRTC.
- **`drm::csd::Presenter`** — abstract seam between painted decoration surfaces and the path that scans them out. `Renderer` and `ShadowCache` stay tier-agnostic; only the presenter changes between tiers (Plane = desktop / well-provisioned ARM / virtio-gpu, Composite + Fb tiers TBD).
- **`drm::csd::PlanePresenter`** (Tier 0) — one DRM overlay plane per decoration. Resolves geometry, blend-mode, per-plane alpha, and zpos property ids once at construction; `base_zpos` parameter controls the stacking value the first reserved plane receives (pass `primary_zpos_max+1` on amdgpu where the primary is pinned at zpos 2). `apply(surfaces, req)` adds property writes to a caller-owned request — caller keeps full control of TEST/COMMIT, `ATOMIC_NONBLOCK`, `PAGE_FLIP_EVENT`, and `IN_FENCE_FD`. Pre-multiplied blend + `alpha=0xFFFF` forced every frame so a previous compositor's state can't bleed through. The `compute_writes` helper is a pure function (slots + surfaces → property writes) for unit testing without a live DRM fd.
- **`drm::csd::WindowAnim`** — value-type per-window animator holding focus + hover progress, eased ease-out-cubic. Shell calls `retarget_focus` / `retarget_hover` on state changes, then `tick(dt)` once per frame. Output mirrors into `WindowState::{focus_progress, hover_progress}`; sentinel `k_progress_unset = -1.0F` keeps callers without an animator visually identical.
- **`ShadowCache::blit_cross_fade(key_a, key_b, t)`** — tweens between cached shadow patches through real intermediate pixels rather than snapping; endpoints (`t == 0` or `1`) short-circuit to `blit_into` so the no-animation path costs the same as before. Per-row scratch buffer keeps allocation off the hot path.
- **Renderer cross-fade hooks** — `draw_glass` lerps the rim color and feeds the focus weight to the shadow cross-fade; `draw_button` takes a `hover_weight` in `[0, 1]` and lerps fill ⇄ hover so transitioning buttons read as the in-between tint.
- **`decoration_geometry(theme, w, h)`** — single source of truth for panel inset, title-bar height, and button center positions. Used by both the renderer's paint path and the mdi_demo shell's hit-tester so paint and click can't desync.
- **`PlaneRegistry::from_capabilities(std::vector<PlaneCapabilities>)`** — synthetic-source factory for unit tests and replay/snapshot tools that don't have a live device.

### `drm::cursor` — sprite sizing + atomic-path correctness

- **Per-rotation hardware-rotation probe** — i915's cursor advertises the `rotation` property but only enumerates `ROTATE_0` / `ROTATE_180`; the prior property-presence shortcut routed all four values to the kernel and EINVAL'd on 90/270. `probe_plane_properties()` now walks the property's enum table and builds `rotation_supported_mask`; unsupported angles transparently route through `blit_frame`'s software pre-rotation while supported ones still ride kernel scanout. `set_rotation()` re-blits across HW↔SW boundaries so a single Renderer can sit in HW for 0/180 and SW for 90/270 on the same i915 plane. `has_hardware_rotation()` reports the per-rotation answer ("is HW handling *this* angle?") instead of static property-presence.
- **Ping-pong dumb buffers on the atomic path** — amdgpu DC reads cursor pixels live each vblank, so an in-place `blit_frame` followed by a position commit visibly tears for ~1 vblank. Two ARGB8888 buffers; `blit_frame` writes the back buffer, `stage_position` emits its `FB_ID`, `commit_position` swaps the active index on success. `commit_buffer()` (the buffer the next commit will publish) drives `CRTC_W/H`, `SRC_W/H`, and the hotspot math without callers needing to know which slot is active. Legacy path stays single-buffered (rotation rejected; `drmModeSetCursor` handles uploads).
- **Oversize sprite handling + accepted-buffer probe** — `box_downscale_rotated` is an area-weighted integer-arithmetic downscaler composed through the rotation sampler; needed because libxcursor returns the theme's closest size, not necessarily ≤ requested (Adwaita ships 24/30/36/48/72/96, so a 64 request hands back 72). `probe_acceptable_size` walks `{256, 128, 64}` via `DRM_MODE_ATOMIC_TEST_ONLY` at create time and keeps the first that passes — `DRM_CAP_CURSOR_WIDTH` is an upper bound, not an enumerated set (i915 accepts only 64; amdgpu DC accepts 64/128/256). `last_blit_w/h` tracked across blits so `buffer_hotspot` scales the hotspot in lockstep with the pixel downscale.
- **System "default" theme bridge in `Theme::resolve`** — between the user's named theme chain and the discovery-order fallback, BFS the system "default" theme (typically `/usr/share/icons/default` symlinked to the active pack — DMZ-White, Yaru, etc.). Stops resolve from landing on whichever partial pack happened to sort first on disk.
- **0-size substitution** — `XcursorFilenameLoadImages(..., 0)` falls back to 24 px, well below the 64 px KMS cursor floor (i915 minimum; amdgpu DC accepts 64/128/256). Substitute 64 px when callers pass 0; explicit sizes pass through unchanged.

### Examples

- **`examples/mdi_demo/`** — multi-document desktop showcase exercising csd Tier 0 end-to-end: `PlanePresenter` scheduling, `OverlayReservation` plane coordination, glass theme on multiple decorated surfaces over a shared background. Each `Document` carries its own `WindowAnim`; the main loop ticks via `Shell::tick_animations(dt)` and pointer motion retargets hover even outside an active drag.
- **`examples/common/cursor_size.hpp`** — shared per-output sizing helper. Derives DPI from the connector's EDID `mmWidth` + active mode resolution, snaps to the conventional 96 / 192 / 288 ladder via `round(dpi / 96)`, and returns separate sprite + buffer sizes — sprite is free to land below 64 (Renderer centers it inside the buffer), buffer must be one of `{64, 128, 256}` for the kernel to accept it.
- **`examples/advanced/csd_smoke/`** gains `--presenter {scene|plane}`. `scene` (default) keeps the single-LayerScene flow as the smallest reproducer for renderer / surface bugs; `plane` drives bg through LayerScene and arms decoration through `OverlayReservation` + `PlanePresenter` via a second atomic commit. Also picks up GBM probe (production allocator when available, dumb fallback otherwise) and equals-form argv parsing (`--theme=lite`).
- **`examples/scene/camera/`** — fps/cameras status badge (320×56 ARGB8888 at zpos=64, bottom-right) repainted on each 1-second fps tick. New `status_overlay_renderer.{hpp,cpp}` TU paints translucent fill unconditionally and centered text via Blend2D when the umbrella gate is on; `CAMERA_STATUS_HAS_BLEND2D` is plumbed through CMake/Meson rather than `__has_include` because Fedora and Arch ship Blend2D headers in `/usr/include` even when not linked, and a header probe would silently pull `BL` paths into the TU and break the link. Camera layers move to `ContentType::Video` so the priority allocator keeps them on real overlays even when budget is one short — the badge (Generic, `update_hint_hz=1`) yields its plane to `CompositeCanvas` instead. VT-switch pause/resume hardened: scene CPU mappings + cached FB ids dropped via `on_session_paused()` before the kernel revokes the fd, and `resume_cb` filtered on `/dev/dri/` so libinput-fd resumes can't clobber the DRM card slot. Validated on amdgpu Granite Ridge by driving `loginctl activate` against a sibling tty.
- **`examples/scene/video_player/`** — minimal demo of `drm::scene::GstAppsinkSource`. Three pipeline shapes: default `videotestsrc` (works on any installed plugin set), `--file PATH` (filesrc → decodebin3, sw or hw decoders depending on what's installed), `--launch "<pipeline>"` (caller-supplied, must terminate in `appsink name=sink`). Pre-pulls the first decoded sample before the first commit — single-layer scene has no always-ready background, so a cold commit would arm no plane and the kernel would modeset the CRTC blank. EACCES from `scene->commit` is treated as a soft pause (drmIsMaster lags libseat's pause_cb); EAGAIN as a single-frame retry. Genuine errors set an `error_exit` flag so the process returns `EXIT_FAILURE` to systemd / shell scripts; EOS, signal, and user-quit stay `EXIT_SUCCESS`. Quit path is the libinput keyboard (Esc / Q / Ctrl+C) — when libseat puts the TTY in KD_GRAPHICS the kernel suppresses Ctrl-C signal delivery. Validated on amdgpu Granite Ridge (2026-05-06): videotestsrc pipeline, multiple Ctrl+Alt+F<n> cycles, no exit, no log output past the startup banner.

### Fixes

- **Allocator surfaces EACCES from test commits** — `try_test_commit` returned `bool`, so `apply_previous_allocation` flattened every TEST_ONLY failure to EAGAIN. Callers that gate on EACCES to soft-pause never saw it; non-trivial pipelines exited on the first frame after master revocation. `try_test_commit` now returns `std::error_code`; `apply_previous_allocation` propagates EACCES verbatim and keeps EAGAIN flattening for every other failure. `Allocator::apply` short-circuits on EACCES from the fast path and the warm-start path so callers see the master-loss signal instead of a pile of doomed test commits and a composition pass that would also EACCES. The fast path also gains the warm-start's recovery semantic: non-EACCES test failure falls through to full_search rather than returning EAGAIN as if no other plane assignment could possibly fit.

### Hardware validation

- `examples/camera/` validated on i915 (UVC + i915 laptop, 2026-05-05). Exercises the per-frame `arm_layer_plane_color_props` path (`COLOR_ENCODING` / `COLOR_RANGE` on overlay planes) that amdgpu DC OVERLAYs don't expose. RPi5 (vc4) and RK3588 (rkisp1) remain untested.

### Scripts

- **`scripts/build-deps.sh`** — from-source builder for the four CI-tracked dependencies (libcamera, blend2d + sibling asmjit clone, libseat/seatd, libyuv). Default install prefix `/usr/local`; `PREFIX`, `WORKDIR`, `JOBS`, `SUDO`, and per-project `*_REF` env vars cover the variations a developer or runner is likely to want. No-arg builds all four; otherwise builds the listed subset. Library-appropriate meson / cmake config per project; one `ldconfig` refresh at the end.

### Tests

- `tests/unit/test_csd_animator.cpp` — target / start / tick / apply_to coverage.
- `decoration_geometry` tests in `test_csd_renderer.cpp` — panel inset, button ordering, undersized clamp, paint↔geometry agreement at the close-button center.
- `OverlayReservation` tests — partitioned (3-overlay-per-CRTC) and shared (6-overlay-pool) hardware shapes, shortfall + try-again, hotplug release/reattach, idempotent release, re-reserve replacement, zpos-floor filter, format-mismatch culling, zpos-less plane skip, span stability, `count == 0` trivial path.
- `PlanePresenter::compute_writes` tests — error paths (`no_buffer_space`, empty slots), disarm path (closed surface emits `FB_ID=0` + `CRTC_ID=0` and skips geometry), armed path (every required write present, SRC dimensions in 16.16, CRTC geometry matches `SurfaceRef`), optional-property gating (blend + alpha emitted only when slot prop ids are non-zero).
- `tests/integration/test_gst_appsink_source_vkms.cpp` — sysmem-memcpy import end-to-end against VKMS, EOS surfacing, mid-stream caps change. DMABUF zero-copy hardware-validated separately (CI has no hw decoder).
- `tests/unit/test_gst_appsink_source.cpp` — argument validation; `function_not_supported` branch when GStreamer is off.

## v1.3.0 — 2026-05-04: CSD module + keyboard repeat / LED state / keymap reload

### `drm::csd` — client-side decorations (gated on Blend2D, `-Dblend2d=enabled` / `DRM_CXX_HAS_BLEND2D`)
- **`drm::csd::Theme`** — toml++-loaded theme schema. `Color` POD (r/g/b/a), `Theme` carries panel gradient stops, specular, noise amplitude, traffic-light fills + hover variants, shadow color, rim color, title font hints. Built-in themes: `default`, `lite`, `minimal`.
- **`drm::csd::Surface`** — one CPU-mappable, KMS-scanout-ready ARGB8888 buffer + framebuffer ID per managed decoration. `Surface::create` tries GBM first (LINEAR ARGB8888 with SCANOUT+WRITE, modifier `DRM_FORMAT_MOD_LINEAR` pinned) and falls back to dumb on GBM unavailability or allocation failure; chosen path recorded on `backing()` (`SurfaceBacking::Gbm` / `SurfaceBacking::Dumb`). A second overload takes just a `Device` for the dumb-only path used by headless tests.
- **`drm::csd::WindowState`** — POD the shell hands the renderer per paint pass: title, focused flag, `HoverButton`, dirty bitfield reserved for partial-redraw. Header-only, Blend2D-free.
- **`drm::csd::ShadowCache`** — LRU of pre-blurred decoration shadows keyed on `(width, height, Elevation, theme_id)`. Single-channel alpha mask of the rounded-rect panel, three-pass separable box blur, PRGB32 tinting by the theme's shadow color. Composites via SRC_OVER through the `ShadowDest` interface; intentionally Blend2D-free.
- **`drm::csd::Renderer`** — paints one decoration into a `Surface` per call. Glass theme: soft shadow halo from `ShadowCache` (Option C — pre-blurred patch from the alpha margin, SRC_OVER underneath the panel), vertical linear-gradient panel, specular highlight via `BL_COMP_OP_SCREEN` clipped to the top edge, frosted noise tile (deterministic 64×64 LCG, MULTIPLY at the theme's noise amplitude), title text (2-pass shadow, skipped when no font face loads), three traffic-light buttons with radial-gradient fills + per-button hover variant, 1-px inner-stroke rim (focused vs blurred color). `RendererConfig` carries the theme + font face + content rect.

### `drm::input` — keyboard repeat, LED state, keymap reload
- **`drm::input::KeyRepeater`** — timerfd-driven auto-repeat synthesis for held keys. Per-key eligibility from `xkb_keymap_key_repeats` (modifiers + lock keys do not repeat). `sym` / `utf8` re-resolved on every tick against current xkb state, so Shift / AltGr level switches during a hold take effect on the next repeat. Defaults: 600 ms initial delay, 25 Hz interval. `fd()` for poll/epoll integration, `dispatch()` to drain expirations and emit one event per tick. `cancel()` to drop in-flight repeat across session pause.
- **`KeyboardEvent::repeat`** — flag distinguishing synthesized events from real ones.
- **`Keyboard::should_repeat`, `caps_lock()` / `num_lock()` / `scroll_lock()` accessors, `KeyboardLeds` snapshot struct, `leds_state()`.**
- **`Keyboard::create_from_string(buffer)`** — wraps `xkb_keymap_new_from_buffer` for Wayland-style mmap'd keymap fds; buffer copied internally; explicit-length form handles non-NUL-terminated `string_view` safely.
- **`Keyboard::reload(KeymapOptions)`** — rebuilds `xkb_keymap` + `xkb_state` in place from new RMLVO names. Strong-exception: a malformed RMLVO leaves the existing keymap intact. Held-key state preserved (replays `XKB_KEY_DOWN` for each tracked evdev keycode so a still-pressed Shift / Ctrl / letter survives the swap and a subsequent release transitions cleanly). Lock latch preserved (snapshots `leds_state()` before the swap, restores via `set_leds()` after the held-key replay). Caller is expected to push the latch out via `seat.update_keyboard_leds(kb.leds_state())` after success.
- **`Keyboard::set_leds(KeyboardLeds)`** — synthesizes press+release for each lock key whose desired state differs from the current xkb-tracked state. Used internally by `reload` and externally by callers honoring an externally-provided lock-state hint (e.g. logind "Caps Lock was on at session start"). Documented limitation: Scroll Lock silently no-ops on layouts whose compat doesn't mod-map `<SCLK>` (xkb's default complete compat).
- **`input::Seat::update_keyboard_leds`** — pushes the xkb-tracked lock state back to the kernel so the physical LEDs follow Caps / Num / Scroll Lock. Tracks keyboard-capable libinput devices internally; the last applied state is re-pushed to any newly-added device so VT-resume and hotplug do not leave LEDs lagging.

### Examples
- **`examples/basics/keyboard/`** — Blend2D-rendered keyboard demo: text entry with auto-repeat + IBus/GTK Ctrl+Shift+u Unicode-codepoint sequence. Downloads Noto Sans at configure time; gated off by default behind `-Dkeyboard` / `-DDRM_CXX_BUILD_KEYBOARD`; hard-requires Blend2D when enabled.
- **`examples/advanced/csd_smoke/`** — throwaway hardware-validation harness for `csd::Renderer`. Paints one glass decoration into a `csd::Surface` and arms it on an overlay plane via `LayerScene`. Validates the `(format, modifier, zpos)` story end-to-end and exposes `--seconds N` / `--theme {default|lite|minimal}` / `--png OUT.png` (round-trip via `drm::capture::snapshot` for headless regression checks). Gated on `DRM_CXX_HAS_BLEND2D`.
- **`examples/scene/layered_demo`** — `KeyRepeater` wired into the poll loop so arrow-key nudging and the `[` / `]` alpha controls auto-repeat. Real and synthesized events share the action handler; `Keyboard::process_key` always runs on the raw event so xkb modifier state stays current for the repeater's re-resolution. `repeater.cancel()` on session pause.

### Fixes
- **`signage_player`** — `overlay_renderer.cpp` Blend2D paths now gate on `DRM_CXX_HAS_BLEND2D` instead of `__has_include(<blend2d/...>)`, so a `-Dblend2d=disabled` configure actually disables the Blend2D path on distros (Fedora, Arch) that always ship `blend2d-devel` under `/usr/include`.

### Tests
- `tests/unit/test_csd_theme.cpp`, `test_csd_surface.cpp`, `test_csd_shadow_cache.cpp`, `test_csd_renderer.cpp`.
- `tests/unit/test_key_repeater.cpp` — config validation, release-disarms-the-timer invariant, repeat-eligibility filtering, synthesized-event ignore path.
- `tests/unit/test_input.cpp` gains Caps Lock latch + LED snapshot coverage; round-trip `create_from_string` against a serialized "us" RMLVO keymap; bogus buffer fails; `set_leds` drives caps + num lock latches up and down idempotently; `reload` preserves a held Shift across the swap (level switch survives, release after reload still transitions); `reload` preserves the Caps Lock latch; `reload` with bogus RMLVO leaves the existing "us" keymap working.

## v1.2.0 — 2026-05-03: Scene API + example tree

### `drm::scene` — high-level layer scene
- **`drm::scene::LayerScene`** — declarative layer API above `planes::Allocator::apply`. `add_layer` / `remove_layer` / `set_dst_rect` / `set_src_rect` / `set_zpos` / `set_alpha` / `set_source` mutate state; `commit()` runs the allocator, builds the `AtomicRequest`, and returns a `CommitReport` with `layers_assigned` / `layers_composited` / `layers_unassigned` / `properties_written` / `fbs_attached` / `test_commits`.
- **Property minimization** — per-plane snapshot diffing skips redundant property writes; `FB_ID` always re-emits (page-flip protocol). `force_full_property_writes` opt-out for debugging.
- **Composition fallback** — `CompositeCanvas` (double-buffered ARGB8888 surface, ping-pong via `begin_frame()`); `compose_unassigned()` blends layers that did not reach a hardware plane and arms the canvas onto a free plane. `LayerDesc::force_composited` knob; canvas plane pre-reservation when `layer_count() > eligible_canvas_planes`.
- **`LayerScene::rebind(crtc, connector, mode)`** — explicit teardown + re-enumerate + rebuild; layer handles + sources survive. `CompatibilityReport` flags off-screen layers.
- **VT-switch lifecycle** — `on_session_paused()` / `on_session_resumed()` tear down + restore buffer mappings; pairs with `drm::session::Seat`.
- **Per-layer placement readout** — `Layer::assigned_plane_id()` exposes which hardware plane the allocator landed each layer on.
- **Polymorphic buffer sources** — `LayerBufferSource` abstract base + `AcquiredBuffer { fb_id, acquire_fence_fd, opaque }`. `cpu_mapping()` returns `nullopt` for tiled / non-LINEAR sources.
  - `DumbBufferSource` — scene-allocated 32bpp dumb buffer.
  - `ExternalDmaBufSource` — caller-owned DMA-BUF fds with `(format, modifier, plane[])` metadata; single-plane LINEAR + multi-plane (NV12, YUV420). `on_release()` callback fires after scanout completes.

### `drm::cursor` — hardware cursor with software fallback
- XCursor theme resolver + KMS cursor renderer with runtime rotation, `HOTSPOT_X` / `HOTSPOT_Y` virtualization, hardware-validated rotation harness.

### `drm::session::Seat` — session manager glue (gated by `DRM_CXX_SESSION`)
- libseat-backed logind / seatd / builtin mux. `enable_seat` / `disable_seat` / `switch_session`. `InputDeviceOpener` lets `input::Seat` route privileged opens through libseat.

### `drm::display::HotplugMonitor`
- Connector hotplug event stream over `udev`. `fd()` for poll/epoll integration, `dispatch()` to drain.

### `drm::capture` — Blend2D-backed CRTC snapshot
- Per-plane composition snapshot of an active CRTC, PNG encode via Blend2D. Companion `capture_demo` example, VKMS integration-test harness.

### Allocator improvements
- **Format-modifier-aware bipartite matching** — `IN_FORMATS` modifier list considered in plane eligibility; `LayerDesc::modifier` field.
- **Priority eviction** — `ContentType::Video` = 100, `update_hint_hz > 30` = 80, `update_hint_hz > 0` = 50, default = 10. Eviction is priority-driven.
- **Warm-start path** — `apply_previous_allocation` re-validates with one `TEST_ONLY`, producing `test_commits=0` (after the validating one) in steady state.
- **Two-tier placement** — per-group spatial placement, then a scene-wide partial fallback (drop most-constrained, retry) when total_assigned == 0.

### Plane registry
- `ColorEncoding` (`BT_601` / `BT_709` / `BT_2020`) + `ColorRange` (`Limited` / `Full`) enums.
- `PlaneCapabilities::has_color_encoding` / `has_color_range` plus cached enum integers.
- `DisplayParams::color_encoding` / `color_range` per-frame overrides; `LayerScene::arm_layer_plane_color_props` arms them on planes that expose the props.

### `drm::PageFlip`
- `add_source(fd, callback)` — register foreign fds (libcamera `eventfd`, `signalfd`, etc.) on the same epoll loop the page-flip dispatcher uses.

### `drm::Device`
- `Device::from_fd(int)` — wrap a caller-owned fd (e.g. one handed back by `libseat_open_device`).

### `drm::input::Seat`
- `InputDeviceOpener { open, close }` — caller-supplied open/close callbacks routed through libseat for `/dev/input/event*` opens. Per-fd cap re-enable on resume.

### Examples
- Bucketed tree: `examples/{basics,scene,allocator,advanced}/`.
- New: `signage_player`, `hotplug_monitor`, `cursor_rotate`, `capture_demo`, `video_grid`, `layered_demo`, `scene_warm_start`, `scene_priority`, `scene_formats`, `test_patterns`, `camera`, `thorvg_janitor`.
- Rewritten: `atomic_modeset` on `LayerScene`, `mouse_cursor` on `drm::cursor`.
- Shared helpers: `examples/common/open_output.hpp` (`open_device` + `open_and_pick_output` factor the libseat fd-open + first-connected-connector pickup), `select_connector.hpp` (`pick_connector` with `k_main_rank` / `k_internal_rank` / `k_external_rank`), `select_device.hpp`, `vt_switch.hpp` (Ctrl+Alt+F<n> chord), `format_probe.hpp`.

### Benchmarks (gated by `DRM_CXX_BUILD_BENCHMARKS=ON` / `-Dbenchmarks=true`)
- `plane_stress` — synthetic LayerScene workload; `--layers / --formats / --size / --churn / --churn-rate / --duration / --csv / --quiet` with per-frame CSV output.
- `allocator_torture` — six adversarial cases (N+1, format cascade, scaler monopoly, rapid churn, slow drift, burst-then-calm); PASS/FAIL/SKIP exit codes.

### Documentation
- `README.md` rewritten around `LayerScene` as the headline feature.
- `docs/scene.md` — design rationale, buffer-source model, extension points (EGL Streams, foreign DMA-BUF, multi-CRTC, animation), out-of-charter items.
- Per-example `README.md` files across the bucketed tree.
- Doxygen briefs filled in across the public scene headers.

### Build + CI
- thorvg 1.0.4, Blend2D, and libcamera v0.5.2 built from source in CI; cached.
- libseat-dev installed from apt.
- Weekly `drmdb` compat CI.
- VKMS integration-test pattern (`tests/integration/test_*_vkms.cpp` with `GTEST_SKIP` self-skip when VKMS isn't loaded).

## v1.1.0 — C++17 migration

- **Project language target lowered from C++23 to C++17.** The library
  still picks up `std::expected`, `std::span`, and
  `std::print` when the toolchain has them; otherwise the `drm::expected`,
  `drm::span`, `drm::print`, and `drm::format` adapter headers transparently
  fall back to `tl::expected`, `tcb::span`, and `{fmt}`.
- Source tree `drm-cxx/` renamed to `src/`. Public `<drm-cxx/...>` include
  layout is unchanged for consumers — served at build time via a `drm-cxx`
  symlink into `src/` in the build tree and at install time via the normal
  `${includedir}/drm-cxx` install layout. Polyfill headers are vendored
  under `${includedir}/drm-cxx/vendor` so downstream consumers don't need
  `tl-expected` / `tcb-span` installed separately.
- `std::move_only_function` replaced with `std::function` in `Seat`,
  `EventDispatcher`, and `PageFlip` handler types. Existing call sites and
  lambdas continue to work unchanged (all were copy-constructible).
- `std::erase_if`, `std::string::starts_with`, and `Container::contains`
  call sites rewritten with their C++17 equivalents; transparent
  heterogeneous `unordered_map::find(string_view)` replaced with a
  `std::string` materialization at the single call site that used it.

## v1.0.0

Initial release of drm-cxx, a C++23 native re-implementation of drmpp.

### Core
- `drm::Device` — DRM device fd RAII with capability enables
- `drm::Resources` / `drm::Connector` / `drm::Encoder` / `drm::CrtcPtr` — RAII wrappers for DRM mode objects
- `drm::PropertyStore` — KMS property ID cache with `drmModeObjectGetProperties`
- `drm::core::format_name()` / `format_bpp()` — DRM format helpers

### Modeset
- `drm::AtomicRequest` — atomic commit builder with `drmModeAtomicAlloc` RAII
- `drm::ModeInfo` — mode selection: preferred, resolution match, refresh targeting
- `drm::PageFlip` — vblank event loop with epoll + `drmHandleEvent` v3

### Plane Allocator (replaces libliftoff)
- `drm::planes::PlaneRegistry` — hardware plane enumeration with capability detection
- `drm::planes::Layer` — virtual layer with dirty tracking, content hints, geometry
- `drm::planes::Output` — per-CRTC output with layer management and zpos sorting
- `drm::planes::Allocator` — constraint-solving allocator with 7 improvements:
  1. Static compatibility matrix pruning
  2. Best-first search order
  3. Warm-start from previous frame
  4. Test-commit failure memoization
  5. Hopcroft-Karp bipartite pre-solve
  6. Content-type layer priority
  7. Spatial intersection splitting
- `drm::planes::BipartiteMatching` — standalone Hopcroft-Karp implementation

### Input
- `drm::input::Seat` — libinput + udev RAII with typed event dispatch
- `drm::input::Keyboard` — xkbcommon RAII with RMLVO and file-based keymap loading
- `drm::input::Pointer` — motion accumulator and button state tracker
- `drm::input::EventDispatcher` — multi-handler fan-out
- Rich event types: `KeyboardEvent`, `PointerEvent` (motion/button/axis), `TouchEvent`, `SwitchEvent`

### Display
- `drm::display::parse_edid()` — libdisplay-info EDID parsing with colorimetry, HDR, EOTF extraction
- `drm::display::ConnectorInfo` / `ColorimetryInfo` / `HdrStaticMetadata`

### GBM
- `drm::gbm::GbmDevice` — `gbm_create_device` RAII
- `drm::gbm::Surface` — `gbm_surface_create` with front buffer locking
- `drm::gbm::Buffer` — `gbm_bo` accessor with smart release (surface-aware)

### Sync
- `drm::sync::SyncFence` — native sync via `linux/sync_file.h` (replaces libsync)

### Vulkan (optional)
- `drm::vulkan::Display` — VK_KHR_display enumeration with dynamic Vulkan loading
- `drm::vulkan::DrmSurface` — surface handle placeholder

### Infrastructure
- `drm::print` / `std::print` logging with `drm::LogLevel` runtime gating
- pkg-config generation
- GTest unit test suite (13 suites, 100+ tests)
- CI: GCC-13/14, Clang-16/17 matrix
- Examples: `atomic_modeset`, `overlay_planes`, `vulkan_display`

### Breaking changes from drmpp
- Namespace: `drmpp::` -> `drm::`
- All returns use `drm::expected<T, std::error_code>` (aliases `std::expected` on C++23, `tl::expected` on C++17)
- No libliftoff, bsdrm, libsync, spdlog, or rapidjson dependencies
- Callbacks use `std::function<>` handlers instead of virtual dispatch
- Header paths: `#include <drm-cxx/...>` canonical layout
