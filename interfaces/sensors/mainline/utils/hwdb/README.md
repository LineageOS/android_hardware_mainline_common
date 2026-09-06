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
Its parser is a port of `import_file()` in systemd's `src/shared/hwdb-util.c`
and produces identical records for all of systemd's own `hwdb.d` files,
including the quirks: a `#` outside the first column starts a trailing comment
on *any* line, only a single leading space marks a property, an empty line
between the match patterns and the properties discards the record, and an
unindented line inside a record ends it. Matching uses `fnmatch(pattern, key, 0)`
like `sd-hwdb`, and on conflicts the property of the later entry wins (systemd
orders by file priority, then line number).

## Matching

For a sensor with parent modalias `M`, optional IIO `label` `L` and machine
DMI modalias `D`, these match strings are tried (first hit per property wins):

```
sensor:L:modalias:M:D      (only when the sensor has a label)
sensor:modalias:M:D
```

This mirrors the `IMPORT{builtin}="hwdb ..."` rules of systemd's
`60-sensor.rules`. The DMI part is **always** appended, so on a machine
without DMI the key ends with `:` — device tree entries depend on it, their
patterns end in `:*`:

```
sensor:modalias:of:NaccelerometerT_null_Csilan,sc7a20:*
```

`M`, `L` and `D` are sanitized exactly like udev sanitizes a `$attr{...}`
substitution (`udev_replace_chars()` with `UDEV_ALLOWED_CHARS_INPUT`): every
character outside `[0-9A-Za-z]`, `#+-.:=@_` and `/ $%?,` that is not part of a
`\x` escape or of a valid UTF-8 sequence becomes `_`. That is what turns the
kernel's device tree modalias `of:NaccelerometerT(null)C...` into the
`of:NaccelerometerT_null_C...` form used in the database.

Unlike systemd, which stops at the first matching rule and therefore never
consults the label-less key for a labelled sensor, a labelled sensor here also
falls back to `sensor:modalias:M:D` so that generic entries still apply.
Properties from the label specific entry take precedence.

`D` comes from `/sys/class/dmi/id/modalias`. When the kernel does not expose it
(no `CONFIG_DMIID`), it is rebuilt from the raw SMBIOS tables in
`/sys/firmware/dmi/tables` with `libsmbios_parser`, reproducing
`get_modalias()` of the kernel's `drivers/firmware/dmi-id.c`: the same field
order (`bvn bvr bd br efr svn pn pvr rvn rn rvr cvn ct cvr sku pfa`), the same
`ascii_filter()` (drop everything outside `' ' < c < 127` and `:`) and the same
rules for omitting absent fields.
