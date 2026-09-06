# IIO Backend

Linux Industrial I/O subsystem backend for the Mainline Sensors HAL.

## Overview

Discovers and reads sensors from the Linux IIO subsystem at `/sys/bus/iio/devices/`.

## Sensor Type Detection

Sensor type is determined using a 5-layer fallback chain:

1. `name` attribute (e.g., `bmc150_accel`, `qcom-smgr-prox`)
2. `of_node/compatible` from device tree (e.g., `bosch,bmc150_magn`)
3. `of_node/name` from device tree (e.g., `magnetometer`)
4. Scan elements prefixes (e.g., `in_accel_x_en` → accelerometer)
5. Sysfs attribute filenames (e.g., presence of `in_illuminance_raw` → light sensor)

Common name patterns recognized: `accel`, `gyro`/`anglvel`, `magn`, `light`/`illuminance`/
`intensity`, `proximity`/`prox`, `temp`, `pressure`/`baro`, `humidity`.

## Data Access Modes

- **Buffer mode**: Reads from the IIO character device (`/dev/iio:deviceN`) ring buffer
  - **Triggered buffers**: Uses an hrtimer trigger for periodic sampling
  - **Push-based buffers**: Receives data via driver push callbacks (no trigger needed)
- **Poll mode**: Reads sysfs attributes (e.g., `in_accel_x_raw`) periodically

Buffer mode is preferred and used for all devices with scan_elements. Poll mode is only
used as fallback for devices that expose `_raw` attributes but lack scan_elements.
Buffer-only devices (those without `_raw` attributes) are fully supported.

## Sensor Info Derivation

Sensor metadata is derived from sysfs rather than hardcoded:

- `maxRange` and `resolution` are computed from `in_*_scale` and channel `realbits`
- `minDelayUs` and `maxDelayUs` are derived from `sampling_frequency_available`
- `flags` are determined by sensor type (continuous vs on-change)

Scale and offset attributes are looked up with fallback: first per-axis attributes
(e.g., `in_accel_x_scale`), then shared-by-type attributes (e.g., `in_accel_scale`).
Sampling frequency is similarly looked up at device level first, then per-channel.

## Channel Discovery and Buffer Layout

Channels are discovered from `scan_elements/` directory regardless of their enable state.
The `_type` and `_index` files are read to determine channel format and position.

For buffer mode, channel byte offsets are calculated sequentially with proper alignment
based on storage size. This handles mixed-size channels correctly (e.g., 32-bit data
channels followed by a 64-bit timestamp).

## Mount Matrix

Mount matrix is resolved using a priority chain (highest priority wins):

1. Android property override (`vendor.sensors.iio.<device_name>.mount_matrix`)
2. `60-sensor.hwdb` `ACCEL_MOUNT_MATRIX` (via `libsensors_hwdb`, matched by device modalias and label)
3. Sysfs attributes (`mount_matrix`, `in_accel_mount_matrix`, `in_*_mount_matrix`, `in_mount_matrix`)
4. Identity matrix (default)

The hwdb lookup uses the device's parent modalias (from `../modalias` sysfs) and optional
sensor label (from `label` sysfs) to match entries in `/vendor/etc/hwdb.d/60-sensor.hwdb`.

## Property Overrides

Hardware-specific properties that cannot be auto-detected can be overridden via android properties:

```
vendor.sensors.iio.<device_name>.vendor = Vendor Name
vendor.sensors.iio.<device_name>.power = 0.13
vendor.sensors.iio.<device_name>.max_range = 78.4
vendor.sensors.iio.<device_name>.resolution = 0.001
vendor.sensors.iio.<device_name>.mount_matrix = 1,0,0;0,-1,0;0,0,1
```

Where `<device_name>` is the IIO device `name` attribute with `-`, ` `, `/` replaced by `_`.

## Dependencies

- `libsensors_hwdb` - Sensor hwdb utility library (provides mount matrix from `60-sensor.hwdb`)
- `libhwdb` - Generic hwdb file parser
- `libsmbios_parser` - SMBIOS parser (fallback for DMI modalias construction)
