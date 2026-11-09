#pragma once

#include "app/poll_source.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Watches game controllers (evdev joysticks/gamepads) so controller input counts as user activity:
// compositors only track keyboard, pointer and touch, so a long session with a pad would otherwise let
// the idle behaviors (lock, screen off, suspend) fire mid-game. Devices are found at startup and as they
// are plugged in. Needs read access to the device node, which logind grants to the logged-in seat user
// for joysticks (uaccess), so no extra group is required on most systems.
class GamepadActivityMonitor final : public PollSource {
public:
  using ActivityCallback = std::function<void()>;

  GamepadActivityMonitor();
  ~GamepadActivityMonitor() override;

  GamepadActivityMonitor(const GamepadActivityMonitor&) = delete;
  GamepadActivityMonitor& operator=(const GamepadActivityMonitor&) = delete;

  void setActivityCallback(ActivityCallback callback) { m_callback = std::move(callback); }
  void setEnabled(bool enabled);
  [[nodiscard]] bool enabled() const noexcept { return m_enabled; }
  [[nodiscard]] std::size_t deviceCount() const noexcept { return m_devices.size(); }

  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  struct Device {
    int fd = -1;
    std::string path;
    std::string name;
    // Axis state used to ignore stick drift: only large moves count as activity.
    struct Axis {
      std::uint16_t code = 0;
      std::int32_t minimum = 0;
      std::int32_t maximum = 0;
      std::int32_t last = 0;
      bool seen = false;
    };
    std::vector<Axis> axes;
  };

  void start();
  void stop();
  void scanDevices();
  void tryOpen(const std::string& path);
  void closeDevice(std::size_t index);
  void drainDevice(Device& device, bool& activity);
  void reportActivity();

  bool m_enabled = false;
  int m_inotifyFd = -1;
  std::vector<Device> m_devices;
  std::vector<std::string> m_rejected; // paths already probed and not a controller (or unreadable)
  ActivityCallback m_callback;
  std::chrono::steady_clock::time_point m_lastReport{};
  std::size_t m_pollStart = 0;
  std::size_t m_pollCount = 0;
};
