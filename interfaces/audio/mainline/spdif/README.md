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
