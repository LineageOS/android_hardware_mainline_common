# AGENTS.md - Mainline Camera HAL

Guidance for AI coding agents working in this directory. Read `README.md` for
the functional description; this file is about how the code is organised and
the rules to follow when changing it.

> See the repository root `AGENTS.md` (`hardware/mainline/common/AGENTS.md`)
> and `docs/` for shared code style, formatting, workflow, and commit
> conventions. This file only covers what's specific to this directory.

## What this is

`android.hardware.camera.provider` (AIDL V4, `camera.device` V4) HAL for
devices running a mainline Linux kernel, backed by V4L2. One process
(`android.hardware.camera.provider-service.mainline`) registers
`ICameraProvider/internal/0` and serves internal and external cameras alike.
It ships in the vendor APEX `com.android.hardware.camera.provider.mainline`
(manifest name `com.android.hardware.camera.provider`, do not change it).

Commit subject prefix: `mainline/common: intf/camera/mainline: ...`.

## Map of the code

| Path                              | Role |
|-----------------------------------|------|
| `main.cpp`                        | Loads properties, starts the provider, optionally waits for internal cameras, registers the service |
| `Properties.{h,cpp}`              | `vendor.camera.*` -> `struct Properties`, per-device selectors |
| `provider/CameraProvider.*`       | `BnCameraProvider`: camera list, IDs, status callbacks |
| `provider/Discovery.*`            | Classifies `/dev/video*` nodes into `CameraCandidate`s, assigns flash LEDs |
| `provider/DeviceMonitor.*`        | inotify on `/dev`, debounced rescans with retry |
| `provider/CameraIdAllocator.*`    | Stable numerical camera IDs |
| `provider/MediaPipeline.*`        | Media controller sensors: paths to video nodes, deliverable formats (`DiscoverMediaCameras()`) |
| `config/CameraHwdb.*`             | systemd `70-cameras.hwdb` lookups (direction, infrared) via `libhwdb` |
| `device/CameraDevice.*`           | `BnCameraDevice`, one object per camera, handed out repeatedly |
| `device/CameraDescription.*`      | Everything fixed per camera: static metadata, stream validation (`PlanStreams()`) |
| `device/StreamPlanner.*`          | Output sizes / durations, capture mode selection for a set of outputs |
| `device/RequestTemplates.*`       | Default request settings |
| `session/CameraDeviceSession.*`   | `BnCameraDeviceSession`: stream configuration, request validation, buffer cache, FMQs, worker thread |
| `session/CaptureStream.*`         | The session's V4L2 device: format, frame interval, streaming, frame to I420 (raw ones through the ISP) with boottime timestamp |
| `session/DeviceControls.*`        | AE / AWB lock, antibanding, constant frame rate on V4L2 controls |
| `session/PipelineController.*`    | Enables a media pipeline's links, sets its formats, sensor frame interval and controls |
| `session/RequestSettings.*`       | Per-request settings (zoom, fps range, locks, test pattern, flash), result metadata |
| `session/FlashControl.*`          | When a session lights the flash (torch / single / auto / always, pre-flash), AE and flash states |
| `isp/BayerFormat.*`               | Raw Bayer pixel formats the ISP reads: pattern, bit depth, packing, unpacking |
| `isp/SoftIsp.*`                   | CPU ISP: per-colour LUTs (black level, WB, gain, gamma), bilinear demosaic, I420, statistics |
| `isp/Isp3A.*`                     | Gray world AWB, AE on sensor exposure / analogue gain, then digital gain |
| `flash/FlashLed.*`                | One flash LED: LED class device (sysfs) or V4L2 flash sub-device; listing flash LEDs |
| `flash/Flash.*`                   | A camera's flash: torch state and levels, taken over by the open session, torch status listener |
| `session/GraphicBuffers.h`, `GrallocBuffers.cpp` | Output buffer import / lock, abstract for tests |
| `convert/`                        | V4L2 formats to I420, crop / scale to YUV and RGBA outputs (libyuv) |
| `jpeg/JpegEncoder.*`              | I420 to JPEG with libjpeg (raw 4:2:0 input; errors `longjmp` back, never `exit()`) |
| `jpeg/JpegOutput.*`               | BLOB outputs: scaling, thumbnail, EXIF (`android.hardware.camera.common-helper`), `CameraBlob` trailer |
| `v4l2/VideoDevice.h`              | Abstract V4L2 capture node; everything above it is testable with a fake |
| `v4l2/MediaDevice.*`, `v4l2/SubDevice.*` | Media controller topology / link setup and sub-devices, abstract likewise |
| `v4l2/Controls.*`                 | `ControlDevice` (shared by video nodes and sub-devices), ioctl helpers |
| `v4l2/DeviceOpeners.h`            | How video / media / sub-device nodes are opened; tests substitute fakes |
| `v4l2/V4l2VideoDevice.cpp`        | The real implementation (ioctls, sysfs identity) |
| `v4l2/PixelFormats.*`             | Which V4L2 formats are processed / Bayer / unsupported |
| `utils/`                          | sysfs helpers, `common::Status` -> binder status, `Metadata` (camera_metadata_t wrapper) |
| `tests/`                          | `camera_provider_mainline_test`, `FakeVideoDevice`, `FakeGraphicBuffers`, `FakeMediaGraph` |
| `permissions/`                    | Feature XMLs without a prebuilt module in `frameworks/native` |

Build modules: `android.hardware.camera.provider-service.mainline` (binary),
`libcamera_provider_mainline` (static, everything but `main()`),
`camera_provider_mainline_test`, `com.android.hardware.camera.provider.mainline`
(APEX). Soong config namespace `camera_hal_mainline` (`run_as_root`,
`include_all_permission_xmls`, see README); keep the two `.rc` files in sync.

## Hard rules

* Never hard-code device specific values. Derive them from V4L2 / sysfs at
  runtime, or make them per-device properties (see below).
* Only `V4l2VideoDevice.cpp`, `MediaDevice.cpp` and `SubDevice.cpp` talk to
  device nodes, plus `FlashLed.cpp` for flash LEDs. Code above them uses the
  abstract classes, opened through `DeviceOpeners` (or `OpenFlashLed()`), so
  that it can be unit tested with `FakeVideoDevice` and `FakeMediaGraph`.
* Media pipelines: discovery must not change the graph (no link setup, no
  formats); only `PipelineController` does, when a session starts streaming.
* `ClassifyPixelFormat()` returns `kProcessed` only for formats the converter
  handles. Adding a format there means handling it in the converter too.
  Likewise `GetBayerFormat()` lists only what `UnpackBayerRow()` reads.
* Camera IDs must not depend on probe order: sort before allocating.
* Placement (internal / external, facing, rotation) is decided only in
  `ResolvePlacement()` / `ApplyFacingByResolution()` in `Discovery.cpp`, in
  the order documented in the README; keep both in sync and record the
  source in `internal_source` / `facing_source`.
* Keep `kRequestKeys` / `kResultOnlyKeys` in `CameraDescription.cpp` in sync
  with what the session actually handles and reports, and the templates within
  the request keys (a unit test checks the latter).
* Sessions: callbacks into the framework happen on the worker thread only
  (plus `close()` / `flush()` waiting for it); a shutter always precedes the
  result of its frame, and every buffer of every request comes back, with an
  error if need be. The worker is the only user of `CaptureStream` except
  `configureStreams()`, which runs while it is idle.
* Characteristics describe the device, not a session. Anything a session can
  not deliver for every advertised stream combination must not be advertised.
* Calls into the framework (`ICameraProviderCallback`) are made without
  `CameraProvider::lock_` held, serialized by `callback_lock_`. Torch status
  changes are reported from inside `Flash` calls, so never call into a
  `Flash` with `lock_` held (`Detach()` excepted, it reports nothing).
* A camera's flash belongs to its open session (`Flash::Acquire()` in
  `CameraDevice::open()`, `Release()` in the session's `close()`); the torch
  API is refused meanwhile.
* Do not crash when there is no camera, or when a device disappears at any
  point; log and carry on.

## When adding a property

1. Add the field to `struct Properties` (or `DeviceProperties`) with a comment
   and default.
2. Read it in `Properties::Load()` (or `LoadDeviceProperties()`) and print it in
   `ToString()`.
3. Document it in the README table.

## Tests

Unit tests live in `tests/` and run with `atest camera_provider_mainline_test`.
Add tests for new logic that can run without hardware. Do not run them
yourself (see `docs/WORKFLOW.md`); the maintainer does.
