# DRM framebuffer Composer3 HAL

`drmfb` is a deliberately small Composer3 V4 implementation for systems where
SurfaceFlinger performs all composition. Every application layer is validated
as `Composition.CLIENT`; the HAL imports and scans out only the resulting
client target through DRM KMS.

## Integration

Add `android.hardware.graphics.composer3-service.drmfb` to the product packages
and include its VINTF fragment. By default the service enumerates DRM primary
nodes with libdrm and deterministically prefers a KMS card with a connected
internal display (including virtual-machine connectors), followed by any
connected display and then a headless KMS card. The read-only
`vendor.hwc.drm.device` property can select an explicit
primary-node path and disables automatic selection. The HAL requires DRM master
access and prefers atomic modesetting with a primary plane. Drivers without
atomic KMS use a legacy CRTC/page-flip backend.

The service can be installed directly with
`android.hardware.graphics.composer3-service.drmfb`, or packaged in the vendor
APEX module `com.android.hardware.graphics.composer.drmfb`. APEX products should
set `drmfb_composer.include_init_rc=false` and
`drmfb_composer.include_vintf_fragments=false` so the standalone service does
not install duplicate init or VINTF declarations. The APEX uses the platform
hardware key and certificate, is non-updatable and SoC-specific, and exposes
the canonical runtime APEX name `com.android.hardware.graphics.composer`.
Install only one composer APEX implementation and only one provider of the
`IComposer/default` service in a product.

The paired allocator must expose mapper 4+ standard metadata for DRM fourcc,
modifier, dimensions, and plane layouts. Plane dma-buf FDs are inferred using
the established convention that a zero offset after plane zero starts a new
FD; nonzero offsets share the preceding FD.

Systems whose CPU renderer produces red/blue-swapped client targets can set the
read-only `vendor.hwc.drmfb.swap_rb=true` property. The HAL first tries a paired
opaque DRM FourCC and otherwise applies the correction during CPU staging. Leave
the property disabled for correctly rendered GPU output.

When a primary plane cannot scan out Android's client-target FourCC/modifier,
the HAL converts unprotected, CPU-readable, single-plane 32-bit RGB targets into
double-buffered linear `XRGB8888` dumb framebuffers. This supports the universal
compatibility format exposed by `efidrm`, `ofdrm`, `simpledrm`, and `vesadrm`
without requiring those drivers to scan out `ABGR8888`. Multiplane, YUV, 10-bit,
protected, or otherwise unmappable client targets are not staged.

The `udl` DisplayLink driver supports generic GEM-shmem PRIME imports and
linear `RGB565`/`XRGB8888` scanout. Android client targets use the same
XRGB8888 staging fallback when necessary. Since `udl` transfers only damaged
regions over USB, drmfb forwards validated `ClientTarget.damage` rectangles
through `FB_DAMAGE_CLIPS` when that standard plane property is available.
Empty damage means full-frame damage, while empty rectangles retain the
no-update signal. This also keeps same-buffer content updates visible without
forcing full transfers on other damage-driven DRM drivers. With CPU conversion
disabled, the allocator must provide a directly importable linear RGB565 or
XRGB8888 target. On multi-card systems, set `vendor.hwc.drm.device` to the UDL
primary node when it should be the composer device; this single-card HAL does
not combine displays from multiple DRM cards.

The `gud` Generic USB Display driver has the same compatible GEM-shmem and
damage-driven model. It exposes linear XRGB8888 either natively or through its
kernel conversion path, so drmfb uses direct import when possible and the
existing XRGB8888 staging fallback otherwise. Full-frame damage ensures every
present is transferred over USB, and synthetic vsync covers the absence of a
hardware vblank source. As with UDL, select its primary node explicitly on a
multi-card system when the USB display should own the composer service.

The `hyperv_drm` synthetic display also uses atomic GEM-shmem scanout, exposes
only linear XRGB8888, and updates VRAM from damage clips. drmfb therefore uses
the same XRGB staging, full-frame damage, deferred CRTC activation, and
synthetic-vsync paths. Its fixed virtual connector has no detect callback and
can remain `DRM_MODE_UNKNOWNCONNECTION`; drmfb treats that state as connected
only for `hyperv_drm` when the connector supplies valid modes. Runtime display
power-down remains limited by the kernel driver's lack of an atomic-disable
callback.

The `qxl` driver exposes atomic XRGB8888/ARGB8888 scanout but explicitly returns
`-ENOSYS` for PRIME SG export and import, and its primary plane requires QXL GEM
objects. drmfb therefore always stages client targets into QXL-allocated
XRGB8888 dumb buffers, independent of the general CPU-conversion opt-out. Pair
this path with a CPU-mappable allocator such as `allocator.fb`; minigbm's dumb
backend correctly rejects QXL because gralloc transport requires PRIME export.
QXL performs full primary updates itself and has no hardware-vblank callbacks,
so drmfb's synthetic-vsync fallback remains applicable.

The `bochs-drm` QEMU standard-VGA driver uses atomic GEM-shmem scanout with
linear XRGB8888/BGRX8888, damage clips, and no hardware-vblank callbacks. It is
covered by direct PRIME import or XRGB8888 staging, client-target damage,
deferred CRTC activation, and synthetic vsync. Like Hyper-V, its fixed virtual
connector has no detect callback; drmfb accepts `UNKNOWNCONNECTION` for this
driver only when valid modes are present.

Direct scanout is always preferred over CPU conversion, including compatible
alpha-to-opaque FourCC reinterpretation. Products can set the read-only
`vendor.hwc.drmfb.cpu_conversion=false` property to disable staging entirely;
unsupported client-target formats will then fail instead of consuming CPU time.
The default is true to retain firmware-KMS compatibility. The exception is
`vboxvideo`: it does not provide PRIME import and its primary plane requires
VRAM GEM objects, so the HAL always stages into driver-allocated XRGB8888 dumb
buffers regardless of this property.

## Behavior

- Physical connectors are ordered internal-first and then by connector ID.
- Mode configuration IDs remain stable for the service process lifetime.
- The first callback registration emits a connected event for every display.
- Validation requests CLIENT composition. Atomic displays use `TEST_ONLY`;
  legacy displays verify the mode and framebuffer dimensions.
- Atomic content updates change only primary-plane state and return the
  `OUT_FENCE_PTR` fence. Modeset state is submitted only for power and mode
  changes, avoiding full modesets on ordinary frame updates.
- Client-target damage is bounds-checked and forwarded to `FB_DAMAGE_CLIPS`;
  drivers can retain partial-update optimizations.
- Legacy content updates use `drmModePageFlip` and synchronously wait for its
  exact completion event. Acquire fences are waited in userspace. Completed
  presents return a signaled sync-file fence when DRM syncobjs are available,
  with a waited acquire fence used as a compatibility fallback.
- A constrained non-seamless mode change waits until its requested monotonic
  time before applying the mode; seamless transitions are not supported.
- OFF disables the CRTC. Atomic and legacy ON transitions defer activation until
  a framebuffer is presented, accommodating drivers that require a primary
  plane whenever a CRTC is active.
- Vsync uses DRM CRTC sequence events and falls back to monotonic timed waits.
- Layer creation and destruction can use Composer3 lifecycle batch commands.
- Fixed-refresh debug callbacks report the active mode period immediately when
  enabled and after mode changes.
- Physical orientation follows the standardized DRM `panel orientation`
  connector property and otherwise defaults to `Transform::NONE`.

## Intentional Scope

Virtual displays, readback/writeback, overlays, sideband streams, HDR and color
processing, content sampling, boot configuration persistence, HDCP, LUTs,
seamless mode changes, idle timers, and low-latency modes are unsupported. No
display capabilities are advertised. The global
`PRESENT_FENCE_IS_NOT_RELIABLE` capability covers legacy KMS, while atomic KMS
still returns explicit present fences. Layer lifecycle batching and fixed-rate
refresh debug callbacks are advertised; `SKIP_VALIDATE` is not. NATIVE color
mode with COLORIMETRIC intent is the only color behavior.

Use `dumpsys android.hardware.graphics.composer3.IComposer/default` for the
selected DRM objects, modes, power state, layer counts, target cache state, and
validation state.
