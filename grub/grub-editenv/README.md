# grub-editenv

A small command line tool to create and edit a GRUB environment block
(`grubenv`). It is a minimal stand-in for GNU `grub-editenv`, built on
[`libgrub_editenv`](../libgrub_editenv/README.md).

| Item | Value |
|------|-------|
| Module | `grub-editenv` |
| Builds for | Vendor, recovery, host |
| Depends on | `libbase`, `libgrub_editenv` |

## Usage

```
grub-editenv <filename> <command> [parameters ...]
```

| Command | Does |
|---------|------|
| `create` | Write an empty environment block. Replaces an existing file |
| `list` | Print every variable as `key=value`, sorted by key |
| `set key=value ...` | Set one or more variables |
| `unset key ...` | Remove one or more variables |

The file name always comes first, then the command.

## Examples

```
grub-editenv grubenv create
grub-editenv grubenv set android_theme=2 grub_timeout=5
grub-editenv grubenv list
grub-editenv grubenv unset grub_timeout
grub-editenv grubenv unset android_theme=2     # only if the value is 2
```

## Behavior

| Case | What happens |
|------|--------------|
| Fewer than 2 arguments | Prints the help text, exit 1 |
| Unknown command | `invalid command`, exit 1 |
| File cannot be read or is not a valid block | `failed to load <file>`, exit 1 |
| `set`/`unset` with nothing to do | `no vars to set` / `no vars to unset`, exit 1 |
| `set` argument without `=` | `invalid variable <arg>`, skipped, the rest are applied |
| `set` value contains `=` | Only the first `=` splits key and value |
| `unset key=value` with another value stored | `value mismatches for key <key>`, skipped |
| `unset` of a missing key | `key <key> not found`, skipped |
| Result does not fit in 1024 bytes | `failed to write to <file>`, exit 1, file untouched |
| Everything else | Writes the file, exit 0 |

All messages go to standard output. Skipped arguments do not change the
exit status.

## Where it is used

| Who | For |
|-----|-----|
| `device/virt/virt-common/build/tasks/20-grub.mk` | Creates `grubenv` and sets the defaults at build time |
| `device/virt/virt-common/configs/init/init.virt.rc` | A service that sets `android_theme` on the persist partition at runtime |

## Differences from GNU `grub-editenv`

| | GNU | This tool |
|-|-----|-----------|
| Default file | `grubenv` in the GRUB directory | A path is always required |
| Commands | `create`, `list`, `set`, `unset`, `incr`, ... | `create`, `list`, `set`, `unset` |
| Options | `-v`, `--help`, ... | None |
| Trimming | n/a | Never; a block that does not fit is an error |
