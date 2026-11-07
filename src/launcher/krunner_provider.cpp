#include "launcher/krunner_provider.h"

#include "core/log.h"
#include "dbus/session_bus.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sdbus-c++/sdbus-c++.h>

namespace {

  constexpr Logger kLog("krunner");
  constexpr const char* kRunnerInterface = "org.kde.krunner1";
  constexpr std::size_t kMaxResults = 30;
  constexpr std::size_t kMinQueryLength = 2;

  using Match = sdbus::Struct<std::string, std::string, std::string, std::int32_t, double, std::map<std::string, sdbus::Variant>>;

  std::string desktopValue(const std::string& text, const std::string& key) {
    const std::string needle = key + "=";
    std::size_t pos = 0;
    while (pos < text.size()) {
      const std::size_t end = text.find('\n', pos);
      const std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
      if (line.starts_with(needle)) {
        return StringUtils::trim(line.substr(needle.size()));
      }
      if (end == std::string::npos) {
        break;
      }
      pos = end + 1;
    }
    return {};
  }

} // namespace

KrunnerProvider::KrunnerProvider(SessionBus& bus) { discoverRunners(bus); }

KrunnerProvider::~KrunnerProvider() = default;

void KrunnerProvider::discoverRunners(SessionBus& bus) {
  namespace fs = std::filesystem;
  std::vector<fs::path> dirs = {"/usr/share/krunner/dbusplugins"};
  if (const char* home = std::getenv("HOME"); home != nullptr) {
    dirs.emplace_back(fs::path(home) / ".local/share/krunner/dbusplugins");
  }
  for (const auto& dir : dirs) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      continue;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (entry.path().extension() != ".desktop") {
        continue;
      }
      std::ifstream file(entry.path());
      const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      const std::string service = desktopValue(text, "X-Plasma-DBusRunner-Service");
      if (service.empty()) {
        continue;
      }
      // Windows and activities already have Noctalia providers / KDE's own switchers.
      if (service == "org.kde.KWin" || service == "org.kde.runners.activities") {
        continue;
      }
      std::string path = desktopValue(text, "X-Plasma-DBusRunner-Path");
      if (path.empty()) {
        path = "/runner";
      }
      try {
        Runner runner;
        runner.name = desktopValue(text, "Name");
        runner.service = service;
        runner.path = path;
        runner.proxy = sdbus::createProxy(bus.connection(), sdbus::ServiceName{service}, sdbus::ObjectPath{path});
        kLog.info("KRunner D-Bus runner: {} ({})", runner.name, service);
        m_runners.push_back(std::move(runner));
      } catch (const sdbus::Error& e) {
        kLog.debug("skipping runner {}: {}", service, e.what());
      }
    }
  }
}

void KrunnerProvider::reset() {
  m_lastText.clear();
  m_results.clear();
  ++m_generation;
  m_pending = 0;
}

std::vector<LauncherResult> KrunnerProvider::query(std::string_view text) const {
  const std::string trimmed = StringUtils::trim(std::string(text));
  if (trimmed.size() < kMinQueryLength || m_runners.empty()) {
    m_lastText.clear();
    m_results.clear();
    return {};
  }
  if (trimmed == m_lastText) {
    return m_results;
  }
  m_lastText = trimmed;
  m_results.clear();
  const std::uint64_t generation = ++m_generation;
  m_pending = 0;

  for (const auto& runner : m_runners) {
    ++m_pending;
    runner.proxy->callMethodAsync("Match")
        .onInterface(kRunnerInterface)
        .withArguments(trimmed)
        .uponReplyInvoke([this, generation, service = runner.service, path = runner.path,
                          name = runner.name](std::optional<sdbus::Error> error, std::vector<Match> matches) {
          if (generation != m_generation) {
            return; // a newer query replaced this one
          }
          --m_pending;
          if (error.has_value()) {
            kLog.debug("{} Match failed: {}", service, error->what());
          } else {
            for (const auto& match : matches) {
              LauncherResult result;
              result.id = service + "\n" + path + "\n" + std::get<0>(match);
              result.title = std::get<1>(match);
              result.iconName = std::get<2>(match);
              result.score = std::get<4>(match);
              result.origin = name;
              result.glyphName = "file";
              const auto& props = std::get<5>(match);
              if (const auto it = props.find("subtext"); it != props.end()) {
                try {
                  result.subtitle = it->second.get<std::string>();
                } catch (const sdbus::Error&) {
                }
              }
              m_results.push_back(std::move(result));
            }
            std::ranges::stable_sort(m_results, [](const LauncherResult& a, const LauncherResult& b) {
              return a.score > b.score;
            });
            if (m_results.size() > kMaxResults) {
              m_results.resize(kMaxResults);
            }
          }
          if (m_resultsChanged) {
            m_resultsChanged();
          }
        });
  }
  return m_results;
}

bool KrunnerProvider::activate(const LauncherResult& result) {
  const std::size_t first = result.id.find('\n');
  const std::size_t second = first == std::string::npos ? std::string::npos : result.id.find('\n', first + 1);
  if (second == std::string::npos) {
    return false;
  }
  const std::string service = result.id.substr(0, first);
  const std::string matchId = result.id.substr(second + 1);
  for (const auto& runner : m_runners) {
    if (runner.service != service) {
      continue;
    }
    runner.proxy->callMethodAsync("Run")
        .onInterface(kRunnerInterface)
        .withArguments(matchId, std::string{})
        .uponReplyInvoke([service](std::optional<sdbus::Error> error) {
          if (error.has_value()) {
            kLog.warn("{} Run failed: {}", service, error->what());
          }
        });
    return true;
  }
  return false;
}
