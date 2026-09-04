# Legacy audio effect HAL (AIDL wrapper)

An AIDL `android.hardware.audio.effect` V4 HAL (`IFactory/default`) that
serves **legacy effect libraries** — the `hardware/audio_effect.h` C API
modules (`libqcompostprocbundle.so`, `libvolumelistener.so`, Dolby / DTS
blobs, `libbundlewrapper.so`, ...) that used to be loaded by the legacy
`EffectsFactory` / the HIDL effect HAL. It reads the vendor's existing
`audio_effects.xml` unchanged.

Why this exists: `libaudiohal` refuses to pair an AIDL core audio HAL with a
HIDL effect HAL (both must be the same HAL type), and HIDL `audio.effect` is
gone from the compatibility matrices since FCM 202504. Devices with prebuilt
effect libraries would otherwise lose them when moving to an AIDL core HAL such
as the mainline audio HAL next door.

| Item              | Value                                                        |
|-------------------|--------------------------------------------------------------|
| Binary            | `/vendor/bin/hw/android.hardware.audio.effect.service-aidl.legacy` |
| Init service      | `vendor.audio-effect-hal-aidl-legacy`                        |
| AIDL instance     | `android.hardware.audio.effect.IFactory/default`             |
| Configuration     | `/odm/etc`, `/vendor/etc` or `/system/etc` `audio_effects.xml` (or `audio_effects_config.xml`) |
| Log tags          | `LegacyEffect_*`                                             |

It is deliberately **not** packaged into an APEX: the legacy libraries and
their private dependencies live in `/vendor/lib[64]` and must be resolved by
the vendor linker namespace, which a vendor APEX cannot link to wholesale.

## Product integration

```makefile
PRODUCT_SOONG_NAMESPACES += hardware/mainline/common
PRODUCT_PACKAGES += android.hardware.audio.effect.service-aidl.legacy

# The vendor's existing effect configuration and libraries, exactly as for
# the legacy / HIDL effect HAL:
PRODUCT_COPY_FILES += $(LOCAL_PATH)/audio/audio_effects.xml:$(TARGET_COPY_OUT_VENDOR)/etc/audio_effects.xml
PRODUCT_PACKAGES += libqcompostprocbundle libvolumelistener ...   # or prebuilts

# Make sure no other IFactory/default is installed (e.g. drop the effect
# service from the mainline audio APEX with its Soong config switch).
```

Properties (read at start-up):

| Key                                     | Meaning |
|-----------------------------------------|---------|
| `vendor.audio.effect_legacy.config`     | Explicit path of the configuration file. |
| `vendor.audio.effect_legacy.log.verbose`| VERBOSE logging. |

SELinux: the service runs as `hal_audio_default`; it `dlopen`s libraries from
`/vendor/lib[64]/soundfx` (`vendor_file`), which the stock policy allows.

## How it works

```
audio_effects.xml ──EffectConfig──▶ libraries, effects (impl UUID, proxy),
                                    pre/post-processing chains
                                          │
LegacyFactory ◀── dlopen(AELI) ──── LegacyLibrary ── get_descriptor(uuid)
   │  IFactory                            │
   │  queryEffects / queryProcessing      │  create_effect(session, io)
   │  createEffect ──▶ LegacyEffect ──────┘
   │                    (EffectImpl)  ──▶ LegacyEffectContext (effect_handle_t)
   │                         │                 │
   │           ParameterTranslator      EFFECT_CMD_* / process()
   │        (typed or vendor bytes)
```

* **Configuration** is parsed by the example HAL's `libeffectconfig`; the
  schema is the same one the legacy stack used, including `effectProxy`
  entries and `<preprocess>` / `<postprocess>` chains.
* **Descriptors** (type UUID, name, implementor, flags, CPU/memory) come from
  the library's `get_descriptor()`, converted with
  `libaudio_aidl_conversion_effect_ndk`. No name table is involved, so vendor
  specific effect types work as-is. The `Descriptor.capability` ranges that
  the framework expects for equalizer, bass boost, virtualizer and visualizer
  are filled by creating a throw-away instance at start-up and querying it.
* **Instances**: `open()` creates the legacy instance for the AIDL session /
  io handle, sends `EFFECT_CMD_INIT` and `EFFECT_CMD_SET_CONFIG`. The AIDL
  data path is 32-bit float; when a library rejects float, the wrapper falls
  back to 16-bit PCM and converts around `process()`. Insert effects with
  equal input / output channel counts process in place.
* **Common parameters** map 1:1 to legacy commands: device →
  `SET_DEVICE` / `SET_INPUT_DEVICE`, mode → `SET_AUDIO_MODE`, source →
  `SET_AUDIO_SOURCE`, volume → `SET_VOLUME` (with the "applied volume" reply
  of `VOLUME_CTRL` effects surfaced through `getParameter(volumeStereo)`),
  START/STOP/RESET → `ENABLE`/`DISABLE`/`RESET`.
* **Effect specific parameters**: the framework sends typed AIDL parameters
  for the effect types it knows and raw `effect_param_t` bytes (inside a
  `DefaultExtension`) for everything else. The vendor bytes are passed through
  untouched in both directions. Typed parameters are translated in
  `params/`, mirroring `frameworks/av/media/libaudiohal/impl/effectsAidlConversion`:

  | Type                     | Support |
  |--------------------------|---------|
  | Equalizer                | preset, band levels, center / band frequencies, presets (+ capability ranges) |
  | BassBoost, Virtualizer   | strength (+ capability), virtualizer forced mode and speaker angles |
  | PresetReverb, EnvironmentalReverb | full |
  | LoudnessEnhancer, Downmix | full |
  | AEC, NS, AGC1, AGC2      | full |
  | Visualizer               | full, incl. `VISUALIZER_CMD_CAPTURE` / `MEASURE` |
  | Volume                   | vendor bytes (the framework itself uses the vendor path for this type) |
  | DynamicsProcessing, HapticGenerator, Spatializer, Eraser | vendor bytes only; typed fields report unsupported |
  | any vendor type          | vendor bytes |

## Limitations

* No reverse stream: the AIDL `IEffect` has a single input / output pair, so
  a legacy AEC relying on `EFFECT_CMD_SET_CONFIG_REVERSE` gets no echo
  reference.
* Offloaded (`HW_ACC`) effects are announced with their flags but no
  `EFFECT_CMD_OFFLOAD` is sent; the AIDL proxy mechanism selects the software
  implementation of an `effectProxy` pair unless the framework decides
  otherwise.
* `EFFECT_CMD_SET_PARAM_DEFERRED` / `_COMMIT` are not used; every parameter
  is applied immediately.
* Libraries are never `dlclose`d (legacy effect libraries are not reliably
  unloadable).

## Debugging

```sh
adb logcat -s LegacyEffect_Factory LegacyEffect_Library LegacyEffect_Effect LegacyEffect_Context
adb shell dumpsys android.hardware.audio.effect.IFactory/default
```
