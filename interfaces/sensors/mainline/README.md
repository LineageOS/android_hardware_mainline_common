# Mainline Sensors HAL

A generic Android Sensors HAL (`android.hardware.sensors`, AIDL version 3) for
devices running a mainline Linux kernel. Sensors are consumed through the
standard Linux interfaces (IIO, input) so that the HAL works on as many devices
as possible without device specific configuration, while still allowing
everything to be tuned through configuration files and properties.

* HAL name: `mainline`
* Service: `vendor.sensors-mainline`
  (`/apex/com.android.hardware.sensors/bin/hw/android.hardware.sensors-service.mainline`)
* APEX module: `com.android.hardware.sensors.mainline`
* AIDL instance: `android.hardware.sensors.ISensors/default`

## Architecture

```
                    ┌───────────────────────────────────────────┐
  framework ◄──FMQ──┤ frontend: android.hardware.sensors-service │
   (AIDL)           │  Sensors ─ SensorManager ─ EventDispatcher │
                    │            │        │                      │
                    │  composite sensors  │ dlopen()             │
                    └─────────────────────┼──────────────────────┘
                 ┌────────────────────────┼────────────────────────┐
                 ▼                        ▼                        ▼
       libsensors_iio.so        libsensors_input.so       libsensors_mock.so
      /sys/bus/iio/devices        /dev/input/event*          fake data
      /dev/iio:deviceN
```

* The **frontend** (this directory) implements the AIDL interface, loads the
  backends with `dlopen()`, merges their sensors into one list with global
  handles, routes requests, feeds composite sensors and writes events to the
  framework's fast message queue. It has no knowledge of specific backends.
* A **backend** (`backends/<name>/`, `libsensors_<name>.so`) bridges one
  subsystem to the [`ISensorBackend`](include/libsensors_mainline/SensorBackend.h)
  interface. Several backends contribute to the sensor list at the same time.
* **Composite sensors** (`composite/`) are virtual sensors computed in the
  frontend from hardware sensor events (currently `DEVICE_ORIENTATION` from the
  accelerometer). They are only registered when no backend provides the type.
* **Utility libraries** (`utils/`) are static libraries shared by frontend and
  backends: `libsensors_common` (sysfs, settings, mount matrix, events, sensor
  type traits, periodic worker) and `libsensors_hwdb` (systemd sensor hwdb).

| Backend | Library              | Source                                  | Documentation                        |
|---------|----------------------|-----------------------------------------|--------------------------------------|
| IIO     | `libsensors_iio.so`  | Linux IIO subsystem (`drivers/iio/`)     | [backends/iio/README.md](backends/iio/README.md) |
| Input   | `libsensors_input.so`| Linux input subsystem (`drivers/input/misc/`) | [backends/input/README.md](backends/input/README.md) |
| Mock    | `libsensors_mock.so` | Fake data, fallback only                 | [backends/mock/README.md](backends/mock/README.md) |

Backends are loaded in the order `iio, input, mock` by default. The mock
backend is *fallback only*: its sensors are dropped for every type a real
backend already provides.

Out-of-tree backends (for example `libsensors_libssc` for Qualcomm SSC
sensors) implement the same interface, are built against
`//hardware/mainline/common:libsensors_mainline_headers` and can either be
bundled into the APEX (`include_custom_backends`) or installed in
`/vendor/lib{,64}{/hw,}`; the APEX linker configuration permits loading from
`/odm` and `/vendor`.

## Building

```makefile
TARGET_SENSORS_HAL := mainline
PRODUCT_PACKAGES += com.android.hardware.sensors.mainline
```

Soong config variables (namespace `sensors_hal_mainline`):

| Variable                       | Type        | Purpose                                                        |
|--------------------------------|-------------|----------------------------------------------------------------|
| `load_custom_backends`         | string      | Default backend list, e.g. `libssc,iio,input,mock`             |
| `include_custom_backends`      | string list | Extra backend modules bundled in the APEX                      |
| `run_as_root`                  | bool        | Run the service as root (development only)                     |
| `include_all_permission_xmls`  | bool        | Ship the `android.hardware.sensor.*` feature XMLs              |

Example (`device/mainline/qcom-common/optional/sensors-hal_mainline/product.mk`):

```makefile
$(call soong_config_set_string_list,sensors_hal_mainline,include_custom_backends,//hardware/mainline/qcom:libsensors_libssc)
$(call soong_config_set,sensors_hal_mainline,load_custom_backends,libssc$(comma)iio)
```

## Configuration

The HAL is designed to need no configuration. When something cannot be
determined at runtime (accelerometer orientation of a board without
`mount-matrix`, proximity threshold, scale of a legacy input driver, ...) it
can be provided through **settings**. A setting is a dotted key such as
`iio.bmi160.mount_matrix` and is looked up as:

1. Android property `vendor.sensors.<key>` (highest priority; `setprop` for
   quick experiments),
2. configuration files `/odm/etc/sensors/*.conf` (overrides `/vendor`),
3. configuration files `/vendor/etc/sensors/*.conf`.

Configuration file format:

```ini
# /vendor/etc/sensors/sensors.conf
backends = iio,input

[iio.bmi160]
mount_matrix = 0, -1, 0; -1, 0, 0; 0, 0, 1
power = 0.18

[iio.stk3310]
proximity_near_level = 800

[input.bma150]
lsb_per_g = 256
```

Names used in keys are sanitized: every character outside `[A-Za-z0-9_]`
becomes `_` (`qcom-smgr-accel` → `qcom_smgr_accel`, `ADXL34x accelerometer` →
`ADXL34x_accelerometer`).

### Frontend settings

| Key                                     | Default          | Meaning                                                    |
|-----------------------------------------|------------------|------------------------------------------------------------|
| `backends`                              | build default or `iio,input,mock` | Comma separated backend list (short names, library names or paths) |
| `composite.device_orientation.enabled`  | `false`          | Register the composite `DEVICE_ORIENTATION` sensor         |
| `orientation.swap_xy`                   | `false`          | Device orientation workaround: swap X and Y                |
| `orientation.invert_x` / `invert_y` / `invert_z` | `false` | Device orientation workaround: negate an axis              |
| `orientation.rotation_offset`           | `0`              | Device orientation workaround: add 90/180/270 degrees      |
| `debug.log_events`                      | `false`          | Log every event at INFO level                              |

The orientation workarounds are read each time the composite sensor is
activated, so they can be tuned live:

```
setprop vendor.sensors.orientation.rotation_offset 180
```

Backend specific keys are documented in the backend READMEs.

### Sensor hardware database

The IIO and input backends use the systemd compatible sensor hwdb
(`60-sensor.hwdb`, maintained by the Linux community) to obtain
`ACCEL_MOUNT_MATRIX` and `PROXIMITY_NEAR_LEVEL` for devices whose kernel
drivers do not provide them. Copy the file to
`/vendor/etc/sensors/hwdb.d/60-sensor.hwdb` (the legacy
`/vendor/etc/hwdb.d/60-sensor.hwdb` location is still read). Entries are
matched with the parent device modalias and the DMI modalias (or SMBIOS
tables when the kernel does not expose it).

## Runtime behaviour

* Handles are assigned sequentially in backend load order and discovery order
  and stay stable across framework restarts (discovery happens once at
  service start).
* `initialize()` deactivates every sensor and re-creates the FMQs, as required
  by the AIDL contract.
* Events carry `CLOCK_BOOTTIME` timestamps.
* WAKE_UP events hold a wake lock (`SensorsHAL_WAKEUP_mainline`) until the
  framework acknowledges them, with the mandatory 1 s auto release.
* Data injection is supported for sensors advertising the
  `DATA_INJECTION` flag (the mock 3-axis sensors); `setOperationMode(
  DATA_INJECTION)` returns `EX_UNSUPPORTED_OPERATION` otherwise.
* Direct channels are not supported.

## Debugging

Everything of interest is logged with tags starting with `MainlineSensors`:

```
adb logcat -s MainlineSensors MainlineSensorsManager MainlineSensorsLoader \
    MainlineSensorsIio MainlineSensorsInput MainlineSensorsMock \
    MainlineSensorsHwdb MainlineSensorsSettings MainlineSensorsComposite
```

At start-up the HAL logs, for every backend, the discovered devices, their
channels, how each sensor value is derived (scale source, mount matrix source,
proximity near level source, buffer or poll mode) and the final `SensorInfo`
of every exposed sensor. Activation logs show the mode and rate chosen.

## SELinux

The service runs in the platform `hal_sensors_default` domain. The device
policy has to allow, in addition to the platform rules:

* reading `/dev/iio:device*` and reading/writing the IIO sysfs attributes
  (`genfs_contexts` label for `/sys/bus/iio/devices` and
  `/sys/devices/.../iio:device*`),
* writing to `/config/iio/triggers/hrtimer` (configfs) for hrtimer triggers,
* reading `/sys/class/input/*/device/modalias` and `/sys/class/dmi/id/*`,
* reading `vendor.sensors.` properties (define a vendor property type for the
  prefix in `property_contexts`).

## Layout

```
Android.bp                    frontend, service binary, APEX
main.cpp                      service entry point
Sensors.{h,cpp}               ISensors AIDL implementation
SensorManager.{h,cpp}         backends, handles, routing, composite sensors
EventDispatcher.{h,cpp}       FMQ writes and wake lock protocol
BackendLoader.{h,cpp}         backend list resolution and dlopen()
composite/                    composite sensors
include/libsensors_mainline/  backend interface (exported header library)
utils/common/                 libsensors_common
utils/hwdb/                   libsensors_hwdb
backends/iio/                 IIO backend
backends/input/               input backend
backends/mock/                mock backend
```

## License

Apache License 2.0 (SPDX-License-Identifier: Apache-2.0).
