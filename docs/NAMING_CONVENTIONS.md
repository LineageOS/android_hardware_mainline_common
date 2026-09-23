# Naming Conventions

Applies whenever you add a new component, or a new alternative implementation
of an existing HAL, in this repository. These aren't arbitrary — they follow
patterns already used consistently across every component here. Before
picking new names, check:

- The reference/example AIDL HAL implementation for the domain, under
  `hardware/interfaces/<domain>/aidl/default/` if you need to look one up.
- Existing sibling implementations under this repository's own `interfaces/`
  tree.

Do not invent a different scheme than what's shown below.

## Executable / binary name

Take the reference implementation's binary name (the `cc_binary` in its
`Android.bp`, normally suffixed `.example` or `.default`) and replace that
suffix with your own short implementation name. For example:

- `android.hardware.sensors-service.example` → `android.hardware.sensors-service.mainline`
- `android.hardware.audio.service-aidl.example` → `android.hardware.audio.service-aidl.mainline`

## Init rc service name

Take the reference implementation's `service` name from its `.rc` file, and
either replace a trailing `-default` with `-<name>`, or append `-<name>` if
there's no `-default` suffix. For example:

- `vendor.sensors-default` → `vendor.sensors-mainline`
- `vendor.audio-hal-aidl` → `vendor.audio-hal-aidl-mainline`

## APEX module name vs. APEX manifest name

These are two different names, and both matter for "Multi-install APEX"
(letting several alternative implementations of the same HAL ship as
separate APEX packages that a device can pick between):

- The **APEX module name** (`apex { name: ... }` in `Android.bp`) is
  `com.android.hardware.<domain>.<implementation-name>`, where `<domain>`
  mirrors the AIDL interface's own package path (e.g. `sensors`, `audio`,
  `graphics.allocator`, `usb.gadget`) and `<implementation-name>` is your
  component's short name (e.g. `mainline`).
- The **APEX manifest name** (`"name"` in `apex_manifest.json`, or the
  generated manifest) is always just `com.android.hardware.<domain>` —
  *without* the implementation suffix — matching the reference/default APEX
  for that domain. This is what makes your APEX installable as an
  alternative to (or replacement of) the default one; do not touch it to
  something implementation-specific.
- APEXes in this repository conventionally share the same
  `key: "com.android.hardware.key"` and
  `certificate: ":com.android.hardware.certificate"`.

Living inside an APEX at all is preferred where the HAL type supports it, but
not a hard requirement (see `docs/INITIAL_IMPLEMENTATION_GUIDELINES.md`).

## LOG_TAG and property prefixes

- `LOG_TAG` (or equivalent logging prefix) should identify your specific
  implementation, e.g. `Mainline<Domain>` (`MainlineSensors`,
  `MainlineAudio`), so its log lines are distinguishable from other
  implementations of the same HAL that might exist on the same device.
- Android properties this component defines are namespaced
  `vendor.<domain>.<key>` — the domain's own namespace, not an
  implementation-specific one — e.g. `vendor.sensors.*`, `vendor.vibrator.*`,
  `vendor.audio.*`.

## Never rename

Never rename the AIDL/HIDL HAL interface itself, regardless of how many
alternative implementations exist.
