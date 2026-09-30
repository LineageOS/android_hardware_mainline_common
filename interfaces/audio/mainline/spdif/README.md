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

None yet.
