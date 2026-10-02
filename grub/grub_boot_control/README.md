# grub_boot_control

A command line front end to
[`libgrub_boot_control`](../libgrub_boot_control/README.md). Use it to
create, inspect and change the A/B state file (`grubenv_abootctrl`)
without the HAL, on a device or on the host.

| Item | Value |
|------|-------|
| Module | `grub_boot_control` |
| Builds for | Platform, vendor, recovery, host |
| Depends on | `libbase`, `libgrub_boot_control`, `libgrub_editenv` |

## Usage

```
grub_boot_control <path> [command] [parameters ...]
```

| Form | Does |
|------|------|
| `grub_boot_control <path>` | Open the file, creating it with the defaults if missing, and log all variables at `DEBUG` level |
| `grub_boot_control <path> help` | List the commands |
| `grub_boot_control <path> <command> [slot or status]` | Run one command |

The file is written back when the tool exits.

## Commands

The commands are the `IBootControl` methods. Slot numbers are plain
integers starting at 0, decimal or `0x` hex.

| Command | Parameter | Prints |
|---------|-----------|--------|
| `getActiveBootSlot` | | Slot number |
| `getCurrentSlot` | | Slot number |
| `getNumberSlots` | | Number of slots |
| `getSnapshotMergeStatus` | | The stored status string |
| `getSuffix` | slot | `_a`, `_b`, ... (empty for an invalid slot) |
| `isSlotBootable` | slot | 1 or 0 |
| `isSlotMarkedSuccessful` | slot | 1 or 0 |
| `markBootSuccessful` | | `OK` |
| `setActiveBootSlot` | slot | `OK` |
| `setSlotAsUnbootable` | slot | `OK` |
| `setSnapshotMergeStatus` | status string | `OK` |

## Output and exit codes

| Output | Meaning | Exit |
|--------|---------|------|
| A number, a string or `OK` | Success | 0 |
| `Invalid slot` | The slot does not exist | 1 |
| `Command failed` | The value could not be read or saved | 1 |
| `Unhandled error` | Any other negative result | 1 |
| `Please specify slot` | Wrong number of parameters | 1 |
| `Invalid slot number <arg>` | Not a non-negative integer | 1 |
| `Please specify merge status string` | Missing status | 1 |
| `subcommand not found` | Unknown command | 1 |
| Help text | No path given | 1 |

## Examples

```
# Create the file at build time (what the virt build does)
grub_boot_control out/persist/grubenv_abootctrl

# Look at the state
grub_boot_control /mnt/vendor/persist/grubenv_abootctrl getActiveBootSlot
grub_boot_control /mnt/vendor/persist/grubenv_abootctrl isSlotBootable 1

# Switch to slot b for the next boot
grub_boot_control /mnt/vendor/persist/grubenv_abootctrl setActiveBootSlot 1

# After a good boot
grub_boot_control /mnt/vendor/persist/grubenv_abootctrl markBootSuccessful
```

## Notes

- On the host there is no `ro.boot.slot_suffix`, so a new file starts
  with the first slot as active and current. On a device, the property
  is used.
- A file that is not a valid GRUB environment block makes the tool
  abort instead of overwriting it.
- Do not run it against a file that the HAL is changing at the same
  time. The two do not share a lock.
- `grub-editenv` can read the same file (`list`), but it does not know
  the slot rules. Prefer this tool for changes.

## Where it is used

| Who | For |
|-----|-----|
| `device/virt/virt-common/build/tasks/20-grub.mk` | Creates `grubenv_abootctrl` for the persist image when `AB_OTA_UPDATER` is true |
