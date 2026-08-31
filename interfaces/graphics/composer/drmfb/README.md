# DRM framebuffer Composer3 HAL

`drmfb` is a deliberately small Composer3 V4 implementation for systems where
SurfaceFlinger performs all composition. Every application layer is validated
as `Composition.CLIENT`; the HAL imports and atomically scans out only the
resulting client target on a primary DRM plane.

## Integration

Add `android.hardware.graphics.composer3-service.drmfb` to the product packages
and include its VINTF fragment. The service opens the path from the read-only
`vendor.hwc.drm.device` property, defaulting to `/dev/dri/card0`. Products only
need to set that property when their KMS primary node differs. The HAL requires
DRM master access, universal planes, atomic modesetting, a primary plane, and
the standard atomic connector/CRTC/plane properties.

The paired allocator must expose mapper 4+ standard metadata for DRM fourcc,
modifier, dimensions, and plane layouts. Plane dma-buf FDs are inferred using
the established convention that a zero offset after plane zero starts a new
FD; nonzero offsets share the preceding FD.

## Behavior

- Physical connectors are ordered internal-first and then by connector ID.
- Mode configuration IDs remain stable for the service process lifetime.
- The first callback registration emits a connected event for every display.
- Validation requests CLIENT composition and performs an atomic `TEST_ONLY`
  check, including the complete primary-plane state when a target is available.
- Present repeats the full atomic test and returns the `OUT_FENCE_PTR` fence.
- A constrained non-seamless mode change waits until its requested monotonic
  time before applying the mode; seamless transitions are not supported.
- OFF tears down the plane, connector, mode, and CRTC atomically. ON restores
  CRTC mode state; scanout is attached by the next present.
- Vsync uses DRM CRTC sequence events and falls back to monotonic timed waits.

## Intentional Scope

Virtual displays, readback/writeback, overlays, sideband streams, HDR and color
processing, content sampling, boot configuration persistence, HDCP, LUTs,
seamless mode changes, idle timers, and low-latency modes are unsupported. No
global or display capabilities are advertised, including `SKIP_VALIDATE` and
layer lifecycle batching. NATIVE color mode with COLORIMETRIC intent is the
only color behavior.

Use `dumpsys android.hardware.graphics.composer3.IComposer/default` for the
selected DRM objects, modes, power state, layer counts, target cache state, and
validation state.
