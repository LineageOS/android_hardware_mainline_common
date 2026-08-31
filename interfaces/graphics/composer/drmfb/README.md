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

## Behavior

- Physical connectors are ordered internal-first and then by connector ID.
- Mode configuration IDs remain stable for the service process lifetime.
- The first callback registration emits a connected event for every display.
- Validation requests CLIENT composition. Atomic displays use `TEST_ONLY`;
  legacy displays verify the mode and framebuffer dimensions.
- Atomic content updates change only primary-plane state and return the
  `OUT_FENCE_PTR` fence. Modeset state is submitted only for power and mode
  changes, avoiding full modesets on ordinary frame updates.
- Legacy content updates use `drmModePageFlip` and synchronously wait for the
  following vblank. Acquire fences are waited in userspace.
- A constrained non-seamless mode change waits until its requested monotonic
  time before applying the mode; seamless transitions are not supported.
- OFF disables the CRTC. ON prepares its mode state; legacy scanout is attached
  by the next present.
- Vsync uses DRM CRTC sequence events and falls back to monotonic timed waits.

## Intentional Scope

Virtual displays, readback/writeback, overlays, sideband streams, HDR and color
processing, content sampling, boot configuration persistence, HDCP, LUTs,
seamless mode changes, idle timers, and low-latency modes are unsupported. No
display capabilities are advertised. The global
`PRESENT_FENCE_IS_NOT_RELIABLE` capability covers legacy KMS, while atomic KMS
still returns explicit present fences. `SKIP_VALIDATE` and layer lifecycle
batching are not advertised. NATIVE color mode with COLORIMETRIC intent is the
only color behavior.

Use `dumpsys android.hardware.graphics.composer3.IComposer/default` for the
selected DRM objects, modes, power state, layer counts, target cache state, and
validation state.
