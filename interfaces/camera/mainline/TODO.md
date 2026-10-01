# Mainline camera HAL: status and TODO

The state of the HAL after its initial implementation, what was verified
where, and what is left. Keep it current: update it in the commit that
changes the state, and remove entries once they are done.

## Status

Everything `INITIAL_IMPLEMENTATION.md` asks for is implemented.

| Area | Verified on |
|------|-------------|
| UVC cameras: discovery, hotplug, preview, still capture (JPEG) | `virtio_arm64only` with a USB camera |
| VTS (`VtsAidlHalCameraProvider_TargetTest`) | passes on `virtio_arm64only`, also with `prefer_rgb` and `advertise_rgb` |
| Media controller sensors, software ISP, facing / rotation from firmware | `msm8953-xiaomi-vince` (`mi8953_a` device tree; ov5675 behind CAMSS): preview, still capture, video |
| Flash and torch | unit tests only; no test device has a flash LED yet |
| Camera hwdb (`70-cameras.hwdb`, installed by the device tree) | not verified: tests pushed only the APEX. It covers few devices, mostly x86 laptops, so it is low priority |
| Discovery format cache (one lookup per video node and code per scan) | not built yet |

Everything up to the software ISP was tested on `virtio_arm64only` with a
USB UVC camera only; the media controller path, the ISP and firmware
placement on `msm8953-xiaomi-vince` once the ISP existed. Test devices run
with SELinux permissive, so the device side policy
(`device/mainline/common`) is not verified in enforcing mode.

Expected to work, from reading the kernels, but untested: msm89x7 (rolex,
prada, land) and sm7150 (davinci) phones; see the platform notes below.

## Known issues

1. `camera_provider_mainline_test` does not run on the device: atest does
   not push the vendor libraries it links (libexif, libyuv, libjpeg,
   libcamera_metadata, ...); `data_libs` did not help; run by hand with
   `LD_LIBRARY_PATH` it segfaults (cause unknown).
2. The flash LED permissions (`ueventd.rc`) and labels (`file_contexts`
   regex relabelled by ueventd) in `device/mainline/common` are unverified:
   check `ls -lZ /sys/class/leds/*:flash*/` on a device with a flash.

## TODO

Correctness first:

1. Fix known issue 1; verify 2.
2. Intel IPU6: its CSI-2 receivers use the streams / routing API
   (`VIDIOC_SUBDEV_G_ROUTING` / `S_ROUTING`). Discovery has to follow only
   routed source pads (or set a route), and `PipelineController` has to set
   formats on routed streams. Add a fake graph shaped like IPU6 to
   `MediaPipelineTest`. The ISYS capture nodes deliver raw Bayer, so the
   software ISP applies.

Quality, mostly for the phones (CAMSS + software ISP):

3. Software ISP: colour correction matrix, lens shading correction,
   denoising / sharpening; multithreaded or NEON demosaicing (full
   resolution, e.g. 13 MP, is slow); flicker (50 / 60 Hz) aware exposure;
   advertise longer minimum frame durations for raw modes where processing
   is slower than the sensor.
4. Autofocus: lens sub-devices (VCM) through ancillary links,
   `V4L2_CID_FOCUS_ABSOLUTE`, a contrast AF algorithm on ISP statistics, and
   the AF modes / states / triggers. Everything is fixed focus now.
5. Exposure compensation (`AE_COMPENSATION_RANGE` is 0 now), through the
   ISP's AE target or V4L2 exposure controls.
6. Flash: real flash current through the LED flash class strobe
   (`flash_brightness`, `flash_timeout`, `flash_strobe`) timed to the
   capture frame; auto flash decided from the ISP's exposure and gain rather
   than preview brightness.
7. Media pipelines: use scaling stages instead of passing sizes through.

Larger features, deferred on purpose:

8. Zero-copy capture (DMABUF) instead of MMAP buffers and CPU copies.
9. FFmpeg for H.264 UVC cameras. Opt-in (it only builds for ARM64); ask the
   maintainer before adding it. libyuv / libjpeg-turbo handle MJPEG.
10. RAW stream output (`RAW_SENSOR`) for raw sensors.
11. Concurrent camera combinations (`getConcurrentCameraIds()` claims none).
12. Manual sensor controls, only if aiming above hardware level `LIMITED`.

## Platform notes

- msm8953 (`mi8953_a`, a unified device tree for as many `msm8953-xiaomi-*`
  devices as possible; tested on `msm8953-xiaomi-vince`): one ov5675 behind
  CAMSS, firmware says front facing with rotation 270. Other `msm8953-xiaomi-*`
  devices are untested. The CAMSS graph gives the sensor 72 paths to 6 video
  nodes. The HAL gives flash LEDs only to an internal back camera, so with
  this front sensor alone there is no torch; set the per-device `facing`
  property if it is really the main camera.
- msm89x7 (rolex, prada, land): ov5675 / ov5670 / s5k3l8 (`s5k2xx` driver),
  SGRBG10; flash LED `white:flash` (`leds-qcom-flash-v1`, PMI8950, no V4L2
  flash). land has a back and a front sensor on one CAMSS: only one can be
  open at a time.
- sm7150 (davinci): ov8856 (SGRBG10) and s5k3l6xx (SGRBG8, no `orientation`
  in the device tree, so it defaults to back); flash `white:flash` and
  `yellow:flash` (`leds-qcom-flash`, PM6150L), switched together. surya,
  tucana and toco have no camera sensors in their device trees.
- No target device tree links a flash to a sensor (`flash-leds`), so flash
  LEDs are assigned by name (see README, "Flash and torch").
- x86 laptops / tablets: never tested; put on hold during the initial
  implementation. Start with UVC cameras, then IPU6.
