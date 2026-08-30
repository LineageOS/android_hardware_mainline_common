/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "vibrator-impl/Vibrator.h"

#include <android-base/logging.h>
#include <android-base/parsedouble.h>
#include <android-base/properties.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

static constexpr int32_t kComposeDelayMaxMs = 1000;
static constexpr int32_t kComposeSizeMax = 256;

static constexpr int32_t kErrorInvalidDuration = 1;

static constexpr const char* kPropertyDevice = "vendor.vibrator.device";
static constexpr const char* kPropertyResonantFrequency = "vendor.vibrator.resonant_frequency_hz";
static constexpr const char* kPropertyQFactor = "vendor.vibrator.q_factor";

static bool getFloatProperty(const std::string& key, float* out) {
    std::string value = android::base::GetProperty(key, "");
    if (value.empty()) {
        return false;
    }
    return android::base::ParseFloat(value, out);
}

Vibrator::Vibrator() = default;

Vibrator::~Vibrator() {
    stopEffect();
}

bool Vibrator::init() {
    if (!findDevice()) {
        LOG(ERROR) << "Vibrator: no FF-capable input device found";
        return false;
    }
    LOG(INFO) << "Vibrator: using device " << mDevicePath << " (" << mDeviceName << ")";

    if (!queryCapabilities()) {
        LOG(ERROR) << "Vibrator: failed to query FF capabilities";
        return false;
    }
    return true;
}

bool Vibrator::findDevice() {
    std::string propDevice = android::base::GetProperty(kPropertyDevice, "");
    if (!propDevice.empty()) {
        LOG(INFO) << "Vibrator: using device path from property: " << propDevice;
        mEventFd.reset(open(propDevice.c_str(), O_RDWR | O_CLOEXEC));
        if (!mEventFd.ok()) {
            PLOG(ERROR) << "Vibrator: failed to open " << propDevice;
            return false;
        }
        mDevicePath = propDevice;

        char name[256] = {};
        if (ioctl(mEventFd.get(), EVIOCGNAME(sizeof(name) - 1), name) >= 0) {
            mDeviceName = name;
        }
        return true;
    }

    const std::filesystem::path inputDir("/dev/input");
    std::error_code ec;
    std::vector<std::filesystem::path> candidates;

    for (const auto& entry : std::filesystem::directory_iterator(inputDir, ec)) {
        if (entry.is_character_device(ec) &&
            entry.path().filename().string().find("event") != std::string::npos) {
            candidates.push_back(entry.path());
        }
    }

    if (ec) {
        LOG(ERROR) << "Vibrator: error scanning /dev/input: " << ec.message();
        return false;
    }

    std::sort(candidates.begin(), candidates.end());

    for (const auto& path : candidates) {
        android::base::unique_fd fd(open(path.c_str(), O_RDWR | O_CLOEXEC));
        if (!fd.ok()) {
            continue;
        }

        uint8_t evBits[(EV_MAX + 7) / 8] = {};
        if (ioctl(fd.get(), EVIOCGBIT(0, sizeof(evBits)), evBits) < 0) {
            continue;
        }

        if (!(evBits[EV_FF / 8] & (1 << (EV_FF % 8)))) {
            continue;
        }

        uint8_t ffBits[(FF_MAX + 7) / 8] = {};
        if (ioctl(fd.get(), EVIOCGBIT(EV_FF, sizeof(ffBits)), ffBits) < 0) {
            continue;
        }

        bool hasAnyEffect = false;
        for (int i = 0; i <= FF_MAX; i++) {
            if (ffBits[i / 8] & (1 << (i % 8))) {
                hasAnyEffect = true;
                break;
            }
        }

        if (!hasAnyEffect) {
            continue;
        }

        mEventFd = std::move(fd);
        mDevicePath = path.string();

        char name[256] = {};
        if (ioctl(mEventFd.get(), EVIOCGNAME(sizeof(name) - 1), name) >= 0) {
            mDeviceName = name;
        }

        memcpy(&mSupportedFfEffects, ffBits, std::min(sizeof(mSupportedFfEffects), sizeof(ffBits)));
        LOG(INFO) << "Vibrator: found FF device " << mDevicePath << " (" << mDeviceName << ")";
        return true;
    }

    return false;
}

bool Vibrator::queryCapabilities() {
    uint8_t ffBits[(FF_MAX + 7) / 8] = {};
    if (ioctl(mEventFd.get(), EVIOCGBIT(EV_FF, sizeof(ffBits)), ffBits) < 0) {
        PLOG(ERROR) << "Vibrator: failed to query FF effects";
        return false;
    }
    memcpy(&mSupportedFfEffects, ffBits, std::min(sizeof(mSupportedFfEffects), sizeof(ffBits)));

    LOG(INFO) << "Vibrator: FF_RUMBLE "
              << ((mSupportedFfEffects & (1ULL << FF_RUMBLE)) ? "supported" : "not supported");
    LOG(INFO) << "Vibrator: FF_CONSTANT "
              << ((mSupportedFfEffects & (1ULL << FF_CONSTANT)) ? "supported" : "not supported");
    LOG(INFO) << "Vibrator: FF_PERIODIC "
              << ((mSupportedFfEffects & (1ULL << FF_PERIODIC)) ? "supported" : "not supported");

    return true;
}

uint16_t Vibrator::amplitudeToMagnitude(float amplitude) {
    if (amplitude <= 0.0f) {
        return 0;
    }
    if (amplitude > 1.0f) {
        amplitude = 1.0f;
    }
    return static_cast<uint16_t>(amplitude * UINT16_MAX);
}

float Vibrator::getAmplitudeForStrength(EffectStrength strength) {
    switch (strength) {
        case EffectStrength::LIGHT:
            return 0.33f;
        case EffectStrength::MEDIUM:
            return 0.67f;
        case EffectStrength::STRONG:
            return 1.0f;
        default:
            return 0.5f;
    }
}

int32_t Vibrator::getEffectDurationMs(Effect effect) {
    switch (effect) {
        case Effect::CLICK:
            return android::base::GetIntProperty<int32_t>("vendor.vibrator.effect.click.duration_ms",
                                                          30);
        case Effect::DOUBLE_CLICK:
            return android::base::GetIntProperty<int32_t>(
                    "vendor.vibrator.effect.double_click.duration_ms", 80);
        case Effect::TICK:
            return android::base::GetIntProperty<int32_t>("vendor.vibrator.effect.tick.duration_ms",
                                                          15);
        case Effect::THUD:
            return android::base::GetIntProperty<int32_t>("vendor.vibrator.effect.thud.duration_ms",
                                                          50);
        case Effect::POP:
            return android::base::GetIntProperty<int32_t>("vendor.vibrator.effect.pop.duration_ms",
                                                          10);
        case Effect::HEAVY_CLICK:
            return android::base::GetIntProperty<int32_t>(
                    "vendor.vibrator.effect.heavy_click.duration_ms", 50);
        case Effect::TEXTURE_TICK:
            return android::base::GetIntProperty<int32_t>(
                    "vendor.vibrator.effect.texture_tick.duration_ms", 10);
        case Effect::RINGTONE_1:
        case Effect::RINGTONE_2:
        case Effect::RINGTONE_3:
        case Effect::RINGTONE_4:
        case Effect::RINGTONE_5:
        case Effect::RINGTONE_6:
        case Effect::RINGTONE_7:
        case Effect::RINGTONE_8:
        case Effect::RINGTONE_9:
        case Effect::RINGTONE_10:
        case Effect::RINGTONE_11:
        case Effect::RINGTONE_12:
        case Effect::RINGTONE_13:
        case Effect::RINGTONE_14:
        case Effect::RINGTONE_15:
            return android::base::GetIntProperty<int32_t>(
                    "vendor.vibrator.effect.ringtone.duration_ms", 500);
        default:
            return 0;
    }
}

int32_t Vibrator::getPrimitiveDurationMs(CompositePrimitive primitive) {
    switch (primitive) {
        case CompositePrimitive::NOOP:
            return 0;
        case CompositePrimitive::CLICK:
            return 30;
        case CompositePrimitive::THUD:
            return 50;
        case CompositePrimitive::SPIN:
            return 80;
        case CompositePrimitive::QUICK_RISE:
            return 60;
        case CompositePrimitive::SLOW_RISE:
            return 100;
        case CompositePrimitive::QUICK_FALL:
            return 60;
        case CompositePrimitive::LIGHT_TICK:
            return 15;
        case CompositePrimitive::LOW_TICK:
            return 20;
        default:
            return 0;
    }
}

bool Vibrator::uploadEffect(int32_t durationMs, float amplitude) {
    if (!mEventFd.ok()) {
        return false;
    }

    stopEffect();

    uint16_t magnitude = amplitudeToMagnitude(amplitude);

    struct ff_effect effect = {};
    effect.type = FF_RUMBLE;
    effect.id = -1;
    effect.u.rumble.strong_magnitude = magnitude;
    effect.u.rumble.weak_magnitude = magnitude;
    effect.replay.length = static_cast<uint16_t>(std::min(durationMs, static_cast<int32_t>(UINT16_MAX)));
    effect.replay.delay = 0;

    if (ioctl(mEventFd.get(), EVIOCSFF, &effect) < 0) {
        PLOG(ERROR) << "Vibrator: failed to upload FF effect";
        return false;
    }

    mLastEffectId = effect.id;
    LOG(VERBOSE) << "Vibrator: uploaded effect id=" << effect.id << " duration=" << durationMs
                 << "ms amplitude=" << amplitude << " magnitude=" << magnitude;
    return true;
}

bool Vibrator::playEffect(int effectId) {
    if (!mEventFd.ok()) {
        return false;
    }

    struct input_event play = {};
    play.type = EV_FF;
    play.code = static_cast<uint16_t>(effectId);
    play.value = 1;

    if (write(mEventFd.get(), &play, sizeof(play)) == -1) {
        PLOG(ERROR) << "Vibrator: failed to play effect " << effectId;
        return false;
    }

    LOG(VERBOSE) << "Vibrator: playing effect " << effectId;
    return true;
}

bool Vibrator::stopEffect() {
    if (!mEventFd.ok()) {
        return false;
    }

    if (mLastEffectId >= 0) {
        struct input_event stop = {};
        stop.type = EV_FF;
        stop.code = static_cast<uint16_t>(mLastEffectId);
        stop.value = 0;

        if (write(mEventFd.get(), &stop, sizeof(stop)) == -1) {
            PLOG(ERROR) << "Vibrator: failed to stop effect " << mLastEffectId;
        }

        eraseEffect(mLastEffectId);
        mLastEffectId = -1;
    }

    return true;
}

bool Vibrator::eraseEffect(int effectId) {
    if (!mEventFd.ok()) {
        return false;
    }

    if (ioctl(mEventFd.get(), EVIOCRMFF, effectId) < 0) {
        PLOG(ERROR) << "Vibrator: failed to erase effect " << effectId;
        return false;
    }

    LOG(VERBOSE) << "Vibrator: erased effect " << effectId;
    return true;
}

void Vibrator::dispatchVibrate(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    {
        std::lock_guard lock(mMutex);
        if (mIsVibrating) {
            return;
        }
        mVibrationCallback = std::shared_ptr<IVibratorCallback>(callback);
        mIsVibrating = true;
    }

    std::thread([timeoutMs, callback, sharedThis = this->ref<Vibrator>()] {
        LOG(VERBOSE) << "Vibrator: dispatch thread waiting " << timeoutMs << "ms";
        usleep(timeoutMs * 1000);

        if (sharedThis) {
            std::shared_ptr<IVibratorCallback> vibrationCallback, globalCallback;
            {
                std::lock_guard lock(sharedThis->mMutex);
                globalCallback = std::move(sharedThis->mGlobalVibrationCallback);
                sharedThis->mIsVibrating = false;
                sharedThis->mGlobalVibrationCallback = nullptr;
                if (sharedThis->mVibrationCallback == callback) {
                    vibrationCallback = std::move(sharedThis->mVibrationCallback);
                    sharedThis->mVibrationCallback = nullptr;
                } else {
                    vibrationCallback = nullptr;
                }
            }
            if (vibrationCallback) {
                LOG(VERBOSE) << "Vibrator: notifying callback onComplete";
                if (!vibrationCallback->onComplete().isOk()) {
                    LOG(ERROR) << "Vibrator: failed to call onComplete";
                }
            }
            if (globalCallback) {
                LOG(VERBOSE) << "Vibrator: notifying global callback onComplete";
                if (!globalCallback->onComplete().isOk()) {
                    LOG(ERROR) << "Vibrator: failed to call onComplete";
                }
            }
        }
    }).detach();
}

void Vibrator::setGlobalVibrationCallback(const std::shared_ptr<IVibratorCallback>& callback) {
    std::shared_ptr<IVibratorCallback> immediateCallback = nullptr;
    {
        std::lock_guard lock(mMutex);
        if (mIsVibrating) {
            mGlobalVibrationCallback = callback;
        } else {
            immediateCallback = callback;
        }
    }
    if (immediateCallback) {
        std::thread([callback] {
            LOG(VERBOSE) << "Vibrator: notifying global callback onComplete (immediate)";
            if (!callback->onComplete().isOk()) {
                LOG(ERROR) << "Vibrator: failed to call onComplete";
            }
        }).detach();
    }
}

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    LOG(VERBOSE) << "Vibrator: getCapabilities";
    std::lock_guard lock(mMutex);
    if (mCapabilities == 0) {
        int32_t version;
        if (!getInterfaceVersion(&version).isOk()) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_STATE));
        }

        mCapabilities = IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                        IVibrator::CAP_AMPLITUDE_CONTROL | IVibrator::CAP_COMPOSE_EFFECTS |
                        IVibrator::CAP_ALWAYS_ON_CONTROL;

        float resonantFreq = 0.0f;
        if (getFloatProperty(kPropertyResonantFrequency, &resonantFreq) && resonantFreq > 0.0f) {
            mCapabilities |= IVibrator::CAP_GET_RESONANT_FREQUENCY;
        }

        float qFactor = 0.0f;
        if (getFloatProperty(kPropertyQFactor, &qFactor) && qFactor > 0.0f) {
            mCapabilities |= IVibrator::CAP_GET_Q_FACTOR;
        }

        if (version >= 3) {
            mCapabilities |= IVibrator::CAP_PERFORM_VENDOR_EFFECTS;
        }
    }

    *_aidl_return = mCapabilities;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    LOG(VERBOSE) << "Vibrator: off";
    std::shared_ptr<IVibratorCallback> callback, globalCallback;
    {
        std::lock_guard lock(mMutex);
        callback = std::move(mVibrationCallback);
        globalCallback = std::move(mGlobalVibrationCallback);
        mIsVibrating = false;
        mVibrationCallback = nullptr;
        mGlobalVibrationCallback = nullptr;
    }

    stopEffect();

    if (callback || globalCallback) {
        std::thread([callback, globalCallback] {
            if (callback) {
                LOG(VERBOSE) << "Vibrator: notifying callback onComplete (off)";
                if (!callback->onComplete().isOk()) {
                    LOG(ERROR) << "Vibrator: failed to call onComplete";
                }
            }
            if (globalCallback) {
                LOG(VERBOSE) << "Vibrator: notifying global callback onComplete (off)";
                if (!globalCallback->onComplete().isOk()) {
                    LOG(ERROR) << "Vibrator: failed to call onComplete";
                }
            }
        }).detach();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                 const std::shared_ptr<IVibratorCallback>& callback) {
    LOG(VERBOSE) << "Vibrator: on for " << timeoutMs << "ms";

    if (!uploadEffect(timeoutMs, mCurrentAmplitude)) {
        LOG(ERROR) << "Vibrator: failed to upload on effect";
    } else {
        playEffect(mLastEffectId);
    }

    dispatchVibrate(timeoutMs, callback);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength strength,
                                      const std::shared_ptr<IVibratorCallback>& callback,
                                      int32_t* _aidl_return) {
    LOG(VERBOSE) << "Vibrator: perform effect=" << toString(effect)
                 << " strength=" << toString(strength);

    std::vector<Effect> supported;
    getSupportedEffects(&supported);
    if (std::find(supported.begin(), supported.end(), effect) == supported.end()) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }
    if (strength != EffectStrength::LIGHT && strength != EffectStrength::MEDIUM &&
        strength != EffectStrength::STRONG) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    int32_t durationMs = getEffectDurationMs(effect);
    float amplitude = getAmplitudeForStrength(strength);

    {
        std::lock_guard lock(mMutex);
        if (mIsVibrating) {
            *_aidl_return = 0;
            return ndk::ScopedAStatus::ok();
        }
        mVibrationCallback = std::shared_ptr<IVibratorCallback>(callback);
        mIsVibrating = true;
    }

    std::thread([=, sharedThis = this->ref<Vibrator>()] {
        if (effect == Effect::DOUBLE_CLICK) {
            int32_t singleDuration = durationMs / 3;
            int32_t gapDuration = durationMs / 3;

            uploadEffect(singleDuration, amplitude);
            playEffect(mLastEffectId);
            usleep(singleDuration * 1000);
            stopEffect();
            usleep(gapDuration * 1000);
            uploadEffect(singleDuration, amplitude);
            playEffect(mLastEffectId);
            usleep(singleDuration * 1000);
            stopEffect();
        } else {
            uploadEffect(durationMs, amplitude);
            playEffect(mLastEffectId);
            usleep(durationMs * 1000);
            stopEffect();
        }

        if (sharedThis) {
            std::shared_ptr<IVibratorCallback> vibrationCallback, globalCallback;
            {
                std::lock_guard lock(sharedThis->mMutex);
                globalCallback = std::move(sharedThis->mGlobalVibrationCallback);
                sharedThis->mIsVibrating = false;
                sharedThis->mGlobalVibrationCallback = nullptr;
                if (sharedThis->mVibrationCallback == callback) {
                    vibrationCallback = std::move(sharedThis->mVibrationCallback);
                    sharedThis->mVibrationCallback = nullptr;
                }
            }
            if (vibrationCallback) {
                LOG(VERBOSE) << "Vibrator: perform callback onComplete";
                if (!vibrationCallback->onComplete().isOk()) {
                    LOG(ERROR) << "Vibrator: failed to call onComplete";
                }
            }
            if (globalCallback) {
                LOG(VERBOSE) << "Vibrator: perform global callback onComplete";
                if (!globalCallback->onComplete().isOk()) {
                    LOG(ERROR) << "Vibrator: failed to call onComplete";
                }
            }
        }
    }).detach();

    *_aidl_return = durationMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::performVendorEffect(
        const VendorEffect& effect, const std::shared_ptr<IVibratorCallback>& callback) {
    LOG(VERBOSE) << "Vibrator: performVendorEffect";

    int32_t capabilities = 0;
    if (!getCapabilities(&capabilities).isOk()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if ((capabilities & IVibrator::CAP_PERFORM_VENDOR_EFFECTS) == 0) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    EffectStrength strength = effect.strength;
    if (strength != EffectStrength::LIGHT && strength != EffectStrength::MEDIUM &&
        strength != EffectStrength::STRONG) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    float scale = effect.scale;
    if (scale < 0) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    float vendorScale = effect.vendorScale;
    if (vendorScale < 0) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    int32_t durationMs = 0;
    if (!effect.vendorData.getInt("DURATION_MS", &durationMs) || durationMs <= 0) {
        return ndk::ScopedAStatus::fromServiceSpecificError(kErrorInvalidDuration);
    }

    float amplitude = getAmplitudeForStrength(strength);
    if (scale > 0) {
        amplitude *= scale;
    }
    if (vendorScale > 0) {
        amplitude *= vendorScale;
    }
    if (amplitude > 1.0f) {
        amplitude = 1.0f;
    }

    uploadEffect(durationMs, amplitude);
    playEffect(mLastEffectId);
    dispatchVibrate(durationMs, callback);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    *_aidl_return = {
            Effect::CLICK,       Effect::DOUBLE_CLICK, Effect::TICK,   Effect::THUD,
            Effect::POP,         Effect::HEAVY_CLICK,  Effect::TEXTURE_TICK,
    };
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
    LOG(VERBOSE) << "Vibrator: setAmplitude " << amplitude;
    if (amplitude <= 0.0f || amplitude > 1.0f) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_ARGUMENT));
    }

    int32_t capabilities = 0;
    if (!getCapabilities(&capabilities).isOk()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (mExternalControlEnabled) {
        if (!(capabilities & IVibrator::CAP_EXTERNAL_AMPLITUDE_CONTROL)) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
        }
    } else {
        if (!(capabilities & IVibrator::CAP_AMPLITUDE_CONTROL)) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
        }
    }

    mCurrentAmplitude = amplitude;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool enabled) {
    LOG(VERBOSE) << "Vibrator: setExternalControl " << enabled;

    int32_t capabilities = 0;
    if (!getCapabilities(&capabilities).isOk()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (!(capabilities & IVibrator::CAP_EXTERNAL_CONTROL)) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    if (!enabled) {
        stopEffect();
    }
    mExternalControlEnabled = enabled;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* maxDelayMs) {
    *maxDelayMs = kComposeDelayMaxMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* maxSize) {
    *maxSize = kComposeSizeMax;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(std::vector<CompositePrimitive>* supported) {
    *supported = {
            CompositePrimitive::NOOP,       CompositePrimitive::CLICK,
            CompositePrimitive::THUD,       CompositePrimitive::SPIN,
            CompositePrimitive::QUICK_RISE, CompositePrimitive::SLOW_RISE,
            CompositePrimitive::QUICK_FALL, CompositePrimitive::LIGHT_TICK,
            CompositePrimitive::LOW_TICK,
    };
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive,
                                                   int32_t* durationMs) {
    std::vector<CompositePrimitive> supported;
    getSupportedPrimitives(&supported);
    if (std::find(supported.begin(), supported.end(), primitive) == supported.end()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    *durationMs = getPrimitiveDurationMs(primitive);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite,
                                      const std::shared_ptr<IVibratorCallback>& callback) {
    if (composite.size() > static_cast<size_t>(kComposeSizeMax)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    std::vector<CompositePrimitive> supported;
    getSupportedPrimitives(&supported);

    for (const auto& e : composite) {
        if (e.delayMs > kComposeDelayMaxMs) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        if (e.scale < 0.0f || e.scale > 1.0f) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        if (std::find(supported.begin(), supported.end(), e.primitive) == supported.end()) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
    }

    int32_t totalDurationMs = 0;
    for (const auto& e : composite) {
        int32_t durationMs = getPrimitiveDurationMs(e.primitive);
        totalDurationMs += e.delayMs + durationMs;
    }

    std::thread([=, sharedThis = this->ref<Vibrator>()] {
        LOG(VERBOSE) << "Vibrator: starting compose on thread";

        for (const auto& e : composite) {
            if (e.delayMs > 0) {
                usleep(e.delayMs * 1000);
            }

            if (e.primitive == CompositePrimitive::NOOP) {
                continue;
            }

            int32_t durationMs = getPrimitiveDurationMs(e.primitive);
            float amplitude = e.scale > 0.0f ? e.scale * mCurrentAmplitude : 0.0f;

            LOG(VERBOSE) << "Vibrator: compose primitive=" << static_cast<int>(e.primitive)
                         << " duration=" << durationMs << "ms scale=" << e.scale;

            uploadEffect(durationMs, amplitude);
            playEffect(mLastEffectId);
            usleep(durationMs * 1000);
            stopEffect();
        }

        if (callback) {
            LOG(VERBOSE) << "Vibrator: notifying compose onComplete";
            if (!callback->onComplete().isOk()) {
                LOG(ERROR) << "Vibrator: failed to call onComplete";
            }
        }
    }).detach();

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return) {
    return getSupportedEffects(_aidl_return);
}

ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t id, Effect effect, EffectStrength strength) {
    std::vector<Effect> effects;
    getSupportedAlwaysOnEffects(&effects);

    if (std::find(effects.begin(), effects.end(), effect) == effects.end()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    LOG(VERBOSE) << "Vibrator: enabling always-on ID " << id << " with " << toString(effect) << "/"
                 << toString(strength);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t id) {
    LOG(VERBOSE) << "Vibrator: disabling always-on ID " << id;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getResonantFrequency(float* resonantFreqHz) {
    int32_t capabilities = 0;
    if (!getCapabilities(&capabilities).isOk()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (!(capabilities & IVibrator::CAP_GET_RESONANT_FREQUENCY)) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    if (!getFloatProperty(kPropertyResonantFrequency, resonantFreqHz)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getQFactor(float* qFactor) {
    int32_t capabilities = 0;
    if (!getCapabilities(&capabilities).isOk()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if (!(capabilities & IVibrator::CAP_GET_Q_FACTOR)) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }

    if (!getFloatProperty(kPropertyQFactor, qFactor)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getFrequencyResolution(float* freqResolutionHz) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float* freqMinimumHz) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float>* _aidl_return) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t* durationMs) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t* maxSize) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking>* supported) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle>& composite,
                                          const std::shared_ptr<IVibratorCallback>& callback) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getFrequencyToOutputAccelerationMap(
        std::vector<FrequencyAccelerationMapEntry>* _aidl_return) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwleV2PrimitiveDurationMaxMillis(int32_t* maxDurationMs) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwleV2PrimitiveDurationMinMillis(int32_t* minDurationMs) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::getPwleV2CompositionSizeMax(int32_t* maxSize) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

ndk::ScopedAStatus Vibrator::composePwleV2(const CompositePwleV2& composite,
                                            const std::shared_ptr<IVibratorCallback>& callback) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
