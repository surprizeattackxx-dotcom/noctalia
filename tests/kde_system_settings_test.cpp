// The KDE System Settings group in Noctalia's Settings window: hidden unless KDE settings are available,
// one clickable button per KDE page, every label translated, and every page launching a real KDE command.

#include "config/config_types.h"
#include "i18n/i18n_service.h"
#include "shell/settings/kde_system_settings.h"
#include "shell/settings/settings_registry.h"
#include "tests/test_check.h"

#include <algorithm>
#include <cstdlib>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

  constexpr std::string_view kGroup = "kde-system-settings";

  std::vector<const settings::SettingEntry*> kdeEntries(const std::vector<settings::SettingEntry>& registry) {
    std::vector<const settings::SettingEntry*> out;
    for (const auto& entry : registry) {
      if (entry.section == settings::SettingsSection::System && entry.group == kGroup) {
        out.push_back(&entry);
      }
    }
    return out;
  }

} // namespace

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::println(stderr, "kde_system_settings_test: expected assets directory");
    return 1;
  }
  setenv("NOCTALIA_ASSETS_DIR", argv[1], 1);
  i18n::Service::instance().init("en");

  const Config config{};

  // Hidden when KDE settings are not available (non-KDE compositor or no systemsettings binary).
  {
    settings::RegistryEnvironment env;
    TEST_CHECK(!env.kdeSystemSettingsAvailable);
    TEST_CHECK(kdeEntries(settings::buildSettingsRegistry(config, nullptr, nullptr, env)).empty());
  }

  // Shown with one button per page.
  settings::RegistryEnvironment env;
  env.kdeSystemSettingsAvailable = true;
  const auto registry = settings::buildSettingsRegistry(config, nullptr, nullptr, env);
  const auto entries = kdeEntries(registry);
  const auto pages = settings::kdeSystemSettingsPages();
  TEST_CHECK(entries.size() == pages.size());
  TEST_CHECK(pages.size() == 10);

  std::set<std::string> titles;
  for (const auto* entry : entries) {
    const auto* button = std::get_if<settings::ButtonSetting>(&entry->control);
    TEST_CHECK(button != nullptr);
    if (button == nullptr) {
      continue;
    }
    TEST_CHECK(static_cast<bool>(button->action));
    TEST_CHECK(!button->label.empty());
    TEST_CHECK(!button->glyph.empty());
    TEST_CHECK(entry->path.empty()); // a button has no config key behind it
    // A missing en.json key would leave the raw key as the visible text.
    TEST_CHECK(!entry->title.empty() && !entry->title.starts_with("settings."));
    TEST_CHECK(!entry->subtitle.empty() && !entry->subtitle.starts_with("settings."));
    TEST_CHECK(!button->label.starts_with("settings."));
    TEST_CHECK(titles.insert(entry->title).second);
    TEST_CHECK(settings::matchesSettingQuery(*entry, "kde"));
  }

  // Every page launches KDE's own tools, and a module page names a real kcm module.
  int mainWindows = 0;
  std::set<std::string_view> ids;
  for (const auto& page : pages) {
    TEST_CHECK(ids.insert(page.id).second);
    TEST_CHECK(!page.glyph.empty());
    if (page.program == "systemsettings") {
      TEST_CHECK(page.module.empty());
      ++mainWindows;
    } else {
      TEST_CHECK(page.program == "kcmshell6");
      TEST_CHECK(page.module.starts_with("kcm_"));
    }
    const auto command = settings::kdeSystemSettingsCommand(page);
    TEST_CHECK(!command.empty());
    TEST_CHECK(command.front() == page.program);
    TEST_CHECK(command.size() == (page.module.empty() ? 1U : 2U));
  }
  TEST_CHECK(mainWindows == 1);
  return 0;
}
