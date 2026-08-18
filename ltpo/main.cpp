#include <aidl/vendor/qti/hardware/display/config/DisplayType.h>
#include <aidl/vendor/qti/hardware/display/config/BnDisplayConfigCallback.h>
#include <aidl/vendor/qti/hardware/display/config/IDisplayConfig.h>
#include <aidl/vendor/qti/hardware/display/config/QsyncMode.h>
#include <android-base/properties.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using aidl::vendor::qti::hardware::display::config::Attributes;
using aidl::vendor::qti::hardware::display::config::BnDisplayConfigCallback;
using aidl::vendor::qti::hardware::display::config::CameraSmoothOp;
using aidl::vendor::qti::hardware::display::config::Concurrency;
using aidl::vendor::qti::hardware::display::config::DisplayType;
using aidl::vendor::qti::hardware::display::config::IDisplayConfig;
using aidl::vendor::qti::hardware::display::config::IDisplayConfigCallback;
using aidl::vendor::qti::hardware::display::config::QsyncMode;
using aidl::vendor::qti::hardware::display::config::TUIEventType;

constexpr char kDisplayConfigService[] =
        "vendor.qti.hardware.display.config.IDisplayConfig/default";
constexpr char kModeSwitchPendingProperty[] = "sys.oplus.ltpo.mode_switch_pending";
constexpr char kQsyncActiveProperty[] = "sys.oplus.ltpo.qsync_active";
constexpr char kDisplayOnProperty[] = "sys.oplus.ltpo.display_on";
constexpr int32_t kPrimaryDisplay = 0;
constexpr int32_t kHighResolutionOaConfig = 3;
constexpr int32_t kLowResolutionOaConfig = 9;
constexpr int32_t kMinFpsCommand = 0x5c000000;
constexpr int32_t kQsyncNone = 0;
constexpr int32_t kQsyncWaitForCommitEachFrame = 3;
constexpr int32_t kPanelMinFps = 1;
constexpr int kRequiredStableSamples = 4;
constexpr auto kPollInterval = 250ms;
constexpr auto kPostQsyncDelay = 500ms;
constexpr auto kRetryDelay = 1s;
constexpr auto kQsyncCallbackTimeout = 2s;

bool publishState(const char* property, bool value) {
    if (android::base::GetBoolProperty(property, !value) == value) {
        return true;
    }
    if (!android::base::SetProperty(property, value ? "true" : "false")) {
        ALOGE("Unable to set %s=%s", property, value ? "true" : "false");
        return false;
    }
    return true;
}

bool isOaConfig(int32_t config) {
    return config == kHighResolutionOaConfig || config == kLowResolutionOaConfig;
}

bool callSetQsyncMode(const std::shared_ptr<IDisplayConfig>& config, int32_t mode) {
    const auto status =
            config->setQsyncMode(kPrimaryDisplay, static_cast<QsyncMode>(mode));
    if (!status.isOk()) {
        ALOGE("setQsyncMode(0x%08x) failed: %s", mode, status.getDescription().c_str());
        return false;
    }
    return true;
}

bool getActiveConfig(const std::shared_ptr<IDisplayConfig>& config, int32_t* activeConfig) {
    const auto status = config->getActiveConfig(DisplayType::PRIMARY, activeConfig);
    if (!status.isOk()) {
        ALOGE("getActiveConfig failed: %s", status.getDescription().c_str());
        return false;
    }
    return true;
}

std::shared_ptr<IDisplayConfig> connectDisplayConfig() {
    ndk::SpAIBinder binder(AServiceManager_checkService(kDisplayConfigService));
    if (binder.get() == nullptr) {
        return nullptr;
    }
    return IDisplayConfig::fromBinder(binder);
}

class QsyncCallback final : public BnDisplayConfigCallback {
  public:
    ndk::ScopedAStatus notifyCWBBufferDone(
            int32_t, const aidl::android::hardware::common::NativeHandle&) override {
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus notifyQsyncChange(bool enabled, int32_t refreshRate,
                                         int32_t qsyncRefreshRate) override {
        {
            std::lock_guard lock(mMutex);
            mEnabled = enabled;
            mGeneration++;
        }
        mCondition.notify_all();
        ALOGI("QSync post-commit state: enabled=%d refresh=%d floor=%d", enabled,
              refreshRate, qsyncRefreshRate);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus notifyIdleStatus(bool) override {
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus notifyCameraSmoothInfo(CameraSmoothOp, int32_t) override {
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus notifyResolutionChange(int32_t, const Attributes&) override {
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus notifyFpsMitigation(int32_t, const Attributes&, Concurrency) override {
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus notifyTUIEventDone(int32_t, DisplayType, TUIEventType) override {
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus notifyContentFps(const std::string&, int32_t) override {
        return ndk::ScopedAStatus::ok();
    }

    bool waitForAfter(bool enabled, uint64_t generation, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mMutex);
        return mCondition.wait_for(lock, timeout,
                                   [&] {
                                       return mGeneration > generation && mEnabled.has_value() &&
                                               *mEnabled == enabled;
                                   });
    }

    std::optional<bool> enabled() const {
        std::lock_guard lock(mMutex);
        return mEnabled;
    }

    uint64_t generation() const {
        std::lock_guard lock(mMutex);
        return mGeneration;
    }

  private:
    mutable std::mutex mMutex;
    std::condition_variable mCondition;
    std::optional<bool> mEnabled;
    uint64_t mGeneration = 0;
};

bool registerQsyncCallback(const std::shared_ptr<IDisplayConfig>& config,
                           const std::shared_ptr<QsyncCallback>& callback,
                           std::shared_ptr<IDisplayConfigCallback>* registeredCallback) {
    std::shared_ptr<IDisplayConfigCallback> binderCallback = callback;
    int64_t handle = 0;
    auto status = config->registerCallback(binderCallback, &handle);
    if (!status.isOk()) {
        ALOGE("registerCallback failed: %s", status.getDescription().c_str());
        return false;
    }
    status = config->controlQsyncCallback(true);
    if (!status.isOk()) {
        ALOGE("controlQsyncCallback failed: %s", status.getDescription().c_str());
        return false;
    }
    *registeredCallback = std::move(binderCallback);
    return true;
}

bool clearAndDisableQsync(const std::shared_ptr<IDisplayConfig>& config,
                          const std::shared_ptr<QsyncCallback>& callback) {
    if (!callback->enabled().has_value()) {
        const uint64_t generation = callback->generation();
        if (!callSetQsyncMode(config, kQsyncWaitForCommitEachFrame) ||
            !callback->waitForAfter(true, generation, kQsyncCallbackTimeout)) {
            ALOGE("Unable to establish QSync state before cleanup");
            return false;
        }
    }

    if (callback->enabled() == false) return true;

    if (!callSetQsyncMode(config, kMinFpsCommand)) return false;
    std::this_thread::sleep_for(kPollInterval);
    const uint64_t generation = callback->generation();
    if (!callSetQsyncMode(config, kQsyncNone)) return false;
    if (!callback->waitForAfter(false, generation, kQsyncCallbackTimeout)) {
        ALOGE("Timed out waiting for the QSync-disable post-commit callback");
        return false;
    }
    std::this_thread::sleep_for(kPostQsyncDelay);
    return true;
}

} // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    ABinderProcess_startThreadPool();

    std::shared_ptr<IDisplayConfig> config;
    std::shared_ptr<QsyncCallback> qsyncCallback;
    std::shared_ptr<IDisplayConfigCallback> registeredCallback;
    std::optional<int32_t> appliedMinFps;
    bool qsyncOwned = false;
    bool stateKnown = false;
    int stableSamples = 0;
    const auto resetConnection = [&] {
        config.reset();
        qsyncCallback.reset();
        registeredCallback.reset();
    };

    while (true) {
        const bool modeSwitchPending =
                android::base::GetBoolProperty(kModeSwitchPendingProperty, false);
        const bool screenOn = android::base::GetBoolProperty(kDisplayOnProperty, false);

        if (!config) {
            config = connectDisplayConfig();
            if (!config) {
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            qsyncCallback = ndk::SharedRefBase::make<QsyncCallback>();
            if (!registerQsyncCallback(config, qsyncCallback, &registeredCallback)) {
                resetConnection();
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            ALOGI("Connected to QTI DisplayConfig");
            stateKnown = false;
        }

        int32_t activeConfig = -1;
        if (!getActiveConfig(config, &activeConfig)) {
            resetConnection();
            qsyncOwned = false;
            stateKnown = false;
            appliedMinFps.reset();
            stableSamples = 0;
            std::this_thread::sleep_for(kRetryDelay);
            continue;
        }

        if (!screenOn) {
            qsyncOwned = false;
            stateKnown = false;
            appliedMinFps.reset();
            stableSamples = 0;
            publishState(kQsyncActiveProperty, qsyncCallback->enabled() != false);
            std::this_thread::sleep_for(kPollInterval);
            continue;
        }

        if (modeSwitchPending) {
            stableSamples = 0;
            if (isOaConfig(activeConfig) &&
                (qsyncOwned || stateKnown ||
                 android::base::GetBoolProperty(kQsyncActiveProperty, false))) {
                if (!clearAndDisableQsync(config, qsyncCallback)) {
                    resetConnection();
                    std::this_thread::sleep_for(kRetryDelay);
                    continue;
                }
            } else if (android::base::GetBoolProperty(kQsyncActiveProperty, false) &&
                       qsyncCallback->enabled() != false) {
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            qsyncOwned = false;
            stateKnown = false;
            appliedMinFps.reset();
            if (!publishState(kQsyncActiveProperty, false) ||
                !publishState(kModeSwitchPendingProperty, false)) {
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            ALOGI("LTPO released for an active mode switch");
            std::this_thread::sleep_for(kPollInterval);
            continue;
        }

        if (!isOaConfig(activeConfig)) {
            if (android::base::GetBoolProperty(kQsyncActiveProperty, false) &&
                qsyncCallback->enabled() != false) {
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            qsyncOwned = false;
            stateKnown = false;
            appliedMinFps.reset();
            stableSamples = 0;
            publishState(kQsyncActiveProperty, false);
            std::this_thread::sleep_for(kPollInterval);
            continue;
        }

        stableSamples = std::min(stableSamples + 1, kRequiredStableSamples);
        if (stableSamples < kRequiredStableSamples) {
            std::this_thread::sleep_for(kPollInterval);
            continue;
        }

        if (!stateKnown) {
            if (!getActiveConfig(config, &activeConfig) || !isOaConfig(activeConfig)) {
                stableSamples = 0;
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            if (!clearAndDisableQsync(config, qsyncCallback)) {
                resetConnection();
                stableSamples = 0;
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            qsyncOwned = false;
            stateKnown = true;
            appliedMinFps.reset();
            if (!publishState(kQsyncActiveProperty, false)) {
                resetConnection();
                stableSamples = 0;
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            ALOGI("Recovered a known disabled LTPO state");
            std::this_thread::sleep_for(kPollInterval);
            continue;
        }

        if (!qsyncOwned) {
            if (!publishState(kQsyncActiveProperty, true)) {
                resetConnection();
                stableSamples = 0;
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            if (android::base::GetBoolProperty(kModeSwitchPendingProperty, false) ||
                !getActiveConfig(config, &activeConfig) || !isOaConfig(activeConfig)) {
                stableSamples = 0;
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            const uint64_t generation = qsyncCallback->generation();
            if (!callSetQsyncMode(config, kQsyncWaitForCommitEachFrame) ||
                !qsyncCallback->waitForAfter(true, generation, kQsyncCallbackTimeout)) {
                const bool disabled = clearAndDisableQsync(config, qsyncCallback);
                if (disabled) {
                    publishState(kQsyncActiveProperty, false);
                }
                resetConnection();
                qsyncOwned = false;
                stateKnown = false;
                stableSamples = 0;
                ALOGE("Failed to confirm QSync enable post-commit");
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            qsyncOwned = true;
            stateKnown = true;
            appliedMinFps.reset();
            ALOGI("QSync enabled on OA config %d", activeConfig);
            std::this_thread::sleep_for(kPostQsyncDelay);
            continue;
        }

        const int32_t requestedMinFps = kPanelMinFps;
        if (appliedMinFps != requestedMinFps) {
            if (android::base::GetBoolProperty(kModeSwitchPendingProperty, false)) {
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            if (qsyncCallback->enabled() != true ||
                !getActiveConfig(config, &activeConfig) || !isOaConfig(activeConfig)) {
                qsyncOwned = false;
                stateKnown = false;
                appliedMinFps.reset();
                stableSamples = 0;
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            if (!callSetQsyncMode(config, kMinFpsCommand | requestedMinFps)) {
                resetConnection();
                qsyncOwned = false;
                stateKnown = false;
                appliedMinFps.reset();
                stableSamples = 0;
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            std::this_thread::sleep_for(kPostQsyncDelay);
            if (android::base::GetBoolProperty(kModeSwitchPendingProperty, false) ||
                qsyncCallback->enabled() != true ||
                !getActiveConfig(config, &activeConfig) || !isOaConfig(activeConfig)) {
                std::this_thread::sleep_for(kPollInterval);
                continue;
            }
            if (!callSetQsyncMode(config, kMinFpsCommand | requestedMinFps)) {
                resetConnection();
                qsyncOwned = false;
                stateKnown = false;
                appliedMinFps.reset();
                stableSamples = 0;
                std::this_thread::sleep_for(kRetryDelay);
                continue;
            }
            std::this_thread::sleep_for(kPollInterval);
            appliedMinFps = requestedMinFps;
            ALOGI("Minimum FPS set to %d", requestedMinFps);
        }

        std::this_thread::sleep_for(kPollInterval);
    }
}
