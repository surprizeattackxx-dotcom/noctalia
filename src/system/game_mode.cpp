#include "system/game_mode.h"

#include "cli/schema_msg.h"
#include "core/log.h"
#include "ipc/ipc_service.h"
#include "util/string_utils.h"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace {

  constexpr Logger kLog("game-mode");
  constexpr auto kPollInterval = std::chrono::seconds(2);

  char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

} // namespace

bool GameMode::globMatch(std::string_view pattern, std::string_view text) {
  std::size_t p = 0;
  std::size_t t = 0;
  std::size_t star = std::string_view::npos;
  std::size_t mark = 0;
  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] != '*' && lower(pattern[p]) == lower(text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    ++p;
  }
  return p == pattern.size();
}

void GameMode::configure(const GameModeConfig& config) {
  m_config = config;
  restartTimer();
  tick();
}

void GameMode::restartTimer() {
  if (m_config.autoDetect && m_provider) {
    m_timer.startRepeating(kPollInterval, [this] { tick(); });
  } else {
    m_timer.stop();
    m_detected = false;
    m_detectedApp.clear();
    recompute();
  }
}

void GameMode::tick() {
  bool detected = false;
  std::string app;
  if (m_config.autoDetect && m_provider) {
    for (const std::string& candidate : m_provider(m_config.requireFocus)) {
      if (candidate.empty()) {
        continue;
      }
      if (std::ranges::any_of(m_config.apps, [&candidate](const std::string& pattern) {
            return globMatch(StringUtils::trim(pattern), candidate);
          })) {
        detected = true;
        app = candidate;
        break;
      }
    }
  }
  if (detected != m_detected || (detected && app != m_detectedApp)) {
    m_detected = detected;
    m_detectedApp = detected ? app : std::string{};
    recompute();
  }
}

void GameMode::setOverride(Override value) {
  m_override = value;
  recompute();
}

void GameMode::recompute() {
  const bool next = m_override == Override::On || (m_override == Override::Auto && m_detected);
  if (next == m_active) {
    return;
  }
  m_active = next;
  kLog.info("game mode {}{}", next ? "on" : "off", m_detectedApp.empty() ? "" : " (" + m_detectedApp + ")");
  if (m_callback) {
    m_callback(next);
  }
}

void GameMode::registerIpc(IpcService& ipc) {
  ipc.bind(noctalia::cli::msg::gameMode, [this](const std::string& args) -> std::string {
    const std::string arg = StringUtils::toLower(StringUtils::trim(args));
    if (arg.empty() || arg == "status") {
      std::string line = m_active ? "on" : "off";
      line +=
          m_override == Override::Auto ? " (auto)" : (m_override == Override::On ? " (forced on)" : " (forced off)");
      if (!m_detectedApp.empty()) {
        line += " " + m_detectedApp;
      }
      return line + "\n";
    }
    if (arg == "on") {
      setOverride(Override::On);
    } else if (arg == "off") {
      setOverride(Override::Off);
    } else if (arg == "auto") {
      setOverride(Override::Auto);
    } else if (arg == "toggle") {
      setOverride(m_active ? Override::Off : Override::On);
    } else {
      return "error: expected on, off, auto, toggle or status\n";
    }
    return "ok\n";
  });
}
