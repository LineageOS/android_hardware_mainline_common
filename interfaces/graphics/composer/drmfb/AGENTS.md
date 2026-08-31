# Agent Notes

This directory implements a client-composition-only Composer3 V4 DRM HAL. Read
`INITIAL_IMPLEMENTATION.md` before changing it; that file limits reference
paths and explicitly prohibits local builds and tests.

## Invariants

- Never advertise a capability unless its full contract is implemented.
- Every framework layer is changed to `Composition.CLIENT`; only the client
  target reaches KMS.
- Buffer and damage updates do not dirty validation. Other layer state does.
- Preserve command-level `commandIndex` errors and the validate, accept,
  present state machine.
- Batched layer CREATE and DESTROY operations are transactional command state,
  not direct Binder lifecycle calls.
- Keep display/config IDs stable for the lifetime of the service process.
- Prefer atomic KMS and retain the legacy CRTC/page-flip fallback. Atomic frame
  updates must not resubmit modeset state. Legacy presents return a signaled
  fence when syncobj or a waited acquire fence makes one available.
- Firmware KMS drivers require CRTC activation and a primary framebuffer in one
  commit. Preserve deferred power-on and the linear XRGB8888 CPU staging path.
- Direct scanout must remain preferred. `vendor.hwc.drmfb.cpu_conversion` is a
  read-only opt-out for staging and defaults true for firmware-KMS compatibility.
- Generic minigbm staging buffers may not support PRIME import into the display
  card. Preserve the CPU staging retry after `drmPrimeFDToHandle` failure.
- `vboxvideo` has no PRIME import and unconditionally requires XRGB8888 dumb
  staging; do not allow the general conversion opt-out to bypass it.
- `qxl` has no usable PRIME sharing and also requires local XRGB8888 dumb
  staging with a CPU-mappable external allocator.
- Damage-driven drivers such as `udl` need `FB_DAMAGE_CLIPS` even when the
  framebuffer ID is unchanged. Preserve client-target damage semantics and
  command-state rollback instead of forcing full-frame damage.
- `gud` uses the same generic shmem, XRGB staging, full-damage, and synthetic
  vsync paths; avoid a driver-specific branch unless its kernel ABI changes.
- `hyperv_drm` has a fixed virtual connector without detect support. Accept its
  unknown connection state only when valid modes exist; do not generalize that
  exception to hotpluggable DRM connectors.
- `bochs-drm` has the same fixed virtual connector behavior and otherwise uses
  generic shmem, XRGB staging, damage, and synthetic-vsync paths.
- Keep imported mapper handles alive as long as their DRM framebuffer IDs.
- Do not call Binder callbacks while `mutex_` is held. Serialize synchronous
  hotplug callbacks with `hotplug_callback_mutex_` and refresh enable/disable
  ordering with `refresh_callback_mutex_`.
- Keep both worker threads stoppable, use CRTC sequence events for vsync, and
  use monotonic timestamps.
- Keep refresh-rate debug callbacks outside `mutex_` and report the active
  fixed-refresh mode period for both callback period fields.
- Do not add a device-specific property assignment. The implementation reads
  `vendor.hwc.drm.device` as an explicit override and otherwise enumerates DRM
  primary nodes with libdrm.
- `vendor.hwc.drmfb.swap_rb` is an opt-in compatibility workaround. Prefer a
  paired FourCC and retain the narrow 32-bit RGB dumb-buffer fallback; do not
  silently apply it to YUV, protected, multiplane, or 10-bit buffers.

## Layout

- `Composer.*`: service singleton and capability policy.
- `ComposerClient.*`: AIDL methods, command/state handling, callbacks/workers.
- `DrmDevice.*`: resource/property discovery, FB import, atomic and legacy KMS.
- `service.cpp`, RC, XML, and `Android.bp`: vendor service integration.
- `drmfb-apex-*`: vendor APEX manifest and SELinux file labels. The APEX init
  module rewrites the standalone `/vendor/bin/` path at build time.

Keep changes minimal and Google C++ style. Do not use exceptions or catch
blocks. Update `README.md` when changing supported behavior.
