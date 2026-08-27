# AGENTS.md - Mainline Sensors HAL

## Project Overview

This is the Android Sensors HAL (`android.hardware.sensors`) implementation for
devices running mainline Linux kernel, following AIDL interface version 3.

## Architecture

- **Frontend** (this directory root): Implements `BnSensors` AIDL interface, loads backends via `dlopen()`, routes AIDL calls to correct backend, handles FMQ event posting and wake locks.
- **Backends** (in `backends/` directory): Each backend is a shared library (`libsensors_*.so`) loaded dynamically.
  - `iio/`: Linux IIO subsystem backend (`libsensors_iio.so`)
  - `input/`: Linux input subsystem backend (`libsensors_input.so`)
  - `mock/`: Mock backend with fake data (`libsensors_mock.so`)

## Key Files

- `main.cpp` - Service entry point
- `Sensors.h/cpp` - AIDL `BnSensors` implementation
- `SensorBackendManager.h/cpp` - Backend loading and routing via `dlopen()`
- `include/libsensors_mainline/SensorBackend.h` - Shared backend interface
- `backends/iio/IioBackend.h/cpp` - IIO backend implementation
- `backends/input/InputBackend.h/cpp` - Input backend implementation
- `backends/mock/MockBackend.h/cpp` - Mock backend implementation

## Build

This project uses the Android build system (Soong/Blueprint). Build modules:

- `android.hardware.sensors-service.mainline` - Service binary
- `libsensors_mainline_frontend` - Frontend static library
- `libsensors_mainline_headers` - Header-only library for backend interface
- `libsensors_iio` - IIO backend shared library
- `libsensors_input` - Input backend shared library
- `libsensors_mock` - Mock backend shared library
- `com.android.hardware.sensors.mainline` - APEX module

## Naming Conventions

- Init RC service: `vendor.sensors-mainline`
- APEX module: `com.android.hardware.sensors.mainline`
- Binary: `android.hardware.sensors-service.mainline`
- APEX manifest name: `com.android.hardware.sensors` (unchanged for Multi-install APEX)
- Android properties: `vendor.sensors.*` prefix
- Library names: `libsensors_*` prefix

## Configuration

- Backend library search paths: `/odm/lib64/hw/`, `/vendor/lib64/hw/`
- Config directory on device: `/{odm,vendor}/etc/sensors/`
- Backend list override property: `vendor.sensors.backends`
- Default backend load order: IIO, Input, Mock

### IIO Backend Sensor Info Overrides

The IIO backend derives `maxRange`, `resolution`, `minDelayUs`, `maxDelayUs` from sysfs
(`in_*_scale`, `realbits`, `sampling_frequency_available`). Values that cannot be auto-determined
(`power`, `vendor`, or overrides for the derived values) are read from android properties:

- `vendor.sensors.iio.<device_name>.vendor` - Override vendor name
- `vendor.sensors.iio.<device_name>.power` - Override power (mA)
- `vendor.sensors.iio.<device_name>.max_range` - Override maxRange
- `vendor.sensors.iio.<device_name>.resolution` - Override resolution

Where `<device_name>` is the IIO device `name` attribute with `-`, ` `, `/` replaced by `_`.

## Code Style

Follow Google C++ Style Guide. Member variables use `snake_case_` with trailing underscore.
Function and class names use `CamelCase`.

## Important Notes

- Do NOT rename the HAL interface (keep `ISensors/default`)
- The APEX manifest name must remain `com.android.hardware.sensors` for Multi-install support
- Backend shared libraries must export `extern "C" ISensorBackend* CreateSensorBackend()`
- The frontend has no special behavior for specific backends
- Mock backend is least preferred (loaded last by default)
- Do NOT try to compile; the user will compile and report issues
- Do NOT browse outside the AOSP source tree for reference
