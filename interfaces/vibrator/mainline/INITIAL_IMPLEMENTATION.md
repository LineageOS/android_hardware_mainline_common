# Details about making the initial implementation of Android Vibrator HAL for mainline kernel

We want a flexible and generic Android Vibrator HAL,
for the usage on Android devices running mainline Linux kernel,
which follows proper Linux standards.

You (AI Coding Agent) act as a professional Android HAL engineer
and you've got to implement this in the directory containing this markdown file.

## Requirements

- The HAL shall be named `mainline`.
  - Init rc service name shall be `vendor.vibrator-mainline`.
  - The APEX module name shall be `com.android.hardware.vibrator.mainline`.
  - Filename for the executable shall be `android.hardware.vibrator-service.mainline`.
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
- Android properties defined in this HAL shall have `vendor.vibrator.` prefix.
- Match with the expectations of the Vendor Test Suite (VTS) module.

## References

The paths mentioned in this section are relative to AOSP source tree root.

### Build system

- **APEX build handling**: In `build/soong/apex/`, mainly on `apex.go` and `apex_test.go` files.
- **C/C++ build handling**: In `build/soong/cc/`, mainly on `cc.go` and `cc_test.go` files.

### AIDL HAL interface definition

In `hardware/interfaces/vibrator/aidl/`.
VTS module is in `vts` subdir there.

### HAL implementations

- **Example AIDL Vibrator HAL**: `hardware/interfaces/vibrator/aidl/default`. Check it out for standard AIDL HAL example implementation.
- **Qualcomm downstream vibrator HAL**: `vendor/qcom/opensource/vibrator`. Check it out for haptic effects related implementations.

### Linux kernel

There is a reference Linux kernel located at `kernel/virt/virtio`. Check it out for more detailed kernel sided implementations.

Additionally, there is `qcom-spmi-haptics.c` driver in `kernel/mainline/msm8953-mainline/drivers/input/misc/`.

### Miscellaneous

- **libbase headers**: `system/libbase/include/android-base`.

## Guidelines

- Add enough log prints for debugging.
- Do NOT browse anywhere outside of AOSP source tree for reference.
- Do not try to search broadly in the root of AOSP source tree.
- Do NOT try to look for other HALs which we did not mention for reference.
- Do NOT try to compile and verify by yourself; The user will do so, and report issues to you if exists.
- Do NOT blindly set hardware-specific properties.
- When you are very unsure about a specific thing, ask the user before proceed.

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

The HAL shall drive haptic controllers or vibrators exposed via Linux Input force-feedback API.

Try to support as many as possible of such hardwares, with drivers in `drivers/input/misc/` in the reference Linux kernels.
You can run `grep EV_FF *.c` in there to look for these drivers.

While trying to support a wide range of hardware, the HAL itself shall still remain generic.

Try to support as many as possible related APIs and features in both Android HAL interface side and Linux driver side.

Ideally, it shall be configuration-less for every of such hardwares, properties shall be automatically detected at runtime.

If defining hardware-specific properties can't be avoided, then let's try to read these properties from Android properties.
Devices using this HAL shall set these properties properly by themselves.

Focus on supporting these drivers in `drivers/input/misc/` in these reference Linux kernels, while still trying to support the other relevant drivers too:
- `gpio-vibra.c`
- `pm8xxx-vibrator.c`
- `pwm-vibra.c`
- `qcom-spmi-haptics.c`
- `regulator-haptic.c`

### Basic haptic effects

Even for basic vibrator hardwares, we shall still try to support basic haptic effects in some ways, but ONLY if it will actually make different vibration.

For example, for vibrator hardwares that supports setting voltage, we can try to do it by manipulating the voltage.

### Haptic effect patterns

IMPORTANT NOTE: Implement this ONLY if this is actually relevant and feasible. Do NOT implement otherwise.

Try to make use of the libraries in `vendor/qcom/opensource/vibrator/effect`.

We shall load these libraries using `dlopen()` with only filename, not including library search paths.

Note that devices may provide theirs own replacement of these libraries.
