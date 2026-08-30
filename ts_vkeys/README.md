# ts_vkeys

A userspace daemon that translates touchscreen events in the virtual key area into key events via uinput. This is primarily used on Qualcomm devices running a mainline Linux kernel, where the downstream kernel's built-in virtual key handling is no longer available.

## Overview

Many Android devices have a touch-sensitive area below the display (e.g., capacitive buttons for *menu*, *home*, *back*). On downstream Qualcomm kernels, virtual keys are handled either by:

- The `qcom,gen-vkeys` kernel driver (`gen_vkeys.c`), which exports virtual key regions via sysfs, or
- Direct support inside the touchscreen kernel driver itself.

When running a mainline kernel, neither of these mechanisms is available. `ts_vkeys` bridges this gap by:

1. Reading touch events from the physical touchscreen input device.
2. Mapping touch coordinates to virtual key codes based on configurable regions or exact coordinates.
3. Emitting corresponding key events through a virtual uinput device.

## How It Works

```
 ┌──────────────┐    input events     ┌───────────┐    key events    ┌──────────────┐
 │  Touchscreen ├────────────────────►│ ts_vkeys  ├─────────────────►│  uinput dev  │
 │  /dev/inputX │  (EV_ABS, EV_SYN)   │  (daemon) │  (EV_KEY)        │  /dev/inputY │
 └──────────────┘                     └───────────┘                  └──────────────┘
                                            │
                                     reads config from
                                    Android properties
```

### Startup Sequence

1. Parse virtual key configuration from Android system properties.
2. Scan `/dev/input/event0` through `/dev/input/event9` (up to 10 rounds with 1-second intervals) for a device that supports `EV_ABS` (i.e., a touchscreen).
3. Create a virtual input device via `/dev/uinput` that reports the configured key codes.
4. Enter an epoll event loop: read touch events from the source device, track multi-touch slots, and emit key press/release events when a touch falls within a configured virtual key region.

### Touch Protocol Support

`ts_vkeys` supports two touch reporting protocols:

- **MT Protocol B** (`ABS_MT_SLOT` / `ABS_MT_TRACKING_ID` / `ABS_MT_POSITION_X` / `ABS_MT_POSITION_Y`): Full multi-touch slot tracking.
- **Legacy single-touch** (`BTN_TOUCH` / `ABS_X` / `ABS_Y`): Falls back to slot 0 for devices that do not implement MT Protocol B.

## Configuration

`ts_vkeys` supports two configuration modes, tried in order of priority:

1. **genvkeys mode** — Region-based layout derived from Qualcomm's `qcom,gen-vkeys` DeviceTree binding.
2. **Exact mode** — Explicit per-key coordinate mapping.

If genvkeys properties are present and valid, they are used. Otherwise, the program falls back to exact mode.

### Mode 1: genvkeys (Region-Based)

This mode mirrors the behavior of the downstream `gen_vkeys.c` kernel driver. Instead of specifying exact key coordinates, you provide the display and panel dimensions along with a list of key codes. The program calculates the key regions automatically — dividing the bottom bezel area into equal-width regions for each key.

The calculation uses the same algorithm as the downstream kernel driver:

- **Border** between keys: `(panel_maxx - disp_maxx) * 2`
- **Key width**: `(disp_maxx - border * (num_keys - 1)) / num_keys`
- **Key height**: `(panel_maxy - disp_maxy) * 8/10`
- **Y region**: from `disp_maxy` to `disp_maxy + height + y_offset`

#### Properties

All properties use the prefix `vendor.ts_vkeys.genvkeys.`.

| Property | Type | Required | Description |
|---|---|---|---|
| `disp_maxx` | uint32 | Yes | Display area width (pixels) |
| `disp_maxy` | uint32 | Yes | Display area height (pixels) |
| `panel_maxx` | uint32 | Yes | Full panel width (pixels) |
| `panel_maxy` | uint32 | Yes | Full panel height (pixels) |
| `key_codes` | string | Yes | Comma-separated list of Linux key codes |
| `y_offset` | int32 | No | Vertical offset for the key region (default: `0`) |
| `y_beyond_maxy` | bool | No | If `true`, accept touches with Y values beyond the calculated max Y (default: `false`) |

#### Example

For a 720x1280 display on a 720x1458 panel with three keys (menu=139, home=172, back=158):

```sh
setprop vendor.ts_vkeys.genvkeys.disp_maxx  720
setprop vendor.ts_vkeys.genvkeys.disp_maxy  1280
setprop vendor.ts_vkeys.genvkeys.panel_maxx 720
setprop vendor.ts_vkeys.genvkeys.panel_maxy 1458
setprop vendor.ts_vkeys.genvkeys.key_codes  139,172,158
```

This produces three key regions spanning the bottom bezel area (Y: 1280–1422):

| Key | Code | X Range |
|---|---|---|
| Menu | 139 (KEY_MENU) | 0 – 239 |
| Home | 172 (KEY_HOMEPAGE) | 240 – 479 |
| Back | 158 (KEY_BACK) | 480 – 719 |

### Mode 2: Exact (Per-Key Coordinates)

This mode maps individual (X, Y) pixel coordinates to key codes. Useful when the virtual key layout does not follow a regular grid, or when only a few keys are needed at specific positions.

#### Properties

| Property | Type | Required | Description |
|---|---|---|---|
| `vendor.ts_vkeys.names` | string | Yes | Comma-separated list of key names |
| `vendor.ts_vkeys.<name>.x` | uint16 | Yes | X coordinate of the key |
| `vendor.ts_vkeys.<name>.y` | uint16 | Yes | Y coordinate of the key |
| `vendor.ts_vkeys.<name>.key_code` | uint16 | Yes | Linux key code for the key |

#### Example

```sh
setprop vendor.ts_vkeys.names menu,home,back
setprop vendor.ts_vkeys.menu.x        160
setprop vendor.ts_vkeys.menu.y        1344
setprop vendor.ts_vkeys.menu.key_code 139
setprop vendor.ts_vkeys.home.x        360
setprop vendor.ts_vkeys.home.y        1344
setprop vendor.ts_vkeys.home.key_code 172
setprop vendor.ts_vkeys.back.x        570
setprop vendor.ts_vkeys.back.y        1344
setprop vendor.ts_vkeys.back.key_code 158
```

## Integration

### Android init Service

The program runs as a `oneshot` init service and is started automatically when the relevant properties are set.

**Normal boot** (`ts_vkeys.rc`):

```
service vendor.ts_vkeys /vendor/bin/ts_vkeys
    class main
    user uhid
    group uhid input
    oneshot
    disabled

on property:vendor.ts_vkeys.genvkeys.key_codes=*
    enable vendor.ts_vkeys

on property:vendor.ts_vkeys.names=*
    enable vendor.ts_vkeys
```

**Recovery** (`ts_vkeys.recovery.rc`):

```
service ts_vkeys /system/bin/ts_vkeys
    user uhid
    group uhid input
    file /dev/kmsg w
    seclabel u:r:vendor_ts_vkeys:s0
    oneshot
    disabled

on property:vendor.ts_vkeys.genvkeys.key_codes=*
    enable ts_vkeys

on property:vendor.ts_vkeys.names=*
    enable ts_vkeys
```

### Triggering the Service

The built-in `.rc` files automatically trigger the service when either:
- `vendor.ts_vkeys.genvkeys.key_codes` is set (genvkeys mode), or
- `vendor.ts_vkeys.names` is set (exact mode)

For genvkeys mode, set all required properties before setting `key_codes` (which triggers the service):

```
on boot
    setprop vendor.ts_vkeys.genvkeys.disp_maxx  720
    setprop vendor.ts_vkeys.genvkeys.disp_maxy  1280
    setprop vendor.ts_vkeys.genvkeys.panel_maxx 720
    setprop vendor.ts_vkeys.genvkeys.panel_maxy 1458
    setprop vendor.ts_vkeys.genvkeys.key_codes  139,172,158
```

For exact mode, set all key properties before setting `names`:

```
on boot
    setprop vendor.ts_vkeys.menu.x        160
    setprop vendor.ts_vkeys.menu.y        1344
    setprop vendor.ts_vkeys.menu.key_code 139
    setprop vendor.ts_vkeys.home.x        360
    setprop vendor.ts_vkeys.home.y        1344
    setprop vendor.ts_vkeys.home.key_code 172
    setprop vendor.ts_vkeys.back.x        570
    setprop vendor.ts_vkeys.back.y        1344
    setprop vendor.ts_vkeys.back.key_code 158
    setprop vendor.ts_vkeys.names menu,home,back
```

### Build

The program is built as two variants via `Android.bp`:

| Target | Install Path | Description |
|---|---|---|
| `ts_vkeys` | `/vendor/bin/ts_vkeys` | Normal boot, vendor partition |
| `ts_vkeys_recovery` | `/system/bin/ts_vkeys` | Recovery mode, system partition |

### SELinux

The service runs under the `uhid` user with `input` group access. In recovery mode, it uses the `vendor_ts_vkeys` SELinux context. Device-specific SELinux policies may be needed to grant access to `/dev/uinput` and the touchscreen input device.

## Common Linux Key Codes

| Name | Code | Description |
|---|---|---|
| `KEY_BACK` | 158 | Back button |
| `KEY_MENU` | 139 | Menu button |
| `KEY_HOMEPAGE` | 172 | Home button |
| `KEY_SEARCH` | 217 | Search button |
| `KEY_APPSELECT` | 443 | App switcher / Recents |

A full list is defined in `linux/input-event-codes.h`.

## Debugging

Logs are emitted via `android-base/logging` (logcat). In recovery mode, logs go to the kernel log (`/dev/kmsg`).

```sh
# Normal boot
adb logcat -s ts_vkeys

# Recovery
adb shell dmesg | grep ts_vkeys
```
