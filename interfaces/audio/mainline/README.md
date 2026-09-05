# Mainline audio HAL

A generic Android audio HAL (AIDL `android.hardware.audio.core` V4) for devices
that run a mainline Linux kernel. Instead of a per-SoC mixer configuration it
builds on the standard Linux audio user space:

* **alsa-lib** (`external/mainline-hw-deps/alsa-lib`) for card enumeration,
  PCM I/O and mixer access, and
* **alsa-ucm-conf** (`external/mainline-hw-deps/alsa-ucm-conf`) for the
  routing knowledge ("Speaker", "Headphones", "HDMI1", ... and the mixer
  sequences that switch between them).

The HAL is packaged as a vendor APEX (`com.android.hardware.audio.mainline`).
It provides the core HAL only; the effect HAL (`IFactory/default`) is expected
to come from the device, for instance the legacy effect library wrapper in
[`../effect/legacy`](../effect/legacy/README.md). The unmodified AIDL effect
service of the AOSP example HAL can be bundled instead, see `internal_effects`
below.

| Item                   | Value                                                       |
|------------------------|-------------------------------------------------------------|
| APEX module            | `com.android.hardware.audio.mainline`                       |
| APEX manifest name     | `com.android.hardware.audio` (multi-install with the example) |
| Core HAL binary        | `/apex/com.android.hardware.audio/bin/hw/android.hardware.audio.service-aidl.mainline` |
| Init services          | `vendor.audio-hal-aidl-mainline`, `vendor.audio-effect-hal-aidl-mainline` (only with `internal_effects`) |
| AIDL instances         | `IConfig/default`, `IModule/default`, `IModule/r_submix`, `IModule/bluetooth` (optional), `IFactory/default` (only with `internal_effects`) |
| Log tags               | `MainlineAudio_*`                                           |

## Product integration

```makefile
# The HAL lives in the hardware/mainline/common Soong namespace.
PRODUCT_SOONG_NAMESPACES += hardware/mainline/common

PRODUCT_PACKAGES += com.android.hardware.audio.mainline

# An effect HAL, since the APEX does not carry one by default. Either the
# wrapper for the device's legacy effect libraries ...
PRODUCT_PACKAGES += android.hardware.audio.effect.service-aidl.legacy
# ... or the bundled example one, by setting internal_effects below.

# UCM profiles: install everything (generic images) ...
PRODUCT_PACKAGES += alsa-ucm-conf-all
# ... or only the card(s) of the device, see
# external/mainline-hw-deps/alsa-ucm-conf/README.md for the module names.
# PRODUCT_PACKAGES += alsa-ucm-conf-card-sof-hda-dsp
```

The APEX `required`s the alsa-lib configuration database (`/vendor/etc/alsa`)
and the UCM top-level files (`alsa-ucm-conf-base`); card profiles are the
product's choice.

Optional build time switches (Soong config namespace `mainline_audio`):

```makefile
# Leave the Bluetooth audio module and the bundled IBluetoothAudioProviderFactory
# out of the APEX, e.g. because the device ships its own Bluetooth audio HAL.
$(call soong_config_set_bool,mainline_audio,disable_bluetooth,true)

# Bundle an effect HAL (IFactory/default, the example effect service and its
# plug-ins) into the APEX. Off by default: the device is expected to provide
# its own, e.g. the legacy effect library wrapper in ../effect/legacy. Set this
# only when the device has no effect HAL of its own, and never together with
# one: exactly one IFactory/default may be installed.
$(call soong_config_set_bool,mainline_audio,internal_effects,true)
```

### Things the device still has to provide

* **SELinux.** The HAL runs in the stock `hal_audio_default` domain. It needs
  read/write access to `/dev/snd/*` (`audio_device`, granted by the platform
  policy), read access to `/vendor/etc/alsa` (`vendor_configs_file`) and to
  `/sys/class/sound/*` (used by UCM to find the kernel driver name), and read
  access to the `vendor.audio.mainline.*` properties.
* **Jack detection.** The HAL exposes wired headphones / headsets / line out as
  *external* device ports; the framework has to report their connection. On a
  mainline kernel the sound card exposes jacks as input devices with
  `SW_HEADPHONE_INSERT` / `SW_MICROPHONE_INSERT` / `SW_LINEOUT_INSERT`, so set
  `config_useDevInputEventForAudioJack=true` in the device's framework
  overlay. HDMI connection events are only reported by the framework on TV
  devices (`HdmiControlService`); see "Device model" for how HDMI-only devices
  are handled.
* **Audio policy.** No `audio_policy_configuration.xml` is needed: the module
  list, ports and routes come from the HAL. The engine configuration
  (strategies, volume curves) is the AOSP phone example shipped in the APEX; a
  device may override it with its own `audio_policy_engine_configuration.xml`
  in `/vendor/etc`.

## Properties

All keys start with `vendor.audio.mainline.`. They are read once when the HAL
starts.

| Key                       | Type   | Default | Meaning |
|---------------------------|--------|---------|---------|
| `cards`                   | string | *(all non-USB cards)* | Comma separated list of cards to use: index (`0`), id (`PCH`) or name (`HDA Intel PCH`). |
| `wait_for_cards_ms`       | int    | `0`     | Maximum time in milliseconds to wait for the cards listed in `cards` to appear before proceeding (0 = no wait, max 60000). Only effective when `cards` is set. |
| `primary_card`            | string | *(auto)* | Card that provides "Speaker" and "Built-In Mic". Auto: first card with an analog output. |
| `include_usb_cards`       | bool   | `false` | Treat USB cards present at boot as static cards instead of leaving them to the framework's USB handling. |
| `ucm.enabled`             | bool   | `true`  | Use UCM profiles when available. |
| `ucm.verb`                | string | `HiFi`  | UCM verb to select (falls back to the first verb of the profile). |
| `mixer.init`              | bool   | `true`  | For cards *without* a UCM profile and for USB cards: unmute and set default volumes at start-up. |
| `mixer.playback_percent`  | int    | `100`   | Playback volume applied by `mixer.init`. |
| `mixer.capture_percent`   | int    | `80`    | Capture volume applied by `mixer.init`. |
| `latency_ms`              | int    | `20`    | Nominal stream latency; drives the buffer size negotiated with the framework (5..500). |
| `multichannel`            | bool   | `true`  | Expose a DIRECT "multichannel output" mix port when a device supports 6+ channels. |
| `log.verbose`             | bool   | `false` | VERBOSE instead of DEBUG logging. |
| `card.<selector>.rates`   | string | *(all)* | Comma separated list of sample rates to allow for the card matching `<selector>` (card id, index, or name with spaces replaced by underscores). Empty means all rates. |
| `card.<selector>.bits`    | string | *(all)* | Comma separated list of bit depths (16, 24, 32) to allow for the card matching `<selector>`. Empty means all bit depths. |

## Device model

At start-up the HAL enumerates the sound cards and turns every playback and
capture path into an **endpoint** (`routing/Endpoint.h`), which becomes one
device port. The path comes from the UCM profile of the card when there is one
(the `PlaybackPCM` / `CapturePCM` of each UCM device), otherwise from the PCM
devices of the card with name heuristics ("HDMI", "IEC958", ...).

Every endpoint gets a **role** (`routing/DeviceRole.h`) that decides how
Android sees it:

| Role         | AudioDeviceType / connection | Kind                | Backed by |
|--------------|------------------------------|---------------------|-----------|
| Speaker      | `OUT_SPEAKER`                | attached, *default* | UCM "Speaker", else first analog playback PCM of the primary card |
| Earpiece     | `OUT_SPEAKER_EARPIECE`       | attached            | UCM "Earpiece" / "Handset" |
| Headphones   | `OUT_HEADPHONE` / analog     | external template   | UCM "Headphones" |
| Headset      | `OUT_HEADSET` / analog       | external template   | UCM "Headset" |
| Line Out     | `OUT_DEVICE` / analog        | external template   | UCM "Line", second analog PCM |
| HDMI         | `OUT_DEVICE` / hdmi          | external template   | UCM "HDMI*", PCMs named HDMI |
| SPDIF        | `OUT_DEVICE` / spdif         | external template   | UCM "SPDIF*", PCMs named IEC958 |
| Bus out      | `OUT_BUS` + address          | attached            | everything else (second cards, extra HDMI ports, ...) |
| Mic          | `IN_MICROPHONE` ("bottom")   | attached, *default* | UCM "Mic" / "Internal Mic", else first analog capture PCM |
| Headset Mic  | `IN_HEADSET` / analog        | external template   | UCM "Headset Mic" |
| Bus in       | `IN_BUS` + address           | attached            | everything else (line in, extra mics, ...) |

Rules applied on top:

* Only the *primary card* provides Speaker, Earpiece and Mic; the same roles
  on other cards become bus ports (`<card id>: <device>`), which apps can
  select explicitly but which never hijack the default routing.
* The framework can connect only one external device per type, so only the
  highest priority template of each kind is kept; the others become bus ports.
* A module must have a default output and input. When the primary card has
  no speaker / mic, the best remaining path is promoted (line out, then a bus
  output, headphones, HDMI, S/PDIF). This is how a desktop codec with only a
  line out, or an HDMI-only TV box, still gets a working default output.
* Without any sound card, **null** endpoints are created so that the HAL keeps
  answering the framework: playback is discarded, capture is silence.
* USB sound cards are *not* enumerated statically. They arrive through
  `connectExternalDevice()` (four USB template ports) with the ALSA card /
  device in the address, exactly like the AOSP USB module.

Mix ports:

* `primary output` (PRIMARY): routed to every output device port.
* `multichannel output` (DIRECT): routed to the outputs that accept six or
  more channels; only present when such an output exists.
* `primary input`: routed from every input device port.
* `usb output` / `usb input`: dynamic profiles, routed to the USB templates.

Multichannel PCM data is reordered from Android's `FL FR FC LFE BL BR (SL SR)`
to ALSA's `FL FR BL BR FC LFE (SL SR)` for 5.1 / 7.1.

## Streams

`stream/StreamMainline.cpp` implements the example HAL's `DriverInterface`.
When a stream is patched, `setConnectedDevices()` resolves the device ports
to endpoints, enables their UCM devices (`routing/RoutingController.cpp`,
reference counted, conflicting UCM devices are disabled first) and posts the
list to the worker thread. The worker opens one PCM per endpoint on the next
`start()` / `transfer()`, so routing changes on a running stream are handled
in place. PCM devices are opened as `hw:` first and fall back to `plughw:`
when the hardware does not accept the requested format / rate / channels
natively, which is what makes an arbitrary card "just work" with the
framework's 48 kHz stereo configuration.

Positions come from `snd_pcm_status()`; under- and overruns are recovered
with `snd_pcm_prepare()` and counted.

## Debugging

```sh
adb logcat -s MainlineAudio_Main MainlineAudio_Inventory MainlineAudio_Ucm \
    MainlineAudio_Stream MainlineAudio_AlsaPcm MainlineAudio_Routing
adb shell dumpsys android.hardware.audio.core.IModule/default
setprop vendor.audio.mainline.log.verbose true   # then restart the HAL
```

The `dumpsys` output starts with the effective properties, the cards, every
endpoint with its capabilities and the UCM devices currently enabled.

## Known limitations

* No telephony (`ITelephony` is null): voice calls need modem specific paths.
* No compressed offload, no MMAP / AAudio exclusive mode.
* Master volume and mute are reported as unsupported; the framework applies
  them in software.
* HDMI / DisplayPort connection state is not detected by the HAL (the AIDL
  interface has no way for a HAL to announce a device); on non-TV devices
  HDMI audio is reachable as a bus port or, on HDMI-only devices, as the
  promoted default output.
* Cards that appear after the HAL started are not picked up (USB excepted).
