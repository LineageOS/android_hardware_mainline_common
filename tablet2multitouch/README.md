# tablet2multitouch

A userspace daemon that translates tablet/stylus input events into multi-touch protocol B events and key events via uinput. This enables virtual machines (VMware, VirtualBox) running Android to properly handle tablet input as touch input.

## Overview

When running Android in a virtual machine, the host's mouse/tablet device is exposed to the guest as a standard input device. However, Android expects multi-touch input for touchscreen interactions. `tablet2multitouch` bridges this gap by:

1. Reading absolute position events from the tablet device (stylus/tablet mode)
2. Translating them into multi-touch protocol B events (MT slots)
3. Converting mouse buttons to Android navigation keys
4. Handling mouse wheel events as volume controls
5. Optionally creating a separate virtual mouse device for relative movement passthrough

The program is particularly useful for:
- **VMware Workstation/Player**: Exposes mouse as "VMware VMware Virtual USB Mouse" (tablet or relative mode)
- **VirtualBox**: Exposes "VirtualBox mouse integration" (tablet) and "ImExPS/2 Generic Explorer Mouse" (relative)

## How It Works

```
 ┌──────────────┐    tablet events    ┌─────────────────────┐    touch events    ┌──────────────┐
 │  Tablet/Mouse├────────────────────►│ tablet2multitouch   ├───────────────────►│  uinput dev  │
 │  /dev/inputX │  (EV_ABS, EV_KEY)   │     (daemon)        │  (EV_ABS MT,       │  /dev/inputY │
 └──────────────┘                     │                     │   EV_KEY)          └──────────────┘
                                      │  ┌──────────────┐   │
                                      │  │  Translator  │   │
                                      │  │  State       │   │
                                      │  │  Machine     │   │
                                      │  └──────────────┘   │
                                      └─────────────────────┘
                                                │
                                      (optional)│ mouse passthrough
                                                ▼
                                         ┌──────────────┐
                                         │  mouse uinput│
                                         │  /dev/inputZ │
                                         └──────────────┘
```

### Input Event Translation

#### Tablet to Touch Translation

| Source Event | Target Event | Description |
|---|---|---|
| `ABS_X`, `ABS_Y` | `ABS_MT_POSITION_X/Y`, `ABS_X/Y` | Position update |
| `BTN_LEFT` (press) | `ABS_MT_TRACKING_ID`, `BTN_TOUCH` | Touch down (contact mode) |
| `BTN_LEFT` (release) | `ABS_MT_TRACKING_ID=-1`, `BTN_TOUCH=0` | Touch up |

#### Button to Key Translation

| Source Button | Target Key | Description |
|---|---|---|
| `BTN_LEFT` | `BTN_TOUCH` | Touch contact |
| `BTN_MIDDLE` | `KEY_MENU` | Menu button |
| `BTN_RIGHT` | `KEY_BACK` | Back button |
| `BTN_GEAR_DOWN` | `KEY_DOWN` (auto-release) | D-pad down |
| `BTN_GEAR_UP` | `KEY_UP` (auto-release) | D-pad up |

#### Wheel to Volume Translation

| Source Event | Target Key | Description |
|---|---|---|
| `REL_WHEEL=1` | `KEY_VOLUMEUP` (auto-release) | Volume up |
| `REL_WHEEL=-1` | `KEY_VOLUMEDOWN` (auto-release) | Volume down |

### Touch Reporting Modes

The program supports two touch reporting modes:

#### Contact Mode (Recovery)
- Touch is reported only when `BTN_LEFT` is pressed
- Simulates finger touch (no hover)
- Used in recovery mode where hover is not needed

#### Hover Mode (Normal Boot)
- Touch position is always tracked (pen hover)
- `BTN_TOUCH` is set only when `BTN_LEFT` is pressed
- Provides more natural stylus behavior

### VMware/VirtualBox Specific Behavior

The `tablet2multitouch_vboxware` variant handles both tablet and mouse devices:

1. **Tablet device**: Translates absolute position events to multi-touch
2. **Mouse device**: 
   - Button events (`BTN_LEFT`, etc.) are forwarded to the tablet translator
   - Relative movement events (`REL_X`, `REL_Y`) are forwarded to a separate virtual mouse device
   - This allows seamless switching between tablet and mouse modes

## Configuration

### Main Program (`tablet2multitouch`)

The main program uses a property to specify which input devices to monitor:

| Property | Type | Required | Description |
|---|---|---|---|
| `vendor.tablet2multitouch.device_names` | string | Yes | Comma-separated list of allowed device names |

#### Example

```sh
setprop vendor.tablet2multitouch.device_names "VMware VMware Virtual USB Mouse,VirtualBox mouse integration"
```

The program will scan `/dev/input/event0` through `/dev/input/event31` and use the first device whose name matches one in the list and supports both `EV_ABS` and `EV_KEY` events.

### VMware/VirtualBox Variant (`tablet2multitouch_vboxware`)

This variant automatically detects VMware and VirtualBox devices by name:

- **VMware tablet**: `VMware VMware Virtual USB Mouse` with `EV_ABS` capability
- **VMware mouse**: `VMware VMware Virtual USB Mouse` with `EV_REL` capability
- **VirtualBox tablet**: `VirtualBox mouse integration`
- **VirtualBox mouse**: `ImExPS/2 Generic Explorer Mouse`

No configuration properties are needed.

## Integration

### Android init Service

The program runs as a `oneshot` init service and starts automatically during boot.

**Normal boot** (`tablet2multitouch.rc`):

```
service vendor.tablet2multitouch /vendor/bin/tablet2multitouch
    class main
    user uhid
    group uhid input
    oneshot
```

**Recovery** (`tablet2multitouch.recovery.rc`):

```
service tablet2multitouch /system/bin/tablet2multitouch
    user uhid
    group uhid input
    seclabel u:r:tablet2multitouch:s0
    oneshot
```

### VMware/VirtualBox Service

**Normal boot** (`tablet2multitouch_vboxware.rc`):

```
service vendor.tablet2multitouch_vboxware /vendor/bin/tablet2multitouch_vboxware
    class core
    user uhid
    group uhid input
    oneshot
```

**Recovery** (`tablet2multitouch_vboxware.recovery.rc`):

```
service tablet2multitouch_vboxware /system/bin/tablet2multitouch_vboxware
    user uhid
    group uhid input
    seclabel u:r:tablet2multitouch:s0
    oneshot
```

### Device Tree Integration

For devices that need the service, set the device names property in the device-specific init script:

```
on boot
    setprop vendor.tablet2multitouch.device_names "Your Tablet Device Name"
```

### Build

The programs are built as multiple variants via `Android.bp`:

#### tablet2multitouch

| Target | Install Path | Description |
|---|---|---|
| `libtablet2multitouch` | `/vendor/lib(64)/libtablet2multitouch.so` | Shared library |
| `tablet2multitouch` | `/vendor/bin/tablet2multitouch` | Normal boot, vendor partition |
| `tablet2multitouch_recovery` | `/system/bin/tablet2multitouch` | Recovery mode, system partition |

#### tablet2multitouch_vboxware

| Target | Install Path | Description |
|---|---|---|
| `tablet2multitouch_vboxware` | `/vendor/bin/tablet2multitouch_vboxware` | Normal boot, vendor partition |
| `tablet2multitouch_vboxware_recovery` | `/system/bin/tablet2multitouch_vboxware` | Recovery mode, system partition |

### SELinux

The services run under the `uhid` user with `input` group access. In recovery mode, they use the `tablet2multitouch` SELinux context. Device-specific SELinux policies may be needed to grant access to `/dev/uinput` and input devices.

## Architecture

### Shared Library (`libtablet2multitouch`)

The shared library provides RAII classes for:

- **`UinputDevice`**: Manages uinput device lifecycle (create/destroy)
- **`MouseUinputDevice`**: Manages virtual mouse device for relative movement passthrough
- **`TabletEventTranslator`**: State machine that translates tablet events to touch events

### Main Program Flow

1. Parse device names from `vendor.tablet2multitouch.device_names`
2. Scan input devices and find one matching the name with `EV_ABS` + `EV_KEY` support
3. Read `ABS_X` and `ABS_Y` axis information
4. Create uinput device with matching axis ranges
5. Enter event loop: read events, translate, and emit

### VMware/VirtualBox Program Flow

1. Scan for VMware/VirtualBox devices by name
2. Create tablet uinput device (if tablet found)
3. Create mouse uinput device (always)
4. Use `select()` to multiplex both input devices
5. Route events appropriately:
   - Tablet absolute events → touch translator
   - Mouse button events → touch translator
   - Mouse relative events → mouse passthrough

## Debugging

Logs are emitted via `android-base/logging` (logcat). In recovery mode, logs go to the kernel log (`/dev/kmsg`).

```sh
# Normal boot
adb logcat -s tablet2multitouch
adb logcat -s tablet2multitouch_vboxware
adb logcat -s libtablet2multitouch

# Recovery
adb shell dmesg | grep tablet2multitouch
```

### Testing Input Devices

To see available input devices and their names:

```sh
adb shell cat /proc/bus/input/devices
adb shell getevent -i
```

To monitor raw input events:

```sh
adb shell getevent -l /dev/input/event0
```

### Common Issues

**Device not found**:
- Check if `vendor.tablet2multitouch.device_names` is set correctly
- Verify device name matches exactly (case-sensitive)
- Ensure device supports both `EV_ABS` and `EV_KEY`

**Touch not working**:
- Check logs for uinput creation errors
- Verify `/dev/uinput` permissions (user `uhid`, group `input`)
- Check SELinux denials: `adb shell dmesg | grep avc`

**Mouse not working (VMware/VirtualBox)**:
- Verify both tablet and mouse devices are detected in logs
- Check if relative events are being forwarded correctly

## Technical Details

### Multi-Touch Protocol B

The program implements Linux multi-touch protocol B (slotted):

- **Slot 0**: Single touch point (stylus position)
- **Tracking ID**: Incremented on each touch release (0 to 65535)
- **Tool type**: `MT_TOOL_PEN` (stylus)
- **Distance**: 0 (touching) or 1 (hovering)

### Key Code Mapping

| Linux Key Code | Value | Android Action |
|---|---|---|
| `KEY_BACK` | 158 | Back button |
| `KEY_MENU` | 139 | Menu/Overview |
| `KEY_UP` | 103 | D-pad up |
| `KEY_DOWN` | 108 | D-pad down |
| `KEY_VOLUMEUP` | 115 | Volume up |
| `KEY_VOLUMEDOWN` | 114 | Volume down |

### Tracking ID Management

The tracking ID is incremented each time a touch is released, wrapping around at `TRKID_MAX` (65535). This ensures each touch sequence has a unique identifier, which is required by Android's input framework for proper gesture tracking.
