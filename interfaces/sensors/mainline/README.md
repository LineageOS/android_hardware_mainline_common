# Mainline Sensors HAL

An Android Sensors HAL implementation for devices running mainline Linux kernel.

Implements the `android.hardware.sensors` AIDL interface (version 3).

## Architecture

The HAL uses a frontend/backend architecture:

- **Frontend**: Registers with Android as `ISensors/default`, loads backends dynamically via `dlopen()`, and routes AIDL calls to the appropriate backend.
- **Backends**: Modular shared libraries, each bridging the frontend to a specific Linux subsystem for sensor access.

### Backends

| Backend | Library | Description |
|---|---|---|
| IIO | `libsensors_iio.so` | Linux IIO subsystem (`/sys/bus/iio/devices/`) |
| Input | `libsensors_input.so` | Linux input subsystem (`/dev/input/event*`) |
| Mock | `libsensors_mock.so` | Fake sensor data for testing |

Backends are loaded in order: IIO, Input, Mock (mock is least preferred).

Multiple backends can be active simultaneously, with each providing sensors to the complete set exposed to Android.

### Composite Sensors

Composite sensors are virtual sensors that derive their data from hardware sensor events.
They are implemented in the frontend and managed by `SensorBackendManager`.

When a composite sensor is activated, its hardware dependencies (e.g., accelerometer) are
automatically activated. When all composite sensors depending on a hardware sensor are
deactivated, the hardware sensor is automatically deactivated.

Current composite sensors:
- **Device Orientation** (`DEVICE_ORIENTATION`) - Reports rotation index (0-3) from accelerometer data. Uses hysteresis to prevent flapping at orientation boundaries.

### Device Orientation Workaround

For devices where the accelerometer mount matrix is incorrect and causes wrong
screen rotation, the following properties can be set as a temporary workaround.
They are read each time the orientation sensor is activated.

Axis transformation (applied before orientation computation):
```
vendor.sensors.orientation.swap_xy = true|false
vendor.sensors.orientation.invert_x = true|false
vendor.sensors.orientation.invert_y = true|false
vendor.sensors.orientation.invert_z = true|false
```

Rotation offset (applied after orientation computation, value: 0/90/180/270):
```
vendor.sensors.orientation.rotation_offset = 0
```

Example: if screen rotates 180° from expected:
```
setprop vendor.sensors.orientation.rotation_offset 180
```

Example: if X axis is inverted due to wrong mount matrix:
```
setprop vendor.sensors.orientation.invert_x true
```

The effective mount matrix is logged on sensor activation for debugging.

To add a new composite sensor, implement the `ICompositeSensor` interface and register it
in `Sensors::Sensors()`.

## Configuration

### Backend Override

Set `vendor.sensors.backends` property to a comma/space-separated list of backend library names:

```
setprop vendor.sensors.backends "libsensors_iio.so,libsensors_mock.so"
```

### Library Search Paths

Backend libraries are searched in:
- `/odm/lib64/hw/` (or `/odm/lib/hw/` on 32-bit)
- `/vendor/lib64/hw/` (or `/vendor/lib/hw/` on 32-bit)
- Default system library paths

### Sensor Configuration

Device-specific sensor configuration files can be placed in:
- `/odm/etc/sensors/`
- `/vendor/etc/sensors/`

### Sensor Hardware Database (hwdb)

The IIO backend reads sensor properties from the systemd-compatible `60-sensor.hwdb` file
at `/vendor/etc/hwdb.d/60-sensor.hwdb`. This provides device-specific calibration data
(e.g., accelerometer mount matrices) maintained by the Linux community.

The hwdb lookup uses the device's modalias and optional label to match entries.
The DMI modalias is read from `/sys/class/dmi/id/modalias` with a fallback to SMBIOS tables.

Mount matrix priority chain (highest wins):
1. Android property override (`vendor.sensors.iio.<device_name>.mount_matrix`)
2. hwdb `ACCEL_MOUNT_MATRIX` from `60-sensor.hwdb`
3. sysfs `mount_matrix` / `in_*_mount_matrix`
4. Identity matrix (default)

Supported hwdb properties:
- `ACCEL_MOUNT_MATRIX` - 3x3 axis correction matrix for accelerometers
- `PROXIMITY_NEAR_LEVEL` - Proximity sensor near threshold value

## Building

Include in your device's build by adding to the device makefile:

```makefile
PRODUCT_PACKAGES += \
    com.android.hardware.sensors.mainline
```

## IIO Backend

The IIO backend discovers sensors from `/sys/bus/iio/devices/iio:device*`.

Supported sensor types:
- Accelerometer (IIO_ACCEL)
- Gyroscope (IIO_ANGL_VEL)
- Magnetometer (IIO_MAGN)
- Ambient Light (IIO_LIGHT/IIO_INTENSITY)
- Proximity (IIO_PROXIMITY)
- Temperature (IIO_TEMP)
- Pressure (IIO_PRESSURE)
- Relative Humidity (IIO_HUMIDITYRELATIVE)

Mount matrix correction is supported via multiple sources (priority order):
1. Android property override (`vendor.sensors.iio.<device_name>.mount_matrix`)
2. `60-sensor.hwdb` `ACCEL_MOUNT_MATRIX` (matched by device modalias and label)
3. sysfs attributes (`mount_matrix`, `in_accel_mount_matrix`, `in_mount_matrix`)
4. Identity matrix (default)

### Sensor Type Detection

Sensor type is determined using a fallback chain:
1. IIO device `name` attribute
2. Device tree `of_node/compatible`
3. Device tree `of_node/name`
4. Scan elements channel prefixes
5. Sysfs attribute filenames

### Sensor Info Overrides

Sensor metadata (`maxRange`, `resolution`, `minDelayUs`, `maxDelayUs`) is derived from
sysfs (`in_*_scale`, channel `realbits`, `sampling_frequency_available`).

Hardware-specific overrides can be set via android properties:

```
vendor.sensors.iio.<device_name>.vendor = Vendor Name
vendor.sensors.iio.<device_name>.power = 0.13
vendor.sensors.iio.<device_name>.max_range = 78.4
vendor.sensors.iio.<device_name>.resolution = 0.001
vendor.sensors.iio.<device_name>.mount_matrix = 1,0,0;0,-1,0;0,0,1
```

Where `<device_name>` is the IIO device `name` with `-`, ` `, `/` replaced by `_`.

The `mount_matrix` property overrides the mount matrix read from sysfs. Format is a 3x3 matrix with rows separated by semicolons and columns by commas. This is useful when the kernel-provided mount matrix is incorrect or missing.

## Input Backend

The Input backend discovers sensors from `/dev/input/event*` devices.

Supported sensor types:
- Accelerometer (via ABS_X/ABS_Y/ABS_Z axes)
- Proximity (via SW_FRONT_PROXIMITY switch)

### Accelerometer Configuration

Unlike IIO, the input subsystem has no standard for accelerometer units. Each kernel driver reports raw counts with different bit resolutions and g-ranges. You must configure the scale factor per-device:

```
vendor.sensors.input.<device_name>.accel_scale = <scale_factor>
```

Where `<device_name>` is the input device name with `-`, ` `, `/` replaced by `_`.

Common scale factors:
- MMA8450 (12-bit ±2g): `0.00958` (9.81 / 1024.0)
- BMA150 (10-bit ±2g): `0.0383` (9.81 / 256.0, default)
- KXTJ9 (12-bit ±2g): `0.0383` (9.81 / 256.0)

**Recommendation**: Use the IIO backend instead when possible, as it provides standardized units via sysfs scale attributes and doesn't require manual configuration.

## Mock Backend

The Mock backend provides fake sensor data for all supported types. It is loaded last by default and should only be used for testing or as a fallback.

## Project Treble Compliance

This HAL is vendor-side, packaged in a vendor APEX, and communicates with the framework via the stable AIDL interface.

## License

Apache License 2.0 (SPDX-License-Identifier: Apache-2.0)
