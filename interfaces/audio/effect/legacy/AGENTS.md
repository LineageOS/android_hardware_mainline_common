# Notes for AI agents working on the legacy effect wrapper

Read `README.md` first. This directory is independent from
`../../mainline` (the core audio HAL); do not create a build dependency between
the two.

## Rules

* C++, Google style with `hardware/mainline/common/.clang-format`; run
  `clang-format -i` on touched files. Our own identifiers use Google naming
  (`CamelCase()`, `snake_case_`), overrides of AIDL / example HAL methods keep
  their `camelCase` names.
* No `try` / `catch`. Failures are `ndk::ScopedAStatus`, `RetCode`,
  `std::optional` or legacy `int32_t` statuses.
* Do not compile or deploy; the human does.
* Commit subject `mainline/common: interfaces/audio/effect/legacy: ...`,
  detailed body, trailer `Assisted-by: <Agent>/<Model ID>`.

## Map

```
main.cpp                    Finds audio_effects.xml, registers IFactory/default.
LegacyFactory.*             IFactory: EffectConfig -> libraries -> descriptors
                            (+ capability probing), create/destroy instances.
LegacyLibrary.*             dlopen + AUDIO_EFFECT_LIBRARY_INFO_SYM ("AELI") and the
                            RAII LegacyEffectHandle (command()/process()).
LegacyEffect.*              EffectImpl subclass: creates the legacy instance in
                            createContext(), dispatches parameters, runs process().
LegacyEffectContext.*       EffectContext subclass: SET_CONFIG (float, 16-bit
                            fallback), device/mode/source/volume commands,
                            ENABLE/DISABLE/RESET, buffer conversion.
params/LegacyParam.*        effect_param_t buffer + SET_PARAM/GET_PARAM helpers
                            (SetSimple/GetSimple, SetIndexed/GetIndexed).
params/ParameterTranslator.* Interface, vendor-bytes pass-through, type dispatch.
params/TypedTranslator.h    Template that unwraps Parameter::Specific / Id and
                            routes <Effect>::vendor to the pass-through.
params/*Translator*.cpp     One family per file (Equalizer, Strength = BassBoost +
                            Virtualizer, Reverb, Simple = Loudness + Downmix,
                            PreProcessing = AEC/NS/AGC1/AGC2, Visualizer).
```

## What comes from where

* `libaudioeffectaidlcommon` (example HAL): `EffectImpl` (FMQ worker thread,
  state machine, common parameter routing), `EffectContext`, `EffectThread`,
  and the `extern "C" destroyEffect()` used by `LegacyFactory::destroyEffect`.
* `libeffectconfig` (example HAL): `EffectConfig`, the audio_effects.xml parser.
* `libaudio_aidl_conversion_effect_ndk` / `_common_ndk` (frameworks/av):
  descriptor, flags, uuid, device/mode/source/channel-mask conversions.
* The *inverse* of every typed translator lives in
  `frameworks/av/media/libaudiohal/impl/effectsAidlConversion/`. When the
  framework changes how it maps a legacy parameter, mirror it here.
* Legacy parameter ids: `system/media/audio/include/system/audio_effects/effect_*.h`.

## Invariants that are easy to break

* `EffectContext`'s constructor calls the virtual `setCommon()` — the base
  version. Legacy `SET_CONFIG` therefore happens in
  `LegacyEffectContext::Initialize()`, not in the constructor.
* `EffectImpl::process()` holds `mImplMutex` while calling
  `effectProcessImpl()`; every legacy call is thus serialised with parameter
  and command handling. Never call back into `EffectImpl` from a translator.
* Vendor parameter *gets* carry the request `effect_param_t` inside the
  `Parameter::Id` (`<Effect>::Id::vendorExtensionTag` for known types,
  `Parameter::Id::vendorEffectTag` for unknown ones); the reply must be the
  same union family (`<Effect>::vendor` / `Specific::vendorEffect`).
* `LegacyParam::GetParam` sends a reply buffer of the same shape as the
  request; legacy effects write `vsize` and the value into it.
* The framework reads `Descriptor.capability.range` instead of asking the
  effect for EQ level range / preset range and for bass boost / virtualizer
  strength support. Keep `FillCapability()` in sync with what
  `AidlConversion{Eq,BassBoost,Virtualizer}.cpp` expect.
* `EFFECT_API_VERSION_MINOR` in `hardware/audio_effect.h` is broken; use the
  local helper.

## Adding a typed effect

1. New `params/<Name>Translator.cpp` deriving from `TypedTranslator<Effect,
   Parameter::Specific::<tag>, Parameter::Id::<tag>Tag>`; implement
   `SetTyped` / `GetTyped` (and `FillCapability` if the framework consults the
   range).
2. Declare its factory function in `params/Translators.h`, wire it in
   `CreateTranslator()` (`params/ParameterTranslator.cpp`), add the file to
   `Android.bp`, update the support table in `README.md`.
