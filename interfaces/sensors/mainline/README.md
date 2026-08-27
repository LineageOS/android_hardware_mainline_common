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

Mount matrix correction is supported via sysfs attributes:
1. `mount_matrix`
2. `in_accel_mount_matrix`
3. `in_mount_matrix`

## Input Backend

The Input backend discovers sensors from `/dev/input/event*` devices.

Supported sensor types:
- Accelerometer (via ABS_X/ABS_Y/ABS_Z axes)
- Proximity (via SW_FRONT_PROXIMITY switch)

## Mock Backend

The Mock backend provides fake sensor data for all supported types. It is loaded last by default and should only be used for testing or as a fallback.

## Project Treble Compliance

This HAL is vendor-side, packaged in a vendor APEX, and communicates with the framework via the stable AIDL interface.

## License

Apache License 2.0 (SPDX-License-Identifier: Apache-2.0)
