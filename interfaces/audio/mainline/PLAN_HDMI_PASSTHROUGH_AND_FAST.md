# Implementation brief: HDMI passthrough and FAST output

This is a hand-over brief from a planning session to the session that will do
the implementation. The plan below has been **confirmed by the maintainer**,
so start implementing, but ask the maintainer if something here turns out to
be wrong or ambiguous once you are in the code. Everything marked
**(verify)** was either not checked, or was inferred from reading code without
running it; check it before relying on it.

This file is a working document, not product documentation. Do not commit it
together with the implementation unless the maintainer asks you to. The
permanent documentation lives in `README.md` / `AGENTS.md`, which the commits
below must update.

Read first, in this order: repository `AGENTS.md` and `docs/` (especially
`CODE_STYLE.md`, `COMMIT_CONVENTIONS.md`, `WORKFLOW.md`, `SCOPE.md`,
`REVIEW.md`), then this directory's `AGENTS.md` and `README.md`. The rules
there apply: no builds / tests by the agent (the maintainer builds and reports
back), clang-format only on touched files (not on the imported `spdif/` fork),
precise searches only (never grep from the AOSP root), commit prefix
`mainline/common: interfaces/audio/mainline: ` (or `intf/audio/mainline`),
`Assisted-by` trailers, README / AGENTS kept in sync in every commit.

Paths below are relative to the AOSP root `/android/LineageOS/24` unless they
start with `/android/common/kernel/...` (the mainline kernel reference tree,
which the maintainer approved as a reference; mainly `sound/`, and
`drivers/gpu/drm/{tegra,bridge/synopsys,msm/dp}` were also consulted).

---

## 1. Goals and scope

1. **HDMI compressed audio passthrough** (bitstreaming, IEC 61937) for the
   formats: AC3, E-AC3, E-AC3-JOC (Atmos in DD+), DTS, DTS-HD (HRA and MA),
   Dolby TrueHD (incl. Atmos, via MAT framing). HBR (8 ch / 192 kHz) formats
   are in scope. Primary test hardware: **NVIDIA Tegra (tegra-hda)**, the only
   hardware the maintainer can verify on. Amlogic (meson, dw-hdmi) is the
   second vendor to keep the design honest (future-proofing, not tested).
2. **FAST output**: make `AudioOutputFlags::FAST` work for real (FastMixer,
   fast tracks), **opt-in** through a property so existing devices keep the
   current behaviour. Per-SoC defaults will be set by device trees (e.g.
   `qcom-common`) through `PRODUCT_VENDOR_PROPERTIES`, outside this directory.
3. **Shared PCM guard**: a direct or passthrough stream must not fail with
   `EBUSY` because `primary output` holds the same exclusive `hw:` PCM (and the
   other way round must not break the direct stream).

### Explicitly deferred (document in README "Known limitations")

* Mixing system sounds into hra / multichannel playback (the guard drops the
  primary output's audio while a direct stream owns the PCM). Maintainer: wait
  for a real use case. Mixing into passthrough is impossible by nature.
* S/PDIF passthrough (no hardware, no known Android device).
* User-forced surround formats beyond what the sink's ELD reports
  (ENCODED_SURROUND_OUTPUT ALWAYS / MANUAL); no TV settings UI for it anyway.
* AC-4, DTS-UHD (DTS:X P2), MPEG-H.
* Qualcomm DP alt mode passthrough (see 6.4).
* MMAP / AAudio exclusive mode, FAST *input*.
* Automated tests. The maintainer is wary of code and tests written by the
  same entity sharing wrong assumptions. If tests are added later, expected
  values must come from an independent source (kernel `/proc/asound/cardN/eld#X`
  dumps for the ELD parser, `ffmpeg -f spdif` output for the packer).

---

## 2. Commit series (in order)

Each commit: one logical change, README / AGENTS updated with it, formatter run
on the touched files (except `spdif/`), `docs/REVIEW.md` self-review before
presenting.

1. **Shared PCM ownership** (section 4). Includes adding the PCM identity
   (card + device number) to `Endpoint`, since the arbiter keys on it.
2. **Opt-in FAST on the primary output** (section 5).
3. **Import `libaudiospdif` unmodified** into `spdif/` (section 7.1), plus a
   row in the repository root `README.md` "Upstreams" table.
4. **Fork: DTS-HD** (IEC 61937 type IV; HRA x4 and MA HBR) (section 7.2).
5. **Fork: TrueHD / MAT** (type 22) and **HBR output** (8 ch 192 kHz) (7.2).
6. **Vendor-neutral passthrough building blocks + vendor boundary**:
   `passthrough/Format`, `Eld`, `SinkCapabilities`, `HdmiControl`,
   `AlsaHdmiControl`, `Quirks`, strict `hw:` open in `alsa::Pcm`
   (sections 6 and 8).
7. **Passthrough mix port, module and stream integration**: `Encoder`,
   `PassthroughSink`, `ModuleMainline` / `ConfigurationBuilder` /
   `StreamMainline` wiring, properties (sections 8 and 9).

---

## 3. Current HAL state (relevant facts)

* `routing/ConfigurationBuilder.cpp`: mix ports `primary output`
  (`PRIMARY`, maxOpen 1 / maxActive 1), optional `multichannel output`
  (`DIRECT`), optional `hra output` (`DIRECT | DIRECT_PCM`), `primary input`,
  `usb output` / `usb input` (flags 0, empty = dynamic profiles). Port names
  are constants in `ConfigurationBuilder.h`. No FAST anywhere.
* `ModuleMainline::getNominalLatencyMs()` returns `properties_.latency_ms`
  (default 20) for every port.
* HDMI: `DeviceRole::kHdmi` -> `OUT_DEVICE` + `CONNECTION_HDMI`, external
  template. `Endpoint::is_hdmi_head`, `jack_control`. Extra heads are bus
  ports. `DeviceInventory::SelectHdmiEndpoint()` picks the plugged head at
  connect / routing time. `ModuleMainline::populateConnectedDevicePort()`
  fills the template's profiles from the selected head.
* `StreamMainline` (worker thread owns all `alsa::Pcm`): `OpenPcms()` opens one
  PCM per active endpoint via `alsa::Pcm::Open()` which tries `hw:` then falls
  back to `plug:`. `MakePcmConfig()`: period = burst/2 (min 64), buffer =
  2 bursts. `TransferOutput()` applies gain and 5.1/7.1 reordering.
  `refinePosition()` subtracts the ALSA delay. `alsa_format_` comes from
  `alsa::ToAlsaFormat()`, which only knows PCM, so encoded formats currently
  fail `init()`.
* `Pcm::TryOpen()` calls `snd_pcm_prepare()` inside the open (DPCM back-end
  constraints are applied at prepare). **Consequence for passthrough:** the
  IEC958 channel status must be written *before* `Pcm::Open()`, because the
  HDA driver samples it at prepare time (see 6.2).
* Threading rules in `AGENTS.md` ("Threading") must be kept: the worker never
  takes routing locks while holding the stream's `lock_`.
* Existing rc already has `capabilities BLOCK_SUSPEND SYS_NICE` and
  `rlimit rtprio 10 10` (`android.hardware.audio.service-aidl.mainline.rc`).

---

## 4. Commit 1: shared PCM ownership

### Why

The audio policy keeps `primary output` open and routed while a direct output
(hra, multichannel, now passthrough) plays to the same device; AudioFlinger
also writes silence for ~3 s after the last track before standby. Both HAL
streams then `snd_pcm_open()` the same exclusive `hw:` device and the second
gets `EBUSY` (`StreamMainline::OpenPcms()` -> `NO_INIT`). With passthrough this
is a realistic failure (start a movie right after a UI click).

### Design

* New `routing/PcmArbiter.{h,cpp}` (or `stream/`, pick what reads best),
  shared like `RoutingController` (created by `ModuleMainline`, passed through
  `StreamDeps`).
* Key: the underlying PCM identity `(card index, device number)`. Store it in
  `Endpoint` at probe time. Get it from `snd_pcm_info_get_card()` /
  `snd_pcm_info_get_device()` on the handle opened in
  `alsa::QueryCapabilities()` (UCM names like `_ucmXXXX.hw:0,3` cannot be
  parsed reliably; AGENTS.md says never gate on the `hw:` prefix). For USB
  endpoints, the ALSA address gives card / device directly. Null endpoints
  have no identity and are never arbitrated.
* Priority: passthrough and direct streams (`DIRECT` flag on the mix port)
  > mixed streams. A direct stream **pre-empts** a mixed holder:
  * The arbiter marks the key as wanted by the higher-priority stream and sets
    an atomic "pre-empted" flag for the holder.
  * The holder's worker checks the flag at every `transfer()` /
    `start()` (bursts are <= ~20 ms apart), closes its PCM and continues on
    the paced `NullDevice` path (audio dropped, timing preserved), then
    releases the key.
  * The direct opener retries `EBUSY` for a bounded time (e.g. a few bursts,
    ~100 ms; pick and document a constant) before giving up.
  * When the direct stream closes its PCM, the mixed stream re-acquires on
    its next `transfer()` (reopen as usual; a short gap is fine).
* Two direct streams on the same PCM: first wins, second fails as today (the
  policy normally does not do this).
* A mixed stream that cannot get the PCM because a direct stream owns it must
  not return an error to the framework; it plays into the null path.
* Worker calls into the arbiter without holding `lock_` (own mutex in the
  arbiter; atomics for the pre-emption flag).
* Log ownership changes (tag `MainlineAudio_...`), include the arbiter state in
  `ModuleMainline::dump()`.
* README / AGENTS: describe the guard, and in "Known limitations" that system
  sounds are dropped while hra / multichannel / passthrough owns the device,
  with mixing deferred.

---

## 5. Commit 2: opt-in FAST on the primary output

### Framework facts (checked in this tree)

* AudioFlinger creates a FastMixer on a MixerThread only when the HAL buffer is
  smaller than the normal sink period: `Threads.cpp` MixerThread constructor
  (~L5199-5228), `kUseFastMixer = FastMixer_Static` (~L233),
  `initFastMixer = mFrameCount < mNormalFrameCount`; normal sink size from
  `kNormalPlaybackPeriodMs` (`persist.audio.normal_playback_period_ms`,
  default 20, ~L181) and `kMaxNormalPlaybackPeriodMs = 24` (~L189),
  computed in `readOutputParameters_l` (~L3376-3405). `DEEP_BUFFER`,
  `SPATIALIZER`, `BIT_PERFECT` threads never get one. The output's FAST flag
  is **not** checked for this.
* Fast track grant (`PlaybackThread::createTrack_l`, ~L2606-2701): linear PCM,
  `sampleRate == HAL rate`, compatible channel mask, `hasFastMixer()`, free
  slot. Otherwise "AUDIO_OUTPUT_FLAG_FAST denied".
* HAL buffer size = `StreamDescriptor.bufferSizeFrames`
  (`StreamHalAidl.cpp` ~L197-208), which `Hal2AidlMapper` takes from
  `AudioPatch.minimumStreamBufferSizeFrames` when the framework passes
  frameCount 0 (`Hal2AidlMapper.cpp` ~L980-982). The example `Module` fills
  that from `getNominalLatencyMs(mixPortConfig)` via
  `calculateBufferSizeFrames` -> `calculateBufferSizeFramesForPcm`
  (`hardware/interfaces/audio/aidl/default/include/core-impl/Module.h`
  ~L223-234): rounds up to a multiple of 16, and **above 512 frames rounds to
  a power of two**. At 48 kHz: 20 ms -> 1024 (no FastMixer, today's state),
  11..20 ms -> 1024 (no FastMixer), 10 ms -> 480 (FastMixer, normal sink 960),
  5 ms -> 240.
* The example HAL raises the stream worker to SCHED_FIFO when the mix port
  config has the FAST flag (`hardware/interfaces/audio/aidl/default/Stream.cpp`
  `setWorkerThreadPriority`, ~L994-1028, called from `initInstance`). Needs
  `SYS_NICE` + rtprio (rc already has them) and SELinux
  `self:capability sys_nice` for `hal_audio_default` **(verify whether the
  platform policy already grants it; document in README "Things the device
  still has to provide")**.
* FastMixer expects each write to block ~one period: a cycle shorter than
  0.5 period makes it sleep, longer than 1.75 period counts as an underrun
  (`fastpath/FastMixer.cpp`, `FastThread.cpp`).
* Policy (`AudioPolicyManager::onNewAudioModulesAvailableInt` ~L7304-7381)
  opens and keeps one output per non-direct mix port. **This is why a
  separate "fast output" port was rejected**: it would be a second concurrent
  stream on the same exclusive PCM, i.e. would need a HAL-internal mixer.
  PRIMARY and FAST on the same port is allowed; with one port, all requests
  land there and AudioFlinger grants FAST purely on `hasFastMixer()`.
* VTS (`VtsHalAudioCoreModuleTargetTest.cpp`): `CheckMixPortsFlags` forbids two
  mix ports with identical non-zero flags routed to the same device (fine);
  patches need `minimumStreamBufferSizeFrames > 0`, `latenciesMs > 0`; the
  position test runs when the burst is small, so reported latency and
  positions must be accurate and monotonic.

### Design

* Property `fast_latency_ms` (int, default **0 = disabled = exactly today's
  configuration**). Document range and the "<= 10 ms at 44.1/48 kHz" caveat
  in README.
* When > 0:
  * `primary output` flags become `PRIMARY | FAST`
    (`ConfigurationBuilder.cpp`).
  * `ModuleMainline::getNominalLatencyMs(portConfig)` returns
    `fast_latency_ms` for configs of the primary output mix port (identify by
    port id; resolve the id from the port name once), `latency_ms` for all
    other ports.
  * At configuration build time, compute the buffer frames the example Module
    will derive (same formula as `calculateBufferSizeFramesForPcm`) for the
    primary port's rates; if it is not below the 20 ms normal sink size
    (rounded up to 16) for 44.1 / 48 kHz, **drop the FAST flag and log a
    warning** (the flag would only buy SCHED_FIFO without a FastMixer).
    Keep the lower latency anyway? Decide and document; suggestion: fall back
    to `latency_ms` entirely so the behaviour is "all or nothing".
  * `StreamMainline::MakePcmConfig()` for FAST streams: period = burst,
    buffer = 2 bursts (so a blocking `snd_pcm_writei()` of one burst paces at
    one period). Non-FAST streams keep period = burst/2, buffer = 2 bursts.
  * After open, if the effective period is larger than the burst (e.g. q6asm
    rounds to multiples of 480 frames), log a warning: FastMixer timing will
    be poor on that endpoint. (10 ms at 48 kHz = 480 frames, which matches
    the q6asm step.)
* The stream's buffer size is fixed when it opens and the latency hook only
  sees the mix port config (not the device), so the property is global, not
  per card.
* README: property table, the FAST behaviour in "Mix ports", SELinux note, and
  that SoC defaults belong in device configs.

---

## 6. Hardware / kernel facts for passthrough

### 6.1 Controls (generic)

* `ELD`: bytes control, interface **PCM**, `device` = PCM device number on HDA
  (`/android/common/kernel/mainline/android-mainline/sound/hda/codecs/hdmi/hdmi.c`
  `hdmi_create_eld_ctl` ~L203-224, `kctl->id.device = device`), on ASoC
  `hdmi-codec` (`sound/soc/codecs/hdmi-codec.c` ~L788, device set at ~L824)
  and on Intel LPE. Empty / zero-size when the ELD is not valid.
* `IEC958 Playback Default` (type IEC958, 4+ status bytes):
  * HDA: interface **MIXER**, one per HDMI PCM, `index` = HDMI ordinal
    (created in `generic_hdmi_build_controls`, `hdmi.c` ~L1927-1935, via
    `snd_hda_create_dig_out_ctls`, `sound/hda/common/codec.c` ~L2400-2440:
    HDMI indices start at 0, an analog S/PDIF on the same bus is moved to
    index 16). Matches alsa-lib's
    `external/mainline-hw-deps/alsa-lib/src/conf/cards/HDA-Intel.conf`
    (`DEVICE=3,CTLINDEX=0`, `DEVICE=7,CTLINDEX=1`, ...). Ordinal = position of
    the PCM among the card's HDMI PCMs sorted by device number **(verify on
    Tegra186+ where DP0/DP1 are separate codecs on one controller; log the
    mapping)**.
  * `hdmi-codec` / LPE: interface **PCM**, `device` = the pcm device of the
    rtd that owns the codec (`hdmi-codec.c` ~L824; LPE: `cards/HdmiLpeAudio.conf`).
  * **DPCM caveat (Amlogic, Qualcomm):** when `hdmi-codec` sits on a back-end
    or codec-to-codec link, `rtd->pcm` is an *internal* PCM whose device
    number is the link id (`sound/soc/soc-pcm.c` ~L2879-2885
    `snd_pcm_new_internal(..., rtd->id, ...)`), not the front-end PCM
    userspace opens. So "device == PCM number" fails there.
* The HAL-side lookup therefore tries, in order (make each a small "locator"
  strategy, see 8.2):
  1. interface PCM, `device` == the opened PCM's device number;
  2. interface MIXER, `index` == HDMI ordinal (HDA);
  3. the only such control on the card (typical single-HDMI DPCM board);
  4. otherwise: passthrough disabled for that head, with a log saying why.
  The same order applies to `ELD`.
* Channel status bytes (IEC 60958 consumer) **(verify values against the
  spec / alsa-lib `iec958.h`)**:
  * AES0: PCM = `0x04` (consumer, not copyright); non-audio =
    `IEC958_AES0_NONAUDIO` (0x02) set in addition -> `0x06`.
  * AES1: `0x82` (original, PCM coder category; alsa-lib `pcm/hdmi.conf`
    default). AES2: `0x00`.
  * AES3 sample-rate code of the *IEC stream* rate: 32k `0x03`, 44.1k `0x00`,
    48k `0x02`, 88.2k `0x08`, 96k `0x0A`, 176.4k `0x0C`, 192k `0x0E`,
    768k (HBR) `0x09`.
  * Restore the previous status on close (RAII guard). The PCM path should
    also clear a stale non-audio bit (e.g. left by a crashed HAL) before
    opening an HDMI head.

### 6.2 HDA / Tegra specifics

* The non-PCM flag of the HDA stream format is derived only from the
  `IEC958 Playback Default` NONAUDIO bit (`check_non_pcm_per_cvt`, `hdmi.c`
  ~L1623-1634; used at prepare ~L1100). The control is bound to the converter
  at PCM open (`snd_hda_spdif_ctls_assign`, ~L932). **Write the status before
  `Pcm::Open()` (which prepares).**
* HBR: `is_hbr_format(format)` = non-PCM and 8 channels (`hdmi.c` ~L680);
  `hdmi_pin_hbr_setup` requires pin capability `AC_PINCAP_HBR`, otherwise
  prepare fails with `-EINVAL` (~L683-735). Intel adds ICT handling.
* Tegra (`sound/hda/codecs/hdmi/tegrahdmi.c`): `tegra_hdmi_pcm_prepare` calls
  the generic prepare, then forwards the HDA format word (including the
  non-PCM bit) to the display driver through scratch registers. Card driver
  string is `"tegra-hda"` (`sound/hda/controllers/tegra.c` ~L273/387); that
  is what `snd_ctl_card_info_get_driver()` returns (use it for the quirk key).
  Display side: Tegra114/124 HDMI `has_hbr = true`, Tegra20/30 `false`
  (`drivers/gpu/drm/tegra/hdmi.c` ~L1720-1765); Tegra186+ SOR enables HBR
  advertising (`drivers/gpu/drm/tegra/sor.c` ~L2019-2020).
* The kernel restricts the HDMI PCM rates / channels to the union of all SADs
  of the ELD (`snd_hdmi_eld_update_pcm_info`, `sound/hda/codecs/hdmi/eld.c`
  ~L189-229, unless `static_hdmi_pcm`), so 8 ch / 192 kHz is only openable
  when the sink advertises e.g. TrueHD. Consistent with ELD-driven profiles.
* ELD parsing reference: `sound/core/pcm_drm_eld.c` (`snd_parse_eld` ~L327)
  and `sound/hda/codecs/hdmi/eld.c`. **GPL: use as a behaviour reference only,
  write our own code.**

### 6.3 Amlogic (meson) specifics (future, untested)

* HDMI path: TDM / I2S (`sound/soc/meson/axg-*`, `aiu-*`) -> `g12a-tohdmitx`
  mux -> `dw-hdmi` via `hdmi-codec`. DPCM card (`meson-card-utils.c`,
  `card.driver_name = dev->driver->name`, e.g. `axg-sound-card` /
  `gx-sound-card` **(verify exact strings)**).
* `dw-hdmi-i2s-audio.c` ~L105 passes the IEC958 channel status (incl.
  NONAUDIO) to the HDMI TX: non-HBR passthrough should work.
* No HBR mode handling on the I2S bridge; an HBR attempt likely "succeeds" and
  sends garbage -> HBR must be **off by default** for such cards (quirk
  `kUndetectable`, enable only via property).
* Controls live on an internal BE/c2c PCM device (see 6.1 DPCM caveat) ->
  locator 3 ("sole control") is the expected path.

### 6.4 Qualcomm DP alt mode (deferred; assessment only)

`drivers/gpu/drm/msm/dp/dp_audio.c` `msm_dp_audio_prepare` only uses the
channel count from `hdmi_codec_params`; the IEC958 channel status is never
programmed, so a sink would treat the stream as PCM. The DSP path (q6apm /
q6asm) is also not known to be bit-exact. Needs kernel work; revisit with
hardware.

### 6.5 ELD layout (for the parser) (verify against CEA-861 / HDA spec)

* Bytes 0-3: header (byte 0 bits 7..3 = ELD version, 2 expected; byte 2 =
  baseline length in 4-byte units).
* Baseline from byte 4: byte 4 bits 4..0 = monitor name length (MNL), bits
  7..5 CEA EDID version; byte 5 bits 7..4 = SAD count, bits 3..2 connection
  type (0 HDMI, 1 DP); byte 7 speaker allocation; bytes 20 .. 20+MNL-1
  monitor name; SADs (3 bytes each) start at byte 20 + MNL.
* SAD: byte 0 bits 6..3 = audio format code, bits 2..0 = max channels - 1;
  byte 1 = sample-rate bitmap (bit0 32k, 1 44.1k, 2 48k, 3 88.2k, 4 96k,
  5 176.4k, 6 192k); byte 2 = format dependent.
* Format codes: 1 LPCM, 2 AC-3, 7 DTS, 10 E-AC-3 (DD+; byte 2 bit 0 = JOC /
  Atmos supported), 11 DTS-HD (HRA/MA not distinguished), 12 MLP / Dolby
  TrueHD (byte 2 bit 0 = Atmos / MAT), 15 extension. Ignore unknown codes.

---

## 7. The `libaudiospdif` fork

### 7.1 Import (commit 3)

* Source: `system/media/audio_utils/spdif/` (AC3FrameScanner, BitFieldParser,
  DTSFrameScanner, FrameScanner, SPDIFDecoder, SPDIFEncoder,
  SPDIFFrameScanner, private headers) and
  `system/media/audio_utils/include/audio_utils/spdif/` (FrameScanner.h,
  SPDIFDecoder.h, SPDIFEncoder.h, SPDIF.h). Apache-2.0; keep the original
  headers. Last upstream commit touching the dir in this tree: `1c6745e9`
  ("Audio: Fix OWNERS"); record the upstream project / revision in a short
  `spdif/README.md` (or the root README Upstreams row).
* Keep the upstream layout and formatting (see `docs/CODE_STYLE.md`,
  "Externally imported components"): do **not** clang-format it.
* Build as its own static library in `Android.bp` (vendor, apex_available for
  our APEX, deps `libcutils`, `liblog`, audio system headers for
  `audio_format_t`). The upstream `libaudiospdif` is not
  `vendor_available`, which is one reason for forking.
* The Decoder can be dropped if unused, or kept to minimise the diff; decide
  and say so in the commit message.
* Add the root `README.md` "Upstreams" table row
  (`interfaces/audio/mainline/spdif` -> AOSP `platform/system/media`,
  `audio_utils/spdif`).

### 7.2 Extensions (commits 4 and 5)

What upstream lacks (checked):
* `SPDIFEncoder::isFormatSupported` / `spdif_rate_multiplier` (`SPDIF.h`
  L31-43, `SPDIFEncoder.cpp` ~L46-96): only AC3, E_AC3, E_AC3_JOC, DTS,
  DTS_HD.
* `DTSFrameScanner.cpp` L33 `// TODO Handle DTS_HD`, L130 forces multiplier 1:
  DTS-HD is sent as a core-only burst.
* No TrueHD / MAT.
* Formats are selected by a fixed switch inside `SPDIFEncoder`: new scanners
  need a change there (fine, we own the fork now).
* The burst output assumes 2 channels (`kSpdifEncodedChannelCount`). For HBR
  the byte stream is the same as a 2 ch stream at 4x the rate; the HAL
  writes it to an 8 ch 192 kHz PCM. Make the output channel count explicit.

To add (IEC 61937-5 / -6 and the Dolby MAT spec; ffmpeg's
`libavformat/spdifenc.c` is a well-known reference but is **not in this tree**
and is LGPL; do not copy it) **(verify all numbers)**:
* DTS-HD, data type 17 (type IV) with the HD sub-type for the repetition
  period: HRA as 2 ch at 4x rate (192 kHz for 48 kHz content), MA as HBR
  (8 ch 192 kHz, 16x in 2 ch-equivalent terms).
* TrueHD, data type 22: collect 24 TrueHD access units into one MAT frame
  (61440 bytes including MAT start/middle/end codes and padding computed from
  the access unit timing), HBR output. `audio/vnd.dolby.mlp` is the AIDL
  encoding for TrueHD.
* Keep the rest of the upstream code unchanged. Put the new code where
  upstream would put it (new `*FrameScanner` files), so the diff to upstream
  stays readable.

---

## 8. Passthrough architecture (vendor boundary)

Maintainer requirement: HDMI passthrough differs a lot between vendors, keep
the code easily refactorable. Everything vendor-specific lives behind one
interface plus a quirk table; the rest is vendor-neutral.

### 8.1 Layout

```
passthrough/
  Format.*            Internal enum EncodedFormat {kAc3, kEac3, kEac3Joc, kDts,
                      kDtsHd, kDtsHdMa, kTrueHd, kIec61937}; table of IEC 61937
                      parameters (rate multiplier, output channels, HBR, burst
                      frames, AES3 code); the ONLY place converting to / from
                      AIDL AudioFormatDescription and the fork's audio_format_t.
  Eld.*               Pure parser: ELD bytes -> SinkCapabilities. No ALSA, no AIDL.
  SinkCapabilities.h  Formats with rates / max channels, plus source (ELD /
                      property override).
  HdmiControl.h       The vendor boundary (8.2).
  AlsaHdmiControl.*   Generic implementation built from locator strategies.
  Quirks.*            Table keyed by CardInfo.driver (and components if needed):
                      locator order, HBR support, notes. Per-card properties
                      override entries.
  Encoder.*           Thin adapter over the forked SPDIFEncoder. Swapping or
                      re-syncing the fork only touches this file.
  PassthroughSink.*   What the stream uses: Open / Write / Position / Flush /
                      Drain / Close. Combines Encoder + HdmiControl + alsa::Pcm.
spdif/                The libaudiospdif fork.
```

### 8.2 Vendor boundary

```cpp
class HdmiControl {
  public:
    virtual ~HdmiControl() = default;
    virtual std::optional<std::vector<uint8_t>> ReadEld() = 0;
    // Returns a guard restoring the previous status on destruction.
    virtual std::unique_ptr<ChannelStatusGuard> SetChannelStatus(const Iec958Status&) = 0;
    virtual HbrSupport Hbr() const = 0;       // kSupported / kUnsupported / kUndetectable
    virtual void ReportHbrFailure() = 0;      // runtime fallback, remembered per head
    virtual std::string Describe() const = 0; // logs / dumpsys
};
std::unique_ptr<HdmiControl> CreateHdmiControl(const alsa::CardInfo&, const routing::Endpoint&);
```

* One implementation now (`AlsaHdmiControl`), parameterised by `Quirks`:
  * `"tegra-hda"` (and HDA in general, e.g. `"HDA-Intel"`): locators
    PCM-device then HDA-ordinal; HBR `kSupported` (runtime fallback handles
    Tegra20/30 and pins without `AC_PINCAP_HBR`, which fail at prepare).
  * Default for unknown cards: locators in the generic order of 6.1; HBR
    `kUndetectable` (off unless enabled by property). **(Decide: is
    "undetectable = off" the right default for unknown HDA-like drivers? HDA
    reports HBR failure as `-EINVAL`, so all HDA drivers can be kSupported.)**
  * Amlogic entry can be added later (HBR `kUndetectable`, sole-control
    locator).
* A vendor that needs something the table cannot express (sysfs ELD, vendor
  controls) gets its own `HdmiControl` subclass registered in the factory. Do
  not create per-vendor classes speculatively.
* HBR runtime state must be thread-safe (read on Binder threads when building
  profiles, written from the worker on failure) and survive stream lifetime
  (keep it in the inventory / a shared registry, keyed by head).

### 8.3 Rules to add to `AGENTS.md`

* Vendor / driver knowledge only in `passthrough/Quirks.*` and `HdmiControl`
  implementations. No driver-name checks anywhere else.
* AIDL types only in `passthrough/Format.*`; the rest of `passthrough/` is
  AIDL-free (like `alsa/`).
* `spdif/` changes only for new frame scanners / formats, and is used only
  through `passthrough/Encoder.*`.
* New vendor checklist: read the kernel driver for control placement and HBR
  behaviour, add a quirk entry (or a subclass), update README hardware notes.

---

## 9. Commit 7: framework integration details

### 9.1 Framework facts (checked in this tree unless marked)

* **Policy needs the encoded formats on the mix port itself.** Direct output
  selection (`AudioPolicyManager::openDirectOutput` ~L1725-1860 ->
  `getProfileForOutput` ~L1227 -> `IOProfile::getCompatibilityScore`
  (`common/managerdefinitions/src/IOProfile.cpp` ~L39-124) with exact format
  match for non-PCM (`common/include/policy.h` ~L228-238)). Listing IEC61937
  only is not enough. Mix port must be `DIRECT` (not `COMPRESS_OFFLOAD`).
  Requested flags must be a subset of the port's; DIRECT must match exactly.
* **Why the HAL packs IEC 61937 itself** (instead of AudioFlinger's
  `SpdifStreamOut`): the fallback (`services/audioflinger/datapath/AudioHwDevice.cpp`
  ~L59-92, `SpdifStreamOut.cpp` ~L57-67) re-opens the same mix port as plain
  `PCM_16_BIT` stereo at rate x multiplier, so the HAL cannot tell it from
  real PCM; it only supports AC3 / E-AC3 / DTS core
  (`system/media/audio_utils/spdif`); and on AIDL `getPresentationPosition`
  is not divided by the multiplier (`AudioStreamOut.cpp` ~L78-88), i.e. E-AC3
  positions would be 4x off **(verify)**. So: accept encoded formats, never
  list PCM on the passthrough port.
* AIDL encodings (`frameworks/av/media/audioaidlconversion/AidlConversionCppNdk.cpp`
  ~L631-785; type NON_PCM, pcm DEFAULT unless noted): AC3 `audio/ac3`, E-AC3
  `audio/eac3`, E-AC3-JOC `audio/eac3-joc`, DTS `audio/vnd.dts`, DTS-HD
  `audio/vnd.dts.hd`, DTS-HD MA `audio/vnd.dts.hd;profile=dtsma`, TrueHD
  `audio/vnd.dolby.mlp`, IEC61937 `audio/x-iec61937` **with pcm INT_16_BIT**.
  Use the constants from the AIDL / media headers if available rather than
  string literals **(check what the example HAL uses)**.
* **Example Module rejects non-PCM buffers**: `Module::calculateBufferSizeFrames`
  (`hardware/interfaces/audio/aidl/default/Module.cpp` ~L396-407) returns
  `EX_UNSUPPORTED_OPERATION` unless `format.type == PCM` (IEC61937 is NON_PCM
  too). It is virtual: override in `ModuleMainline` for our encoded formats
  (buffer size in bytes; frame size for encoded formats is 1 byte, IEC61937
  is 2 x channels, see `hardware/interfaces/audio/aidl/common/include/Utils.h`
  ~L122-147 **(verify)**). Pick sizes large enough for the biggest input unit
  (TrueHD / MAT needs tens of KiB); `ModulePrimary.cpp` ~L58-70 is an example
  override.
* `Module::setAudioPortConfigImpl` (~L1237-1409) validates flags exactly and
  the format / mask / rate against the **HAL's** port profiles; device port
  configs are validated the same way, so the **HDMI device port profiles must
  contain the encoded formats too**.
* **A DIRECT mix port with only dynamic profiles never opens on AIDL**: the
  policy's first open uses the profile's picked config, i.e. format DEFAULT
  for a dynamic profile (`AudioOutputDescriptor.cpp` ~L776-783); the HAL
  returns a suggested config with `applied=false`; `Hal2AidlMapper` retries
  only for PCM / non-DIRECT / offload (`Hal2AidlMapper.cpp` ~L937-958), so
  `DeviceHalAidl` returns BAD_VALUE (~L575) and `updateAudioProfiles` never
  runs (`AudioPolicyManager.cpp` ~L9807-9828). **Workaround chosen:** the
  passthrough port has one **static** profile (`audio/x-iec61937`, INT_16_BIT,
  stereo (+7.1 for HBR), 32k..192k) **plus a dynamic placeholder profile**
  (format DEFAULT, empty masks / rates) so the policy treats the port as
  dynamic. The first open then succeeds with IEC61937, `updateAudioProfiles`
  calls `getAudioMixPort` -> `IModule::getAudioPort(mixPortId)` (no
  `getParameters`), imports what the HAL reports, and re-opens.
  **(verify: that an AIDL AudioProfile with format DEFAULT and empty lists
  converts to a policy dynamic profile (`hasDynamicAudioProfile()`), that
  the example Module and VTS accept such a profile on a mix port, and that
  the whole connect flow works on the device.)**
* Because the port is not dynamic-only, `Module::connectExternalDevice`
  (~L820-828) will **not** copy device profiles into it; the HAL maintains the
  passthrough mix port profiles itself: fill on connect (in
  `onExternalDeviceConnectionChanged`, via `getConfig().ports`), restore the
  static + placeholder set on disconnect (Module only clears the ports it
  copied, ~L896-909). `Hal2AidlMapper::updateAudioPort` (~L1194-1259)
  notices the change and refreshes the port on later connects.
* `AudioPortDeviceExt.encodedFormats` on the HDMI device port feeds the
  policy's "enforced" formats (AUTO mode = reported + enforced,
  `modifySurroundFormats` ~L9532-9584) and `getReportedSurroundFormats`
  (~L6669-6722). Fill it from the ELD in `populateConnectedDevicePort`.
* Surround handling only applies to legacy `AUDIO_DEVICE_OUT_HDMI` / ARC /
  eARC, i.e. AIDL `OUT_DEVICE` + `hdmi` (already what the template uses).
  Bus ports for extra heads do not get passthrough.
* `IConfig::getSurroundSoundConfig` already provides the default list
  (`MainlineConfig`); nothing to change there.
* libaudiohal sends no flags in `openOutputStream`; the stream sees the mix
  port's flags through its port config. `offloadInfo` is only sent for
  COMPRESS_OFFLOAD / HW_AV_SYNC; `ModuleMainline::createOutputStream`
  currently rejects any `offload_info`, keep that.

### 9.2 Configuration / module changes

* `ConfigurationBuilder`: new mix port `hdmi passthrough` (constant in
  `ConfigurationBuilder.h`), flags `DIRECT`, maxOpen 1 / maxActive 1, routed
  only to the HDMI template device port, created only when `hdmi.passthrough`
  is enabled and an HDMI template exists. Initial profiles as in 9.1.
* `ModuleMainline::populateConnectedDevicePort` for the HDMI template: after
  `SelectHdmiEndpoint`, build `HdmiControl`, read the ELD, parse into
  `SinkCapabilities`, apply the property override and the HBR state, then:
  device port profiles = existing PCM profiles + encoded profiles;
  `encodedFormats` = the encoded formats.
* On connect / disconnect, update the passthrough mix port profiles (9.1).
* Encoded profile per format: rates from the SAD, restricted so that
  rate x multiplier is a valid IEC stream rate (E-AC3 x4 -> content <= 48k);
  channel masks: positional masks up to the SAD's max channels (apps request
  the stream's own layout, e.g. 5.1 for AC3). Always keep `audio/x-iec61937`
  (INT_16_BIT, stereo and 7.1, 32k..192k).
* `calculateBufferSizeFrames` override (9.1). `getNominalLatencyMs` for the
  passthrough port: something sensible (e.g. `latency_ms`), document it.

### 9.3 Stream changes

* `ModuleMainline::createOutputStream` (or `StreamMainline` construction)
  detects an encoded / IEC61937 format and gives the stream a
  `PassthroughSink`. `StreamMainline` keeps one branch per DriverInterface
  method; it must not know about vendors.
* `init()`: accept formats `passthrough::Format` knows.
* `setConnectedDevices()`: an encoded stream only accepts the HDMI template
  endpoint (resolve via `SelectHdmiEndpoint` as today); reject anything else.
* Opening (worker thread, lazily like today): acquire the PCM from the arbiter
  with passthrough priority; `HdmiControl::SetChannelStatus()` (non-audio,
  AES3 per IEC rate) **before** `alsa::Pcm::Open()`; open **strict** (no plug
  fallback: new option / entry point in `alsa::Pcm`), `S16_LE`, 2 ch at
  content rate x multiplier, or 8 ch 192 kHz for HBR. On HBR open failure
  (`-EINVAL` at prepare): `ReportHbrFailure()`, log, fail the stream (the
  framework falls back to decoding); profiles drop HBR formats from the next
  connection on.
* `transfer()`: feed raw bytes to the Encoder (not frame aligned; the fork
  buffers partial frames), write produced bursts with blocking writes, report
  all input bytes as consumed. No gain, no channel reordering.
  `audio/x-iec61937` input goes to the PCM unchanged (only the channel status
  is set).
* Positions (`refinePosition`): report **content frames**, i.e. frames of
  the decoded stream at the content rate, which is what AudioTrack expects for
  direct encoded output (SpdifStreamOut's HIDL path does the same by dividing
  by the multiplier): `(iec_frames_written - alsa_delay) / multiplier` in
  2 ch-equivalent terms (HBR: 8 ch 192 kHz frame = 4 two-channel frames at
  768 kHz). Must be monotonic. For app-supplied IEC61937, positions are in
  IEC frames. **(verify how AudioFlinger's DirectOutputThread and AudioTrack
  interpret positions for non-proportional formats on AIDL.)**
* `flush()`: reset the Encoder, drop the PCM. `drain()`: drain the PCM (a
  partial burst in the Encoder is discarded; document). `pause()`: stop
  writing (use `snd_pcm_pause` if `can_pause`, else let it underrun; decide
  after checking what the HDMI sink tolerates on Tegra). `standby()` /
  close: close PCM, restore channel status (guard), release the arbiter.
* Latency: from the PCM delay as today, converted to ms at the IEC rate.

### 9.4 Properties (README table + `Properties.{h,cpp}` per AGENTS.md)

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `fast_latency_ms` | int | `0` | 0 = no FAST (today). > 0: `primary output` becomes PRIMARY \| FAST with this latency; FAST is dropped with a warning if the resulting buffer is >= 20 ms. |
| `hdmi.passthrough` | bool | `true` | Expose the `hdmi passthrough` mix port. |
| `hdmi.passthrough_formats` | string | *(ELD)* | Comma separated forced format list (e.g. `ac3,eac3,eac3-joc,dts,dtshd,dtshd-ma,truehd`) used instead of the ELD, for sinks / bridges with a missing or wrong ELD. |
| `card.<selector>.hbr` | bool | *(quirk)* | Force HBR formats on / off for a card, overriding the quirk table. |

(Names follow the existing style; adjust if the maintainer prefers others.)

---

## 10. Documentation to update along the way

* `README.md`: properties table; "Mix ports" (FAST, `hdmi passthrough`);
  "Streams" (arbiter, passthrough path); "Things the device still has to
  provide" (SELinux `sys_nice`, ELD / IEC958 control availability); a
  hardware notes section (Tegra tested, HDA generic, Amlogic expectations,
  Qualcomm DP not supported); "Known limitations" (deferred items of
  section 1, system sounds dropped while a direct stream owns the device).
* `AGENTS.md`: "Where things are" (`passthrough/`, `spdif/`, arbiter), the
  rules of 8.3, design decisions (why FAST is on the primary port and opt-in,
  why the HAL packs IEC 61937, the static + dynamic profile trick and why,
  DPCM control placement).
* Root `README.md`: Upstreams row for the fork.

## 11. Verification the maintainer will do (for commit messages / README)

Do not claim any of this was tested unless the maintainer reports it:
* dumpsys: new ports / profiles, `encodedFormats` after HDMI connect.
* Passthrough on Tegra with an AVR / TV: AC3, E-AC3, E-AC3-JOC, DTS, DTS-HD
  HRA/MA, TrueHD/Atmos (e.g. Kodi or ExoPlayer passthrough), AVR display
  shows the bitstream format; UI sound during playback does not break it.
* FAST: `dumpsys media.audio_flinger` shows FastMixer on the primary thread
  with `fast_latency_ms` set, fast tracks granted; nothing changes with the
  property unset.
* VTS `VtsHalAudioCoreModuleTargetTest` still passes.
