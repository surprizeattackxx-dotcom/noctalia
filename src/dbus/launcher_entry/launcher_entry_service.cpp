#include "dbus/launcher_entry/launcher_entry_service.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "util/string_utils.h"

#include <map>
#include <sdbus-c++/sdbus-c++.h>

namespace {

  constexpr Logger kLog("launcher-entry");
  const LauncherEntryService* g_instance = nullptr;

  std::string normalizeEntryId(std::string_view id) {
    constexpr std::string_view kScheme = "application://";
    constexpr std::string_view kSuffix = ".desktop";
    if (id.starts_with(kScheme)) {
      id.remove_prefix(kScheme.size());
    }
    if (id.ends_with(kSuffix)) {
      id.remove_suffix(kSuffix.size());
    }
    return StringUtils::toLower(id);
  }

} // namespace

LauncherEntryService::LauncherEntryService(SessionBus& bus) {
  try {
    m_matchSlot = bus.connection().addMatch(
        "type='signal',interface='com.canonical.Unity.LauncherEntry',member='Update'",
        [this](sdbus::Message message) {
          try {
            std::string appUri;
            std::map<std::string, sdbus::Variant> properties;
            message >> appUri >> properties;
            const std::string key = normalizeEntryId(appUri);
            if (key.empty()) {
              return;
            }
            Entry entry = m_entries[key];
            if (const auto it = properties.find("count"); it != properties.end()) {
              entry.count = it->second.get<std::int64_t>();
            }
            if (const auto it = properties.find("count-visible"); it != properties.end()) {
              entry.countVisible = it->second.get<bool>();
            }
            const Entry previous = m_entries[key];
            m_entries[key] = entry;
            if (previous.count != entry.count || previous.countVisible != entry.countVisible) {
              kLog.debug("{} count={} visible={}", key, entry.count, entry.countVisible);
              if (m_changeCallback) {
                m_changeCallback();
              }
            }
          } catch (const sdbus::Error& e) {
            kLog.debug("ignoring malformed LauncherEntry update: {}", e.what());
          }
        },
        sdbus::return_slot
    );
    g_instance = this;
  } catch (const sdbus::Error& e) {
    kLog.warn("cannot watch launcher entry badges: {}", e.what());
  }
}

LauncherEntryService::~LauncherEntryService() {
  if (g_instance == this) {
    g_instance = nullptr;
  }
}

std::int64_t LauncherEntryService::countForEntry(std::string_view entryId) const {
  const auto it = m_entries.find(normalizeEntryId(entryId));
  if (it == m_entries.end() || !it->second.countVisible || it->second.count <= 0) {
    return 0;
  }
  return it->second.count;
}

const LauncherEntryService* LauncherEntryService::instance() noexcept { return g_instance; }
