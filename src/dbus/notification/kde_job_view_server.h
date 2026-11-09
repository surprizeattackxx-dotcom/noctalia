#pragma once

#include "core/timer_manager.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <string_view>
#include <unordered_map>

class SessionBus;
class NotificationManager;
struct Notification;
enum class NotificationEvent;

/// Plasma's job progress service (`org.kde.kuiserver`, `/JobViewServer`), so KIO apps such as Dolphin report file
/// copies, moves and deletes here instead of opening their own progress dialogs. Each job becomes one internal
/// notification with a progress bar and a Cancel button, shown once it has run for about a second.
class KdeJobViewServer {
public:
  KdeJobViewServer(SessionBus& bus, NotificationManager& manager);
  ~KdeJobViewServer();

  KdeJobViewServer(const KdeJobViewServer&) = delete;
  KdeJobViewServer& operator=(const KdeJobViewServer&) = delete;

private:
  friend struct KdeJobViewServerTestAccess;

  static constexpr std::string_view kOpenFolderAction = "kde-job-open-folder:";
  static std::string makeOpenFolderAction(const std::string& folder) { return std::string(kOpenFolderAction) + folder; }
  static std::optional<std::string> folderFromAction(std::string_view key) {
    if (!key.starts_with(kOpenFolderAction) || key.size() == kOpenFolderAction.size()) {
      return std::nullopt;
    }
    return std::string(key.substr(kOpenFolderAction.size()));
  }

  static std::optional<std::uint64_t> amountFromDouble(double value) {
    // 2^64 is exactly representable; uint64_t max is not representable as a double.
    if (!std::isfinite(value) || value >= 18446744073709551616.0) {
      return std::nullopt;
    }
    return value <= 0 ? 0 : static_cast<std::uint64_t>(value);
  }
  static std::optional<std::uint64_t> asU64(const sdbus::Variant& value);

  struct Job {
    std::optional<int> effectivePercent() const {
      if (percent.has_value() || totalBytes == 0) {
        return percent;
      }
      if (processedBytes >= totalBytes) {
        return 100;
      }
      // Compare against ceil(totalBytes * p / 100), without overflowing either product.
      int low = 0, high = 100;
      while (low + 1 < high) {
        const auto mid = static_cast<std::uint64_t>((low + high) / 2);
        const auto threshold = (totalBytes / 100) * mid + ((totalBytes % 100) * mid + 99) / 100;
        if (processedBytes >= threshold) {
          low = static_cast<int>(mid);
        } else {
          high = static_cast<int>(mid);
        }
      }
      return low;
    }

    void authorizeSender(std::string_view caller) const {
      if (sender.empty() || sender.front() != ':' || caller != sender) {
        throw sdbus::Error(
            sdbus::Error::Name{"org.freedesktop.DBus.Error.AccessDenied"}, "Job belongs to another sender"
        );
      }
    }

    bool setDescriptionField(std::uint32_t number, const std::string& name, const std::string& value) {
      if (number > 1) {
        return false;
      }
      (number == 0 ? label1 : label2) = name;
      (number == 0 ? value1 : value2) = value;
      dirty = true;
      return true;
    }
    std::uint32_t serial = 0;
    std::string path;
    std::string sender;
    std::string desktopEntry;
    std::string appName;
    std::string appIcon;
    int capabilities = 0;
    bool immediate = false;
    std::unique_ptr<sdbus::IObject> object;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();

    std::string title;
    std::string infoMessage;
    std::string label1, value1, label2, value2;
    std::string destUrl;
    std::optional<int> percent;
    bool suspended = false;
    std::uint64_t processedBytes = 0, totalBytes = 0;
    std::uint64_t processedFiles = 0, totalFiles = 0;
    std::uint64_t processedItems = 0, totalItems = 0;
    std::uint64_t speed = 0;
    std::uint32_t errorCode = 0;

    std::uint32_t notificationId = 0;
    bool dirty = true;
    bool dismissed = false; // the user closed the popup; stay quiet until the job ends
    bool reopen = false;    // an action button closed the popup; show it again instead of treating it as dismissed
    bool dead = false;      // finished; removed on the next tick (cannot unregister inside its own method call)
  };

  sdbus::ObjectPath requestView(
      const std::string& desktopEntry, const std::string& appName, const std::string& appIcon, int capabilities,
      bool immediate
  );
  void registerJobObject(Job& job);
  Job* findJob(std::uint32_t serial);
  Job& authorizedJob(std::uint32_t serial);
  void cleanupRegistrations() noexcept;
  void update(std::uint32_t serial, const std::map<std::string, sdbus::Variant>& properties);
  void terminate(std::uint32_t serial, std::uint32_t errorCode, const std::string& errorMessage);
  void finish(Job& job, std::uint32_t errorCode, const std::string& errorMessage);
  void emitRequest(Job& job, const char* signal);
  void tick();
  void pushNotification(Job& job);
  void onNotificationEvent(const Notification& n, NotificationEvent event);
  void onAction(std::uint32_t id, const std::string& actionKey);
  void onNameOwnerChanged(const std::string& name, const std::string& newOwner);
  void ensureTimer();
  void publishState();
  void showDismissed();

  SessionBus& m_bus;
  NotificationManager& m_manager;
  std::unique_ptr<sdbus::IObject> m_serverObject;
  std::unique_ptr<sdbus::IProxy> m_nameWatch;
  std::unordered_map<std::uint32_t, std::unique_ptr<Job>> m_jobs;
  std::uint32_t m_nextSerial = 1;
  int m_eventToken = -1;
  int m_actionToken = -1;
  Timer m_timer;
  bool m_nameAcquired = false;
  bool m_jobViewNameAcquired = false;
  bool m_showHandlerRegistered = false;
};
