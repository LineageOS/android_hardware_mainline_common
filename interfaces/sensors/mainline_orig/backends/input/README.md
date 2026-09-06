# Input Backend

Linux input subsystem backend for the Mainline Sensors HAL.

## Overview

Discovers and reads sensors from Linux input event devices at `/dev/input/event*`.

## Supported Sensors

- **Accelerometer**: Devices exposing ABS_X, ABS_Y, ABS_Z absolute axes
- **Proximity**: Devices exposing SW_FRONT_PROXIMITY switch capability
