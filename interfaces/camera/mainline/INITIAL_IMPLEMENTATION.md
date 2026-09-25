# Initial implementation of Mainline Camera HAL

We want a flexible and generic Android Camera HAL,
for the usage on Android devices running mainline Linux kernel,
which follows proper Linux standards.

You (AI Coding Agent) act as a professional Android HAL engineer
and you've got to implement this in the directory containing this markdown file.

> See the repository root `docs/INITIAL_IMPLEMENTATION_GUIDELINES.md`
> (`hardware/mainline/common/docs/INITIAL_IMPLEMENTATION_GUIDELINES.md`) for
> requirements, references, and guidelines shared by every component. This
> file only lists what's specific to this HAL.

## Reference paths for implementing a Camera AIDL HAL

**Interface itself (`hardware/interfaces/camera/`)**
- `hardware/interfaces/camera/provider/aidl/android/hardware/camera/provider/` — `ICameraProvider`, `ICameraProviderCallback` (device discovery/hotplug).
- `hardware/interfaces/camera/device/aidl/android/hardware/camera/device/` — `ICameraDevice`, `ICameraDeviceSession`, `ICameraDeviceCallback`, `CaptureRequest`/`CaptureResult`/`StreamConfiguration`.
- `hardware/interfaces/camera/metadata/aidl/` — generated `CameraMetadataEnumAndroid*.aidl`, mirrors `system/media/camera/include/system/camera_metadata_tags.h`.
- `hardware/interfaces/camera/common/aidl/` — shared types (`Status`, `VendorTagSection`, `HelperFunctions`).

**Reference/example implementations to model code after**
- `hardware/interfaces/camera/provider/default/` (`ExternalCameraProvider.*`) — a real, working AIDL provider you can pattern a mainline provider after.
- `hardware/interfaces/camera/device/default/` (`ExternalCameraDevice*.cpp/h`, `ExternalCameraDeviceSession.*`, `convert.*`) — a full UVC-webcam-backed `ICameraDevice`/session implementation; closest existing analogue to a mainline (V4L2/UVC) camera HAL.
- `hardware/interfaces/camera/device/default/ExternalCameraUtils.*` — config file (`external_camera_config.xml`) parsing, useful pattern for device-tunable HAL config.

**Framework-side consumer (what calls into the HAL — check when debugging framework interaction)**
- `frameworks/av/services/camera/libcameraservice/device3/aidl/` — `AidlCamera3Device`/session wrapper, the main caller of `ICameraDevice`.
- `frameworks/av/services/camera/libcameraservice/device3/` — HAL-version-agnostic capture request/result pipeline (`Camera3Device`, `Camera3OutputStream`, `Camera3BufferManager`), builds on top of the aidl/ wrapper.
- `frameworks/av/services/camera/libcameraservice/common/` — `CameraProviderManager` (provider discovery over AIDL/HIDL, hotplug), `HalConversionsTemplated.h`.
- `frameworks/av/services/camera/libcameraservice/api2/` — Camera2 API-facing binder service that ultimately drives `device3/`.
- `frameworks/av/camera/aidl/android/` — `hardware/camera2` framework binder AIDL (app ⟷ cameraserver, distinct from the HAL AIDL).
- `frameworks/av/camera/ndk/` — NDK camera API (`libcamera2ndk`) built on the above.

**Metadata/vendor-tag plumbing**
- `system/media/camera/include/system/camera_metadata_tags.h`, `camera_metadata.h` — static tag definitions and the metadata buffer ABI every HAL must produce/consume.
- `system/media/camera/docs/` — `metadata_definitions.xml` and the doc generator; add new vendor tags here if needed.

**Packaging/manifest examples already in this tree**
- `device/mainline/common/optional/external-camera-provider-hal_default-aidl/` — shows how to wire the AOSP `ExternalCameraProvider` into a mainline product (VINTF manifest fragment, init rc, `product.mk`); a good template for packaging your own provider.
- `device/mainline/common/optional/camera-provider-hal_libcamera/`, `camera-provider-hal_emulated/` — other existing provider packaging options to compare against.

## Other reference paths

**Linux Kernel**
- `/android/common/kernel/mainline/android-mainline`: Maintain compatibility with very common camera drivers, such as USB UVC, and the common ones on x86 laptops/tablets.
- `/android/common/kernel/mainline/msm89x7-mainline`: Maintain compatibility with the camera sensor and flashlight drivers used by ARM64 `qcom/msm89{17,37,40}-xiaomi-*.dts`.
- `/android/common/kernel/mainline/msm8953-mainline`: Maintain compatibility with the camera sensor and flashlight drivers used by ARM64 `qcom/msm8953-xiaomi-*.dts`.
- `/android/common/kernel/mainline/sm7150-mainline`: Maintain compatibility with the camera sensor and flashlight drivers used by ARM64 `qcom/sm7150-xiaomi-*.dts`.

**Libraries**
- `external/ffmpeg` (MUST be opt-in, as it only compile for ARM64 so far).
- `external/libyuv`: Useful for pixel format transitions, etc.

## Guidelines

- Please firstly understand the AIDL interface, and then everything else.

## Design

The HAL shall **basically** work on as many as possible devices without any additional configuration.

The HAL will also be used in a generic Android device configuration,
which might also run on x86 laptops/tablets.

If additional configurations is needed, prefer using Android properties
(`vendor.camera.*`). Per-device properties shall accept several selectors of a
device, like the `card.<selector>.*` properties of `../../audio/mainline`.

Register as `ICameraProvider/internal/0`. Package it in the device tree
(`device/mainline/common/optional/`, including SELinux as far as possible,
e.g. `property_contexts` for `vendor.camera.*`),
and block installing it together with other camera provider HALs there.

How to distingulish front/rear camera:
- Read from properties.
- Read from ACPI/DT, or other possible runtime detections.
- Support parsing `/mnt/zpool/disk-0/source/systemd/hwdb.d/70-cameras.hwdb` file. Existing example of hwdb parsing is in `../../sensors/mainline`.
- Add a option to automatically distingulish front/rear camera, by comparing camera sensor resolution (i.e. the smallest one is the front camera).

How to distingulish internal/external camera:
- Read from properties.
- Read from ACPI/DT, or other possible runtime detections.
- Treat all available devices as external by default, also have a option to flip to internal by default.
- Internal cameras report hardware level `LIMITED`, external ones `EXTERNAL`.

Camera IDs shall be stable across boots: derive the order of internal cameras
from their properties (e.g. facing), not from probe order. Merge all V4L2
nodes of one physical device (e.g. UVC metadata nodes) into one camera.

When hotplugged, retry opening a new node for a short while, as ueventd may not
have applied its permissions yet.

Beside regular camera devices, it shall also support other compatible V4L2
class devices such as USB HDMI capture card.

Do not crash when no camera device is present.

Add a option to wait for a number of internal devices to appear.

Things that shall be supported:
- Flashlight (can require configuration from properties)
- RGB pixel format output to Android in addition, when possible, and optionally prefer it over other pixel formats
  (advertising RGB stream formats is off by default, CTS failures when it's enabled are ignored)

Start with V4L2 MMAP buffers and CPU conversion, leave zero-copy (DMABUF) for
later. Keep V4L2 access behind a thin interface, so the request pipeline can be
unit tested against a fake device. Document testing without camera hardware
(`vivid`, `vimc` kernel drivers) in `README.md`.

Left for a follow-up (the ISP is the first one):
- Software ISP for raw Bayer only sensors (e.g. behind Qualcomm CAMSS). Until
  then, detect such sensors and skip them with a log message.
