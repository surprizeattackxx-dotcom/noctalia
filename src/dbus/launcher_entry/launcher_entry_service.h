#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <sdbus-c++/sdbus-c++.h>
#include <string>
#include <string_view>
#include <unordered_map>

class SessionBus;

// Listens for the Unity launcher API (`com.canonical.Unity.LauncherEntry.Update`) that apps such as
// Discord, Telegram and Thunderbird broadcast on the session bus to publish an unread/badge count for
// their desktop entry. Plasma's task manager honors the same signal.
class LauncherEntryService {
public:
  using ChangeCallback = std::function<void()>;

  explicit LauncherEntryService(SessionBus& bus);
  ~LauncherEntryService();

  LauncherEntryService(const LauncherEntryService&) = delete;
  LauncherEntryService& operator=(const LauncherEntryService&) = delete;

  void setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

  // Unread count for a desktop entry id ("discord", "discord.desktop", case-insensitive); 0 when none or hidden.
  [[nodiscard]] std::int64_t countForEntry(std::string_view entryId) const;

  // Process-wide accessor so the dock model can read counts without plumbing; null before construction.
  [[nodiscard]] static const LauncherEntryService* instance() noexcept;

private:
  struct Entry {
    std::int64_t count = 0;
    bool countVisible = false;
  };

  std::unordered_map<std::string, Entry> m_entries; // key: lowercase desktop id without ".desktop"
  sdbus::Slot m_matchSlot;
  ChangeCallback m_changeCallback;
};
