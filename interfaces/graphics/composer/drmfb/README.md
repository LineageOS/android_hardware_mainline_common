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

## Behavior

- Physical connectors are ordered internal-first and then by connector ID.
- Mode configuration IDs remain stable for the service process lifetime.
- The first callback registration emits a connected event for every display.
- Validation requests CLIENT composition. Atomic displays use `TEST_ONLY`;
  legacy displays verify the mode and framebuffer dimensions.
- Atomic content updates change only primary-plane state and return the
  `OUT_FENCE_PTR` fence. Modeset state is submitted only for power and mode
  changes, avoiding full modesets on ordinary frame updates.
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
