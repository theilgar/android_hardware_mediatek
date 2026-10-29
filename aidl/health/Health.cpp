/*
 * Copyright (C) 2021 The Android Open Source Project
 * Copyright (C) 2022 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <android-base/logging.h>
#include <android/binder_interface_utils.h>
#include <health-impl/Health.h>
#include <health/utils.h>

#include <algorithm>

#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>

#ifndef CHARGER_FORCE_NO_UI
#define CHARGER_FORCE_NO_UI 0
#endif

#if !CHARGER_FORCE_NO_UI
#include <health-impl/ChargerUtils.h>
#endif

using aidl::android::hardware::health::HalHealthLoop;
using aidl::android::hardware::health::Health;

#if !CHARGER_FORCE_NO_UI
using aidl::android::hardware::health::charger::ChargerCallback;
using aidl::android::hardware::health::charger::ChargerModeMain;
#endif

static constexpr const char* gInstanceName = "default";

namespace aidl::android::hardware::health {
// Some MTK fuel gauge drivers report POWER_SUPPLY_PROP_CHARGE_COUNTER in mAh instead of the
// µAh Android expects (e.g. 2821 at 71% of a 4053 mAh battery), which leaves BatteryStats and
// the Settings battery usage screen with a near-zero charge. Opt in per device with
// ro.vendor.health.charge_counter_in_mah=true to scale it back to µAh.
class MediatekHealth : public Health {
  public:
    using Health::Health;

    // BatteryManager's BATTERY_PROPERTY_CHARGE_COUNTER reads the node directly, not HealthInfo.
    ndk::ScopedAStatus getChargeCounterUah(int32_t* out) override {
        auto status = Health::getChargeCounterUah(out);
        if (status.isOk()) ScaleChargeCounter(out);
        return status;
    }

  protected:
    void UpdateHealthInfo(HealthInfo* health_info) override {
        Health::UpdateHealthInfo(health_info);
        ScaleChargeCounter(&health_info->batteryChargeCounterUah);
        if (mStaticUsbMax && health_info->chargerUsbOnline && IsStandardUsbPort()) {
            health_info->maxChargingCurrentMicroamps = kStandardUsbCurrentUa;
            health_info->maxChargingVoltageMicrovolts = kStandardUsbVoltageUv;
        } else if (mStaticUsbMax &&
                   (health_info->chargerAcOnline || health_info->chargerUsbOnline)) {
            ReportNegotiatedPower(health_info);
        }
    }

  private:
    void ScaleChargeCounter(int32_t* value) const {
        if (mScaleChargeCounter && *value > 0 && *value < kMaxPlausibleMah) *value *= 1000;
    }

    // Some charger drivers (e.g. Xiaomi hq_chg) report constant current_max/voltage_max for the
    // usb supply whatever is plugged in, so a PC port looks like a 100+ W charger and the lock
    // screen says "Charging rapidly". Opt in with ro.vendor.health.usb_max_is_static=true to
    // report a standard USB port (detected charger type USB = SDP) as 500 mA at 5 V instead.
    static bool IsStandardUsbPort() {
        std::string type;
        if (!::android::base::ReadFileToString(kUsbRealTypePath, &type)) return false;
        return ::android::base::Trim(type) == "USB";
    }

    // The same drivers do classify the charger in quick_charge_type (0 normal, 1 fast, 2 flash,
    // 3 turbo, 4 super) and report the negotiated adapter power in power_max (W). Map them to a
    // power at a nominal 5 V for the framework's charging speed thresholds (AOSP: slow < 5 W,
    // fast > 7.5 W): a plain 5 V charger stays at the regular-charging limit, PD/QC chargers
    // report 18-25 W and only the vendor's turbo/super chargers report their full power.
    static void ReportNegotiatedPower(HealthInfo* health_info) {
        int qc, watts = 0;
        if (!ReadInt(kUsbQuickChargeTypePath, &qc) || qc < 0) return;
        ReadInt(kUsbPowerMaxPath, &watts);
        if (qc >= kQuickChargeTurbo) {
            watts = watts > 0 ? std::min(watts, kMaxTurboW) : kDefaultTurboW;
        } else if (qc > 0) {
            watts = std::clamp(watts > 0 ? watts : kDefaultFastW, kDefaultFastW, kMaxFastW);
        } else {
            health_info->maxChargingCurrentMicroamps = kRegularCurrentUa;
            health_info->maxChargingVoltageMicrovolts = kStandardUsbVoltageUv;
            return;
        }
        health_info->maxChargingCurrentMicroamps = watts * 1000000 / 5;
        health_info->maxChargingVoltageMicrovolts = kStandardUsbVoltageUv;
    }

    static bool ReadInt(const char* path, int* value) {
        std::string buf;
        return ::android::base::ReadFileToString(path, &buf) &&
               ::android::base::ParseInt(::android::base::Trim(buf), value);
    }

    // No phone battery holds 100 Ah; anything below this is a mAh value.
    static constexpr int32_t kMaxPlausibleMah = 100000;
    static constexpr const char* kUsbRealTypePath = "/sys/class/power_supply/usb/real_type";
    static constexpr int32_t kStandardUsbCurrentUa = 500000;
    static constexpr int32_t kStandardUsbVoltageUv = 5000000;
    static constexpr const char* kUsbQuickChargeTypePath =
            "/sys/class/power_supply/usb/quick_charge_type";
    static constexpr const char* kUsbPowerMaxPath = "/sys/class/power_supply/usb/power_max";
    static constexpr int kQuickChargeTurbo = 3;
    static constexpr int kDefaultTurboW = 33;
    static constexpr int kMaxTurboW = 240;  // keeps watts * 1000000 / 5 in an int32_t
    static constexpr int kDefaultFastW = 18;
    static constexpr int kMaxFastW = 25;
    static constexpr int32_t kRegularCurrentUa = 1500000;  // 7.5 W at 5 V: "Charging"
    const bool mScaleChargeCounter =
            ::android::base::GetBoolProperty("ro.vendor.health.charge_counter_in_mah", false);
    const bool mStaticUsbMax =
            ::android::base::GetBoolProperty("ro.vendor.health.usb_max_is_static", false);
};
}  // namespace aidl::android::hardware::health
static constexpr std::string_view gChargerArg{"--charger"};

#if !CHARGER_FORCE_NO_UI
namespace aidl::android::hardware::health {
class ChargerCallbackImpl : public ChargerCallback {
public:
    using ChargerCallback::ChargerCallback;
    bool ChargerEnableSuspend() override { return true; }
};
} // namespace aidl::android::hardware::health
#endif

int main(int argc, char** argv) {
#ifdef __ANDROID_RECOVERY__
    android::base::InitLogging(argv, android::base::KernelLogger);
#endif

    // make a default health service
    auto config = std::make_unique<healthd_config>();
    ::android::hardware::health::InitHealthdConfig(config.get());
    // Some fuel gauges report the state of health in a node of their own instead of the battery
    // power supply (e.g. Xiaomi bms/soh), so Settings > Device diagnostics showed none. Point the
    // HAL at it with ro.vendor.health.state_of_health_path; BatteryMonitor keeps a path set here.
    const std::string soh_path =
            ::android::base::GetProperty("ro.vendor.health.state_of_health_path", "");
    if (!soh_path.empty()) config->batteryStateOfHealthPath = soh_path.c_str();
    auto binder = ndk::SharedRefBase::make<aidl::android::hardware::health::MediatekHealth>(
            gInstanceName, std::move(config));

    if (argc >= 2 && argv[1] == gChargerArg) {
#if !CHARGER_FORCE_NO_UI
        // If charger shouldn't have UI for your device, simply drop the line below
        // for your service implementation. This corresponds to
        // ro.charger.no_ui=true
        return ChargerModeMain(binder, std::make_shared<aidl::android::hardware::health::ChargerCallbackImpl>(binder));
#endif

        LOG(INFO) << "Starting charger mode without UI.";
    } else {
        LOG(INFO) << "Starting health HAL.";
    }

    auto hal_health_loop = std::make_shared<HalHealthLoop>(binder, binder);
    return hal_health_loop->StartLoop();
}
