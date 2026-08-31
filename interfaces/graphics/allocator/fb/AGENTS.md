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
  semantics, and the validate/accept/present state machine.
- Wait acquire fences before CPU access. Never manufacture an eventfd fence.
- Keep synthetic vsync stoppable and invoke Binder callbacks without `mutex_`.
- Do not broaden the red/blue workaround beyond supported RGB conversion.
- Update `README.md` when capabilities, formats, ABI, properties, or packaging
  change.

The vendor APEX and standalone modules are mutually exclusive. Board SELinux
policy and framebuffer device labels live outside this directory.
