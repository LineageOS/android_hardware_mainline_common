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
| Camera devices, capture sessions          | not yet |
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

An internal camera is listed at boot and faces front or back; an external one
is announced when it appears and faces "external". Cameras are external unless
`default_internal` is set, or the per-device `internal` property says
otherwise.

## Properties

All keys start with `vendor.camera.`. They are read once when the HAL starts;
per-device keys when the device is discovered.

| Key                    | Type | Default | Meaning |
|------------------------|------|---------|---------|
| `default_internal`     | bool | `false` | Treat cameras as internal unless something says otherwise. |
| `wait_internal_count`  | int  | `0`     | Number of internal cameras to wait for before registering the provider (max 64). |
| `wait_internal_ms`     | int  | `10000` | Maximum time to wait for them (max 60000). |
| `external_id_offset`   | int  | `100`   | First camera ID of external cameras. |
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

Example: `setprop vendor.camera.device.usb:046d:082d.internal true`

## Testing

Unit tests (on the device):

```
atest camera_provider_mainline_test
```
