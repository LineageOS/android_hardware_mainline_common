# Details about making the initial implementation of Android Sensors HAL for mainline kernel

We want a flexible and generic Android Sensors HAL,
for the usage on Android devices running mainline Linux kernel,
which follows proper Linux standards.

You (AI Coding Agent) act as a professional Android HAL engineer
and you've got to implement this.

## Requirements

- The HAL shall be named `mainline`.
  - Init rc service name shall be `vendor.sensors-mainline`.
  - The APEX module name shall be `com.android.hardware.sensors.mainline`.
  - Filename for the executable shall be `android.hardware.sensors-service.mainline`.
  - `LOG_TAG` define on top of our source files shall begin with `MainlineSensors`.
  - The name on the APEX manifest shall NOT be touched, for Multi-install APEX support.
  - Init rc and vintf fragment shall be renamed accordingly.
  - Do NOT rename the HAL interface.
- Libraries name shall begin with `libsensors_`.
- Configuration files directory on the target device shall be either of `/{odm,vendor}/etc/sensors`.
- The HAL shall comply with Project Treble rules.
- The HAL shall use latest AIDL interface.
- The entire HAL shall support living inside an APEX.
- Implement the HAL in C++ language.
- Strictly follow Google C++ Style Guide.
- Try to use C++ functions instead of C functions as much as possible, but you MUST not use `try...catch` approach.
- Write `AGENTS.md` to help the future AI sessions to understand the project.
- Write `README.md`s to provide useful informations to human developers.
- Try to use `libbase` from `system/libbase` for Android platform helper functions.
- Android properties defined in this HAL shall have `vendor.sensors.` prefix.
- Match with the expectations of the Vendor Test Suite (VTS) module.
- Run clang-format (available at `/android/LineageOS/24/prebuilts/clang/host/linux-x86/clang-r584948b/bin/clang-format`) on the written source code files.
- Make git commit with detailed git commit message for every changes.
- Git commit messages shall begin with: `mainline/common: interfaces/sensors/mainline: `.
- Git commit messages shall have proper `Assisted-by: <Agent>/<Model ID>` attribute at the end.

## References

The paths mentioned in this section are relative to AOSP source tree root.

### Build system

- **APEX build handling**: In `build/soong/apex/`, mainly on `apex.go` and `apex_test.go` files.
- **C/C++ build handling**: In `build/soong/cc/`, mainly on `cc.go` and `cc_test.go` files.

### AIDL HAL interface definition

In `hardware/interfaces/sensors/aidl/`.
VTS module is in `vts` subdir there.

### HAL implementations

- **Example AIDL Sensors HAL**: `hardware/interfaces/sensors/aidl/default`. Check it out for standard example AIDL HAL implementation.
- **The previous faulty implementation of this HAL**: `../mainline_orig` relative to here, and `hardware/mainline/qcom/libraries/libsensors_libssc` for external libssc backend, and `external/mainline-hw-deps/*` for dependencies of the external libssc backend, and `hardware/mainline/common/libraries/` for its `libhwdb` and `smbios-parser` dependencies.

### Linux kernel

- Default: `kernel/virt/virtio`.
- For `qcom_smgr` IIO drivers: `kernel/mainline/msm89x7-mainline`.
- For other IIO drivers: `kernel/mainline/msm8953-mainline`.

Note that the kernel source root paths are actually symlinks.

### Miscellaneous

- **libbase headers**: `system/libbase/include/android-base`.
- **iio-sensor-proxy**: `external/mainline-hw-deps/iio-sensor-proxy`.

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

The HAL shall be made of these parts:
- One frontend
- Multiple backends

### Frontend

The frontend shall iterate through the available backends for available
sensors, and register to the HAL interface, using appropriate backends
to provide sensors data to Android System.

Multiple backends providing the complete sensors set shall be made possible.

Print basic sensor info with its backend name to log on sensor initialization,
no matter whether if it succeeds or fails.

### Backends

Backends shall be modular.

Each backends shall be a bridge between the frontend and
a subsystem providing access to the sensor hardware.

For the initial implementation, we want the these backends:
- Linux IIO subsystem backend (for sensor drivers in `drivers/iio/*/` in Linux kernel)
- Linux input subsystem backend (for sensor drivers in `drivers/input/misc/` in Linux kernel)
- Mock backend (providing fake sensor data, ensure it's the least preferred backend)

More backends will be added after the initial implementation is made.

Hardware-specific properties shall not be defined in the backends.
If these cannot be automatically determined at runtime, please
either read these from configuration files, or from android properties.

### The connection

- There shall be a generic interface between the frontend and the backends.
- The frontend shall load backends using `dlopen()`.
- The frontend shall NOT have special behaviors for specific backends.
- The frontend can contain a known backend list of internal backends, plus reading backend list override from a property.

### Directory structure

The root directory of the HAL implementation should be
`hardware/mainline/common/interfaces/sensors/mainline`
relative to AOSP root directory.

The frontend shall be at root of the HAL directory.

The backends shall be in its own subdir in `backends` directory
in root of the HAL directory. For example: `backends/iio/`.

Android.bp build rules for each module shall stay in the directory
same as where the module is located at.

### Key features to keep from the `mainline_orig` HAL

You still have to check for improvements while keeping these features.

- The entire build rules design
- Backend override from property
- Load external backends from `/vendor` partition (Requires APEX linkerconfig)
- Composite Sensors + Composite Device Orientation sensor
- Device Orientation Workarounds
- Sensor Hardware Database (hwdb)
- Remove trailing '\0' or '\x0a' when reading from sysfs
- Parse vendor string from `of_node/compatible`
- Parse sensor type from `of_node/name` as fallback
- IIO: Allow `CONFIG_IIO_CONFIGFS` to be disabled in kernel
- IIO: Try multiple types of triggers
- IIO: Poll mode fallback

### IIO drivers to focus on compatibility

You still have to keep the IIO backend generic while focusing on these drivers.

- All IIO sensors drivers used by `arch/arm64/boot/dts/qcom/msm8953-xiaomi-*.dts` in the `msm8953-mainline` kernel.
- All HID sensors drivers in IIO subsystem.
- `qcom_smgr` IIO drivers.
