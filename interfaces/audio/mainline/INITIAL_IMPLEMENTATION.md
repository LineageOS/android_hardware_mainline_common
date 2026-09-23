# Details about making the initial implementation of Android Audio HAL for mainline kernel

We want a flexible and generic Android Audio HAL,
based on alsa-lib and alsa-ucm-conf,
for the usage on Android devices running mainline Linux kernel,
which follows proper Linux standards.

You (AI Coding Agent) act as a professional Android HAL engineer
and you've got to implement this in the directory containing this markdown file.

> See the repository root `docs/INITIAL_IMPLEMENTATION_GUIDELINES.md`
> (`hardware/mainline/common/docs/INITIAL_IMPLEMENTATION_GUIDELINES.md`) for
> requirements, references, and guidelines shared by every component. This
> file only lists what's specific to this HAL.

## Requirements

- The HAL shall be named `mainline`.
  - Init rc service name shall be `vendor.audio-hal-aidl-mainline`.
  - The APEX module name shall be `com.android.hardware.audio.mainline`.
  - Filename for the executable shall be `android.hardware.audio.service-aidl.mainline`.
  - `LOG_TAG` define on top of our source files shall begin with `MainlineAudio`.
  - Init rc and vintf fragment shall be renamed accordingly.
- Android properties defined in this HAL shall have `vendor.audio.` prefix.

## References

The paths mentioned in this section are relative to AOSP source tree root.

### AIDL HAL interface definition

In `hardware/interfaces/audio/aidl/`.
VTS module is in `vts` subdir there.

### ALSA components

- **alsa-lib**: `external/mainline-hw-deps/alsa-lib`
- **alsa-ucm-conf**: `external/mainline-hw-deps/alsa-ucm-conf`

### HAL implementations

- **Example AIDL Audio HAL**: `hardware/interfaces/audio/aidl/default`. Check it out for standard AIDL HAL example implementation.

### Linux kernel

There is a reference Linux kernel located at `kernel/virt/virtio`. Check it out for more detailed kernel sided implementations.

Note that the kernel source root path is actually a symlink.

### Android framework

- `frameworks/av/media/*audio*/`
- `frameworks/av/services/audio*/`

## Guidelines

- Please firstly understand the AIDL interface, and then everything else.

## Design

The HAL shall basically work on as many as possible devices without any additional configuration.

These are possible types of Android devices:
- Phone (Usually has integrated DSP)
- Tablet
- Desktop PC (possibly with ancient or advanced sound cards)
- Laptop PC
- TV Box (possibly with HDMI audio output only)
- Virtual Machine (like Desktop PC)

The HAL will also be used in a generic Android device configuration,
which might run on every of these device types.

If additional configurations is needed, prefer using Android properties.

Keep the HAL responding to calls from Android system even if no sound card exist.

Support using audio input/output devices from multiple sound cards.

USB sound cards shall be supported too.

Multi-channels (like 5.1 CH / 7.1 CH) shall be supported too if feasible.

Allow devices to set explicit sound card to be used via Android properties.
Devices may specify sound card using sound card name, or sound card number.

Try to reuse audio effects related components from the Example AIDL Audio HAL.
