# fbdev graphics HAL

`fb` is a software-only graphics stack for systems with a legacy Linux fbdev
display and a software renderer such as SwiftShader or llvmpipe. It contains an
allocator AIDL V2 service, a Stable-C mapper V5 SP-HAL, and a Composer3 V4
service. It is not a GPU allocator and does not produce dma-bufs.

## Integration

Install these standalone modules together:

- `android.hardware.graphics.allocator-service.fb`
- `mapper.fb`
- `android.hardware.graphics.composer3-service.fb`

Alternatively install the non-updatable, SoC-specific vendor-bootstrap APEX
`com.android.hardware.graphics.allocator.fb`. For APEX products, set
`fb_graphics.include_init_rc=false` and
`fb_graphics.include_vintf_fragments=false` to avoid duplicate standalone init
and VINTF installation. The APEX uses the platform hardware key and certificate
and contains both services, the mapper, all VINTF fragments, and generated init
scripts. Its linker configuration exposes `mapper.fb` from the APEX namespace
to Stable-C mapper clients. A product must install exactly one
allocator/mapper/composer stack and
must not install both this APEX and another graphics HAL APEX.

SELinux policy outside this directory must allow the allocator to create and
map memfds, SurfaceFlinger and graphics clients to use the mapper SP-HAL, and
the composer domain to open, ioctl, and map the selected framebuffer node.
Device-node labels and allow rules are board-specific and are intentionally not
provided here. The product sepolicy must also map `mapper/fb` to
`u:object_r:hal_graphics_mapper_service:s0` in `service_contexts`.

## Properties

- `vendor.hwc.fbdev.device` is a read-only explicit fbdev path. Without it the
  composer tries `/dev/graphics/fb0` and then `/dev/fb0`.
- `vendor.hwc.fbdev.swap_rb` is a read-only boolean, default false. It swaps
  source red and blue only in the bounded RGB conversion path.

## Buffers

The transport handle has two FDs and fixed-width integer fields only. One memfd
contains linear pixels; the other contains shared standard metadata followed by
the exact requested client reserved region. Process mappings and lock counts are
never serialized. Protected allocations and unknown V2 usage bits or options
are rejected.

Supported formats are RGBA_8888, RGBX_8888, BGRA_8888, RGB_888, RGB_565,
RGBA_FP16, BLOB, RAW16, YV12, NV21/YCRCB_420_SP, planar YCBCR_420_888, P010, and
P210. YUV is linear media/VTS support only; Composer accepts RGB client targets
in the five 8-bit/565 formats and converts them to the fbdev channel layout.
There is no camera-vendor tiling, implementation-defined format selection,
compression, or GPU-private memory.

GPU texture/render usage flags are accepted only to interoperate with software
EGL/Vulkan implementations that consume mapper-locked shared memory. These
allocations do not satisfy a hardware GPU driver's dma-buf or private-memory
requirements. Mapper CPU locks are therefore permitted on these software GPU
buffers even when the original descriptor did not contain CPU usage bits.
Front-buffer allocations are intentionally unsupported.

Mutable dataspace, blend mode, SMPTE2086, and CTA861_3 values are in the shared
metadata memfd and serialized across processes with advisory file locks.
Standard metadata encoding uses the platform mapper helpers. Pixel mappings
support nested locks, explicit flush/reread, and return no fabricated release
fence.

## Composer Scope

The composer exposes one internal physical display and one fixed configuration
from fbdev. Every layer is validated as `Composition.CLIENT`; only the client
target is presented. It supports transactional lifecycle batch commands,
target slots, expected-present timestamps, fixed-refresh debug callbacks,
NATIVE/COLORIMETRIC behavior, client-target damage, and hardware vsync through
`FBIO_WAITFORVSYNC`. Unsupported wait ioctls are cached and use a stoppable
synthetic monotonic fallback. After a hardware wait, `FBIOGET_VBLANK` capability
flags and valid retrace counts improve timestamp sampling and diagnostics. The
generic UAPI has no vblank timestamp field, so reserved fields are ignored and
the callback uses the closest monotonic sample. Empty damage means the full
frame, while one empty rectangle means no changed pixels. Damage is clipped
before bounded conversion and flush.

When the initial mode has only one page, initialization requests two complete
pages by changing only `yres_virtual` through `FBIOPUT_VSCREENINFO`. Returned
mode fields, stride, and mapping bounds are revalidated; unsupported or unsafe
expansion retains the original single-page mode. If multiple complete pages are
available, frames are copied to an inactive page and presented with
`FBIOPAN_DISPLAY`. Otherwise Composer copies to the visible page. Acquire fences
are waited for up to three seconds. A waited acquire sync-file is duplicated as
the present fence when available; otherwise no fence is returned under
`PRESENT_FENCE_IS_NOT_RELIABLE`. Eventfds are never exposed as fences.
Switching backing pages still copies the full frame because page contents are
not assumed to be synchronized.

Virtual displays, readback, overlays, sideband, HDR display output, color
transforms, content sampling, boot config persistence, HDCP, LUTs, seamless
mode changes, hotplug discovery, and refresh-rate switching are unsupported.
Fbdev has no reliable completion fence, and fallback vsync is not hardware phase
locked. Packed true-color fbdev outputs may use arbitrary non-overlapping
RGB and optional alpha bitfields within 1-32 bits per pixel, including RGB565
and XRGB2101010. Native mutable C8 pseudocolor requires successful RGB332
colormap programming; static or unprogrammable palettes are rejected rather
than displaying incorrect colors. Grayscale, planar, FOURCC, direct-color, and
nonstandard fbdev modes remain unsupported.

DRM fbdev emulation, including `efidrm`, `ofdrm`, `simpledrm`, and `vesadrm`,
maps a shadow framebuffer rather than the firmware aperture. After conversion,
the HAL uses `pwrite` on the fbdev node to trigger immediate damage propagation,
with `msync` as a compatibility fallback. It treats `smem_len == 0` as unknown,
uses the reported line stride, does not depend on physical `smem_start`, and
caches permanent pan/blank limitations. Use the Composer Binder dump for
geometry, channel layout, page, power, target, layer, and validation state.
