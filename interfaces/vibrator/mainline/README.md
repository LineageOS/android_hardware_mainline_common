# Mainline Vibrator HAL

Android Vibrator HAL implementation for devices running mainline Linux kernel.

## Overview

This HAL drives haptic controllers or vibrators exposed via the Linux Input force-feedback API.
It automatically discovers compatible input devices at runtime and provides full Android
vibrator functionality including basic on/off, predefined effects, composed effects, and
amplitude control.

## Architecture

```
mainline/
├── Android.bp                          # Build configuration
├── main.cpp                            # Service entry point
├── Vibrator.cpp                        # IVibrator AIDL implementation
├── VibratorManager.cpp                 # IVibratorManager AIDL implementation
├── VibrationSession.cpp                # IVibrationSession AIDL implementation
├── vibrator-mainline.rc                # Init service definition (APEX variant)
├── android.hardware.vibrator.mainline.xml  # VINTF manifest fragment
├── include/
│   └── vibrator-impl/
│       ├── Vibrator.h
│       ├── VibratorManager.h
│       └── VibrationSession.h
├── apex/
│   ├── Android.bp                      # APEX build configuration
│   ├── apex_manifest.json              # APEX manifest
│   └── file_contexts                   # SELinux file contexts
├── AGENTS.md                           # AI agent instructions
└── README.md                           # This file
```

## How It Works

1. **Device Discovery**: On startup, the HAL scans `/dev/input/event*` devices for those
   supporting the Linux force-feedback API (EV_FF). The first compatible device is used.

2. **Vibration**: Effects are uploaded to the kernel driver via `EVIOCSFF` ioctl as
   `FF_RUMBLE` type effects with configurable magnitude and duration. The effect is then
   triggered by writing an `EV_FF` input event.

3. **Amplitude Control**: Android's 0.0-1.0 amplitude range is mapped to the Linux
   FF magnitude range (0-65535). The kernel driver translates this to hardware-specific
   voltage, PWM duty cycle, or other control mechanism.

4. **Composed Effects**: Sequences of primitives are played as timed series of FF effects
   with configurable delays between them.

## Configuration

### Auto-Detection

The HAL attempts to work without configuration by auto-discovering input devices.
In most cases, no configuration is needed.

### Device Override

To force a specific input device:
```
adb shell setprop vendor.vibrator.device /dev/input/event3
```

### Effect Durations

Custom effect durations can be set via Android properties:
```
adb shell setprop vendor.vibrator.effect.click.duration_ms 30
adb shell setprop vendor.vibrator.effect.tick.duration_ms 15
adb shell setprop vendor.vibrator.effect.thud.duration_ms 50
```

### Hardware Parameters

For devices with known hardware parameters:
```
adb shell setprop vendor.vibrator.resonant_frequency_hz 150
adb shell setprop vendor.vibrator.q_factor 11
```

## Supported Kernel Drivers

### Primary (focused support)

| Driver | Hardware | Amplitude Control |
|--------|----------|-------------------|
| `gpio-vibra` | GPIO-based vibrators | Binary (on/off) |
| `pm8xxx-vibrator` | Qualcomm PM8xxx PMIC | Voltage (100mV steps) |
| `pwm-vibra` | PWM-based vibrators | PWM duty cycle |
| `qcom-spmi-haptics` | Qualcomm SPMI haptics | Voltage (VMAX) |
| `regulator-haptic` | Regulator-based haptics | Voltage range |

### Additional

- `drv260x` (TI DRV2604/DRV2605)
- `da7280` (Dialog DA7280)
- Any other driver exposing EV_FF with FF_RUMBLE

## AIDL Interface Version

This HAL implements version 4 of the `android.hardware.vibrator` AIDL interface.

## Building

The HAL is built as part of the LineageOS/AOSP build system:

```
m com.android.hardware.vibrator.mainline
```

## Deployment

The HAL runs inside the `com.android.hardware.vibrator.mainline` APEX module.
The APEX manifest name is `com.android.hardware.vibrator` (without `.mainline` suffix)
to support Multi-install APEX.

## Service Names

- Init RC service: `vendor.vibrator-mainline`
- IVibrator instance: `android.hardware.vibrator/default`
- IVibratorManager instance: `android.hardware.vibrator/IVibratorManager/default`
