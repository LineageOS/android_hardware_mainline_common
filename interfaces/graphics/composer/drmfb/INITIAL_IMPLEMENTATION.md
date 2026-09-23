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

> See the repository root `docs/INITIAL_IMPLEMENTATION_GUIDELINES.md`
> (`hardware/mainline/common/docs/INITIAL_IMPLEMENTATION_GUIDELINES.md`) for
> requirements, references, and guidelines shared by every component. This
> file only lists what's specific to this HAL. Note this HAL is not an APEX
> module and has no `mainline`-branded naming requirements, unlike most other
> components covered by that document.

## References

The paths mentioned in this section are relative to AOSP source tree root.

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
- **libdisplay-info**: `external/libdisplay-info-upstream`.
- **libdrm**: `external/libdrm`.
- **libfmq**: `system/libfmq`.
- **AIDL HAL Common libraries**: `hardware/interfaces/common`. There contains `support/include/aidlcommonsupport/NativeHandle.h`.

### Gralloc HAL that is usually paired with drmfb-composer HAL

In `external/minigbm-upstream`.

### drm_hwcomposer HAL

In `external/drm_hwcomposer-upstream`.

## Guidelines

- Please firstly understand the AIDL interface, and then understand the latest Linux DRM APIs.
