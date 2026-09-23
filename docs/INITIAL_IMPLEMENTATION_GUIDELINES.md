# Initial Implementation Guidelines

This document collects the requirements, references, and guidelines that are
common to every "build this component from scratch" bring-up task in this
repository. A component's own `INITIAL_IMPLEMENTATION.md` should only list
what's specific to it (naming, non-obvious reference paths, and its own
`## Design`); everything below applies in addition to that, and does not
need to be repeated there.

`INITIAL_IMPLEMENTATION.md` is a historical, one-time bring-up brief: once
the component exists, day-to-day conventions live in the repository root
`AGENTS.md` and `docs/` (`CODE_STYLE.md`, `COMMIT_CONVENTIONS.md`,
`WORKFLOW.md`, `NAMING_CONVENTIONS.md`) and the component's own `AGENTS.md`.
Do not edit `INITIAL_IMPLEMENTATION.md` files as part of normal feature work.
Written for both AI agents and human contributors.

## Requirements

- Comply with Project Treble rules and use the latest AIDL interface where
  applicable.
- Never rename the HAL interface.
- Living inside an APEX is preferred, but not a hard requirement — some
  components legitimately can't or don't need to (e.g. a standalone tool).
  If you do ship an APEX, follow `docs/NAMING_CONVENTIONS.md` for the APEX
  module vs. manifest naming scheme; do not touch the manifest name.
- C++ is preferred, but not a hard requirement; see `docs/CODE_STYLE.md` for
  language, style, error handling, and formatting, including the SPDX header
  for new files.
- Follow `docs/NAMING_CONVENTIONS.md` for the executable, init rc service,
  APEX, `LOG_TAG`, and Android property naming patterns.
- Prefer `libbase` (`system/libbase`) for Android platform helper functions
  (when using C++).
- Write an `AGENTS.md` for future AI sessions and a `README.md` for human
  developers.
- Match the expectations of the Vendor Test Suite (VTS) module for the
  interface you're implementing.
- Commit as you go; see `docs/COMMIT_CONVENTIONS.md` for the subject, body,
  and trailer format.

## References

Paths below are relative to the AOSP source tree root.

- **APEX build handling**: `build/soong/apex/`, mainly `apex.go` and
  `apex_test.go`.
- **C/C++ build handling**: `build/soong/cc/`, mainly `cc.go` and
  `cc_test.go`.
- **AIDL HAL interface definitions and VTS**: under
  `hardware/interfaces/<domain>/aidl/` (VTS module in its `vts` subdir).
- **libbase headers**: `system/libbase/include/android-base`.

## Guidelines

- Add enough log prints for debugging.
- Do NOT browse anywhere outside the AOSP source tree for reference, and do
  not search broadly across the root of the AOSP source tree.
- Do NOT look for other HALs that weren't mentioned as reference.
- Do NOT try to compile or verify yourself; the user will do so and report
  issues back to you. See `docs/WORKFLOW.md`.
- Do NOT blindly set hardware-specific properties: read them from
  configuration files or Android properties when they can't be derived at
  runtime.
- When you are very unsure about something, ask the user before proceeding.
- This is normally a big enough project that readability matters for human
  developers: avoid letting a single source file grow too large, split into
  separate files when needed.

## Design principles: generic and flexible by default

New components should work generically across the range of hardware/devices
they target, not be tailored to a single device. Approaches that get you
there:

- Prefer deriving behavior and capabilities at runtime (from the kernel,
  sysfs/ioctl, hardware probing, or the AIDL request itself) over hardcoding
  per-device values.
- When something genuinely can't be derived at runtime, make it configurable
  through configuration files or Android properties instead of hardcoding
  it, and document the new key in the component's `README.md`.
- Favor a modular/pluggable structure (e.g. a frontend plus swappable
  backends, or a driver/quirk table keyed by a detected identifier) so
  support for new hardware can be added without changing interface-facing
  code.
- Aim to work out of the box, without additional configuration, on as many
  supported devices as feasible; only fall back to configuration for cases
  that genuinely can't be auto-detected.

## Design write-up

Structure the `## Design` section of the component's own
`INITIAL_IMPLEMENTATION.md` as: an opening paragraph stating the overall
goal/principle, the major structural parts (e.g. a frontend/backend split, or
a single-module design), any device-diversity or configuration-less-operation
goals, and an explicit list of the hardware/drivers to focus compatibility on
first.
