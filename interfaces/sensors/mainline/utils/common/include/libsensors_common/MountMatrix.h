/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <optional>
#include <string>

namespace aidl::android::hardware::sensors::mainline {

/*
 * 3x3 rotation matrix describing how a 3-axis sensor is mounted relative to the
 * Android device coordinate system, in the same row-major convention as the
 * Linux IIO "mount_matrix" attribute and the systemd hwdb "ACCEL_MOUNT_MATRIX"
 * property: out = M * in, i.e. row i of the matrix produces output axis i.
 */
class MountMatrix {
  public:
    // Identity matrix.
    MountMatrix();

    // Parses "a, b, c; d, e, f; g, h, i". Whitespace is ignored. Returns
    // nullopt for malformed input or an all-zero matrix.
    static std::optional<MountMatrix> Parse(const std::string& text);

    static MountMatrix Identity() { return MountMatrix(); }

    bool IsIdentity() const;

    // Applies the matrix to the vector in place.
    void Apply(float* x, float* y, float* z) const;

    // Returns the matrix in IIO textual form.
    std::string ToString() const;

    const std::array<float, 9>& values() const { return values_; }
    float& at(int row, int col) { return values_[row * 3 + col]; }
    float at(int row, int col) const { return values_[row * 3 + col]; }

  private:
    std::array<float, 9> values_;
};

}  // namespace aidl::android::hardware::sensors::mainline
