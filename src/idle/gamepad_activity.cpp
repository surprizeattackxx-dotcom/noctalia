#include "idle/gamepad_activity.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

  constexpr Logger kLog("gamepad");
  constexpr auto kReportInterval = std::chrono::seconds(1);
  constexpr const char* kInputDir = "/dev/input";

  bool testBit(const std::uint8_t* bits, std::size_t bit) { return (bits[bit / 8] & (1U << (bit % 8))) != 0; }

  bool isControllerDevice(int fd) {
    std::array<std::uint8_t, (KEY_MAX / 8) + 1> keyBits{};
    std::array<std::uint8_t, (ABS_MAX / 8) + 1> absBits{};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, keyBits.size()), keyBits.data()) < 0) {
      return false;
    }
    if (ioctl(fd, EVIOCGBIT(EV_ABS, absBits.size()), absBits.data()) < 0) {
      return false;
    }
    // Pads advertise BTN_SOUTH/BTN_GAMEPAD, old joysticks BTN_TRIGGER; mice (BTN_LEFT), touchpads and
    // tablets also expose absolute axes, so they are excluded.
    const bool hasPadButtons = testBit(keyBits.data(), BTN_SOUTH) || testBit(keyBits.data(), BTN_TRIGGER);
    const bool isPointer = testBit(keyBits.data(), BTN_LEFT) || testBit(keyBits.data(), BTN_TOUCH);
    const bool hasAxes =
        testBit(absBits.data(), ABS_X) || testBit(absBits.data(), ABS_RX) || testBit(absBits.data(), ABS_HAT0X);
    return hasPadButtons && hasAxes && !isPointer;
  }

  std::string deviceName(int fd) {
    std::array<char, 128> name{};
    if (ioctl(fd, EVIOCGNAME(static_cast<int>(name.size())), name.data()) < 0) {
      return {};
    }
    return std::string(name.data());
  }

} // namespace

GamepadActivityMonitor::GamepadActivityMonitor() = default;

GamepadActivityMonitor::~GamepadActivityMonitor() { stop(); }

void GamepadActivityMonitor::setEnabled(bool enabled) {
  if (m_enabled == enabled) {
    return;
  }
  m_enabled = enabled;
  if (enabled) {
    start();
  } else {
    stop();
  }
}

void GamepadActivityMonitor::start() {
  m_inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (m_inotifyFd >= 0) {
    inotify_add_watch(m_inotifyFd, kInputDir, IN_CREATE | IN_ATTRIB);
  } else {
    kLog.debug("inotify unavailable ({}); controllers plugged in later are not detected", std::strerror(errno));
  }
  scanDevices();
}

void GamepadActivityMonitor::stop() {
  for (auto& device : m_devices) {
    if (device.fd >= 0) {
      close(device.fd);
    }
  }
  m_devices.clear();
  m_rejected.clear();
  if (m_inotifyFd >= 0) {
    close(m_inotifyFd);
    m_inotifyFd = -1;
  }
}

void GamepadActivityMonitor::scanDevices() {
  DIR* dir = opendir(kInputDir);
  if (dir == nullptr) {
    return;
  }
  while (const dirent* entry = readdir(dir)) {
    if (std::strncmp(entry->d_name, "event", 5) == 0) {
      tryOpen(std::string(kInputDir) + "/" + entry->d_name);
    }
  }
  closedir(dir);
}

void GamepadActivityMonitor::tryOpen(const std::string& path) {
  if (std::ranges::contains(m_rejected, path)
      || std::ranges::any_of(m_devices, [&](const Device& d) { return d.path == path; })) {
    return;
  }
  const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    // Permission errors are the normal case for keyboards and mice; remember so rescans stay cheap.
    // A device that was not readable yet (udev still applying the ACL) gets another chance on IN_ATTRIB.
    if (errno == EACCES || errno == EPERM) {
      m_rejected.push_back(path);
    }
    return;
  }
  if (!isControllerDevice(fd)) {
    close(fd);
    m_rejected.push_back(path);
    return;
  }

  Device device;
  device.fd = fd;
  device.path = path;
  device.name = deviceName(fd);
  for (const std::uint16_t code : {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y}) {
    input_absinfo info{};
    if (ioctl(fd, EVIOCGABS(code), &info) < 0 || info.maximum <= info.minimum) {
      continue;
    }
    device.axes.push_back(Device::Axis{.code = code, .minimum = info.minimum, .maximum = info.maximum});
  }
  kLog.info("watching controller '{}' ({}) for idle activity", device.name, path);
  m_devices.push_back(std::move(device));
}

void GamepadActivityMonitor::closeDevice(std::size_t index) {
  if (index >= m_devices.size()) {
    return;
  }
  kLog.info("controller '{}' removed", m_devices[index].name);
  close(m_devices[index].fd);
  // The node may come back under the same name later (replug); allow a fresh probe.
  std::erase(m_rejected, m_devices[index].path);
  m_devices.erase(m_devices.begin() + static_cast<std::ptrdiff_t>(index));
}

void GamepadActivityMonitor::doAddPollFds(std::vector<pollfd>& fds) {
  m_pollStart = fds.size();
  m_pollCount = 0;
  if (!m_enabled) {
    return;
  }
  if (m_inotifyFd >= 0) {
    fds.push_back(pollfd{.fd = m_inotifyFd, .events = POLLIN, .revents = 0});
    ++m_pollCount;
  }
  for (const auto& device : m_devices) {
    fds.push_back(pollfd{.fd = device.fd, .events = POLLIN, .revents = 0});
    ++m_pollCount;
  }
}

void GamepadActivityMonitor::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  if (!m_enabled || m_pollCount == 0) {
    return;
  }
  std::size_t index = startIdx;
  bool rescan = false;
  if (m_inotifyFd >= 0) {
    if ((fds[index].revents & POLLIN) != 0) {
      std::array<char, 4096> buffer{};
      while (read(m_inotifyFd, buffer.data(), buffer.size()) > 0) {
      }
      rescan = true;
    }
    ++index;
  }

  bool activity = false;
  std::vector<std::size_t> gone;
  for (std::size_t i = 0; index < startIdx + m_pollCount && i < m_devices.size(); ++i, ++index) {
    const short revents = fds[index].revents;
    if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      gone.push_back(i);
      continue;
    }
    if ((revents & POLLIN) != 0) {
      drainDevice(m_devices[i], activity);
    }
  }
  for (auto it = gone.rbegin(); it != gone.rend(); ++it) {
    closeDevice(*it);
  }
  if (rescan) {
    scanDevices();
  }
  if (activity) {
    reportActivity();
  }
}

void GamepadActivityMonitor::drainDevice(Device& device, bool& activity) {
  std::array<input_event, 64> events{};
  for (;;) {
    const ssize_t bytes = read(device.fd, events.data(), sizeof(events));
    if (bytes <= 0) {
      return; // EAGAIN: drained; ENODEV is caught by POLLERR/POLLHUP on the next round
    }
    const auto count = static_cast<std::size_t>(bytes) / sizeof(input_event);
    for (std::size_t i = 0; i < count; ++i) {
      const input_event& event = events[i];
      if (event.type == EV_KEY && event.value == 1) {
        activity = true; // button press
      } else if (event.type == EV_ABS) {
        for (auto& axis : device.axes) {
          if (axis.code != event.code) {
            continue;
          }
          const std::int32_t range = axis.maximum - axis.minimum;
          // Hats are digital (-1/0/1); sticks and triggers must move by >12% of their range from the
          // last counted position, which ignores drift and resting noise.
          if (!axis.seen || std::abs(event.value - axis.last) * 100 > range * 12) {
            if (axis.seen) {
              activity = true;
            }
            axis.last = event.value;
            axis.seen = true;
          }
        }
      }
    }
  }
}

void GamepadActivityMonitor::reportActivity() {
  const auto now = std::chrono::steady_clock::now();
  if (now - m_lastReport < kReportInterval) {
    return;
  }
  m_lastReport = now;
  if (m_callback) {
    m_callback();
  }
}
