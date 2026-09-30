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
| `provider/Discovery.*`            | Classifies `/dev/video*` nodes into `CameraCandidate`s |
| `provider/DeviceMonitor.*`        | inotify on `/dev`, debounced rescans with retry |
| `provider/CameraIdAllocator.*`    | Stable numerical camera IDs |
| `device/CameraDevice.*`           | `BnCameraDevice`, one object per camera, handed out repeatedly |
| `device/CameraDescription.*`      | Everything fixed per camera: static metadata, stream validation (`PlanStreams()`) |
| `device/StreamPlanner.*`          | Output sizes / durations, capture mode selection for a set of outputs |
| `device/RequestTemplates.*`       | Default request settings |
| `session/CameraDeviceSession.*`   | `BnCameraDeviceSession`: stream configuration, request validation, buffer cache, FMQs, worker thread |
| `session/CaptureStream.*`         | The session's V4L2 device: format, frame interval, streaming, frame to I420 with boottime timestamp |
| `session/DeviceControls.*`        | AE / AWB lock, antibanding, constant frame rate on V4L2 controls |
| `session/RequestSettings.*`       | Per-request settings (zoom, fps range, locks, test pattern), result metadata |
| `session/GraphicBuffers.h`, `GrallocBuffers.cpp` | Output buffer import / lock, abstract for tests |
| `convert/`                        | V4L2 formats to I420, crop / scale to YUV and RGBA outputs (libyuv) |
| `v4l2/VideoDevice.h`              | Abstract V4L2 capture node; everything above it is testable with a fake |
| `v4l2/V4l2VideoDevice.cpp`        | The real implementation (ioctls, sysfs identity) |
| `v4l2/PixelFormats.*`             | Which V4L2 formats are processed / Bayer / unsupported |
| `utils/`                          | sysfs helpers, `common::Status` -> binder status, `Metadata` (camera_metadata_t wrapper) |
| `tests/`                          | `camera_provider_mainline_test`, `FakeVideoDevice`, `FakeGraphicBuffers` |
| `permissions/`                    | Feature XMLs without a prebuilt module in `frameworks/native` |

Build modules: `android.hardware.camera.provider-service.mainline` (binary),
`libcamera_provider_mainline` (static, everything but `main()`),
`camera_provider_mainline_test`, `com.android.hardware.camera.provider.mainline`
(APEX). Soong config namespace `camera_hal_mainline` (`run_as_root`,
`include_all_permission_xmls`, see README); keep the two `.rc` files in sync.

## Hard rules

* Never hard-code device specific values. Derive them from V4L2 / sysfs at
  runtime, or make them per-device properties (see below).
* Only `V4l2VideoDevice.cpp` talks to V4L2 video nodes. Code above it uses
  `VideoDevice`, so that it can be unit tested with `FakeVideoDevice`.
* `ClassifyPixelFormat()` returns `kProcessed` only for formats the converter
  handles. Adding a format there means handling it in the converter too.
* Camera IDs must not depend on probe order: sort before allocating.
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
  `CameraProvider::lock_` held, serialized by `callback_lock_`.
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
