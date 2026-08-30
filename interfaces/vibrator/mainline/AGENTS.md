# AGENTS.md - Mainline Vibrator HAL

## Project Overview

This is an Android Vibrator HAL implementation for devices running mainline Linux kernel.
It drives haptic controllers exposed via the Linux Input force-feedback (EV_FF) API.

## Architecture

- **Vibrator.cpp**: Core HAL implementation. Auto-discovers input devices with EV_FF support,
  uploads and plays FF_RUMBLE effects to produce haptic feedback.
- **VibratorManager.cpp**: Manages the vibrator lifecycle, synced vibrations, and sessions.
- **VibrationSession.cpp**: Implements IVibrationSession for session-based vibration control.
- **main.cpp**: Service entry point. Registers both IVibrator and IVibratorManager AIDL services.

## Build System

- Build target: `android.hardware.vibrator-service.mainline`
- APEX module: `com.android.hardware.vibrator.mainline`
- Static library: `libvibratormainlineimpl`
- AIDL interface version: V4 (`android.hardware.vibrator-V4-ndk`)

## Key Design Decisions

- Device auto-discovery: scans `/dev/input/event*` for devices with EV_FF support
- Uses FF_RUMBLE effects with magnitude control for amplitude
- Composed effects play sequences of FF effects with delays
- No PWLE frequency control (most mainline vibrator drivers don't support it)
- Hardware-specific properties use `vendor.vibrator.*` prefix

## Android Properties

| Property | Description |
|----------|-------------|
| `vendor.vibrator.device` | Override input device path (e.g., `/dev/input/event3`) |
| `vendor.vibrator.resonant_frequency_hz` | Actuator resonant frequency in Hz |
| `vendor.vibrator.q_factor` | Actuator Q factor |
| `vendor.vibrator.effect.*.duration_ms` | Custom effect durations (click, tick, thud, etc.) |

## Coding Conventions

- C++ with Google C++ Style Guide
- Use `libbase` for Android utilities (logging, properties, unique_fd)
- No try/catch blocks
- LOG(VERBOSE) for debug, LOG(INFO) for important events, LOG(ERROR) for errors

## Supported Kernel Drivers

Focused on: `gpio-vibra`, `pm8xxx-vibrator`, `pwm-vibra`, `qcom-spmi-haptics`, `regulator-haptic`.
Also supports: `drv260x`, `da7280`, and other drivers exposing EV_FF with FF_RUMBLE.

## Verification

Do NOT try to compile. The user will compile and report issues.
Run lint/format checks if requested.
