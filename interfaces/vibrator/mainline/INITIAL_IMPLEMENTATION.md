# Details about making the initial implementation of Android Vibrator HAL for mainline kernel

We want a flexible and generic Android Vibrator HAL,
for the usage on Android devices running mainline Linux kernel,
which follows proper Linux standards.

You (AI Coding Agent) act as a professional Android HAL engineer
and you've got to implement this in the directory containing this markdown file.

> See the repository root `docs/INITIAL_IMPLEMENTATION_GUIDELINES.md`
> (`hardware/mainline/common/docs/INITIAL_IMPLEMENTATION_GUIDELINES.md`) for
> requirements, references, and guidelines shared by every component. This
> file only lists what's specific to this HAL.

## Requirements

- The HAL shall be named `mainline`.
  - Init rc service name shall be `vendor.vibrator-mainline`.
  - The APEX module name shall be `com.android.hardware.vibrator.mainline`.
  - Filename for the executable shall be `android.hardware.vibrator-service.mainline`.
  - Init rc and vintf fragment shall be renamed accordingly.
- Android properties defined in this HAL shall have `vendor.vibrator.` prefix.

## References

The paths mentioned in this section are relative to AOSP source tree root.

### AIDL HAL interface definition

In `hardware/interfaces/vibrator/aidl/`.
VTS module is in `vts` subdir there.

### HAL implementations

- **Example AIDL Vibrator HAL**: `hardware/interfaces/vibrator/aidl/default`. Check it out for standard AIDL HAL example implementation.

### Linux kernel

There is a reference Linux kernel located at `kernel/virt/virtio`. Check it out for more detailed kernel sided implementations.

Additionally, there is `qcom-spmi-haptics.c` driver in `kernel/mainline/msm8953-mainline/drivers/input/misc/`.

## Guidelines

- Please firstly understand the AIDL interface, and then understand the drivers.

## Design

The HAL shall drive haptic controllers or vibrators exposed via Linux Input force-feedback API.

Try to support as many as possible of such hardwares, with drivers in `drivers/input/misc/` in the reference Linux kernels.
You can run `grep EV_FF *.c` in there to look for these drivers.

While trying to support a wide range of hardware, the HAL itself shall still remain generic.

Try to support as many as possible related APIs and features in both Android HAL interface side and Linux driver side.

Ideally, it shall be configuration-less for every of such hardwares, properties shall be automatically detected at runtime.

If defining hardware-specific properties can't be avoided, then let's try to read these properties from Android properties.
Devices using this HAL shall set these properties properly by themselves.

Focus on supporting these drivers in `drivers/input/misc/` in these reference Linux kernels, while still trying to support the other relevant drivers too:
- `gpio-vibra.c`
- `pm8xxx-vibrator.c`
- `pwm-vibra.c`
- `qcom-spmi-haptics.c`
- `regulator-haptic.c`

### Basic haptic effects

Even for basic vibrator hardwares, we shall still try to support basic haptic effects in some ways, but ONLY if it will actually make different vibration.

For example, for vibrator hardwares that supports setting voltage, we can try to do it by manipulating the voltage.
