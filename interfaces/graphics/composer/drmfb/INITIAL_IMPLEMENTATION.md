# Details about making the initial implementation of Android DRM Framebuffer Graphics Composer AIDL HAL

We already have a Android DRM Framebuffer Graphics Composer HIDL HAL,
in `../drmfb-hidl` relative to the directory which this markdown file is in,
named `drmfb-composer`.

However, the HIDL interface is going to be deprecated in really soon,
and the original HAL is unmaintained, also having some known issues.

Therefore, we want a fresh reimplementation of the `drmfb-composer` HAL,
which strictly follows the core principles of the original HAL,
but using latest AIDL interface and latest Linux DRM APIs.

You (AI Coding Agent) act as a professional Android HAL engineer
and you've got to implement this in the directory containing this markdown file.

You shall ignore the `sepolicy` directory of the original HAL.

## Requirements

- The HAL shall comply with Project Treble rules.
- The HAL shall use latest AIDL interface.
- Implement the HAL in C++ language.
- Strictly follow Google C++ Style Guide.
- Try to use C++ functions instead of C functions as much as possible, but you MUST not use `try...catch` approach.
- Write `AGENTS.md` to help the future AI sessions to understand the project.
- Write `README.md`s to provide useful informations to human developers.
- Try to use `libbase` from `system/libbase` for Android platform helper functions.
- Match with the expectations of the Vendor Test Suite (VTS) module.

## References

The paths mentioned in this section are relative to AOSP source tree root.

### Build system

- **C/C++ build handling**: In `build/soong/cc/`, mainly on `cc.go` and `cc_test.go` files.

### AIDL HAL interface definition

In `hardware/interfaces/graphics/composer/aidl/`.
VTS module is in `vts` subdir there.

### HIDL HAL interface definition

In subdirectories of `hardware/interfaces/graphics/composer/`.

### Linux kernel

There is a reference Linux kernel located at `kernel/virt/virtio`. Check it out for more detailed kernel sided implementations.

Note that the path is actually a symlink, using the path without `/` at the end may not work.

You can only take this kernel as reference, not any other kernels.

### Libraries

- `android.hardware.graphics.composer@2.1-resources` and its dependencies: `hardware/interfaces/graphics/composer/2.1/utils`.
- **libbase headers**: `system/libbase/include/android-base`.
- **libdisplay-info**: `external/libdisplay-info-upstream`.
- **libdrm**: `external/libdrm`.
- **libfmq**: `system/libfmq`.
- **AIDL HAL Common libraries**: `hardware/interfaces/common`. There contains `support/include/aidlcommonsupport/NativeHandle.h`.

### Gralloc HAL that is usually paired with drmfb-composer HAL

In `external/minigbm-upstream`.

### drm_hwcomposer HAL

In `external/drm_hwcomposer-upstream`.

## Guidelines

- Add enough log prints for debugging.
- Do NOT browse anywhere outside of AOSP source tree for reference.
- Do NOT try to search broadly in the root of AOSP source tree.
- Do NOT try to look for other HALs which we did not mention for reference.
- Do NOT try to compile and verify by yourself; The user will do so, and report issues to you if exists.
- Do NOT blindly set hardware-specific properties.
- Please firstly understand the AIDL interface, and then understand the latest Linux DRM APIs.
- When you are very unsure about a specific thing, ask the user before proceed.
