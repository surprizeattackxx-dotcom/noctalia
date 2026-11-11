#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace settings {

  // One button in Noctalia Settings that opens the matching KDE System Settings page. `id` is also the
  // translation key segment (settings.schema.system.kde-settings.pages.<id>).
  struct KdeSystemSettingsPage {
    std::string_view id;
    std::string_view glyph;
    std::string_view program; // "systemsettings" (main window) or "kcmshell6" (one page)
    std::string_view module;  // kcmshell6 module, empty for the main window
  };

  inline constexpr std::array<KdeSystemSettingsPage, 10> kKdeSystemSettingsPages{{
      {"open", "settings", "systemsettings", ""},
      {"display", "device-desktop", "kcmshell6", "kcm_kscreen"},
      {"audio", "volume", "kcmshell6", "kcm_pulseaudio"},
      {"power", "bolt", "kcmshell6", "kcm_powerdevilprofilesconfig"},
      {"shortcuts", "keyboard", "kcmshell6", "kcm_keys"},
      {"notifications", "bell", "kcmshell6", "kcm_notifications"},
      {"colors", "palette", "kcmshell6", "kcm_colors"},
      {"fonts", "typography", "kcmshell6", "kcm_fonts"},
      {"icons", "icons", "kcmshell6", "kcm_icons"},
      {"autostart", "rocket", "kcmshell6", "kcm_autostart"},
  }};

  inline std::span<const KdeSystemSettingsPage> kdeSystemSettingsPages() { return kKdeSystemSettingsPages; }

  inline std::vector<std::string> kdeSystemSettingsCommand(const KdeSystemSettingsPage& page) {
    std::vector<std::string> command{std::string(page.program)};
    if (!page.module.empty()) {
      command.emplace_back(page.module);
    }
    return command;
  }

} // namespace settings
