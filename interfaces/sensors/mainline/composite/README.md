# Composite sensors

Composite (virtual) sensors are implemented in the frontend and derive their
data from hardware sensors provided by the backends.

* Interface: `ICompositeSensor` / `CompositeSensorBase` (`CompositeSensor.h`).
* Registration: `Sensors::Initialize()` registers enabled composite sensors
  with `SensorManager::RegisterCompositeSensor()` before the manager is
  initialised. A composite sensor is only exposed when no backend provides a
  sensor of its type and when all its input types are available.
* Runtime: `SensorManager` activates the input hardware sensors while the
  composite sensor is active (at the rate returned by
  `GetInputSamplingPeriodNs()`, merged with the framework's own requests) and
  passes their events to `ProcessEvent()`. Returned events are delivered with
  the composite sensor's handle.

## DeviceOrientationSensor

`DEVICE_ORIENTATION` (0..3) computed from the accelerometer. Requires
`composite.device_orientation.enabled = true` (property
`vendor.sensors.composite.device_orientation.enabled`).

The rotation is the quadrant of the gravity vector in the screen plane, with
30 degrees of angular hysteresis around quadrant boundaries, a 250 ms settle
time and no change while the device lies flat.

Workarounds for boards whose accelerometer mount matrix is wrong, re-read at
every activation:

| Key                                            | Effect                                     |
|------------------------------------------------|--------------------------------------------|
| `composite.device_orientation.swap_xy`         | Swap the X and Y axes                      |
| `composite.device_orientation.invert_x`        | Negate X                                   |
| `composite.device_orientation.invert_y`        | Negate Y                                   |
| `composite.device_orientation.invert_z`        | Negate Z                                   |
| `composite.device_orientation.rotation_offset` | Add 90, 180 or 270 degrees to the result   |

The effective transformation matrix is logged on activation. Prefer fixing
the mount matrix (device tree, hwdb or `iio.<device>.mount_matrix`) so that
every consumer of the accelerometer benefits.

## Adding a composite sensor

1. Derive from `CompositeSensorBase`, fill `info_` in the constructor
   (`ApplySensorTypeDefaults()` gives sane defaults), implement
   `GetInputSensorTypes()`, `GetInputSamplingPeriodNs()`, `Activate()` and
   `ProcessEvent()`.
2. Add the source to `libsensors_mainline_frontend` in `Android.bp`.
3. Register it in `Sensors::Initialize()` behind a setting.
