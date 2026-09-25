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
