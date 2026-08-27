# IIO Backend

Linux Industrial I/O subsystem backend for the Mainline Sensors HAL.

## Overview

Discovers and reads sensors from the Linux IIO subsystem at `/sys/bus/iio/devices/`.

## Discovery

Scans `/sys/bus/iio/devices/iio:device*` for devices with recognized sensor types.

## Data Access Modes

- **Poll mode**: Reads sysfs attributes (e.g., `in_accel_x_raw`) periodically
- **Buffer mode**: Reads from the IIO character device (`/dev/iio:deviceN`) ring buffer

Buffer mode is preferred when available. Poll mode is used as fallback.

## Mount Matrix

Supports automatic axis correction via mount matrix read from sysfs.
