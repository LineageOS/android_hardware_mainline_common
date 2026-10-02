# Wiring a HAL into device trees

Applies when you add a HAL here and want device trees to select it
with a `TARGET_*` variable. Written for AI agents and human
contributors.

**TL;DR:** The code lives in `interfaces/<domain>/<name>/`. The switch
lives in `device/mainline/common/optional/<domain>-hal_<name>/`.

## The two halves

| Half | Repo | Path |
|------|------|------|
| Implementation | `hardware/mainline/common` | `interfaces/<domain>/<name>/` |
| Selection | `device/mainline/common` | `optional/<domain>-hal_<name>/` |
| Switch documentation | `device/mainline/common` | `optional/README.md` |
| Default choice | `device/mainline/common` | `optional/options.mk` |

## Checklist

| # | Do | Rule |
|---|----|------|
| 1 | Name things | `docs/NAMING_CONVENTIONS.md` |
| 2 | Plan first | `docs/INITIAL_IMPLEMENTATION_GUIDELINES.md`: get a confirmed plan |
| 3 | Build the HAL | Ship as an APEX if possible; add `vintf` fragment and `.rc` |
| 4 | Add `AGENTS.md`, `CLAUDE.md`, `README.md` | Next to the code |
| 5 | Add `optional/<dir>/product.mk` (and `board.mk` / `sepolicy` if needed) | `optional/_template.mk` |
| 6 | Add the variable to `optional/README.md` | One table per variable |
| 7 | Add a default in `optional/options.mk` only if it is safe for every device | Use `?=` |
| 8 | Add SELinux policy | `device/mainline/common/sepolicy/vendor/`, checked by `build/tools/check_sepolicy.py` |
| 9 | Record upstream sources | `README.md` "Upstreams" table |

## Selection file pattern

```make
ifeq ($(TARGET_SENSORS_HAL),mainline)

PRODUCT_PACKAGES += \
    com.android.hardware.sensors.mainline

endif # TARGET_SENSORS_HAL
```

## Documentation row in `optional/README.md`

| Value | Directory | Description |
|-------|-----------|-------------|
| `mainline` | `sensors-hal_mainline` | One-line description, link to the HAL README |

## Do not

- Rename the AIDL or HIDL interface.
- Hardcode device values; read them from the kernel, config files or
  properties (`docs/INITIAL_IMPLEMENTATION_GUIDELINES.md`).
- Change what existing devices get by default.

## Using it from a device tree

Set the variable in the device makefile. See
`device/mainline/common/docs/CHOOSING_OPTIONAL_MODULES.md`.
