# IIO backend (`libsensors_iio.so`)

Bridges sensors driven by the Linux Industrial I/O subsystem (`drivers/iio/`)
to the Sensors HAL. It is the primary backend: on a mainline kernel almost every
phone/tablet sensor (IMUs, magnetometers, light/proximity sensors, barometers,
HID sensors of x86 tablets, Qualcomm SMGR sensors, ...) ends up as
`/sys/bus/iio/devices/iio:deviceN`.

The backend is generic: it never matches on driver names to decide *what* a
device is. Everything is derived from the standard IIO sysfs ABI
(`Documentation/ABI/testing/sysfs-bus-iio`). A small quirk table covers the few
drivers that deviate from the ABI.

## Source files

| File              | Content                                                                   |
|-------------------|---------------------------------------------------------------------------|
| `IioBackend.*`    | `ISensorBackend` implementation, device enumeration, name de-duplication  |
| `IioDevice.*`     | One `iio:deviceN`: channel probing, sensor creation, buffer/poll data path |
| `IioSensor.*`     | One Android sensor: `SensorInfo` derivation, value conversion, filtering  |
| `IioChannel.*`    | Scan element (`_type`, `_index`) parsing, scan layout, raw decoding       |
| `IioTrigger.*`    | Trigger discovery (driver triggers) and hrtimer trigger creation          |
| `IioTypes.*`      | Attribute name parsing, channel group → sensor type mapping, units, quirks |

## Discovery

For every `iio:deviceN` (sorted by number, so handles are stable for a given
hardware configuration):

1. `name`, `label`, `of_node/name`, `of_node/compatible` and the parent
   `../modalias` are read and logged.
2. Every `in_*` attribute of the device directory is parsed as
   `in_<type>[<index>][_<modifier>]_<postfix>`. `_raw`/`_input` attributes
   make a channel readable in poll mode (processed `_input` values are
   preferred), `_scale`/`_offset` are recorded per channel or per type.
3. `scan_elements/` (or `buffer0/`) provides the buffer description of each
   channel (`_index`, `_type`).
4. Channel groups are mapped to Android sensor types:

   | IIO channels                                   | Android type                | Unit conversion            |
   |------------------------------------------------|-----------------------------|----------------------------|
   | `accel_x/y/z`                                  | `ACCELEROMETER`             | m/s² → m/s²                |
   | `anglvel_x/y/z`                                | `GYROSCOPE`                 | rad/s → rad/s              |
   | `magn_x/y/z`                                   | `MAGNETIC_FIELD`            | Gauss → µT (×100)          |
   | `gravity_x/y/z`                                | `GRAVITY`                   | m/s²                       |
   | `illuminance`, else `intensity_both`, else `intensity_clear` | `LIGHT`       | lux                        |
   | `proximity`                                    | `PROXIMITY`                 | see below                  |
   | `pressure`                                     | `PRESSURE`                  | kPa → hPa (×10)            |
   | `humidityrelative`                             | `RELATIVE_HUMIDITY`         | milli-% → %                |
   | `temp` (dedicated temperature devices only)    | `AMBIENT_TEMPERATURE`       | milli-°C → °C              |
   | `rot_quaternion`                               | `ROTATION_VECTOR` family    | as is (see quirks)         |
   | `angl` (HID hinge)                             | `HINGE_ANGLE`               | rad → degrees              |
   | `steps`                                        | `STEP_COUNTER`              | count                      |

   A `temp` channel is only exposed when the device has no other sensor
   channels (an IMU die temperature is not an ambient temperature); set
   `iio.<device>.expose_temperature = true` to override.

5. If nothing could be mapped, a type guess based on `name`, `of_node/name`
   and `compatible` is logged to help debugging.

## Sensor metadata

* **Name**: `<model> <Type>` (e.g. `bmi160 Accelerometer`). `<model>` is the
  `name` attribute, or the model part of the `compatible` string when `name`
  is just a bus address (e.g. `3-000c` for `ak09918`). Duplicates get an
  `(iio:deviceN)` suffix.
* **Vendor**: vendor prefix of `compatible` (`bosch,bmi160` → `Bosch`), else
  derived from the modalias (`HID`, `ACPI`), else `Linux IIO`.
* **Range / resolution**: from `_scale` and the `realbits` of the scan element
  (`(2^(bits-1) - 1) × scale`), converted to Android units. Type defaults are
  used when the information is missing.
* **Rates**: from `in_<type>_sampling_frequency_available`,
  `sampling_frequency_available` or `buffer/sampling_frequency_available`.
  The fastest rate is limited to 200 Hz in buffer mode and 50 Hz in poll mode,
  the slowest to 1 Hz.
* **Mount matrix** (3-axis sensors), highest priority first:
  configuration → hwdb `ACCEL_MOUNT_MATRIX` → `in_<type>_mount_matrix`,
  `in_mount_matrix`, `mount_matrix` → identity. The effective matrix and its
  source are logged.
* **Proximity**: IIO proximity values are unit-less counts, higher meaning
  closer. Android expects a distance in cm; the backend reports `0` (near) or
  `5` (far) based on a near level with 10 % hysteresis. Near level sources, in
  order: configuration `proximity_near_level` → hwdb `PROXIMITY_NEAR_LEVEL` →
  sysfs `in_proximity_nearlevel` (device tree `proximity-near-level`) → driver
  quirk → half of the raw full scale. Without any of these the sensor always
  reports "far" and a warning is logged. Drivers reporting distances
  (`qcom-smgr-prox`) are converted to cm.
* **Wake-up**: proximity sensors are wake-up sensors by default
  (`wake_up = false` to change).

## Data path

Buffer mode is used when `/dev/iio:deviceN` exists and **every** sensor of the
device has scan elements (mixing buffer reads and `_raw` reads on one device
makes many drivers return `EBUSY`). Otherwise the device is polled.

Buffer enable sequence (`IioDevice::StartBufferLocked`):

1. `current_timestamp_clock = boottime` so kernel timestamps match Android's
   `elapsedRealtimeNano()`.
2. If the device has a `trigger/` directory: keep an already assigned
   trigger, else look for a driver trigger (`<name>-dev<N>`, `<name>-trigger`,
   `<name>-trig*`, `<name>*rdy*`), else create
   `/config/iio/triggers/hrtimer/sensors-hal-hrtimer-<N>`. Devices without
   `trigger/` (hardware FIFO / push based drivers such as HID sensors and
   `qcom_smgr`) need none.
3. Enable every scan element, read back which ones are enabled and compute the
   scan layout the way the kernel does (natural alignment, timestamp last,
   padding to the largest element).
4. `buffer/length = 128`, `buffer/watermark = 1`, program the sampling
   frequency, `buffer/enable = 1`.
5. Open `/dev/iio:deviceN` and start one reader thread per device. Each scan
   is demultiplexed to all active sensors of the device; each sensor decimates
   to its own requested rate and on-change sensors suppress duplicates.

Kernel timestamps are only trusted when the boottime clock could be selected,
the timestamp channel is 64 bit and the value is within 2 s of the current
time; otherwise the HAL timestamps the samples itself.

A watchdog switches the device to poll mode when the buffer delivers nothing
for `max(3 s, 5 × period)` (e.g. runtime-suspended magnetometers whose trigger
handler fails, or drivers that never push). Failing to enable the buffer also
falls back to poll mode. `iio.<device>.mode = poll|buffer` forces a mode.

Sampling frequency writes go to `in_<type>_sampling_frequency`,
`sampling_frequency` or `buffer/sampling_frequency` (first existing), rounded
up to the next value of the matching `*_available` list, and to the hrtimer
trigger. Several sensors sharing a device run the device at the fastest
requested rate.

## Quirks (`IioTypes.cpp`)

| Device name prefix        | Quirk                                                                                          |
|---------------------------|------------------------------------------------------------------------------------------------|
| `qcom-smgr-prox`          | Proximity is a distance: `raw × scale + offset` metres; timestamp channel is not nanoseconds  |
| `qcom-smgr-pressure`      | Reports hPa (not kPa), bogus `offset` attribute, timestamp channel is not nanoseconds          |
| `qcom-smgr-*`             | Timestamp channel is a 32-bit tick counter                                                     |
| `prox` (HID)              | Human presence: near level 1                                                                   |
| `relative_orientation`    | Quaternion is a `GAME_ROTATION_VECTOR`                                                         |
| `geomagnetic_orientation` | Quaternion is a `GEOMAGNETIC_ROTATION_VECTOR`                                                  |
| `dev_rotation`            | Quaternion is a `ROTATION_VECTOR`                                                              |

## Configuration keys

`<device>` is the sanitized model name (see above), e.g. `bmi160`, `ak09918`,
`lsm6dsl_accel`, `qcom_smgr_accel`. When it differs from the `name` attribute,
keys using the sanitized `name` are accepted as well. Per-sensor keys may
insert the type between device and key: `iio.bmi160.gyro.power`.

Every key is available as property `vendor.sensors.<key>` or in
`/{odm,vendor}/etc/sensors/*.conf`.

| Key                                   | Meaning                                                      |
|---------------------------------------|--------------------------------------------------------------|
| `iio.discovery_wait_ms`               | Wait up to this long for the first IIO device (late DSP sensors) |
| `iio.discovery_settle_ms`             | Extra delay once a device appeared                           |
| `iio.<device>.disable`                | Skip the device                                              |
| `iio.<device>.mode`                   | `auto` (default), `buffer`, `poll`                           |
| `iio.<device>.expose_temperature`     | Expose the `temp` channel of a multi-sensor device           |
| `iio.<device>[.<type>].name`          | Sensor name                                                  |
| `iio.<device>[.<type>].vendor`        | Vendor string                                                |
| `iio.<device>[.<type>].power`         | Power estimate (mA)                                          |
| `iio.<device>[.<type>].max_range`     | Range in Android units                                       |
| `iio.<device>[.<type>].resolution`    | Resolution in Android units                                  |
| `iio.<device>[.<type>].min_delay_us`  | Fastest sampling period                                      |
| `iio.<device>[.<type>].max_delay_us`  | Slowest sampling period                                      |
| `iio.<device>[.<type>].mount_matrix`  | `a, b, c; d, e, f; g, h, i`                                  |
| `iio.<device>[.<type>].proximity_near_level` | Raw count above which an object is "near"             |
| `iio.<device>[.<type>].wake_up`       | Override the wake-up flag                                    |

## Kernel requirements

* `CONFIG_IIO_BUFFER`, `CONFIG_IIO_KFIFO_BUF`, `CONFIG_IIO_TRIGGERED_BUFFER`
  for buffer mode.
* `CONFIG_IIO_CONFIGFS` and `CONFIG_IIO_HRTIMER_TRIGGER` (with configfs mounted
  on `/config`) for devices whose drivers do not register a trigger (bmi160
  without interrupt line, st_lsm6dsx without interrupt line, ak8975, yas530,
  ltr501, ...). Without them such devices work in poll mode.

## SELinux

The HAL runs as `hal_sensors_default`. The device policy must grant:

* read/write on the IIO sysfs attributes (typically labelled
  `vendor_sysfs_iio` through `genfs_contexts`) and read on `/dev/iio:device*`
  (`iio_device`),
* write access to `/config/iio/triggers/hrtimer` (`configfs`) for hrtimer
  triggers,
* read access to the `vendor.sensors.` properties.
