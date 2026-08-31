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
- Keep display/config IDs stable for the lifetime of the service process.
- Use atomic KMS only. A real present must provide a valid out-fence.
- Keep imported mapper handles alive as long as their DRM framebuffer IDs.
- Do not call Binder callbacks while `mutex_` is held; serialize callbacks with
  `callback_mutex_`.
- Keep both worker threads stoppable, use CRTC sequence events for vsync, and
  use monotonic timestamps.
- Do not add a device-specific property assignment. The implementation reads
  `vendor.hwc.drm.device` and otherwise uses `/dev/dri/card0`.

## Layout

- `Composer.*`: service singleton and capability policy.
- `ComposerClient.*`: AIDL methods, command/state handling, callbacks/workers.
- `DrmDevice.*`: resource/property discovery, FB import, atomic KMS, modes.
- `service.cpp`, RC, XML, and `Android.bp`: vendor service integration.

Keep changes minimal and Google C++ style. Do not use exceptions or catch
blocks. Update `README.md` when changing supported behavior.
