#include "config/config_types.h"
#include "dbus/upower/upower_service.h"
#include "system/battery_warning_monitor.h"
#include "tests/test_check.h"

namespace {
  UPowerDeviceInfo laptopBattery(const char* path) {
    UPowerDeviceInfo info;
    info.path = path;
    info.type = UPowerDeviceType::Battery;
    info.powerSupply = true;
    return info;
  }

} // namespace

int main() {
  BatteryConfig config; // warning_threshold defaults to 10; deviceThresholds empty

  const UPowerDeviceInfo systemBattery = laptopBattery("/org/freedesktop/UPower/devices/battery_BAT0");
  const UPowerDeviceInfo secondBattery = laptopBattery("/org/freedesktop/UPower/devices/battery_BAT1");

  // The system laptop battery keeps the global threshold on the combined charge.
  TEST_CHECK(batteryWarningThresholdForDevice(config, systemBattery, &systemBattery) == config.warningThreshold);

  // A second laptop pack stays silent on its own; an explicit entry warns it at its own percent.
  TEST_CHECK(batteryWarningThresholdForDevice(config, secondBattery, &systemBattery) == 0);
  BatteryConfig withSecondEntry;
  withSecondEntry.deviceThresholds.push_back({secondBattery.path, 15});
  TEST_CHECK(batteryWarningThresholdForDevice(withSecondEntry, secondBattery, &systemBattery) == 15);

  // No system battery pointer: a laptop pack resolves to entry-or-0, never the global threshold.
  TEST_CHECK(batteryWarningThresholdForDevice(config, secondBattery, nullptr) == 0);

  // A game controller warns at gaming_warning_threshold with no entry; an explicit entry wins; 0 disables it.
  UPowerDeviceInfo controller;
  controller.path = "/org/freedesktop/UPower/devices/gaming_input_hid_001";
  controller.model = "Xbox Wireless Controller";
  controller.type = UPowerDeviceType::GamingInput;
  TEST_CHECK(batteryWarningThresholdForDevice(config, controller, &systemBattery) == config.gamingWarningThreshold);
  TEST_CHECK(config.gamingWarningThreshold == 15);
  BatteryConfig withController;
  withController.deviceThresholds.push_back({"Xbox Wireless Controller", 30});
  TEST_CHECK(batteryWarningThresholdForDevice(withController, controller, &systemBattery) == 30);
  BatteryConfig gamingOff;
  gamingOff.gamingWarningThreshold = 0;
  TEST_CHECK(batteryWarningThresholdForDevice(gamingOff, controller, &systemBattery) == 0);

  return 0;
}
