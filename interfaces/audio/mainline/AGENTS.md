# Notes for AI agents working on this HAL

> See the repository root `AGENTS.md` (`hardware/mainline/common/AGENTS.md`)
> and `docs/` for shared code style, formatting, workflow, and commit
> conventions. This file only covers what's specific to this directory.

Read `README.md` first for the product view, `INITIAL_IMPLEMENTATION.md` for
the original requirements. This file is about the code.

## Ground rules

* Our own code uses Google naming (`CamelCase()` functions, `snake_case_`
  members, `kConstant`); overrides of AIDL / example HAL methods keep their
  original `camelCase` names.
* Log tags start with `MainlineAudio_`.
* Commit subject prefix: `mainline/common: interfaces/audio/mainline: `.
  See root `AGENTS.md` → `docs/COMMIT_CONVENTIONS.md` for the rest of the
  message format.
* Keep `README.md` (properties table, device model) in sync with the code.

## Where things are

```
main.cpp                   Process entry: registers IConfig/default, IModule/default,
                           IModule/r_submix and (build option) IModule/bluetooth.
MainlineConfig.*           IConfig: engine config XML from the APEX (or /vendor/etc),
                           default surround config.
ModuleMainline.*           IModule: subclass of the example HAL's Module. Creates
                           streams, fills profiles of connected external devices,
                           owns the DeviceInventory and RoutingController.
Properties.*               vendor.audio.mainline.* -> struct Properties.
alsa/                      Thin C++ wrappers over alsa-lib. No Android types except
                           in AlsaFormat (AIDL <-> ALSA formats/channels/profiles).
  AlsaCard.*               Card / PCM enumeration through snd_ctl.
  AlsaPcm.*                RAII PCM: open (hw: then plughw: fallback), read/write with
                           xrun recovery, position, latency, capability probing.
  AlsaMixer.*              "alsactl init"-like mixer initialisation (no-UCM cards, USB).
  AlsaError.*              RAII handle types, error strings, alsa-lib error handler.
ucm/                       alsa-lib Use Case Manager.
  UcmManager.*             snd_use_case_mgr_t wrapper: boot sequences, verb, devices
                           with their values, enable/disable with conflict handling.
  UcmDeviceMapper.*        UCM device name -> routing::DeviceRole.
routing/                   Android side model.
  DeviceRole.h             Enum of the roles a path can play (speaker, headphones, ...).
  Endpoint.h               One device port: AIDL device + ALSA path + capabilities.
  DeviceInventory.*        Start-up discovery: cards -> endpoints, role assignment,
                           promotion, null endpoints, USB endpoint synthesis.
  ConfigurationBuilder.*   Endpoints -> Module::Configuration (ports, routes, configs).
  RoutingController.*      Reference counted UCM device enable/disable.
stream/
  StreamMainline.*         DriverInterface on top of alsa::Pcm, in/out stream classes.
  NullDevice.*             Paced discard / silence when there is no hardware.
config/                    XMLs installed into the APEX (effects, policy engine).
```

The policy engine XMLs are parsed by the example HAL's xsdc-generated parser,
whose schema is frozen and *narrower* than the legacy audio policy engine
parser in `frameworks/av` these files were derived from. An unknown enumerator
becomes `UNKNOWN` and makes the HAL `LOG_ALWAYS_FATAL` at start-up, so validate
after every edit:

```sh
xmllint --noout --xinclude \
    --schema hardware/interfaces/audio/aidl/default/config/audioPolicy/engine/audio_policy_engine_configuration.xsd \
    config/audio_policy_engine_configuration.xml
```

## Reused from the example HAL (`hardware/interfaces/audio/aidl/default`)

We link `libaudioserviceexampleimpl` statically and derive from:

* `Module` (port / patch / stream bookkeeping, connectExternalDevice logic,
  debug simulation). Extension points we override: `createInputStream`,
  `createOutputStream`, `populateConnectedDevicePort`,
  `onExternalDeviceConnectionChanged`, `getNominalLatencyMs`, plus a few
  IModule methods (mute/volume, sub-interfaces).
* `StreamCommonImpl` / `StreamIn` / `StreamOut` (worker thread, FMQ state
  machine). We implement `DriverInterface`. Read the state machine comments in
  `hardware/interfaces/audio/aidl/android/hardware/audio/core/StreamDescriptor.aidl`
  before touching `StreamMainline.cpp`; note that a `burst` may arrive in
  STANDBY without a prior `start()`.
* `Module::createInstance(R_SUBMIX / BLUETOOTH)` for the software modules. Pass
  a null configuration: `Module` only falls back to the built-in one of its
  type when `mConfig` is null, and the single argument overload passes an empty
  configuration, which silently leaves the module with no ports at all.
* The APEX carries the core HAL only. Setting the `mainline_audio.internal_effects`
  Soong config variable adds the effect service binary and its plug-in
  libraries unmodified, together with their rc and VINTF fragment; their
  `visibility` in `frameworks/av/media/libeffects` and
  `hardware/interfaces/audio/aidl/default/*` was extended to allow this.
  Without it the device supplies `IFactory/default`, normally the legacy
  library wrapper in `../effect/legacy`. Exactly one of the two, never both.

## Threading

* Binder threads: everything in `ModuleMainline`, `StreamMainline::
  setConnectedDevices` / `setGain`, `RoutingController`, `UcmManager`.
* One worker thread per stream (created by `StreamCommonImpl`): all
  `DriverInterface` methods and every `alsa::Pcm` call. PCM handles are never
  touched from Binder threads.
* Hand-over: `connected_endpoints_` (guarded by `lock_`) + atomic
  `endpoints_updated_`; the worker copies into `active_endpoints_`.
* `UcmManager` and `RoutingController` have their own mutexes; never call
  into them while holding a stream's `lock_` from the worker thread (the
  Binder side does hold `lock_` while calling `RoutingController`, which is
  fine because the worker never takes a routing lock).

## Design decisions worth knowing

* Device *types* are chosen so that the default Android policy engine does the
  right thing without configuration: exactly one attached `OUT_SPEAKER` /
  `IN_MICROPHONE` (default flags), wired things as external templates the
  framework connects, everything else as addressed `*_BUS` ports that are
  selectable but never auto-selected.
* `plughw:` fallback is what guarantees 16-bit / 48 kHz / stereo everywhere;
  profiles are augmented with that combination even if the hardware does not
  do it natively (`AugmentCapabilities`).
* Mix port profiles are the *intersection* (formats, rates, channel count
  range) of the endpoints they are routed to (`IntersectCapabilities`),
  clamped to a channel window per mix port. The augmentation above is what normally keeps the
  primary ports non-empty, but `FilterCapabilities` (card rates / bits
  properties) runs after it and can remove the common subset. A mix port
  whose profiles end up empty is treated by `Module` / the framework as a
  *dynamic* port, not as an error, so never create one: the primary ports go
  through `OrFallback()` (16-bit 44.1 / 48 kHz, served by the plug layer),
  optional ports are skipped when `HasCommonProfile()` fails.
* High resolution output is split off `primary output` (`HraFilter`,
  `kHraOutputCutoff`): the primary port keeps 8 / 16-bit below 88.2 kHz,
  `hra output` (DIRECT | DIRECT_PCM) gets 24 / 32-bit / float at 88.2 kHz and
  above. The combinations in between (e.g. 24-bit at 48 kHz) are on neither.
  For the primary port the split only removes formats / rates as long as
  some remain, so a card restricted to e.g. `bits=24` keeps a usable
  primary output.
  The policy manager never opens a direct output for a linear PCM stereo
  stream up to 192 kHz unless the client asks for one, so normal playback
  always mixes on the primary port.
* Default output promotion (`DeviceInventory::AssignRoles`) never picks HDMI:
  it must stay a template that `WiredAccessoryManager` connects. Extra HDMI /
  DP heads are demoted to bus outputs, so `AssignRoles` remembers them
  (`extra_hdmi_heads`) and skips them when promoting a bus output. Other bus
  outputs (unrecognised UCM devices, a speaker on a secondary card, ...) stay
  promotable. Without a promotable path a null speaker is added.
  The HDMI template's stream backing is selected at routing time from all
  plugged HDMI heads using their ALSA jack controls, not just the head that
  won template priority at start-up. Additional heads remain bus ports.
  Promoting a wired template to a default device retains the template so
  framework jack events can still route to it. When UCM has headphone playback
  and headset-mic capture but no headset playback, the headphone path also
  supplies an `OUT_HEADSET` template for four-pole plugs.
* Master volume / mute are unsupported on purpose (framework does it
  digitally); mic mute is done by zeroing captured data.
* USB is handled the AOSP way (templates + `connectExternalDevice` with an
  `alsa` address), not by static enumeration, so that the framework's USB
  stack stays in charge.
* `UcmManager::EnableDevice` disables conflicting devices itself: alsa-lib does
  not.
* A PCM name is not always a `hw:` name. With a use case profile alsa-lib
  returns `_ucmXXXX.hw:card,N` and only `snd_pcm_open()` resolves that prefix,
  against the private configuration of the use case manager. A plugin slave is
  resolved against the global configuration, so anything that wraps a device
  (`Pcm::Open`'s plug fallbacks) has to strip the prefix first. Never gate
  behaviour on the name starting with `hw:`.
* What a device answers to `hw_params_any()` / `test_rate()` is not what it
  accepts in `hw_params()`. On a DPCM card (every Qualcomm QDSP6 one) the
  front-end answers the queries alone and the back-end constraints only apply
  on commit: the q6asm front-end announces 8 kHz - 192 kHz and 1 - 8 channels
  while the back-end can refuse with `-EINVAL`. Hence the plug fallbacks, and
  the pinned hardware rate for the case where the plug layer trusts the same
  optimistic answers.
* Initial (dynamic) port configs carry `gain = null`, and
  `ModuleMainline::setAudioPortConfig` strips a value-less gain for ports
  without gain controllers. `Hal2AidlMapper` reuses the device port config it
  got from `getAudioPortConfigs()` as the template for its requests, and
  `Module::setAudioPortConfigGain` rejects any gain on a port without `gains`,
  which fails every stream open ("gains for port N is undefined").

## Framework Interaction (AOSP source)

When you need to check how the framework talks to this HAL, look at:

- `hardware/interfaces/audio/aidl/android/hardware/audio/core/` - the
  `IModule`/`IConfig`/`StreamDescriptor` AIDL interface this HAL implements.
- `hardware/interfaces/audio/aidl/default/` (example HAL) - `Module`,
  `StreamCommonImpl`/`StreamIn`/`StreamOut` this directory subclasses (see
  `## Reused from the example HAL` above); read this before touching
  `ModuleMainline.cpp` or `StreamMainline.cpp`.
- `frameworks/av/media/libaudiohal/impl/DeviceHalAidl.*`,
  `StreamHalAidl.*`, `Hal2AidlMapper.*` - the framework-side client that
  calls `IModule`/streams and maps AIDL ports/patches to the legacy
  `audio_devices_t`/`audio_patch` world.
- `frameworks/av/services/audiopolicy/` - the policy engine that decides
  routing/port selection; consumes `config/audio_policy_engine_configuration.xml`
  installed by this HAL (schema in
  `hardware/interfaces/audio/aidl/default/config/audioPolicy/engine/`).
- `frameworks/av/services/audioflinger/` - opens streams and drives the
  data path on top of `libaudiohal`.

## When adding a property

1. Add the field to `struct Properties` with a comment and default.
2. Read it in `Properties::Load()` and print it in `ToString()`.
3. Document it in the README table.

## Quick sanity checks (on a device)

```sh
adb shell dumpsys android.hardware.audio.core.IModule/default | head -80
adb logcat -s MainlineAudio_Inventory MainlineAudio_Ucm
adb shell cat /proc/asound/cards
```
