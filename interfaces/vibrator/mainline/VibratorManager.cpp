/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#include "vibrator-impl/VibratorManager.h"
#include "vibrator-impl/VibrationSession.h"

#include <aidl/android/hardware/vibrator/BnVibratorCallback.h>

#include <android-base/logging.h>
#include <thread>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

static constexpr int32_t kDefaultVibratorId = 1;

class VibratorCallback : public BnVibratorCallback {
  public:
    VibratorCallback(int32_t delayMs, std::shared_ptr<IVibrationSession> session,
                     std::shared_ptr<VibratorManager> manager)
        : mDelayMs(delayMs), mSession(std::move(session)), mManager(std::move(manager)) {}
    ndk::ScopedAStatus onComplete() override {
        LOG(VERBOSE) << "VibratorManager: closing session after vibrator became idle";
        usleep(mDelayMs * 1000);
        if (mManager) {
            mManager->clearSession(mSession);
        }
        return ndk::ScopedAStatus::ok();
    }

  private:
    const int32_t mDelayMs;
    std::shared_ptr<IVibrationSession> mSession;
    std::shared_ptr<VibratorManager> mManager;
};

ndk::ScopedAStatus VibratorManager::getCapabilities(int32_t* _aidl_return) {
    LOG(VERBOSE) << "VibratorManager: getCapabilities";
    std::lock_guard lock(mMutex);
    if (mCapabilities == 0) {
        int32_t version;
        if (!getInterfaceVersion(&version).isOk()) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_STATE));
        }
        mCapabilities = IVibratorManager::CAP_SYNC | IVibratorManager::CAP_PREPARE_ON |
                        IVibratorManager::CAP_PREPARE_PERFORM |
                        IVibratorManager::CAP_PREPARE_COMPOSE |
                        IVibratorManager::CAP_MIXED_TRIGGER_ON |
                        IVibratorManager::CAP_MIXED_TRIGGER_PERFORM |
                        IVibratorManager::CAP_MIXED_TRIGGER_COMPOSE |
                        IVibratorManager::CAP_TRIGGER_CALLBACK;

        if (version >= 3) {
            mCapabilities |= IVibratorManager::CAP_START_SESSIONS;
        }
    }

    *_aidl_return = mCapabilities;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::getVibratorIds(std::vector<int32_t>* _aidl_return) {
    LOG(VERBOSE) << "VibratorManager: getVibratorIds";
    *_aidl_return = {kDefaultVibratorId};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::getVibrator(int32_t vibratorId,
                                                 std::shared_ptr<IVibrator>* _aidl_return) {
    LOG(VERBOSE) << "VibratorManager: getVibrator " << vibratorId;
    if (vibratorId == kDefaultVibratorId) {
        *_aidl_return = mDefaultVibrator;
        return ndk::ScopedAStatus::ok();
    } else {
        *_aidl_return = nullptr;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
}

ndk::ScopedAStatus VibratorManager::prepareSynced(const std::vector<int32_t>& vibratorIds) {
    LOG(VERBOSE) << "VibratorManager: prepareSynced";
    if (vibratorIds.size() != 1 || vibratorIds[0] != kDefaultVibratorId) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard lock(mMutex);
    if (mIsPreparing) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    mIsPreparing = true;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::triggerSynced(
        const std::shared_ptr<IVibratorCallback>& callback) {
    LOG(VERBOSE) << "VibratorManager: triggerSynced";
    {
        std::lock_guard lock(mMutex);
        if (!mIsPreparing) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        }
        mIsPreparing = false;
    }
    if (callback) {
        std::thread([callback] {
            LOG(VERBOSE) << "VibratorManager: notifying triggerSynced onComplete";
            callback->onComplete();
        }).detach();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::cancelSynced() {
    LOG(VERBOSE) << "VibratorManager: cancelSynced";
    std::lock_guard lock(mMutex);
    mIsPreparing = false;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::startSession(
        const std::vector<int32_t>& vibratorIds, const VibrationSessionConfig&,
        const std::shared_ptr<IVibratorCallback>& callback,
        std::shared_ptr<IVibrationSession>* _aidl_return) {
    LOG(VERBOSE) << "VibratorManager: startSession";
    *_aidl_return = nullptr;
    int32_t capabilities = 0;
    if (!getCapabilities(&capabilities).isOk()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if ((capabilities & IVibratorManager::CAP_START_SESSIONS) == 0) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }
    if (vibratorIds.size() != 1 || vibratorIds[0] != kDefaultVibratorId) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard lock(mMutex);
    if (mIsPreparing || mSession) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    mSessionCallback = std::shared_ptr<IVibratorCallback>(callback);
    mSession = ndk::SharedRefBase::make<VibrationSession>(this->ref<VibratorManager>());
    *_aidl_return = std::shared_ptr<IVibrationSession>(mSession);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::clearSessions() {
    LOG(VERBOSE) << "VibratorManager: clearSessions";
    abortSession();
    return ndk::ScopedAStatus::ok();
}

void VibratorManager::abortSession() {
    std::shared_ptr<IVibrationSession> session;
    {
        std::lock_guard lock(mMutex);
        session = mSession;
    }
    if (session) {
        mDefaultVibrator->off();
        clearSession(session);
    }
}

void VibratorManager::closeSession(int32_t delayMs) {
    std::shared_ptr<IVibrationSession> session;
    {
        std::lock_guard lock(mMutex);
        session = mSession;
    }
    if (session) {
        auto callback = ndk::SharedRefBase::make<VibratorCallback>(delayMs, session,
                                                                    this->ref<VibratorManager>());
        mDefaultVibrator->setGlobalVibrationCallback(callback);
    }
}

void VibratorManager::clearSession(const std::shared_ptr<IVibrationSession>& session) {
    std::shared_ptr<IVibratorCallback> callback;
    {
        std::lock_guard lock(mMutex);
        if (mSession != session) {
            return;
        }
        callback = std::move(mSessionCallback);
        mSession = nullptr;
        mSessionCallback = nullptr;
    }
    if (callback) {
        std::thread([callback] {
            LOG(VERBOSE) << "VibratorManager: notifying session onComplete";
            if (!callback->onComplete().isOk()) {
                LOG(ERROR) << "VibratorManager: failed to call onComplete";
            }
        }).detach();
    }
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
