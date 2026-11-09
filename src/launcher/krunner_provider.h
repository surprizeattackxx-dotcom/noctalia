#pragma once

#include "launcher/launcher_provider.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class SessionBus;

namespace sdbus {
  class IProxy;
}

// Searches KDE's D-Bus runners (org.kde.krunner1): Baloo file search, browser history and tabs, ...
// KRunner's C++ plugin runners have no external query API, so only D-Bus runners are reachable.
// Queries run asynchronously on the session bus; results land on the main thread.
class KrunnerProvider : public LauncherProvider {
public:
  explicit KrunnerProvider(SessionBus& bus);
  ~KrunnerProvider() override;

  [[nodiscard]] std::string_view defaultPrefix() const override { return "krunner"; }
  [[nodiscard]] std::string_view id() const override { return "KRunner"; }
  [[nodiscard]] std::string displayName() const override { return "KRunner"; }
  [[nodiscard]] std::string_view defaultGlyphName() const override { return "file-search"; }

  void setResultsChangedCallback(std::function<void()> callback) override { m_resultsChanged = std::move(callback); }
  [[nodiscard]] bool isLoading() const override { return m_pending > 0; }
  void reset() override;

  [[nodiscard]] std::vector<LauncherResult> query(std::string_view text) const override;
  bool activate(const LauncherResult& result) override;

private:
  struct Runner {
    std::string name;
    std::string service;
    std::string path;
    std::unique_ptr<sdbus::IProxy> proxy;
  };

  void discoverRunners(SessionBus& bus);

  std::vector<Runner> m_runners;
  std::function<void()> m_resultsChanged;
  mutable std::string m_lastText;
  mutable std::vector<LauncherResult> m_results;
  mutable std::uint64_t m_generation = 0;
  mutable int m_pending = 0;
};
