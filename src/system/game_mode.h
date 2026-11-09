#pragma once

#include "config/config_types.h"
#include "core/timer_manager.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

class IpcService;

// Game mode: while a game is focused (or the user forces it on), the shell quiets itself. The class only
// decides *when* game mode is active; what happens on entry and exit (do-not-disturb, idle inhibit, power
// profile, hooks) is done by the Application through the change callback.
//
// Detection polls the focused window's app id every two seconds against the `game_mode.apps` globs
// (`steam_app_*` covers every Proton game launched from Steam). A manual override from IPC wins over detection
// until set back to auto.
class GameMode {
public:
  using ChangeCallback = std::function<void(bool active)>;
  /// App ids to test against `apps`: just the focused window, or every running window when `requireFocus` is off.
  using ActiveAppProvider = std::function<std::vector<std::string>(bool focusedOnly)>;

  enum class Override : std::uint8_t { Auto, On, Off };

  GameMode() = default;
  ~GameMode() = default;

  GameMode(const GameMode&) = delete;
  GameMode& operator=(const GameMode&) = delete;

  void setChangeCallback(ChangeCallback callback) { m_callback = std::move(callback); }
  void setActiveAppProvider(ActiveAppProvider provider) { m_provider = std::move(provider); }
  void configure(const GameModeConfig& config);
  void setOverride(Override value);

  [[nodiscard]] bool active() const noexcept { return m_active; }
  [[nodiscard]] Override overrideState() const noexcept { return m_override; }
  [[nodiscard]] const GameModeConfig& config() const noexcept { return m_config; }
  /// App id of the window that triggered detection; empty when not detected.
  [[nodiscard]] const std::string& detectedApp() const noexcept { return m_detectedApp; }

  void registerIpc(IpcService& ipc);

  /// Case-insensitive glob match where `*` matches any run of characters.
  [[nodiscard]] static bool globMatch(std::string_view pattern, std::string_view text);

private:
  void tick();
  void recompute();
  void restartTimer();

  GameModeConfig m_config;
  ChangeCallback m_callback;
  ActiveAppProvider m_provider;
  Override m_override = Override::Auto;
  bool m_detected = false;
  bool m_active = false;
  std::string m_detectedApp;
  Timer m_timer;
};
