/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "device/StreamPlanner.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

#include "v4l2/PixelFormats.h"

namespace aidl::android::hardware::camera::mainline {

namespace {

// Sizes offered in addition to the capture sizes when they fit into the
// largest one: the sizes apps and CTS commonly ask for.
constexpr Size kStandardSizes[] = {
        {1920, 1080}, {1280, 720}, {640, 480}, {320, 240}, {176, 144},
};

// Frame rate up to which a higher one is preferred over a smaller size.
constexpr int64_t kPreferredFrameDurationNs = 1'000'000'000LL / 30;

// Frame rates are rounded to whole numbers; allow a little slack for
// intervals like 1001/30000.
int32_t FrameRate(int64_t frame_duration_ns) {
    if (frame_duration_ns <= 0) return 0;
    return static_cast<int32_t>(std::lround(1e9 / static_cast<double>(frame_duration_ns)));
}

}  // namespace

int64_t CaptureMode::MinFrameDurationNs() const {
    return intervals.empty() ? 0 : intervals.front().ToNanoseconds();
}

StreamPlanner::StreamPlanner(const std::vector<FormatDescription>& formats) {
    for (const auto& format : formats) {
        if (!IsProcessedPixelFormat(format.fourcc)) continue;
        for (const auto& frame_size : format.sizes) {
            if (frame_size.width == 0 || frame_size.height == 0) continue;
            if (frame_size.intervals.empty()) continue;
            CaptureMode mode;
            mode.fourcc = format.fourcc;
            mode.size = {static_cast<int32_t>(frame_size.width),
                         static_cast<int32_t>(frame_size.height)};
            mode.emulated = format.emulated;
            mode.intervals = frame_size.intervals;
            modes_.push_back(std::move(mode));
        }
    }

    for (const auto& mode : modes_) {
        if (mode.size.Area() > max_size_.Area()) max_size_ = mode.size;
    }

    // Capture sizes plus the standard ones that fit, each with the shortest
    // frame duration of the modes that contain it.
    std::vector<Size> sizes;
    for (const auto& mode : modes_) sizes.push_back(mode.size);
    for (const auto& standard : kStandardSizes) {
        if (max_size_.Contains(standard)) sizes.push_back(standard);
    }
    for (const auto& size : sizes) {
        if (IsOutputSize(size)) continue;
        int64_t best = std::numeric_limits<int64_t>::max();
        for (const auto& mode : modes_) {
            if (mode.size.Contains(size)) best = std::min(best, mode.MinFrameDurationNs());
        }
        if (best == std::numeric_limits<int64_t>::max() || best <= 0) continue;
        output_sizes_.push_back({size, best});
    }
    std::sort(output_sizes_.begin(), output_sizes_.end(),
              [](const OutputSize& a, const OutputSize& b) {
                  return std::make_tuple(a.size.Area(), a.size.width) >
                         std::make_tuple(b.size.Area(), b.size.width);
              });
}

bool StreamPlanner::IsOutputSize(const Size& size) const {
    return std::any_of(output_sizes_.begin(), output_sizes_.end(),
                       [&](const OutputSize& output) { return output.size == size; });
}

std::vector<int32_t> StreamPlanner::FrameRates() const {
    std::set<int32_t> rates;
    for (const auto& mode : modes_) {
        for (const auto& interval : mode.intervals) {
            const int32_t rate = FrameRate(interval.ToNanoseconds());
            if (rate > 0) rates.insert(rate);
        }
    }
    return std::vector<int32_t>(rates.begin(), rates.end());
}

std::optional<CaptureMode> StreamPlanner::Plan(const std::vector<Size>& outputs) const {
    Size needed;
    for (const auto& output : outputs) {
        needed.width = std::max(needed.width, output.width);
        needed.height = std::max(needed.height, output.height);
    }

    // Lower is better.
    auto score = [](const CaptureMode& mode) {
        const int64_t duration = std::max(mode.MinFrameDurationNs(), kPreferredFrameDurationNs);
        return std::make_tuple(duration, mode.size.Area(), IsCompressedPixelFormat(mode.fourcc),
                               mode.emulated, mode.fourcc);
    };

    const CaptureMode* best = nullptr;
    for (const auto& mode : modes_) {
        if (!mode.size.Contains(needed)) continue;
        if (best == nullptr || score(mode) < score(*best)) best = &mode;
    }
    if (best == nullptr) return std::nullopt;
    return *best;
}

}  // namespace aidl::android::hardware::camera::mainline
