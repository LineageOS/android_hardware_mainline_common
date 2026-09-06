# Input backend (`libsensors_input.so`)

Bridges sensors exposed through the Linux input subsystem (`/dev/input/event*`)
to the Sensors HAL. This covers the legacy accelerometer drivers living in
`drivers/input/misc/` (`bma150`, `mma8450`, `kxtj9`, `adxl34x`, `cma3000_d0x`,
...) and proximity sensors wired as `SW_FRONT_PROXIMITY` switches (usually
`gpio-keys` device tree nodes).

Prefer the IIO backend whenever a driver exists for both subsystems: IIO has
standardised units, the input subsystem does not.

## Discovery

Every `/dev/input/eventN` node is probed once at start-up:

| Capability                                        | Sensor        |
|---------------------------------------------------|---------------|
| `EV_ABS` with `ABS_X`, `ABS_Y`, `ABS_Z`            | Accelerometer |
| `EV_SW` with `SW_FRONT_PROXIMITY`                  | Proximity     |

Devices that look like touch or game controllers (`ABS_MT_POSITION_X`,
`INPUT_PROP_DIRECT`, `INPUT_PROP_POINTER`, `BTN_TOUCH`, `BTN_JOYSTICK`,
`BTN_GAMEPAD`) are not treated as accelerometers unless they set
`INPUT_PROP_ACCELEROMETER` or the configuration forces it.

The event node is opened only while one of its sensors is active so that polled
kernel drivers stay idle otherwise. Event timestamps are requested in
`CLOCK_BOOTTIME` (`EVIOCSCLOCKID`).

## Accelerometer scale

The input subsystem does not define units for accelerometer axes. The scale
(LSB per g) is determined in this order:

1. configuration: `input.<device>.lsb_per_g`
2. `input_absinfo.resolution` when the device sets `INPUT_PROP_ACCELEROMETER`
3. built-in table of known legacy drivers (`bma150`: 256, `mma8450`: 1024,
   `kxtj9_accel`: 1024, `ADXL34x accelerometer`: 256, `cma3000-accelerometer`:
   1000, `ST LIS3LV02DL Accelerometer`: 1000)
4. default of 256 LSB/g with a warning in the log

The mount matrix comes from `input.<device>.mount_matrix`, then from the hwdb
(`ACCEL_MOUNT_MATRIX` keyed by the modalias of the parent device), then identity.

## Configuration keys

`<device>` is the input device name (`EVIOCGNAME`) with every character that is
not `[A-Za-z0-9_]` replaced by `_`, e.g. `input.ADXL34x_accelerometer.lsb_per_g`.
Each key can be set as property `vendor.sensors.<key>` or in a
`/{odm,vendor}/etc/sensors/*.conf` file.

| Key                               | Meaning                                                   |
|-----------------------------------|-----------------------------------------------------------|
| `input.<device>.type`             | `ignore` to skip the device, `accel` to force accelerometer classification |
| `input.<device>.lsb_per_g`        | Accelerometer scale                                       |
| `input.<device>.mount_matrix`     | `a, b, c; d, e, f; g, h, i`                               |
| `input.<device>.vendor`           | Vendor string (default `Linux Input`)                     |
| `input.<device>.power`            | Power estimate in mA                                      |
| `input.<device>.max_range`        | Accelerometer range in m/s²                               |
| `input.<device>.min_delay_us`     | Fastest sampling period                                   |
| `input.<device>.max_delay_us`     | Slowest sampling period                                   |
| `input.<device>.wake_up`          | `false` to drop the wake-up flag of the proximity sensor  |

## SELinux

`hal_sensors_default` already has read access to `input_device` nodes in the
platform policy. Reading `/sys/class/input/*/device/modalias` needs
`sysfs` read access in the device policy.
