# Mock backend (`libsensors_mock.so`)

Provides fake sensor data. It exists so that the HAL keeps working (and the
VTS module can be exercised) on devices without any supported sensor hardware.

The backend is flagged *fallback only*: the frontend silently drops every mock
sensor whose type is already provided by a backend loaded before it. Since the
mock backend is the last one in the default backend list, real sensors always
win.

## Sensors

By default one sensor of each of these types is created: accelerometer,
gyroscope, magnetometer, light, proximity, pressure, ambient temperature and
relative humidity. Values are constant or slowly oscillating; on-change sensors
toggle between two values every ~5 seconds.

The 3-axis sensors advertise `SENSOR_FLAG_BITS_DATA_INJECTION`, which enables
the data injection code path of the frontend.

## Configuration

| Setting key    | Property                     | Meaning                                                          |
|----------------|------------------------------|------------------------------------------------------------------|
| `mock.sensors` | `vendor.sensors.mock.sensors` | Comma separated list of sensor types to create, or `none`. Default: all listed above. |

Type names accept the configuration names (`accel`, `gyro`, `magn`, `light`,
`proximity`, `pressure`, `temperature`, `humidity`, `step_counter`, ...) or the
AIDL enum names (`ACCELEROMETER`, ...).

To leave the mock backend out entirely, override the backend list, e.g.
`setprop vendor.sensors.backends iio,input`.
