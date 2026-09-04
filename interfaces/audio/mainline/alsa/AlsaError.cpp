/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "MainlineAudio_AlsaLib"

#include "alsa/AlsaError.h"

#include <cstdarg>
#include <cstdio>

#include <android-base/logging.h>

namespace aidl::android::hardware::audio::core::mainline::alsa {

namespace {

// alsa-lib prints its diagnostics to stderr by default, which is lost on
// Android. Forward them to logcat instead. UCM lookups produce quite a few
// expected "file not found" messages, so keep them at DEBUG.
void AlsaErrorHandler(const char* file, int line, const char* function, int err, const char* fmt,
                      ...) {
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    LOG(DEBUG) << "alsa-lib: " << function << " (" << file << ":" << line << "): " << message
               << (err != 0 ? " [" + ErrorString(err) + "]" : "");
}

}  // namespace

void InstallAlsaErrorHandler() {
    if (const int err = snd_lib_error_set_handler(AlsaErrorHandler); err < 0) {
        LOG(WARNING) << __func__ << ": snd_lib_error_set_handler failed: " << ErrorString(err);
    }
}

}  // namespace aidl::android::hardware::audio::core::mainline::alsa
