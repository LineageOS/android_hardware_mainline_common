/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/vibrator/BnVibrator.h>
#include <android-base/thread_annotations.h>
#include <android-base/unique_fd.h>
#include <linux/input.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

class Vibrator : public BnVibrator {
  public:
    Vibrator();
    ~Vibrator() override;

    bool init();

    ndk::ScopedAStatus getCapabilities(int32_t* _aidl_return) override;
    ndk::ScopedAStatus off() override;
    ndk::ScopedAStatus on(int32_t timeoutMs,
                          const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus perform(Effect effect, EffectStrength strength,
                               const std::shared_ptr<IVibratorCallback>& callback,
                               int32_t* _aidl_return) override;
    ndk::ScopedAStatus performVendorEffect(
            const VendorEffect& effect,
            const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus getSupportedEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus setAmplitude(float amplitude) override;
    ndk::ScopedAStatus setExternalControl(bool enabled) override;
    ndk::ScopedAStatus getCompositionDelayMax(int32_t* maxDelayMs) override;
    ndk::ScopedAStatus getCompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus getSupportedPrimitives(std::vector<CompositePrimitive>* supported) override;
    ndk::ScopedAStatus getPrimitiveDuration(CompositePrimitive primitive,
                                            int32_t* durationMs) override;
    ndk::ScopedAStatus compose(const std::vector<CompositeEffect>& composite,
                               const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus alwaysOnEnable(int32_t id, Effect effect, EffectStrength strength) override;
    ndk::ScopedAStatus alwaysOnDisable(int32_t id) override;
    ndk::ScopedAStatus getResonantFrequency(float* resonantFreqHz) override;
    ndk::ScopedAStatus getQFactor(float* qFactor) override;
    ndk::ScopedAStatus getFrequencyResolution(float* freqResolutionHz) override;
    ndk::ScopedAStatus getFrequencyMinimum(float* freqMinimumHz) override;
    ndk::ScopedAStatus getBandwidthAmplitudeMap(std::vector<float>* _aidl_return) override;
    ndk::ScopedAStatus getPwlePrimitiveDurationMax(int32_t* durationMs) override;
    ndk::ScopedAStatus getPwleCompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus getSupportedBraking(std::vector<Braking>* supported) override;
    ndk::ScopedAStatus composePwle(const std::vector<PrimitivePwle>& composite,
                                   const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus getFrequencyToOutputAccelerationMap(
            std::vector<FrequencyAccelerationMapEntry>* _aidl_return) override;
    ndk::ScopedAStatus getPwleV2PrimitiveDurationMaxMillis(int32_t* maxDurationMs) override;
    ndk::ScopedAStatus getPwleV2PrimitiveDurationMinMillis(int32_t* minDurationMs) override;
    ndk::ScopedAStatus getPwleV2CompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus composePwleV2(const CompositePwleV2& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) override;

    void setGlobalVibrationCallback(const std::shared_ptr<IVibratorCallback>& callback);

  private:
    mutable std::mutex mMutex;
    bool mIsVibrating GUARDED_BY(mMutex) = false;
    int32_t mCapabilities GUARDED_BY(mMutex) = 0;
    std::shared_ptr<IVibratorCallback> mVibrationCallback GUARDED_BY(mMutex) = nullptr;
    std::shared_ptr<IVibratorCallback> mGlobalVibrationCallback GUARDED_BY(mMutex) = nullptr;

    ::android::base::unique_fd mEventFd;
    std::string mDevicePath;
    std::string mDeviceName;
    int mLastEffectId = -1;
    float mCurrentAmplitude = 1.0f;
    std::atomic<bool> mExternalControlEnabled{false};

    uint8_t mFfBits[(FF_MAX + 7) / 8] = {};

    float mCachedResonantFrequency = 0.0f;
    float mCachedQFactor = 0.0f;
    int32_t mCachedClickDuration = 0;
    int32_t mCachedDoubleClickDuration = 0;
    int32_t mCachedTickDuration = 0;
    int32_t mCachedThudDuration = 0;
    int32_t mCachedPopDuration = 0;
    int32_t mCachedHeavyClickDuration = 0;
    int32_t mCachedTextureTickDuration = 0;
    int32_t mCachedRingtoneDuration = 0;

    bool findDevice();
    bool queryCapabilities();
    void loadProperties();
    bool uploadEffect(int32_t durationMs, float amplitude);
    bool playEffect(int effectId);
    bool stopEffect();
    bool eraseEffect(int effectId);

    void dispatchVibrate(int32_t timeoutMs, const std::shared_ptr<IVibratorCallback>& callback);

    int32_t getEffectDurationMs(Effect effect) const;
    int32_t getPrimitiveDurationMs(CompositePrimitive primitive) const;
    float getAmplitudeForStrength(EffectStrength strength) const;
    uint16_t amplitudeToMagnitude(float amplitude) const;
    bool hasFfEffect(int type) const;
};

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
