/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>

namespace libhwdb {

// Applies what udev does to a sysfs attribute substituted with "$attr{...}"
// in a rule (udev_replace_chars() with UDEV_ALLOWED_CHARS_INPUT): characters
// outside udev's allow-list that are not part of a "\x" escape or of a valid
// UTF-8 sequence become '_', whitespace becomes ' '. hwdb match patterns are
// written against udev's output, so lookup keys built from sysfs values have
// to go through the same transformation.
std::string UdevSanitize(const std::string& value);

}  // namespace libhwdb
