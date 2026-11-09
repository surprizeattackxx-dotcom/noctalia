#include "pipewire/audio_card_profiles.h"

#include "core/log.h"
#include "system/process.h"

#include <chrono>
#include <nlohmann/json.hpp>

namespace {

  constexpr Logger kLog("audio-cards");

  std::string jsonString(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
  }

} // namespace

std::optional<AudioCardInfo> queryAudioCardProfiles(std::uint32_t deviceId) {
  if (deviceId == 0) {
    return std::nullopt;
  }
  const auto result = process::runSyncWithTimeout(
      std::vector<std::string>{"pactl", "-f", "json", "list", "cards"}, std::chrono::seconds(2)
  );
  if (!result) {
    return std::nullopt;
  }
  const auto parsed = nlohmann::json::parse(result.out, nullptr, false);
  if (!parsed.is_array()) {
    return std::nullopt;
  }

  for (const auto& card : parsed) {
    const auto props = card.find("properties");
    if (props == card.end() || !props->is_object()) {
      continue;
    }
    const std::string objectId = jsonString(*props, "object.id");
    if (objectId.empty() || objectId != std::to_string(deviceId)) {
      continue;
    }

    AudioCardInfo info;
    info.name = jsonString(card, "name");
    info.description = jsonString(*props, "device.description");
    const std::string activeProfile = jsonString(card, "active_profile");
    const auto profiles = card.find("profiles");
    if (profiles == card.end() || !profiles->is_object()) {
      return std::nullopt;
    }
    for (const auto& [name, profile] : profiles->items()) {
      const bool active = name == activeProfile;
      const bool available = profile.value("available", true);
      if (name == "off" || (!available && !active)) {
        continue;
      }
      info.profiles.push_back(
          AudioCardProfile{
              .name = name, .description = jsonString(profile, "description"), .available = available, .active = active
          }
      );
    }
    if (info.name.empty() || info.profiles.empty()) {
      return std::nullopt;
    }
    return info;
  }
  return std::nullopt;
}

bool setAudioCardProfile(const std::string& cardName, const std::string& profileName) {
  if (cardName.empty() || profileName.empty()) {
    return false;
  }
  kLog.info("setting card {} profile {}", cardName, profileName);
  return process::runAsync(std::vector<std::string>{"pactl", "set-card-profile", cardName, profileName});
}
