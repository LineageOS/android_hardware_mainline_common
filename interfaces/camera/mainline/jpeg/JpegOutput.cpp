/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineCamera_JpegOutput"

#include "jpeg/JpegOutput.h"

#include <cstring>
#include <memory>

#include <CameraMetadata.h>
#include <Exif.h>
#include <aidl/android/hardware/camera/device/CameraBlob.h>
#include <aidl/android/hardware/camera/device/CameraBlobId.h>
#include <android-base/logging.h>
#include <system/camera_metadata.h>

#include "convert/FormatConverter.h"
#include "jpeg/JpegEncoder.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

using ::android::hardware::camera::common::helper::ExifUtils;
using HelperMetadata = ::android::hardware::camera::common::helper::CameraMetadata;

// Largest APP1 segment payload.
constexpr size_t kMaxApp1Size = 65533;

constexpr uint8_t kDefaultQuality = 95;

// Scales the `region` of `source` to `size` into `destination`, full range.
void Prepare(const I420Image& source, const Rect& region, Size size, I420Image* destination) {
    destination->Resize(size.width, size.height);
    destination->full_range = source.full_range;
    ScaleToI420(source, CenterCropToAspect(region, size), destination);
    ToFullRange(destination);
}

// Builds the APP1 (EXIF) segment into `app1`, with the thumbnail if one is
// given and it fits.
bool BuildApp1(const Metadata& settings, const JpegContext& context, Size size,
               const std::vector<uint8_t>& thumbnail, std::vector<uint8_t>* app1) {
    Metadata merged;
    if (context.characteristics != nullptr) merged = *context.characteristics;
    merged.Merge(settings);
    HelperMetadata metadata;
    metadata = merged.Raw();

    for (const bool with_thumbnail : {!thumbnail.empty(), false}) {
        std::unique_ptr<ExifUtils> exif(ExifUtils::create());
        if (exif == nullptr || !exif->initialize()) {
            LOG(ERROR) << "failed to initialize EXIF";
            return false;
        }
        exif->setFromMetadata(metadata, size.width, size.height);
        if (!context.make.empty()) exif->setMake(context.make);
        if (!context.model.empty()) exif->setModel(context.model);
        const bool generated = with_thumbnail
                                       ? exif->generateApp1(thumbnail.data(),
                                                            static_cast<uint32_t>(thumbnail.size()))
                                       : exif->generateApp1(nullptr, 0);
        if (generated && exif->getApp1Length() <= kMaxApp1Size) {
            app1->assign(exif->getApp1Buffer(), exif->getApp1Buffer() + exif->getApp1Length());
            return true;
        }
        if (!with_thumbnail) break;
        LOG(WARNING) << "EXIF with a " << thumbnail.size() << " byte thumbnail too large, "
                     << "dropping the thumbnail";
    }
    LOG(ERROR) << "failed to generate EXIF";
    return false;
}

}  // namespace

bool WriteJpeg(const I420Image& source, const Rect& region, Size size, const Metadata& settings,
               const JpegContext& context, uint8_t* buffer, size_t buffer_size,
               JpegWorkspace* workspace) {
    using device::CameraBlob;
    using device::CameraBlobId;

    if (buffer_size <= sizeof(CameraBlob)) {
        LOG(ERROR) << "BLOB buffer of " << buffer_size << " bytes too small";
        return false;
    }
    const int quality = settings.GetU8(ANDROID_JPEG_QUALITY).value_or(kDefaultQuality);
    const int thumbnail_quality =
            settings.GetU8(ANDROID_JPEG_THUMBNAIL_QUALITY).value_or(kDefaultQuality);

    // Thumbnail, unless 0x0 is asked for.
    workspace->thumbnail_jpeg.clear();
    const auto thumbnail_size = settings.GetI32s(ANDROID_JPEG_THUMBNAIL_SIZE);
    if (thumbnail_size.size() == 2 && thumbnail_size[0] > 0 && thumbnail_size[1] > 0) {
        const Size thumb = {thumbnail_size[0], thumbnail_size[1]};
        Prepare(source, region, thumb, &workspace->thumbnail);
        workspace->thumbnail_jpeg.resize(static_cast<size_t>(thumb.Area()) * 3 + 4096);
        const size_t encoded =
                EncodeJpeg(workspace->thumbnail, thumbnail_quality, {},
                           workspace->thumbnail_jpeg.data(), workspace->thumbnail_jpeg.size());
        workspace->thumbnail_jpeg.resize(encoded);
    }

    if (!BuildApp1(settings, context, size, workspace->thumbnail_jpeg, &workspace->app1)) {
        workspace->app1.clear();
    }

    Prepare(source, region, size, &workspace->image);
    const size_t encoded = EncodeJpeg(workspace->image, quality, workspace->app1, buffer,
                                      buffer_size - sizeof(CameraBlob));
    if (encoded == 0) return false;

    // The framework finds the JPEG size in a trailer at the very end.
    CameraBlob blob;
    blob.blobId = CameraBlobId::JPEG;
    blob.blobSizeBytes = static_cast<int32_t>(encoded);
    memcpy(buffer + buffer_size - sizeof(CameraBlob), &blob, sizeof(CameraBlob));
    return true;
}

}  // namespace aidl::android::hardware::camera::mainline
