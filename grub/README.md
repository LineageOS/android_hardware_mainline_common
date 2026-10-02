# GRUB utilities

Libraries and tools for the GRUB based A/B boot control. They let
Android and GRUB share slot state through one small file, the GRUB
environment block.

**TL;DR:** GRUB decides which slot to boot and counts the attempts.
Android reports success and picks the next slot. Both read and write
`grubenv_abootctrl`.

## Components

| Directory | Module | Role | README |
|-----------|--------|------|--------|
| `libgrub_editenv/` | `libgrub_editenv` | Read and write the GRUB environment block | [README](libgrub_editenv/README.md) |
| `grub-editenv/` | `grub-editenv` | Command line tool for the same file | [README](grub-editenv/README.md) |
| `libgrub_boot_control/` | `libgrub_boot_control` | A/B slot logic on top of the environment block | [README](libgrub_boot_control/README.md) |
| `grub_boot_control/` | `grub_boot_control` | Command line tool for the slot logic | [README](grub_boot_control/README.md) |

The Boot Control AIDL HAL that uses them is in
`../interfaces/boot/grub/` (`android.hardware.boot-service.grub`).

```
 Android                                   GRUB (grub.cfg)
 ┌──────────────────────────┐              ┌──────────────────────────┐
 │ IBootControl AIDL HAL    │              │ ab_init, pre_boot_hook   │
 │   libgrub_boot_control   │              │   load_env / save_env    │
 │     libgrub_editenv      │              │                          │
 └────────────┬─────────────┘              └────────────┬─────────────┘
              │         grubenv_abootctrl               │
              └──────────►  1024-byte block  ◄──────────┘
                         on the persist partition
```

## Who does what

| Step | GRUB | Android |
|------|------|---------|
| Pick the slot to boot | Reads `active_slot`, checks `is_bootable` and `retry_count`, may switch slot | |
| Count the attempt | Appends one `X` to `retry_count`, records `current_slot` | |
| Boot succeeded | | `markBootSuccessful()` clears `retry_count`, sets `is_successful` |
| Update installed | | `setActiveBootSlot()` makes the other slot active |
| Slot is broken | Sets `is_bootable=false` (no kernel, retries used up) | `setSlotAsUnbootable()` |
| Recovery boot | Boots with `androidboot.mode=recovery` | Recovery HAL gives one attempt back |

## The variables

All names start with the prefix `abootctrl_`. `<s>` is a slot name,
`a` or `b`.

| Variable | Values | Meaning |
|----------|--------|---------|
| `abootctrl_global_active_slot` | `a`, `b` | Slot GRUB tries to boot |
| `abootctrl_global_current_slot` | `a`, `b` | Slot GRUB last booted |
| `abootctrl_global_no_auto_slot_switch` | `true`, `false` | `true` stops GRUB from switching slots by itself |
| `abootctrl_global_snapshot_merge_status` | `none`, `unknown`, `snapshotted`, `merging`, `cancelled` | Virtual A/B merge state, kept for Android |
| `abootctrl_slot_<s>_retry_count` | empty, `X`, `XX`, ... | One `X` per boot attempt. Three or more means "tried enough" |
| `abootctrl_slot_<s>_is_bootable` | `true`, `false` | `false` means do not boot the slot |
| `abootctrl_slot_<s>_is_successful` | `true`, `false` | Android finished booting this slot once |

New files start with: active and current = the slot from
`ro.boot.slot_suffix` (or the first slot), `no_auto_slot_switch=false`,
`snapshot_merge_status=none`, empty `retry_count`, `is_bootable=true`,
`is_successful=false`.

## The file

| Item | Value |
|------|-------|
| Name | `grubenv_abootctrl` |
| Where (system) | `/mnt/vendor/persist/grubenv_abootctrl` |
| Where (recovery) | `/mnt/vendor/_persist/grubenv_abootctrl` |
| Where (GRUB) | The partition with the FAT label `PERSIST` |
| Size | 1024 bytes, padded with `#` |
| Capacity | 930 bytes for `key=value` lines (94 bytes are the header) |

The persist partition must be readable by GRUB, so use a filesystem it
supports (the virt trees use FAT).

## Integration

| What | How |
|------|-----|
| Pick the HAL | `TARGET_BOOT_HAL := grub` (needs `AB_OTA_UPDATER`). This installs the APEX `com.android.hardware.boot.grub` and the recovery service |
| Persist partition | Mount it at `/mnt/vendor/persist` in the fstab |
| Create the file | Run `grub_boot_control <path>` on the host at build time. It creates the file with the defaults |
| GRUB side | Write the logic in `grub.cfg`, see below |

### `grub.cfg` integration example

The only `grub.cfg` integration right now is in the virt trees:

| File | What |
|------|------|
| `device/virt/virt-common/bootmgr/grub/grub-boot.cfg` | The A/B logic: `ab_init`, `pre_boot_hook`, `ab_switch_active_slot` and helpers |
| `device/virt/virtio-common/bootmgr/grub/grub-boot.cfg` | Device specific parts on top of it |
| `device/virt/vboxware/bootmgr/grub/grub-boot.cfg` | The same for VirtualBox and VMware |
| `device/virt/virt-common/build/tasks/20-grub.mk` | Builds GRUB, creates `grubenv` and `grubenv_abootctrl` with the host tools |

Treat it as the reference. Other devices have to write their own.

### What a `grub.cfg` must do

1. Load the block: `load_env -f <persist>/grubenv_abootctrl` for the
   variables above. If loading fails, fall back to slot `a` and boot.
2. Check that the active slot has a kernel. If not, mark it not
   bootable.
3. Check `is_bootable` and `retry_count` of the active slot. If the
   slot is not bootable or has used its attempts, mark it not bootable
   and switch to the other slot. Skip the switch when
   `no_auto_slot_switch` is `true`, or when the other slot is not
   bootable either.
4. After a switch, clear the new slot's `retry_count` and set its
   `is_bootable` to `true`.
5. Add one `X` to `retry_count`, and save `current_slot`
   (`save_env -f <persist>/grubenv_abootctrl`).
6. Pass `androidboot.slot_suffix=_<slot>` on the kernel command line.
   The HAL trusts it for the current slot.
7. For recovery, also pass `androidboot.mode=recovery`. Otherwise pass
   `androidboot.force_normal_boot=1`.

## Try it on the host

```
grub_boot_control /tmp/grubenv_abootctrl                 # create and dump
grub_boot_control /tmp/grubenv_abootctrl setActiveBootSlot 1
grub_boot_control /tmp/grubenv_abootctrl getActiveBootSlot
grub-editenv /tmp/grubenv_abootctrl list
```

## Good to know

- A corrupt `grubenv_abootctrl` (wrong header or size) makes the HAL
  abort at start, instead of guessing.
- The files are written in place at the same size, so GRUB can read
  them at any time.
- No backslash or newline escaping is done. Keep values simple.
- When the block is full, the HAL drops variables it does not know and
  retries. Keep unrelated variables in `grubenv`, not here.

## Rules for changes

See `../docs/` for the standards. Keep the variable names in sync with
the `grub.cfg` examples, and update these READMEs in the same commit.
