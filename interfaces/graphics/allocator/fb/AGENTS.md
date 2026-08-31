# Agent Notes

This directory is a self-contained software allocator, Stable-C mapper V5, and
client-composition-only Composer3 V4 stack for legacy fbdev systems.

## Invariants

- Keep the native handle transport-only: two shared-memory FDs and integers,
  with no addresses, mutexes, or other process-local state.
- Keep descriptor acceptance centralized in `BuildLayout`; `isSupported` and
  allocation must agree.
- Reject protected memory. This implementation has no secure heap or scanout.
- Preserve shared mutable metadata and place the client reserved region exactly
  after `SharedMetadata`.
- Use platform standard metadata encoders and decoders.
- Every Composer layer becomes CLIENT. YUV buffers never reach fbdev scanout.
- Preserve command-index errors, transactional lifecycle batches, target slot
  and damage semantics, and the validate/accept/present state machine. Empty
  damage is full-frame; one empty rectangle means no changed pixels.
- Wait acquire fences before CPU access. Never manufacture an eventfd fence.
- Keep hardware vsync waits interruptible, cache permanent ioctl limitations,
  keep fallback vsync stoppable, and invoke Binder callbacks without `mutex_`.
- Trust `FBIOGET_VBLANK` fields only when their capability bits are set. The
  generic UAPI has no timestamp field; never interpret its reserved words.
- Do not broaden the red/blue workaround beyond supported RGB conversion.
- Keep fbdev output generic across validated packed true-color bitfields up to
  32 bits; do not silently accept unprogrammable palettes, grayscale, planar,
  or nonstandard memory organizations. DIRECTCOLOR requires saved, bounded
  linear ramps and colormap restoration on device release.
- Mutable C8 uses RGB332. Read-only 8-bit STATIC_PSEUDOCOLOR must query but never
  change its palette and use the precomputed bounded nearest-color lookup.
  Preserve the fbdev `pwrite` damage-notification path because DRM sysfb drivers
  expose deferred shadow mappings rather than direct firmware scanout memory.
- Keep partial conversion and flush bounded to clipped damage, but copy a full
  frame before panning to a backing page whose contents are not synchronized.
- Virtual-height expansion may change only `yres_virtual`; re-query fixed and
  variable geometry, validate complete mapping bounds, and restore the original
  mode when a two-page request cannot be used safely.
- Update `README.md` when capabilities, formats, ABI, properties, or packaging
  change.

The vendor APEX and standalone modules are mutually exclusive. Board SELinux
policy and framebuffer device labels live outside this directory.
