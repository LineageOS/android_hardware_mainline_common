# Details about making the initial implementation of Android Audio HAL for mainline kernel

We want a flexible and generic Android Audio HAL,
based on alsa-lib and alsa-ucm-conf,
for the usage on Android devices running mainline Linux kernel,
which follows proper Linux standards.

You (AI Coding Agent) act as a professional Android HAL engineer
and you've got to implement this in the directory containing this markdown file.

## Requirements

- The HAL shall be named `mainline`.
  - Init rc service name shall be `vendor.audio-hal-aidl-mainline`.
  - The APEX module name shall be `com.android.hardware.audio.mainline`.
  - Filename for the executable shall be `android.hardware.audio.service-aidl.mainline`.
  - The name on the APEX manifest shall NOT be touched, for Multi-install APEX support.
  - Init rc and vintf fragment shall be renamed accordingly.
  - Do NOT rename the HAL interface.
- The HAL shall comply with Project Treble rules.
- The HAL shall use latest AIDL interface.
- The entire HAL shall support living inside an APEX.
- Implement the HAL in C++ language.
- Strictly follow Google C++ Style Guide.
- Try to use C++ functions instead of C functions as much as possible, but you MUST not use `try...catch` approach.
- Write `AGENTS.md` to help the future AI sessions to understand the project.
- Write `README.md`s to provide useful informations to human developers.
- Try to use `libbase` from `system/libbase` for Android platform helper functions.
- Android properties defined in this HAL shall have `vendor.audio.` prefix.
- Match with the expectations of the Vendor Test Suite (VTS) module.
- Run clang-format (available at `/android/LineageOS/24/prebuilts/clang/host/linux-x86/clang-r584948b/bin/clang-format`) on the written source code files.
- Make git commit with detailed git commit message for every changes.
- Git commit messages shall have proper `Assisted-by: <Agent>/<Model ID>` attribute at the end.

## References

The paths mentioned in this section are relative to AOSP source tree root.

### Build system

- **APEX build handling**: In `build/soong/apex/`, mainly on `apex.go` and `apex_test.go` files.
- **C/C++ build handling**: In `build/soong/cc/`, mainly on `cc.go` and `cc_test.go` files.

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

### Miscellaneous

- **libbase headers**: `system/libbase/include/android-base`.

## Guidelines

- Add enough log prints for debugging.
- Do NOT browse anywhere outside of AOSP source tree for reference.
- Do NOT try to search broadly in the root of AOSP source tree.
- Do NOT try to look for other HALs which we did not mention for reference.
- Do NOT try to compile and verify by yourself; The user will do so, and report issues to you if exists.
- Do NOT blindly set hardware-specific properties.
- Please firstly understand the AIDL interface, and then everything else.
- When you are very unsure about a specific thing, ask the user before proceed.
- As this would be a big project, please maintain some readability for human developers.
- Avoid making one single source file too large; Split into separate files if needed.

### Copyright header

Use this on Android.bp files:

```
//
// SPDX-FileCopyrightText: The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
//
```

Use this on source code files:

```
/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
```

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
