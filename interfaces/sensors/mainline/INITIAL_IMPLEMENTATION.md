# Details about making the initial implementation of Android Sensors HAL for mainline kernel

We want a flexible and generic Android Sensors HAL,
for the usage on Android devices running mainline Linux kernel,
which follows proper Linux standards.

You (AI Coding Agent) act as a professional Android HAL engineer
and you've got to implement this.

## Requirements

- The HAL shall be named `mainline`.
  - For the executable name, you can append `.mainline`.
  - Init rc service name shall be `vendor.sensors-mainline`.
  - The APEX module name shall be `com.android.hardware.sensors.mainline`.
  - Filename for the executable shall be `android.hardware.sensors-service.mainline`.
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
- Write `AGENTS.md` to help the future AI sessions to understand the project.
- Write `README.md`s to provide useful informations to human developers.
- Try to use `libbase` from `system/libbase` for Android platform helper functions.
- Android properties defined in this HAL shall have `vendor.sensors.` prefix.

## References

The paths mentioned in this section are relative to AOSP source tree root.

### Build system

- **APEX build handling**: In `build/soong/apex/`, mainly on `apex.go` and `apex_test.go` files.
- **C/C++ build handling**: In `build/soong/cc/`, mainly on `cc.go` and `cc_test.go` files.

### AIDL HAL interface definition

In `hardware/interfaces/sensors/aidl/`.

### HAL implementations

- **Example AIDL Sensors HAL**: `hardware/interfaces/sensors/aidl/default`.
- **IIO Sensors HAL built on Legacy HAL interface**: `hardware/intel/sensors-iio`.

### Linux kernel

There is a reference Linux kernel located at `kernel/virt/virtio`.

### Miscellaneous

- **libbase headers**: `system/libbase/include/android-base`.
- **iio-sensor-proxy**: `external/mainline-hw-deps/iio-sensor-proxy`.

## Guidelines

- Add enough log prints for debugging.
- Do NOT browse anywhere outside of AOSP source tree for reference.
- Do NOT try to look for other HALs which we did not mention for reference.
- Do NOT try to compile and verify by yourself; The user will do so, and report issues to you if exists.
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
- The frontend can contain a known backend list, plus reading backend list override from a property.

### Directory structure

The root directory of the HAL implementation should be
`hardware/mainline/common/interfaces/sensors/mainline`
relative to AOSP root directory.

The frontend shall be at root of the HAL directory.

The backends shall be in its own subdir in `backends` directory
in root of the HAL directory. For example: `backends/iio/`.

Android.bp build rules for each module shall stay in the directory
same as where the module is located at.
