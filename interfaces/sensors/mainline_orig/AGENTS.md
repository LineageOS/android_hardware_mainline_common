# AGENTS.md - Mainline Sensors HAL

## Project Overview

This is the Android Sensors HAL (`android.hardware.sensors`) implementation for
devices running mainline Linux kernel, following AIDL interface version 3.

## Architecture

- **Frontend** (this directory root): Implements `BnSensors` AIDL interface, loads backends via `dlopen()`, routes AIDL calls to correct backend, handles FMQ event posting and wake locks. Also manages composite (virtual/derived) sensors.
- **Composite Sensors** (this directory root): Virtual sensors that derive data from hardware sensor events. Registered in the frontend via `RegisterCompositeSensor()`.
- **Backends** (in `backends/` directory): Each backend is a shared library (`libsensors_*.so`) loaded dynamically.
  - `iio/`: Linux IIO subsystem backend (`libsensors_iio.so`)
  - `input/`: Linux input subsystem backend (`libsensors_input.so`)
  - `mock/`: Mock backend with fake data (`libsensors_mock.so`)
- **Utility libraries** (in `utils/` directory): Internal static libraries used by backends.
  - `hwdb/`: Sensor-specific hwdb utility library (`libsensors_hwdb`), uses `libhwdb` and `libsmbios_parser`

### External Libraries

- **`libhwdb`** (`hardware/mainline/common/libraries/libhwdb`): Generic hwdb text file parser (agnostic to sensors). Parses `.hwdb` files and supports fnmatch-based property lookups.
- **`libsmbios_parser`** (`hardware/mainline/common/libraries/smbios-parser`): SMBIOS table parser, used as fallback for DMI modalias construction when `/sys/class/dmi/id/modalias` is unavailable.

## Key Files

- `main.cpp` - Service entry point
- `Sensors.h/cpp` - AIDL `BnSensors` implementation
- `SensorBackendManager.h/cpp` - Backend loading, routing, and composite sensor management
- `CompositeSensor.h` - `ICompositeSensor` interface for virtual/derived sensors
- `DeviceOrientationSensor.h/cpp` - Device orientation derived from accelerometer
- `include/libsensors_mainline/SensorBackend.h` - Shared backend interface
- `backends/iio/IioBackend.h/cpp` - IIO backend implementation
- `backends/input/InputBackend.h/cpp` - Input backend implementation
- `backends/mock/MockBackend.h/cpp` - Mock backend implementation
- `utils/hwdb/include/SensorHwdb.h` - Sensor hwdb utility interface
- `utils/hwdb/SensorHwdb.cpp` - Sensor hwdb utility implementation

## Build

This project uses the Android build system (Soong/Blueprint). Build modules:

- `android.hardware.sensors-service.mainline` - Service binary
- `libsensors_mainline_frontend` - Frontend static library
- `libsensors_mainline_headers` - Header-only library for backend interface
- `libsensors_iio` - IIO backend shared library
- `libsensors_input` - Input backend shared library
- `libsensors_mock` - Mock backend shared library
- `libsensors_hwdb` - Sensor hwdb utility static library (in `utils/hwdb/`)
- `libhwdb` - Generic hwdb parser static library (in `hardware/mainline/common/libraries/libhwdb/`)
- `libsmbios_parser` - SMBIOS parser static library (in `hardware/mainline/common/libraries/smbios-parser/`)
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
- Backend list override property: `vendor.sensors.backends` (short names like `libssc,iio`)
- Default backend load order: IIO, Input, Mock
- Sensor hwdb file path: `/{odm,vendor}/etc/hwdb.d/60-sensor.hwdb`

### Sensor Hardware Database (hwdb)

The IIO backend reads sensor properties from the systemd-compatible `60-sensor.hwdb` file.
This provides device-specific calibration data maintained by the Linux community.

The hwdb lookup uses the device's modalias (from `../modalias` sysfs) and optional label
(from `label` sysfs) to match against entries in the hwdb file. The DMI modalias is read
from `/sys/class/dmi/id/modalias` with a fallback to SMBIOS tables via `libsmbios_parser`.

Properties provided by hwdb:
- `ACCEL_MOUNT_MATRIX` - 3x3 mount matrix (overrides sysfs-provided matrix)
- `PROXIMITY_NEAR_LEVEL` - Proximity sensor near threshold

Mount matrix priority chain (highest wins):
1. Android property override (`vendor.sensors.iio.<device_name>.mount_matrix`)
2. hwdb `ACCEL_MOUNT_MATRIX`
3. sysfs `mount_matrix` / `in_*_mount_matrix`
4. Identity matrix (default)

### IIO Backend Sensor Info Overrides

The IIO backend derives `maxRange`, `resolution`, `minDelayUs`, `maxDelayUs` from sysfs
(`in_*_scale`, `realbits`, `sampling_frequency_available`). Values that cannot be auto-determined
(`power`, `vendor`, or overrides for the derived values) are read from android properties:

- `vendor.sensors.iio.<device_name>.vendor` - Override vendor name
- `vendor.sensors.iio.<device_name>.power` - Override power (mA)
- `vendor.sensors.iio.<device_name>.max_range` - Override maxRange
- `vendor.sensors.iio.<device_name>.resolution` - Override resolution
- `vendor.sensors.iio.<device_name>.mount_matrix` - Override mount matrix (format: `r1c1,r1c2,r1c3;r2c1,r2c2,r2c3;r3c1,r3c2,r3c3`)

Where `<device_name>` is the IIO device `name` attribute with `-`, ` `, `/` replaced by `_`.

The `mount_matrix` property overrides the mount matrix read from sysfs. This is useful when the kernel-provided mount matrix is incorrect or missing, allowing proper axis transformation for sensor data.

### Input Backend Sensor Info Overrides

The input backend reads accelerometer data from `/dev/input/event*` devices. Unlike IIO, the input subsystem has no standard for accelerometer units - each kernel driver reports raw counts with different bit resolutions and g-ranges.

The accelerometer scale factor must be set per-device via android properties:

- `vendor.sensors.input.<device_name>.accel_scale` - Scale factor to convert raw counts to m/s²

Common scale factors:
- MMA8450 (12-bit ±2g): `0.00958` (9.81 / 1024.0)
- BMA150 (10-bit ±2g): `0.0383` (9.81 / 256.0, default)
- KXTJ9 (12-bit ±2g): `0.0383` (9.81 / 256.0)

Where `<device_name>` is the input device name with `-`, ` `, `/` replaced by `_`.

**Note**: Consider using the IIO backend instead, which provides standardized units via sysfs scale attributes and doesn't require manual configuration.

### Composite Sensors

Composite sensors are virtual sensors implemented in the frontend. They derive data from
hardware sensor events. To add a new composite sensor:

1. Implement `ICompositeSensor` interface (see `CompositeSensor.h`)
2. Register it in `Sensors::Sensors()` constructor via `backend_manager_.RegisterCompositeSensor()`
3. Add the source file to `Android.bp` under `libsensors_mainline_frontend`

The `SensorBackendManager` handles:
- Assigning global handles to composite sensors
- Forwarding relevant hardware sensor events to active composite sensors
- Auto-activating hardware sensor dependencies when a composite sensor is activated
- Auto-deactivating hardware dependencies when no composite sensor needs them
- Including composite sensors in `GetSensorsList()`

Current composite sensors:
- `DeviceOrientationSensor` - Reports rotation index (0-3) from accelerometer data

### DeviceOrientationSensor Workaround Properties

For devices with incorrect accelerometer mount matrix causing wrong screen rotation.
These properties are read each time the sensor is activated:

- `vendor.sensors.orientation.swap_xy` - Swap X and Y axes before processing
- `vendor.sensors.orientation.invert_x` - Negate X axis before processing
- `vendor.sensors.orientation.invert_y` - Negate Y axis before processing
- `vendor.sensors.orientation.invert_z` - Negate Z axis before processing
- `vendor.sensors.orientation.rotation_offset` - Rotate output by 90/180/270 degrees

Example: if screen rotates 180° from expected:
```
setprop vendor.sensors.orientation.rotation_offset 180
```

Example: if X axis is inverted:
```
setprop vendor.sensors.orientation.invert_x true
```

The effective mount matrix (after applying transformations) is logged on activation.

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
