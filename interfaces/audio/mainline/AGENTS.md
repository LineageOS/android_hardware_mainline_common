# Notes for AI agents working on this HAL

Read `README.md` first for the product view, `INITIAL_IMPLEMENTATION.md` for
the original requirements. This file is about the code.

## Ground rules

* C++ only, Google C++ style with the repository's `.clang-format`
  (`hardware/mainline/common/.clang-format`, 4 spaces, 100 columns). Run
  `prebuilts/clang/host/linux-x86/clang-r*/bin/clang-format -i` on every file
  you touch. Our own code uses Google naming (`CamelCase()` functions,
  `snake_case_` members, `kConstant`); overrides of AIDL / example HAL methods
  keep their original `camelCase` names.
* No `try` / `catch`. Report failures through return values
  (`std::optional`, `::android::status_t`, `ndk::ScopedAStatus`).
* Use `libbase` (`android-base/*.h`) for logging, properties, strings. Log
  tags start with `MainlineAudio_`.
* Do not compile or deploy yourself; the human does and reports back.
* Every commit: subject `mainline/common: interfaces/audio/mainline: ...`,
  detailed body, trailer `Assisted-by: <Agent>/<Model ID>`.
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
* `Module::createInstance(R_SUBMIX / BLUETOOTH)` for the software modules.
* The effect service binary and its plug-in libraries are bundled unmodified
  (unless the `mainline_audio.external_effects` Soong config variable is set,
  which drops them together with their rc and VINTF fragment); their
  `visibility` in `frameworks/av/media/libeffects` and
  `hardware/interfaces/audio/aidl/default/*` was extended to allow this.

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
* Master volume / mute are unsupported on purpose (framework does it
  digitally); mic mute is done by zeroing captured data.
* USB is handled the AOSP way (templates + `connectExternalDevice` with an
  `alsa` address), not by static enumeration, so that the framework's USB
  stack stays in charge.
* `UcmManager::EnableDevice` disables conflicting devices itself: alsa-lib does
  not.

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
