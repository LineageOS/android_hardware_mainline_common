# libsensors_common

Static helper library shared by the frontend and every backend of the mainline
Sensors HAL. It is linked statically so that out-of-tree backends living in
`/vendor` do not depend on a library inside the APEX.

| Header (`libsensors_common/`) | Content                                                                 |
|-------------------------------|-------------------------------------------------------------------------|
| `Sysfs.h`                     | Read/write sysfs attributes (trailing `\0`/`\n` stripped), list directories, parse `*_available` lists |
| `Settings.h`                  | Unified configuration: `vendor.sensors.<key>` property, then `/odm` and `/vendor` `etc/sensors/*.conf` |
| `MountMatrix.h`               | 3x3 mount matrix in the IIO / hwdb textual format                        |
| `SensorEvents.h`              | `GetBootTimeNs()`, event builders (`MakeVec3Event`, `MakeScalarEvent`, `MakeFlushCompleteEvent`, ...), pretty printers |
| `SensorTypes.h`               | Per sensor type traits (reporting mode, label, config name, defaults), flag helpers, `ClampSamplingPeriodNs()` |
| `PeriodicWorker.h`            | Thread running a task at a fixed, adjustable period                      |

## Settings file format

```ini
# comment
key = value
[section]            ; keys below are prefixed with "section."
other_key = value
```

Later files override earlier ones; `/odm/etc/sensors` is read after
`/vendor/etc/sensors`. Properties always win over files.
