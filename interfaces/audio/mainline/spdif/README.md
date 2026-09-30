# libaudiospdif fork

IEC 61937 packer ("SPDIF encoder") used for HDMI compressed audio
passthrough. It wraps AC-3, E-AC-3 and DTS frames into data bursts that are
sent to the sink as if they were 16-bit PCM.

| Item     | Value |
|----------|-------|
| Upstream | AOSP `platform/system/media`, `audio_utils/spdif` and `audio_utils/include/audio_utils/spdif` |
| Revision | `1c6745e909157d47727317fceaa84b56bb71be96` ("Audio: Fix OWNERS"), the last upstream change of these directories |
| License  | Apache-2.0, original headers kept |

Why a fork: the upstream `libaudiospdif` is not available to vendor
modules, and it lacks what HDMI passthrough needs beyond AC-3 / E-AC-3 / DTS
core (DTS-HD, Dolby TrueHD / MAT, high bit rate output).

## Layout

The files keep their upstream names and formatting so that the diff against
upstream stays readable. Do not run `clang-format` on them.

* `*.cpp`, private `*.h`: from `audio_utils/spdif/`.
* `include/audio_utils/spdif/`: from `audio_utils/include/audio_utils/spdif/`.

Left out on purpose: `SPDIFDecoder` and `SPDIFFrameScanner` (IEC 61937
unpacking, unused here, and the only part that needs `libaudioutils`), and
the `OWNERS` files.

## Local changes

New files carry the LineageOS SPDX header; changes to upstream files follow
the upstream style.

* DTS-HD (`AUDIO_FORMAT_DTS_HD`, `AUDIO_FORMAT_DTS_HD_MA`): new
  `DTSHDFrameScanner`, IEC 61937-5 type IV bursts (data type 17). Upstream
  sent only the DTS core of a DTS-HD stream. A burst holds a core frame and
  the extension substream frame that follows it, behind a 12 byte header
  (start code `01 00 00 00 00 00 00 00 FE FE`, then the DTS-HD frame size),
  and its length code counts bytes. The repetition period is the core's
  sample count times the rate multiplier, which the caller chooses through the
  new `SPDIFEncoder(format, rateMultiplier)` constructor: 4 (stereo at 4x the
  core rate, enough for High Resolution Audio) or 16 (high bit rate, needed
  for Master Audio); the sub-type in the burst info encodes the period
  (512 << sub-type). Defaults: 4 for DTS-HD, 16 for DTS-HD MA. Extension
  substreams without a core (e.g. DTS Express) are skipped. A core without
  an extension substream is sent when the next core arrives.
  The numbers come from IEC 61937-5 / ETSI TS 102 114 as understood without
  the documents at hand and have to be verified, e.g. against the output of
  `ffmpeg -f spdif -dtshd_rate 768000`.
* Dolby TrueHD (`AUDIO_FORMAT_DOLBY_TRUEHD`, incl. Atmos): new
  `TrueHDFrameScanner` and `SPDIFEncoderMat.cpp`, MAT frames in IEC 61937-9
  bursts (data type 22) of 61440 bytes. Access units have no sync word, so
  the scanner waits for a major sync and then follows the access unit
  lengths (`resetBurst()` drops that lock). Each MAT frame (61424 bytes) has
  a start code at offset 0, a middle code at 30708 and an end code at its
  end; the access units go in between, split around the codes where needed,
  each preceded by zero padding up to the position its input timing asks
  for (2560 bytes of MAT stream per 40 samples, codes, preamble and stuffing
  included), so that 24 access units of 40 samples fill one burst. The
  length code counts bytes. `write()` hands TrueHD to `writeMat()`, and the
  sending part of `flushBurstBuffer()` became `sendBurstBuffer()`, because a
  MAT burst ends in the middle of an access unit, where `reset()` must not
  run. Same caveat as above: code values and offsets are to be verified, e.g.
  against `ffmpeg -f spdif` output.
* High bit rate: `getOutputChannelCount()` tells whether the bursts go out
  as 2 channels or, for a rate multiplier of 16 (TrueHD, DTS-HD at 16), as
  8 channels at 4 times the base rate (same byte stream), and
  `getBytesPerOutputFrame()` follows it. Upstream always assumed 2 channels.
