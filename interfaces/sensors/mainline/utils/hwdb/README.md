# libsensors_hwdb

Static library giving backends access to the systemd compatible sensor hardware
database (`60-sensor.hwdb`). The database maps sensors, identified by the
modalias of their parent device and the DMI modalias of the machine, to
properties such as:

* `ACCEL_MOUNT_MATRIX` - orientation of an accelerometer/IMU,
* `ACCEL_LOCATION` - `display` or `base` on convertibles,
* `PROXIMITY_NEAR_LEVEL` - raw value above which an object is "near".

## Files

Database files are read from, in order (later files override earlier ones):

1. `/vendor/etc/sensors/hwdb.d/*.hwdb`
2. `/odm/etc/sensors/hwdb.d/*.hwdb`
3. `/vendor/etc/hwdb.d/60-sensor.hwdb` (legacy location)
4. `/odm/etc/hwdb.d/60-sensor.hwdb` (legacy location)

Parsing is done by `libhwdb` (`hardware/mainline/common/libraries/libhwdb`),
which understands the text format directly (no `systemd-hwdb update` step).

## Matching

For a sensor with parent modalias `M`, optional IIO `label` `L` and machine
DMI modalias `D`, the following match strings are tried (first hit per
property wins):

```
sensor:L:modalias:M:D
sensor:L:modalias:M
sensor:modalias:M:D
sensor:modalias:M
```

`D` comes from `/sys/class/dmi/id/modalias`, or is rebuilt from the raw SMBIOS
tables in `/sys/firmware/dmi/tables` (`libsmbios_parser`) when the kernel does
not expose it. Device tree based machines have no DMI modalias; their entries
use `of:` modalias patterns.
