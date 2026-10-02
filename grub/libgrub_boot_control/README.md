# libgrub_boot_control

The A/B slot logic behind the GRUB based Boot Control HAL. It stores
the slot state as variables in a GRUB environment block, so GRUB and
Android can both use it. See the [overview](../README.md) for how the
two sides work together.

| Item | Value |
|------|-------|
| Module | `libgrub_boot_control` (shared library) |
| Header | `GrubBootControl.h`, namespace `libgrub_boot_control` |
| Class | `GrubBootControl` |
| Builds for | Platform, vendor, recovery, host, APEX `com.android.hardware.boot.grub` |
| Depends on | `libbase`, `libgrub_editenv` |
| Log tags | `GrubBootControl`, `GrubBootControlUtil` |
| Used by | `android.hardware.boot-service.grub`, `grub_boot_control` |

## Creating it

```cpp
GrubBootControl(std::string grubenv_path = "/mnt/vendor/persist/grubenv_abootctrl",
                std::vector<std::string> slots = {"a", "b"},
                std::string var_key_prefix = "abootctrl_");
```

| Parameter | Meaning |
|-----------|---------|
| `grubenv_path` | The environment block file |
| `slots` | Slot names in order. Slot number = index. At least one |
| `var_key_prefix` | Prefix of every variable this class owns |

| File state | What happens |
|------------|--------------|
| Exists and valid | Loaded as is |
| Exists but is not a valid block | The process aborts (`CHECK`), nothing is guessed |
| Does not exist | Created with the defaults (below) |

The file is written again when the object is destroyed.

## Variables

Key = prefix + `global_<item>` or prefix + `slot_<slot>_<item>`.
The meanings and who writes them are in the [overview](../README.md#the-variables).

| Item | Default on creation |
|------|---------------------|
| `global_active_slot` | Slot from `ro.boot.slot_suffix`, else the first slot |
| `global_current_slot` | Same |
| `global_no_auto_slot_switch` | `false` |
| `global_snapshot_merge_status` | `none` |
| `slot_<s>_retry_count` | empty |
| `slot_<s>_is_bootable` | `true` |
| `slot_<s>_is_successful` | `false` |

## Methods

Same names and meaning as `android.hardware.boot.IBootControl`.
Error codes are mirrored: `INVALID_SLOT = -1`, `COMMAND_FAILED = -2`.

| Method | Does | Errors |
|--------|------|--------|
| `getNumberSlots()` | Number of slots | |
| `getCurrentSlot()` | Slot that is running (see below) | `COMMAND_FAILED` if unknown |
| `getActiveBootSlot()` | Slot GRUB will try next | `COMMAND_FAILED` if the variable is unknown |
| `getSuffix(slot)` | `_<slot name>` | Empty string if the slot is invalid |
| `isSlotBootable(slot)` | 0 only if `is_bootable` is `false` | `INVALID_SLOT` |
| `isSlotMarkedSuccessful(slot)` | 1 only if `is_successful` is `true` | `INVALID_SLOT` |
| `markBootSuccessful()` | For the current slot: clear `retry_count`, `is_bootable=true`, `is_successful=true` | `COMMAND_FAILED` |
| `setActiveBootSlot(slot)` | Clear that slot's `retry_count`, `is_bootable=true`, make it active | `INVALID_SLOT`, `COMMAND_FAILED` |
| `setSlotAsUnbootable(slot)` | `is_bootable=false` and `is_successful=false` | `INVALID_SLOT`, `COMMAND_FAILED` |
| `getSnapshotMergeStatus()` | The stored string | |
| `setSnapshotMergeStatus(str)` | Store any string. The HAL checks the value | `COMMAND_FAILED` |
| `DecreaseRetryCountForCurrentSlot()` | Take one `X` off the current slot's `retry_count` | Does nothing on an invalid slot |
| `PrintGrubVars()` | Log all variables at `DEBUG` level | |

### Which slot is current?

1. `ro.boot.slot_suffix` (for example `_a`), if it names a known slot.
   The bootloader is the authority, because the value has to match
   what it passed to Android.
2. Otherwise the `global_current_slot` variable that GRUB saved.

The property is only read in vendor, recovery and APEX builds. On the
host (for example `grub_boot_control`), the variable is used.

### Recovery

`DecreaseRetryCountForCurrentSlot()` is for boots that should not count
against a slot. The recovery HAL calls it at start, because GRUB
already added an `X` for the recovery boot.

## Saving

Every setter that changes state writes the file at once, except where
several changes belong together (for example, `markBootSuccessful()`
writes once at the end). A write that does not fit drops every
variable the class does not own and tries again. If that also fails,
the call returns `COMMAND_FAILED` and logs an error.

## Threading

One mutex protects the variable map, a second one the merge status.
The methods are safe to call from several threads.

## Limits

- Variable names and values are plain. No backslash or newline
  escaping.
- The slot list is configurable, but the GRUB side must agree with it.
  The virt `grub.cfg` uses `a` and `b`.
- It never touches variables of other owners, except when the block is
  full and they have to go.
