# IIO Backend

Linux Industrial I/O subsystem backend for the Mainline Sensors HAL.

## Overview

Discovers and reads sensors from the Linux IIO subsystem at `/sys/bus/iio/devices/`.

## Sensor Type Detection

Sensor type is determined using a 4-layer fallback chain:

1. `name` attribute (e.g., `bmc150_accel`)
2. `of_node/compatible` from device tree (e.g., `bosch,bmc150_magn`)
3. `of_node/name` from device tree (e.g., `magnetometer`)
4. Scan elements prefixes (e.g., `in_accel_x_en` → accelerometer)
5. Sysfs attribute filenames (e.g., presence of `in_illuminance_raw` → light sensor)

## Data Access Modes

- **Poll mode**: Reads sysfs attributes (e.g., `in_accel_x_raw`) periodically
- **Buffer mode**: Reads from the IIO character device (`/dev/iio:deviceN`) ring buffer

Buffer mode is preferred when available. Poll mode is used as fallback.

## Sensor Info Derivation

Sensor metadata is derived from sysfs rather than hardcoded:

- `maxRange` and `resolution` are computed from `in_*_scale` and channel `realbits`
- `minDelayUs` and `maxDelayUs` are derived from `sampling_frequency_available`
- `flags` are determined by sensor type (continuous vs on-change)

## Property Overrides

Hardware-specific properties that cannot be auto-detected can be overridden via android properties:

```
vendor.sensors.iio.<device_name>.vendor = Vendor Name
vendor.sensors.iio.<device_name>.power = 0.13
vendor.sensors.iio.<device_name>.max_range = 78.4
vendor.sensors.iio.<device_name>.resolution = 0.001
```

Where `<device_name>` is the IIO device `name` attribute with `-`, ` `, `/` replaced by `_`.

## Mount Matrix

Supports automatic axis correction via mount matrix read from sysfs.
