# Mainline camera HAL

`android.hardware.camera.provider` (AIDL V4) HAL for devices running a
mainline Linux kernel. It turns V4L2 video capture devices into Android
cameras: USB (UVC) webcams, USB HDMI capture cards and other V4L2 capture
devices that deliver processed images, on phones, tablets, laptops and
generic x86 devices alike, without per-device configuration.

It registers `ICameraProvider/internal/0` and handles both internal (built-in,
front / back facing) and external (hotpluggable) cameras.

## Status

| Feature                                   | State |
|-------------------------------------------|-------|
| Discovery and hotplug of capture nodes    | done |
| Camera devices (characteristics, templates, stream combination queries) | done |
| Capture sessions, YUV and RGBA outputs    | done |
| JPEG outputs                              | not yet (buffers are returned with an error) |
| Sensors behind a media controller pipeline (Qualcomm CAMSS, Intel IPU6, ...) | not yet |
| Raw Bayer sensors (software ISP)          | not yet, detected and skipped |

## Packaging

Everything ships in the vendor APEX `com.android.hardware.camera.provider.mainline`
(APEX manifest name `com.android.hardware.camera.provider`):

* `bin/hw/android.hardware.camera.provider-service.mainline`, run as the init
  service `vendor.camera.provider-mainline` (user `cameraserver`, group
  `camera`),
* its init script and VINTF fragment (`ICameraProvider/internal/0`, version 4).

The service runs in the `hal_camera_default` SELinux domain. Besides what
AOSP grants camera HALs (`/dev/video*`), it needs to read its
`vendor.camera.*` properties, list and watch `/dev` (hotplug) and read sysfs;
`device/mainline/common/sepolicy/vendor/hal_camera_default.te` has these.

Install only one camera provider HAL: this HAL also handles external cameras,
so it must not be combined with the AOSP external camera provider either.

## Building

With `device/mainline/common`:

```makefile
TARGET_CAMERA_PROVIDER_HAL := mainline
```

which installs the APEX and the camera hwdb (see below), and refuses to be
combined with `TARGET_EXTERNAL_CAMERA_PROVIDER_HAL`. Otherwise:

```makefile
PRODUCT_PACKAGES += com.android.hardware.camera.provider.mainline
```

Soong config variables (namespace `camera_hal_mainline`):

| Variable                       | Type | Purpose |
|--------------------------------|------|---------|
| `run_as_root`                  | bool | Run the service as root, so that no device node permissions are needed (development only) |
| `include_all_permission_xmls`  | bool | Ship the feature XMLs of every kind of camera the HAL can provide: `android.hardware.camera` (back), `.front` and `.external` |

The feature XMLs claim what the device has, not what the HAL can do. Without
`include_all_permission_xmls`, the product installs the ones matching its
cameras itself. No XML claims autofocus, flash or a capability level above
`LIMITED`, as the HAL does not provide them.

Example:

```makefile
$(call soong_config_set_bool,camera_hal_mainline,run_as_root,true)
$(call soong_config_set_bool,camera_hal_mainline,include_all_permission_xmls,true)
```

## Camera discovery

Every `/dev/videoN` node is opened and classified:

* Nodes without video capture, memory to memory devices (codecs), output
  devices and nodes without streaming I/O are ignored. UVC metadata nodes
  are ignored this way.
* Nodes whose input has to be configured through the media controller
  (`V4L2_CAP_IO_MC`, e.g. Qualcomm CAMSS, Intel IPU6) are not supported yet.
* Of the remaining nodes, those offering at least one pixel format the HAL can
  convert become cameras: packed and planar YUV, RGB, grey and MJPEG / JPEG.
  Nodes with raw Bayer formats only need a software ISP, which does not exist
  yet; they are logged and skipped.
* All capture nodes of one device (same parent in sysfs, e.g. the USB
  interface of a UVC camera) form one camera; the lowest numbered node is
  used.

`/dev` is watched with inotify, so cameras can come and go at any time. Nodes
that can not be opened yet because ueventd has not applied their permissions
are retried. The HAL starts fine without any camera.

### Camera IDs

Internal cameras get the IDs `0`, `1`, ..., external cameras IDs from
`external_id_offset` (default `100`) on. IDs are handed out in a stable order
(by the sysfs path of the device), not in probe order. A camera that is
replugged into the same port gets its previous ID back as long as no other
camera took it.

The framework reads the list of internal cameras once, when the provider
registers. Set `wait_internal_count` if some internal camera probes late.

### Internal and external cameras

An internal camera is listed at boot, faces front or back and reports the
hardware level `LIMITED`; an external one is announced when it appears, faces
"external" and reports the hardware level `EXTERNAL`. Cameras are external
unless `default_internal` is set, or the per-device `internal` property says
otherwise. Internal cameras face back unless the per-device `facing` property
says otherwise; back facing ones get the lower IDs.

## Camera characteristics

Everything is derived from what the capture node offers:

* Output sizes are the capture sizes of all usable formats, plus 1920x1080,
  1280x720, 640x480, 320x240 and 176x144 where they fit into the largest one.
  Every output size is offered as `PRIVATE`, `YUV_420_888` and `JPEG`, with the
  shortest frame duration of the capture modes that contain it. An output is
  produced by center cropping a capture frame to its aspect ratio and scaling
  it down.
* A session has at most two processed (`PRIVATE` / `YUV_420_888`) streams and
  one `JPEG` stream, all fed from one capture mode. The mode is picked per
  session: the fastest up to 30 fps, then the smallest that contains every
  stream, then uncompressed over MJPEG, then native over emulated formats.
* AE target fps ranges: a fixed range for every frame rate the device offers,
  plus a variable one from 15 fps up.
* Digital zoom up to 4x, center crop only.
* Fixed focus, no flash (yet), no manual sensor or post processing controls.
* V4L2 does not describe optics. Focal length, aperture and physical sensor
  size are nominal values of a typical webcam (3.6 mm wide sensor, 70 degree
  horizontal field of view, f/2.0); they only affect field of view
  calculations in apps.
* `SENSOR_ORIENTATION` is 0 for external cameras, and the per-device
  `rotation` property (default 0) for internal ones.
* With `advertise_rgb`, every output size is also offered as `RGBA_8888`.

## Capture sessions

A session streams from the capture node in the mode picked for its stream
configuration and processes one request per captured frame, in order, on its
own thread:

* Every frame is converted to I420 once (packed / planar / semi-planar YUV,
  RGB and grey with libyuv, MJPEG with libyuv's libjpeg based decoder), then
  center cropped to the zoom region and each output's aspect ratio and scaled
  into the output buffers. Frames the driver flags as corrupt or that fail to
  decode are skipped.
* `PRIVATE` streams get `YUV_420_888` buffers, or `RGBA_8888` ones with
  `prefer_rgb` unless they feed a video encoder.
* The frame interval follows the upper end of the AE target fps range; a
  fixed range also turns off the device's `exposure_auto_priority`, so that
  auto exposure keeps the frame rate. Changing the frame interval restarts
  streaming.
* AE and AWB locks use `V4L2_CID_3A_LOCK`, or freeze the automatic exposure /
  white balance by switching to manual mode with the current value. Power
  line frequency (antibanding) is set to automatic where the device offers it.
  AE and AWB always report converged (or locked); there is no precapture
  metering or focus control.
* The `BLACK` and `SOLID_COLOR` test patterns output black frames (camera
  privacy mode); `SOLID_COLOR` ignores the requested color.
* Timestamps are the driver's frame timestamps converted to
  `CLOCK_BOOTTIME`.
* A device that disappears while streaming ends the session with
  `ERROR_DEVICE`.

## Properties

All keys start with `vendor.camera.`. They are read once when the HAL starts;
per-device keys when the device is discovered.

| Key                    | Type | Default | Meaning |
|------------------------|------|---------|---------|
| `default_internal`     | bool | `false` | Treat cameras as internal unless something says otherwise. |
| `wait_internal_count`  | int  | `0`     | Number of internal cameras to wait for before registering the provider (max 64). |
| `wait_internal_ms`     | int  | `10000` | Maximum time to wait for them (max 60000). |
| `external_id_offset`   | int  | `100`   | First camera ID of external cameras. |
| `prefer_rgb`           | bool | `false` | Write RGBA 8888 instead of YUV into `PRIVATE` streams that do not feed a video encoder, for GPU consumers that handle YUV buffers badly. |
| `advertise_rgb`        | bool | `false` | Also offer RGBA 8888 output streams. Not a format camera apps expect; some CTS tests fail with it. |
| `log.verbose`          | bool | `false` | VERBOSE instead of DEBUG logging. |

### Per-device properties

`vendor.camera.device.<selector>.<key>`, where `<selector>` is any of the
following, most specific first. For every key the most specific selector that
sets it wins.

| Selector       | Example |
|----------------|---------|
| node name      | `video0` |
| bus_info       | `usb-0000:00:14_0-6` |
| USB ID         | `usb:046d:082d` |
| card name      | `HD_Pro_Webcam_C920` |

Characters other than `[0-9A-Za-z]`, `:`, `@` and `-` are replaced by `_`,
including dots. The HAL logs the selectors of every camera it finds:

```
adb logcat -s MainlineCamera_Discovery
```

| Key        | Type | Meaning |
|------------|------|---------|
| `enabled`  | bool | `false` ignores the device. |
| `internal` | bool | Internal (`true`) or external (`false`) camera. |
| `facing`   | string | `back` (or `rear`) / `front`, for internal cameras. Default `back`. |
| `rotation` | int  | `ANDROID_SENSOR_ORIENTATION` of an internal camera: clockwise rotation (0, 90, 180, 270) that makes the image upright on the display in its natural orientation. Default 0. |
| `prefer_rgb` | bool | Overrides the global `prefer_rgb`. |
| `advertise_rgb` | bool | Overrides the global `advertise_rgb`. |

Example: `setprop vendor.camera.device.usb:046d:082d.internal true`

## Testing

Unit tests (on the device):

```
atest camera_provider_mainline_test
```
