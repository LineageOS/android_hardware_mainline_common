# IIO Backend

Linux Industrial I/O backend for the Mainline Sensors HAL. Devices are discovered in
`/sys/bus/iio/devices` in numeric device order. Sensor types are collected from scan-channel and
sysfs attribute names, with IIO `name`, device-tree `compatible`, and device-tree `name` as
fallbacks. One physical IIO device may therefore publish multiple Android sensors.

## Data Paths

Buffered devices have one character-device fd and one reader thread per physical IIO device. Only
channels needed by active sensor types plus timestamp are enabled. The resulting `_en` values are
read back before offsets and stride are calculated. Layout follows `iio_compute_scan_bytes()`:
data-channel scan-index order, timestamp last, alignment to the complete storage width including
`Xrepeat`, and final alignment to the largest element. Scan types support little/big endian,
signed/unsigned values, real bits, storage bits through 64, repeat, and shift.

An already assigned trigger is preferred and never detached by this HAL. Otherwise a matching
device trigger is tried, followed by a triggerless push buffer (including qcom-smgr), then an owned
configfs hrtimer. Triggers attached or created by the HAL are tracked and released on shutdown.
The fastest active sensor period controls shared device and hrtimer frequency; decimal frequencies
are retained in sysfs writes.

Every complete scan from each read is processed and incomplete trailing bytes are retained. A
plausible 64-bit kernel boottime timestamp is used when present. Other timestamps, including the
qcom-smgr 32-bit counter, are replaced with monotonic `CLOCK_BOOTTIME` timestamps backfilled over
the received scans.

If buffering is unavailable, devices with real direct attributes use periodic sysfs reads. `_raw`
values receive `(raw + offset) * scale`; `_input` values are already scaled. This fallback is only
advertised when all required scalar or XYZ files actually exist, covering non-buffered BMI160,
AK09918, and HID sensor configurations.

## Android Semantics

IIO values and metadata use the same unit conversion: accelerometer and angular velocity unchanged,
magnetic field Gauss to microtesla (`x100`), pressure kPa to hPa (`x10`), relative humidity and
temperature divided by 1000, and proximity meters to centimeters (`x100`). When hwdb supplies
`PROXIMITY_NEAR_LEVEL`, raw proximity is mapped to Android near/far values instead.

Accelerometer, gyroscope, magnetic field, and pressure are continuous. Light, proximity,
temperature, and humidity are on-change and suppress duplicate values. FIFO counts are zero, so
report latency is accepted but does not change buffering. Data injection is not advertised and
physical output stops while the backend is in data-injection mode.

Flush requests for buffered sensors are queued to the device reader, after samples already returned
by the fd as practically possible. Direct sensors complete flush immediately.

## Calibration And Overrides

Vector scan channels are ordered X/Y/Z by channel name before applying the row-major IIO mount
matrix. Matrix priority is:

1. `vendor.sensors.iio.<device_name>.mount_matrix`
2. hwdb `ACCEL_MOUNT_MATRIX` for accelerometers, matched by parent modalias and optional label
3. Type-specific `in_*_mount_matrix`, then `mount_matrix` and `in_mount_matrix`
4. Identity

Other Android properties are `vendor`, `power`, `max_range`, and `resolution` under the same device
prefix. Device names replace `-`, spaces, and `/` with `_` in property keys. Vendor is otherwise
derived from the device-tree compatible string.

Pure scan parsing, layout, decoding, and unit conversion live in `IioScan.*` and are covered by the
`libsensors_iio_scan_test` cc_test.
