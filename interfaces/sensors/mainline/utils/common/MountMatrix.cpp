/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineSensorsMountMatrix"

#include "libsensors_common/MountMatrix.h"

#include <android-base/logging.h>
#include <android-base/parsedouble.h>
#include <android-base/strings.h>

#include <cstdio>

namespace aidl::android::hardware::sensors::mainline {

MountMatrix::MountMatrix() : values_{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f} {}

std::optional<MountMatrix> MountMatrix::Parse(const std::string& text) {
    std::vector<std::string> rows = ::android::base::Split(text, ";");
    if (rows.size() != 3) {
        LOG(DEBUG) << "Mount matrix '" << text << "' does not have 3 rows";
        return std::nullopt;
    }

    MountMatrix matrix;
    bool all_zero = true;
    for (int row = 0; row < 3; row++) {
        std::vector<std::string> cols = ::android::base::Split(rows[row], ",");
        if (cols.size() != 3) {
            LOG(DEBUG) << "Mount matrix row '" << rows[row] << "' does not have 3 columns";
            return std::nullopt;
        }
        for (int col = 0; col < 3; col++) {
            std::string token = ::android::base::Trim(cols[col]);
            float value = 0.0f;
            if (!::android::base::ParseFloat(token.c_str(), &value)) {
                LOG(DEBUG) << "Mount matrix element '" << token << "' is not a number";
                return std::nullopt;
            }
            matrix.at(row, col) = value;
            if (value != 0.0f) {
                all_zero = false;
            }
        }
    }

    if (all_zero) {
        LOG(DEBUG) << "Mount matrix '" << text << "' is all zero";
        return std::nullopt;
    }
    return matrix;
}

bool MountMatrix::IsIdentity() const {
    return values_ == MountMatrix().values_;
}

void MountMatrix::Apply(float* x, float* y, float* z) const {
    const float in_x = *x;
    const float in_y = *y;
    const float in_z = *z;
    *x = at(0, 0) * in_x + at(0, 1) * in_y + at(0, 2) * in_z;
    *y = at(1, 0) * in_x + at(1, 1) * in_y + at(1, 2) * in_z;
    *z = at(2, 0) * in_x + at(2, 1) * in_y + at(2, 2) * in_z;
}

std::string MountMatrix::ToString() const {
    char buffer[160];
    snprintf(buffer, sizeof(buffer), "%g, %g, %g; %g, %g, %g; %g, %g, %g", at(0, 0), at(0, 1),
             at(0, 2), at(1, 0), at(1, 1), at(1, 2), at(2, 0), at(2, 1), at(2, 2));
    return buffer;
}

}  // namespace aidl::android::hardware::sensors::mainline
