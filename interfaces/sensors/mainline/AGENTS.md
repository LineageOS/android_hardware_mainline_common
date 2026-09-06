# AGENTS.md - Mainline Sensors HAL

Guidance for AI coding agents working in this directory. Read `README.md` and
the backend READMEs for the functional description; this file is about how the
code is organised and the rules to follow when changing it.

## What this is

`android.hardware.sensors` (AIDL V3) HAL for devices running a mainline Linux
kernel. One frontend process (`android.hardware.sensors-service.mainline`)
loads backend shared libraries (`libsensors_<name>.so`) with `dlopen()`; each
backend bridges one Linux subsystem (IIO, input) or an external sensor service
to the `ISensorBackend` interface. Everything ships in the vendor APEX
`com.android.hardware.sensors.mainline` (manifest name
`com.android.hardware.sensors`, do not change it).

## Map of the code

| Path                                        | Role                                                                 |
|---------------------------------------------|----------------------------------------------------------------------|
| `main.cpp`                                  | Creates `Sensors`, runs discovery, registers `ISensors/default`      |
| `Sensors.{h,cpp}`                           | `BnSensors` implementation; errno → binder status mapping            |
| `SensorManager.{h,cpp}`                     | Backends, global handles, request routing, composite sensors, injection |
| `EventDispatcher.{h,cpp}`                   | Event FMQ + wake lock protocol (libpower)                            |
| `BackendLoader.{h,cpp}`                     | Backend list resolution, `dlopen()`, symbol/version checks           |
| `composite/CompositeSensor.h`               | `ICompositeSensor` / `CompositeSensorBase`                           |
| `composite/DeviceOrientationSensor.*`       | `DEVICE_ORIENTATION` from accelerometer + orientation workarounds    |
| `include/libsensors_mainline/SensorBackend.h` | **ABI** between frontend and backends (see below)                  |
| `utils/common/`                             | `libsensors_common`: `Sysfs`, `Settings`, `MountMatrix`, `SensorEvents`, `SensorTypes`, `PeriodicWorker` |
| `utils/hwdb/`                               | `libsensors_hwdb`: systemd `60-sensor.hwdb` lookups, DMI/SMBIOS modalias |
| `backends/iio/`                             | IIO backend: `IioBackend`, `IioDevice`, `IioSensor`, `IioChannel`, `IioTrigger`, `IioTypes` |
| `backends/input/`                           | Input backend: `InputBackend`, `InputDevice`                          |
| `backends/mock/`                            | Mock backend (fallback only)                                          |

Build modules: `android.hardware.sensors-service.mainline` (binary),
`libsensors_mainline_frontend` (static), `libsensors_mainline_headers`
(header lib, public), `libsensors_common`, `libsensors_hwdb` (static),
`libsensors_iio`, `libsensors_input`, `libsensors_mock` (shared),
`com.android.hardware.sensors.mainline` (APEX). External dependencies:
`libhwdb` and `libsmbios_parser` from `hardware/mainline/common/libraries/`.

## Hard rules

* **Do not change the `ISensorBackend` virtual method layout** (order, number,
  signatures) in `SensorBackend.h`: out-of-tree backends such as
  `hardware/mainline/qcom/libraries/libsensors_libssc` are built against it
  (that backend also links `libsensors_common`, so keep that library's API
  stable or update the backend along with it).
  Add new functionality through new optional exported C symbols (see
  `GetSensorBackendFlags`) and bump `kSensorBackendInterfaceVersion` only for
  incompatible changes.
* The frontend must not contain backend specific logic. Anything a backend
  needs to tell the frontend goes through the interface (e.g. flags).
* Backends must not hard-code hardware specific values. Derive them from the
  kernel (sysfs, ioctl), the hwdb, or `Settings` keys documented in the
  backend README. Type-level defaults live in `utils/common/SensorTypes.cpp`.
  Driver quirks that cannot be derived belong in the backend's quirk table
  (`backends/iio/IioTypes.cpp`) with a comment referencing the kernel source.
* Never call into a backend while holding a lock that the backend's event
  callback path also needs (`SensorManager::state_mutex_`), and never call the
  frontend callback while holding a backend lock that `Activate()/Batch()` take.
  Backends unlock before joining worker threads.
* Event timestamps are `CLOCK_BOOTTIME` nanoseconds; use `GetBootTimeNs()`.
* Event handles inside a backend are backend-local; the frontend re-maps them.
* No exceptions (`try`/`catch` is forbidden; use `android::base::Parse*` and
  `std::optional`). Prefer C++ standard library and `libbase`
  (`system/libbase/include/android-base`) over raw C.
* Every source file starts with the SPDX header (see existing files) and a
  `#define LOG_TAG "MainlineSensors..."` before the includes in `.cpp` files.
* Google C++ style: `CamelCase` types and functions, `snake_case_` members,
  `kCamelCase` constants. Keep files focused; split rather than grow.
* Run clang-format before committing:
  `prebuilts/clang/host/linux-x86/clang-r584948b/bin/clang-format -i --style=file <files>`
  (the repository `.clang-format` is `build/soong/scripts/system-clang-format`).
* Commit messages start with `mainline/common: interfaces/sensors/mainline: `
  and end with `Assisted-by: <Agent>/<Model ID>`.
* Do not build or run tests yourself; the maintainer compiles and reports.

## Conventions worth knowing

* Configuration: every tunable is a dotted key read through `Settings`
  (`vendor.sensors.<key>` property, then `/odm|/vendor/etc/sensors/*.conf`).
  Device names in keys go through `Settings::SanitizeKeyComponent()`. Document
  new keys in the relevant README.
* Backend exports: use `DEFINE_SENSOR_BACKEND(Class, flags)` at file scope in
  the backend `.cpp` and build the library with `-fvisibility=hidden`.
* Backends without a FIFO return `kFlushHandledByFrontend` from `Flush()`.
* Backends implement `SetOperationMode()` by pausing event delivery in
  `DATA_INJECTION` mode (the frontend drops events anyway).
* On-change sensors emit an initial event on activation and then only on
  change (`HaveSamePayload()`); continuous sensors decimate to the requested
  period when the hardware runs faster.
* The mock backend sets `kSensorBackendFlagFallbackOnly`, so the frontend
  drops its sensors for types already provided by earlier backends.
* Composite sensors are registered in `Sensors::Initialize()` before
  `SensorManager::Initialize()`, only when enabled by a setting, and only
  survive registration if no backend provides their type and all their input
  types exist.

## IIO backend specifics

* Type detection is channel based (`IioTypes.cpp`: `ParseIioAttributeName()`,
  `MatchIioSensorSpecs()`), never name based. Name/compatible are used for the
  vendor, the model name and a logged hint only.
* Device data path is decided per device (`IioDevice`): buffer mode only when
  every sensor of the device has scan elements; otherwise poll mode. Buffer
  failures and the watchdog fall back to poll mode permanently.
* Scan layout follows the kernel (`ComputeScanLayout()` in `IioChannel.cpp`).
* Trigger handling is in `IioTrigger.cpp` (driver trigger name patterns, then
  hrtimer via configfs; stale hrtimer triggers are cleaned up at start).
* Unit conversions IIO → Android are in `MatchIioSensorSpecs()`
  (`unit_factor`); proximity is special-cased (`IioSensor::ConvertProximity`).

## Reference material inside the tree

* AIDL interface: `hardware/interfaces/sensors/aidl/android/hardware/sensors/`
* Reference HAL and VTS: `hardware/interfaces/sensors/aidl/default`,
  `hardware/interfaces/sensors/aidl/vts`
* IIO ABI: `<kernel>/Documentation/ABI/testing/sysfs-bus-iio`,
  `drivers/iio/industrialio-{core,buffer}.c`
* Kernel trees: `kernel/virt/virtio` (generic), `kernel/mainline/msm8953-mainline`
  (bmi160, st_lsm6dsx, ak8975, yas530, ltr501, ltrf216a, stk3310, HID sensors),
  `kernel/mainline/msm89x7-mainline` (qcom_smgr)
* iio-sensor-proxy (userspace reference): `external/mainline-hw-deps/iio-sensor-proxy`
* Previous implementation (archived): `../mainline_orig`
